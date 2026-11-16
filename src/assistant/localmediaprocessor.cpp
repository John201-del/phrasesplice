#include "localmediaprocessor.h"
#include "workspacepaths.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSize>
#include <QUuid>
#include <QVariant>
#include <cmath>
#include <array>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
bool reparse(const QFileInfo &file)
{
#ifdef Q_OS_WIN
    const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(file.absoluteFilePath()).utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    return file.isSymLink() || file.isJunction();
}
bool safePath(const QString &path)
{
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    if (!WorkspacePaths::isInside(path)) return false;
    QFileInfo current(clean);
    while (!current.filePath().isEmpty()) {
        if (reparse(current)) return false;
        const QString parent = current.absolutePath();
        if (parent == current.filePath()) break;
        current.setFile(parent);
    }
    return true;
}
bool ordinaryLocalFile(const QString &path)
{
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    if (!QDir::isAbsolutePath(clean) || !QFileInfo(clean).isFile()) return false;
#ifdef Q_OS_WIN
    if (clean.size() < 4 || clean.at(1) != QLatin1Char(':') || clean.at(2) != QLatin1Char('/')) return false;
    const auto driveType = GetDriveTypeW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(clean.left(3)).utf16()));
    if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE && driveType != DRIVE_CDROM) return false;
#endif
    QFileInfo current(clean);
    while (!current.filePath().isEmpty()) {
        if (reparse(current)) return false;
        const auto parent = current.absolutePath();
        if (parent == current.filePath()) break;
        current.setFile(parent);
    }
    return true;
}
QByteArray snapshot(const QString &path)
{
    const QFileInfo file(path);
    if (!file.isFile() || !file.isReadable()) return {};
    return QByteArray::number(file.size()) + ':' + QByteArray::number(file.lastModified().toMSecsSinceEpoch());
}
QJsonObject stream(const QJsonObject &metadata, const QString &type)
{
    for (const auto &value : metadata.value(QStringLiteral("streams")).toArray()) {
        const auto item = value.toObject();
        if (item.value(QStringLiteral("codec_type")).toString() == type) return item;
    }
    return {};
}
double duration(const QJsonObject &metadata)
{
    return metadata.value(QStringLiteral("format")).toObject().value(QStringLiteral("duration")).toVariant().toDouble();
}
QSize displaySize(const QJsonObject &video)
{
    const QString sar = video.value(QStringLiteral("sample_aspect_ratio")).toString();
    if (!sar.isEmpty() && sar != QStringLiteral("N/A") && sar != QStringLiteral("0:1")) {
        const auto parts = sar.split(QLatin1Char(':'));
        bool first = false, second = false;
        const auto num = parts.value(0).toLongLong(&first), den = parts.value(1).toLongLong(&second);
        if (parts.size() != 2 || !first || !second || num <= 0 || num != den) return {};
    }
    const auto quarterTurn = [](double angle) {
        if (!std::isfinite(angle)) return -1;
        double normalized = std::fmod(angle, 360.0);
        if (normalized < 0) normalized += 360;
        const int quarter = qRound(normalized / 90);
        return std::abs(normalized - quarter * 90) < .001 ? quarter % 4 : -1;
    };
    int quarter = 0;
    bool matrixFound = false;
    for (const auto &value : video.value(QStringLiteral("side_data_list")).toArray()) {
        const auto data = value.toObject();
        if (data.value(QStringLiteral("side_data_type")) != QStringLiteral("Display Matrix")) continue;
        if (matrixFound) return {};
        matrixFound = true;
        QStringList numbers;
        for (const auto &line : data.value(QStringLiteral("displaymatrix")).toString().split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            if (!line.contains(QLatin1Char(':'))) return {};
            numbers.append(line.section(QLatin1Char(':'), 1).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts));
        }
        if (numbers.size() != 9) return {};
        std::array<qint64, 9> matrix{};
        for (int i = 0; i < 9; ++i) { bool ok = false; matrix[i] = numbers[i].toLongLong(&ok); if (!ok) return {}; }
        if (matrix[2] || matrix[5] || matrix[6] || matrix[7] || matrix[8] != 1073741824) return {};
        const std::array<qint64, 4> axes{matrix[0], matrix[1], matrix[3], matrix[4]};
        const std::array<std::array<qint64, 4>, 4> rotations{{{65536, 0, 0, 65536}, {0, -65536, 65536, 0},
            {-65536, 0, 0, -65536}, {0, 65536, -65536, 0}}};
        quarter = -1;
        for (int i = 0; i < 4; ++i) if (axes == rotations[i]) quarter = i;
        if (quarter < 0 || (data.contains(QStringLiteral("rotation"))
            && (!data.value(QStringLiteral("rotation")).isDouble() || quarterTurn(data.value(QStringLiteral("rotation")).toDouble()) != quarter))) return {};
    }
    if (!matrixFound && video.value(QStringLiteral("tags")).toObject().contains(QStringLiteral("rotate"))) {
        bool ok = false;
        const auto angle = video.value(QStringLiteral("tags")).toObject().value(QStringLiteral("rotate")).toVariant().toDouble(&ok);
        quarter = ok ? quarterTurn(angle) : -1;
        if (quarter < 0) return {};
    }
    const int width = video.value(QStringLiteral("width")).toInt(), height = video.value(QStringLiteral("height")).toInt();
    return width > 0 && height > 0 ? (quarter % 2 ? QSize(height, width) : QSize(width, height)) : QSize();
}
}
LocalMediaProcessor::LocalMediaProcessor(QObject *parent) : QObject(parent)
{
#ifdef Q_OS_WIN
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) setProperty("processJob", QVariant::fromValue<qulonglong>(reinterpret_cast<qulonglong>(job)));
        else CloseHandle(job);
    }
    connect(&m_process, &QProcess::started, this, [this] {
        const HANDLE jobHandle = reinterpret_cast<HANDLE>(property("processJob").toULongLong());
        const HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(m_process.processId()));
        const bool assigned = jobHandle && child && AssignProcessToJobObject(jobHandle, child);
        if (child) CloseHandle(child);
        if (!assigned) { m_cancelled = true; m_process.kill(); }
    });
#endif
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
        m_stdout += m_process.readAllStandardOutput();
        if (m_stage != 2) return;
        int newline;
        while ((newline = m_stdout.indexOf('\n')) >= 0) {
            const auto object = QJsonDocument::fromJson(m_stdout.left(newline)).object();
            m_stdout.remove(0, newline + 1);
            consumeReport(object);
            const auto value = object.value(QStringLiteral("progress"));
            if (value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0 && value.toDouble() <= 100)
                Q_EMIT progress(QStringLiteral("正在本地处理素材：%1%").arg(qRound(value.toDouble())));
        }
    });
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] { m_process.readAllStandardError(); });
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &LocalMediaProcessor::completed);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_busy) fail(QStringLiteral("local_process_start_failed"));
    });
}
LocalMediaProcessor::~LocalMediaProcessor()
{
    disconnect(&m_process, nullptr, this, nullptr);
#ifdef Q_OS_WIN
    const HANDLE jobHandle = reinterpret_cast<HANDLE>(property("processJob").toULongLong());
    if (jobHandle) { TerminateJobObject(jobHandle, 1); CloseHandle(jobHandle); }
#endif
    m_process.kill();
    m_process.waitForFinished(3000);
    if (m_busy) { QFile::remove(m_output); QFile::remove(m_background); }
}
void LocalMediaProcessor::configure(const QString &ffmpeg, const QString &ffprobe, const QString &python, const QString &script,
                                  const QString &modelRoot, const QString &runtimeRoot, const QString &outputRoot)
{
    if (m_busy) return;
    m_ffmpeg = ffmpeg; m_ffprobe = ffprobe; m_python = python; m_script = script;
    m_models = modelRoot; m_runtime = runtimeRoot; m_outputRoot = outputRoot;
}
void LocalMediaProcessor::fail(const QString &message)
{
#ifdef Q_OS_WIN
    const HANDLE jobHandle = reinterpret_cast<HANDLE>(property("processJob").toULongLong());
    if (jobHandle) TerminateJobObject(jobHandle, 1);
#endif
    m_busy = false;
    if (!m_output.isEmpty()) QFile::remove(m_output);
    if (!m_background.isEmpty()) QFile::remove(m_background);
    m_output.clear(); m_background.clear();
    const QMap<QString, QString> messages{
        {QStringLiteral("unsupported_operation"), QStringLiteral("不支持该本地处理操作。")},
        {QStringLiteral("invalid_local_source"), QStringLiteral("请选择真实的本机素材文件，不能使用网络路径或链接。")},
        {QStringLiteral("unsafe_local_storage"), QStringLiteral("本地处理目录必须位于当前工作区内的普通目录。")},
        {QStringLiteral("local_runtime_missing"), QStringLiteral("本地处理运行环境尚未准备完整。")},
        {QStringLiteral("output_directory_failed"), QStringLiteral("无法创建本地处理结果目录。")},
        {QStringLiteral("source_read_failed"), QStringLiteral("无法读取原始素材。")},
        {QStringLiteral("output_reservation_failed"), QStringLiteral("无法为处理结果创建独立文件。")},
        {QStringLiteral("unsafe_local_cache"), QStringLiteral("无法使用工作区内的本地缓存目录。")},
        {QStringLiteral("unsafe_media_path"), QStringLiteral("素材路径发生变化，已停止本地处理。")},
        {QStringLiteral("cancelled"), QStringLiteral("已取消本地处理。")},
        {QStringLiteral("local_process_start_failed"), QStringLiteral("无法启动本地处理程序，请检查运行环境。")},
        {QStringLiteral("local_process_failed"), QStringLiteral("本地处理程序未能完成，请检查模型和运行环境。")},
        {QStringLiteral("invalid_processor_result"), QStringLiteral("本地处理程序没有返回有效结果。")},
        {QStringLiteral("invalid_media"), QStringLiteral("处理结果不是有效的媒体文件。")},
        {QStringLiteral("invalid_audio_stream"), QStringLiteral("处理结果的音频格式无效。")},
        {QStringLiteral("invalid_video_stream"), QStringLiteral("素材的画面尺寸或帧率无效。")},
        {QStringLiteral("media_duration_changed"), QStringLiteral("处理结果的时长与原始素材不匹配。")},
        {QStringLiteral("video_geometry_changed"), QStringLiteral("处理结果的画面尺寸或帧率发生变化。")},
        {QStringLiteral("unsupported_display_geometry"), QStringLiteral("素材含非方形像素或特殊画面变换，请先转为正常显示方向和方形像素后再处理。")},
        {QStringLiteral("audio_track_lost"), QStringLiteral("处理结果缺少原始音轨。")},
        {QStringLiteral("alpha_output_missing"), QStringLiteral("去背景结果缺少可合成的透明通道。")},
        {QStringLiteral("invalid_photo"), QStringLiteral("请选择可解码的 8 位 JPG 或 PNG 照片（最多 2400 万像素）。")},
        {QStringLiteral("invalid_photo_options"), QStringLiteral("修图参数无效，请确认磨皮强度、瑕疵位置或背景。")},
        {QStringLiteral("no_face"), QStringLiteral("未检测到清晰的正面人脸，请换一张照片。")},
        {QStringLiteral("no_person"), QStringLiteral("未找到足够清晰的人像，无法完成抠图。")},
        {QStringLiteral("invalid_background_photo"), QStringLiteral("换背景请使用不带透明通道的照片，或指定纯色。")},
        {QStringLiteral("missing_matte_model"), QStringLiteral("本地人像抠图模型缺失。")},
        {QStringLiteral("photo_output_conflict"), QStringLiteral("无法保存 PNG：目标名称冲突或不可用。已有文件已保留。")},
        {QStringLiteral("photo_processing_failed"), QStringLiteral("本地修图失败，请检查照片格式或尝试更小的图片。")},
        {QStringLiteral("face_tracking_lost"), QStringLiteral("视频中有画面无法可靠定位单个人脸，未生成成片。请缩短到人脸清晰且始终可见的片段。")},
        {QStringLiteral("single_face_required"), QStringLiteral("没有检测到唯一的清晰正面人脸，请用单人照片。")},
        {QStringLiteral("invisible_sticker"), QStringLiteral("头像图片是透明的，请选择包含可见头像的图片。")},
        {QStringLiteral("invalid_cover_video"), QStringLiteral("卡通遮脸支持最长 2 分钟的视频，请先导入头像，或先剪短源视频。")},
        {QStringLiteral("invalid_animated_video"), QStringLiteral("立体口型卡通头支持最长 30 秒、最高 4K 和 60 帧/秒的视频。")},
        {QStringLiteral("animated_model_unavailable"), QStringLiteral("本地人脸关键点模型缺失或校验失败。")},
        {QStringLiteral("source_changed"), QStringLiteral("原始素材在处理期间发生变化，已停止导入。")}
    };
    Q_EMIT failed(messages.value(message, QStringLiteral("本地处理未能完成。")));
}
void LocalMediaProcessor::process(const QString &sourcePath, const QString &operation, const QString &stickerPath)
{
    if (m_busy) { Q_EMIT failed(QStringLiteral("正在处理另一份素材，请等待或取消后重试。")); return; }
    m_output.clear(); m_background.clear();
    m_photoDestination.clear(); m_photoOptions = {};
    const QStringList allowed{QStringLiteral("denoise_audio"), QStringLiteral("separate_vocals"), QStringLiteral("remove_background"),
        QStringLiteral("enhance_video"), QStringLiteral("smooth_skin"), QStringLiteral("cover_face"), QStringLiteral("animate_face")};
    if (!allowed.contains(operation)) { fail(QStringLiteral("unsupported_operation")); return; }
    m_extraSource = operation == QStringLiteral("cover_face") ? stickerPath : QString();
    if (operation == QStringLiteral("cover_face") && !ordinaryLocalFile(m_extraSource)) {
        fail(QStringLiteral("invalid_local_source")); return;
    }
    if (prepare(sourcePath, operation)) startProbe(m_source, false);
}
void LocalMediaProcessor::retouchPhoto(const QString &sourcePath, const QString &destination, const QJsonObject &options)
{
    if (m_busy) { Q_EMIT failed(QStringLiteral("正在处理另一份素材，请等待或取消后重试。")); return; }
    m_output.clear(); m_background.clear();
    const QFileInfo dest(destination);
    if (!dest.isAbsolute() || dest.suffix().compare(QStringLiteral("png"), Qt::CaseInsensitive) != 0
        || !safePath(destination) || dest.absolutePath() != QDir(m_outputRoot).absolutePath() || dest.exists()) {
        fail(QStringLiteral("photo_output_conflict")); return;
    }
    m_photoDestination = destination;
    m_photoOptions = options;
    m_extraSource = options.value(QStringLiteral("stickerPath")).toString();
    if (m_extraSource.isEmpty()) m_extraSource = options.value(QStringLiteral("background")).toObject().value(QStringLiteral("path")).toString();
    if (prepare(sourcePath, QStringLiteral("retouch_photo"))) startProcessing();
}
bool LocalMediaProcessor::prepare(const QString &sourcePath, const QString &operation)
{
    QFileInfo source(sourcePath);
    if (!source.isAbsolute() || !ordinaryLocalFile(sourcePath) || (!m_extraSource.isEmpty() && !ordinaryLocalFile(m_extraSource))) {
        fail(QStringLiteral("invalid_local_source")); return false;
    }
    if (!safePath(m_outputRoot) || !safePath(m_models) || !safePath(m_runtime)) { fail(QStringLiteral("unsafe_local_storage")); return false; }
    for (const QString &program : {m_ffmpeg, m_ffprobe, m_python, m_script}) {
        if (!QFileInfo(program).isAbsolute() || !QFileInfo(program).isFile()) { fail(QStringLiteral("local_runtime_missing")); return false; }
    }
    if (!QDir().mkpath(m_outputRoot)) { fail(QStringLiteral("output_directory_failed")); return false; }
    m_source = source.absoluteFilePath(); m_operation = operation; m_sourceSnapshot = snapshot(m_source);
    m_extraSourceSnapshot = m_extraSource.isEmpty() ? QByteArray() : snapshot(m_extraSource);
    if (m_sourceSnapshot.isEmpty() || (!m_extraSource.isEmpty() && m_extraSourceSnapshot.isEmpty())) {
        fail(QStringLiteral("source_read_failed")); return false;
    }
    m_photoReport = {}; m_photoError.clear();
    m_busy = true; m_cancelled = false; m_reportedOk = false;
    return true;
}
void LocalMediaProcessor::consumeReport(const QJsonObject &report)
{
    const bool photo = m_operation == QStringLiteral("retouch_photo");
    const auto error = report.value(QStringLiteral("error")).toString();
    if (photo || m_operation == QStringLiteral("cover_face") || m_operation == QStringLiteral("animate_face")) {
        if (!error.isEmpty()) m_photoError = error;
    }
    if (!report.value(QStringLiteral("ok")).toBool()) return;
    const QString path = report.value(QStringLiteral("path")).toString();
    m_reportedOk = (photo ? QDir::fromNativeSeparators(path) : path) == m_output;
    if (photo && m_reportedOk) m_photoReport = report;
}
void LocalMediaProcessor::cancel()
{
    if (!m_busy) return;
    m_cancelled = true;
#ifdef Q_OS_WIN
    const HANDLE jobHandle = reinterpret_cast<HANDLE>(property("processJob").toULongLong());
    if (jobHandle) TerminateJobObject(jobHandle, 1);
#endif
    m_process.kill();
}
void LocalMediaProcessor::startProbe(const QString &path, bool output)
{
    if (!ordinaryLocalFile(path) || (output && !safePath(path))) { fail(QStringLiteral("unsafe_media_path")); return; }
    m_stage = output ? (path == m_background ? 4 : 3) : 1;
    m_stdout.clear();
    QStringList arguments{QStringLiteral("-v"), QStringLiteral("error")};
    if (m_operation == QStringLiteral("cover_face") || m_operation == QStringLiteral("animate_face")) {
        arguments << QStringLiteral("-protocol_whitelist") << QStringLiteral("file,pipe");
    }
    arguments << QStringLiteral("-show_streams") << QStringLiteral("-show_format") << QStringLiteral("-of") << QStringLiteral("json") << path;
    m_process.start(m_ffprobe, arguments);
}
void LocalMediaProcessor::startProcessing()
{
    if (!safePath(m_outputRoot) || !safePath(m_models) || !safePath(m_runtime)) { fail(QStringLiteral("unsafe_local_storage")); return; }
    if (!ordinaryLocalFile(m_source) || snapshot(m_source) != m_sourceSnapshot
        || (!m_extraSource.isEmpty() && (!ordinaryLocalFile(m_extraSource) || snapshot(m_extraSource) != m_extraSourceSnapshot))) {
        fail(QStringLiteral("source_changed")); return;
    }
    const bool photo = m_operation == QStringLiteral("retouch_photo");
    const bool audio = m_operation == QStringLiteral("denoise_audio") || m_operation == QStringLiteral("separate_vocals");
    const QString suffix = photo ? QStringLiteral(".png") : audio ? QStringLiteral(".wav")
        : m_operation == QStringLiteral("remove_background") ? QStringLiteral(".mov") : QStringLiteral(".mp4");
    m_output = QDir(m_outputRoot).filePath((photo ? QStringLiteral(".") : QString()) + QUuid::createUuid().toString(QUuid::WithoutBraces) + suffix);
    QFile reserved(m_output);
    if (!reserved.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { m_output.clear(); fail(QStringLiteral("output_reservation_failed")); return; }
    reserved.close();
    QStringList arguments{m_script, QStringLiteral("--operation"), m_operation, QStringLiteral("--input"), m_source, QStringLiteral("--output"), m_output,
                          QStringLiteral("--ffmpeg"), m_ffmpeg, QStringLiteral("--ffprobe"), m_ffprobe, QStringLiteral("--model-root"), m_models,
                          QStringLiteral("--runtime-root"), m_runtime};
    if (photo) {
        arguments = {QStringLiteral("-I"), QStringLiteral("-B"), m_script, QStringLiteral("--input"), m_source,
            QStringLiteral("--output"), m_output, QStringLiteral("--model-root"), m_models, QStringLiteral("--options"),
            QString::fromUtf8(QJsonDocument(m_photoOptions).toJson(QJsonDocument::Compact))};
    } else if (m_operation == QStringLiteral("cover_face") || m_operation == QStringLiteral("animate_face")) {
        arguments.prepend(QStringLiteral("-B"));
        arguments.prepend(QStringLiteral("-I"));
        if (m_operation == QStringLiteral("cover_face")) arguments << QStringLiteral("--sticker") << m_extraSource;
    }
    if (m_operation == QStringLiteral("separate_vocals")) {
        m_background = QDir(m_outputRoot).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".wav"));
        QFile background(m_background);
        if (!background.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { m_background.clear(); fail(QStringLiteral("output_reservation_failed")); return; }
        background.close(); arguments << QStringLiteral("--background-output") << m_background;
    }
    auto environment = QProcessEnvironment::systemEnvironment();
    const QString cache = QDir(m_runtime).filePath(QStringLiteral("cache"));
    if (!safePath(cache) || !QDir().mkpath(cache)) { fail(QStringLiteral("unsafe_local_cache")); return; }
    for (const QString &key : {QStringLiteral("TMP"), QStringLiteral("TEMP"), QStringLiteral("TMPDIR"), QStringLiteral("TORCH_HOME"), QStringLiteral("HF_HOME"), QStringLiteral("XDG_CACHE_HOME"), QStringLiteral("NUMBA_CACHE_DIR"), QStringLiteral("PIP_CACHE_DIR")}) environment.insert(key, cache);
    environment.insert(QStringLiteral("PYTHONPATH"), m_runtime);
    environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("HF_HUB_OFFLINE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("TRANSFORMERS_OFFLINE"), QStringLiteral("1"));
    m_process.setProcessEnvironment(environment); m_process.setWorkingDirectory(m_runtime);
    m_stage = 2; m_stdout.clear(); m_process.start(m_python, arguments);
}
void LocalMediaProcessor::completed(int code, QProcess::ExitStatus status)
{
    if (!m_busy) return;
    m_stdout += m_process.readAllStandardOutput();
    if (m_cancelled) { fail(QStringLiteral("cancelled")); return; }
    if (m_stage == 2) {
        for (const auto &line : m_stdout.split('\n')) consumeReport(QJsonDocument::fromJson(line).object());
        if (status != QProcess::NormalExit || code != 0) {
            if (m_photoError == QStringLiteral("photo_output_conflict")) m_output.clear();
            fail(m_photoError.isEmpty() ? QStringLiteral("local_process_failed") : m_photoError); return;
        }
        if (!m_reportedOk) { fail(QStringLiteral("invalid_processor_result")); return; }
        startProbe(m_output, true); return;
    }
    if (status != QProcess::NormalExit || code != 0) { fail(QStringLiteral("local_process_failed")); return; }
    const auto metadata = QJsonDocument::fromJson(m_stdout).object();
    if (m_operation == QStringLiteral("retouch_photo")) {
        const auto video = stream(metadata, QStringLiteral("video"));
        const int width = video.value(QStringLiteral("width")).toInt(), height = video.value(QStringLiteral("height")).toInt();
        const bool alpha = m_photoReport.value(QStringLiteral("alpha")).toBool();
        if (video.value(QStringLiteral("codec_name")).toString() != QStringLiteral("png") || width < 1 || height < 1
            || width != m_photoReport.value(QStringLiteral("width")).toInt() || height != m_photoReport.value(QStringLiteral("height")).toInt()
            || video.value(QStringLiteral("pix_fmt")).toString() != (alpha ? QStringLiteral("rgba") : QStringLiteral("rgb24"))) {
            fail(QStringLiteral("invalid_processor_result")); return;
        }
        if (!ordinaryLocalFile(m_source) || snapshot(m_source) != m_sourceSnapshot
            || (!m_extraSource.isEmpty() && (!ordinaryLocalFile(m_extraSource) || snapshot(m_extraSource) != m_extraSourceSnapshot))) {
            fail(QStringLiteral("source_changed")); return;
        }
        if (!safePath(m_photoDestination) || !safePath(m_output) || !QFile::rename(m_output, m_photoDestination)) {
            fail(QStringLiteral("photo_output_conflict")); return;
        }
        m_output.clear(); m_busy = false;
        Q_EMIT finished(QJsonObject{{"operation", m_operation}, {"path", m_photoDestination}, {"width", width}, {"height", height}, {"alpha", alpha}});
        return;
    }
    const bool audio = m_operation == QStringLiteral("denoise_audio") || m_operation == QStringLiteral("separate_vocals");
    if (metadata.isEmpty() || !std::isfinite(duration(metadata)) || duration(metadata) <= 0 || stream(metadata, audio ? QStringLiteral("audio") : QStringLiteral("video")).isEmpty()) {
        fail(QStringLiteral("invalid_media")); return;
    }
    const auto audioStream = stream(metadata, QStringLiteral("audio"));
    if (!audioStream.isEmpty() && (audioStream.value(QStringLiteral("codec_name")).toString().isEmpty()
        || audioStream.value(QStringLiteral("sample_rate")).toVariant().toInt() <= 0
        || audioStream.value(QStringLiteral("channels")).toInt() <= 0)) { fail(QStringLiteral("invalid_audio_stream")); return; }
    if (!audio) {
        const auto videoStream = stream(metadata, QStringLiteral("video"));
        const auto rate = videoStream.value(QStringLiteral("avg_frame_rate")).toString().split(QLatin1Char('/'));
        if (videoStream.value(QStringLiteral("width")).toInt() <= 0 || videoStream.value(QStringLiteral("height")).toInt() <= 0
            || rate.size() != 2 || rate.at(0).toDouble() <= 0 || rate.at(1).toDouble() <= 0) { fail(QStringLiteral("invalid_video_stream")); return; }
    }
    if (!audio && displaySize(stream(metadata, QStringLiteral("video"))).isEmpty()) { fail(QStringLiteral("unsupported_display_geometry")); return; }
    if (m_stage == 1) { m_sourceMetadata = metadata; startProcessing(); return; }
    if (std::abs(duration(metadata) - duration(m_sourceMetadata)) > 0.25) { fail(QStringLiteral("media_duration_changed")); return; }
    if (!audio) {
        const auto before = stream(m_sourceMetadata, QStringLiteral("video"));
        const auto after = stream(metadata, QStringLiteral("video"));
        if (displaySize(before) != displaySize(after) || before.value(QStringLiteral("avg_frame_rate")) != after.value(QStringLiteral("avg_frame_rate"))) {
            fail(QStringLiteral("video_geometry_changed")); return;
        }
        if (!stream(m_sourceMetadata, QStringLiteral("audio")).isEmpty() && stream(metadata, QStringLiteral("audio")).isEmpty()) { fail(QStringLiteral("audio_track_lost")); return; }
        if (m_operation == QStringLiteral("remove_background") && (after.value(QStringLiteral("codec_name")).toString() != QStringLiteral("qtrle") || after.value(QStringLiteral("pix_fmt")).toString() != QStringLiteral("argb"))) { fail(QStringLiteral("alpha_output_missing")); return; }
    }
    if (m_stage == 3) {
        m_outputMetadata = metadata;
        if (!m_background.isEmpty()) { startProbe(m_background, true); return; }
    }
    if (!ordinaryLocalFile(m_source) || snapshot(m_source) != m_sourceSnapshot
        || (!m_extraSource.isEmpty() && (!ordinaryLocalFile(m_extraSource) || snapshot(m_extraSource) != m_extraSourceSnapshot))) {
        fail(QStringLiteral("source_changed")); return;
    }
    QJsonObject result{{QStringLiteral("operation"), m_operation}, {QStringLiteral("sourcePath"), m_source}, {QStringLiteral("path"), m_output},
                       {QStringLiteral("sourceMetadata"), m_sourceMetadata}, {QStringLiteral("outputMetadata"), m_outputMetadata}};
    if (!m_background.isEmpty()) result.insert(QStringLiteral("backgroundPath"), m_background);
    m_busy = false; Q_EMIT finished(result);
}
