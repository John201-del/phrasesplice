/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "avatargenerator.h"
#include "workspacepaths.h"
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QUuid>
#include <cmath>
#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace {
bool ordinary(const QString &path, bool workspace = false)
{
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    if (!QDir::isAbsolutePath(clean)) return false;
#ifdef Q_OS_WIN
    if (clean.size() < 4 || !clean[0].isLetter() || clean[1] != ':' || clean[2] != '/') return false;
#endif
    if (workspace && !WorkspacePaths::isInside(path)) return false;
    QFileInfo current(clean);
    for (;;) {
        if (current.isSymLink() || current.isJunction()) return false;
#ifdef Q_OS_WIN
        const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(current.absoluteFilePath()).utf16()));
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
        const QString parent = current.absolutePath();
        if (parent == current.filePath()) break;
        current.setFile(parent);
    }
#ifdef Q_OS_WIN
    if (GetDriveTypeW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(clean.left(3)).utf16())) != DRIVE_FIXED) return false;
#endif
    return true;
}
QByteArray snapshot(const QString &path)
{
    const QFileInfo file(path);
    return ordinary(path) && file.isFile() && file.isReadable()
        ? QByteArray::number(file.size()) + ':' + QByteArray::number(file.lastModified().toMSecsSinceEpoch()) : QByteArray();
}
QJsonObject stream(const QJsonObject &metadata, const QString &type)
{
    for (const auto &value : metadata.value("streams").toArray())
        if (value.toObject().value("codec_type") == type) return value.toObject();
    return {};
}
double number(const QJsonObject &object, const QString &key) { return object.value(key).toVariant().toDouble(); }
bool audioValid(const QJsonObject &audio)
{
    return !audio.value("codec_name").toString().isEmpty() && number(audio, "sample_rate") > 0
        && number(audio, "channels") > 0 && number(audio, "nb_read_frames") > 0;
}
QJsonObject cleanMetadata(const QJsonObject &metadata)
{
    QJsonArray streams;
    for (const auto &value : metadata.value("streams").toArray()) {
        QJsonObject clean;
        const auto source = value.toObject();
        for (const auto &key : {"codec_type", "codec_name", "width", "height", "avg_frame_rate", "sample_rate", "channels", "duration", "nb_frames", "nb_read_frames"})
            if (source.contains(key)) clean.insert(key, source.value(key));
        streams.append(clean);
    }
    const auto format = metadata.value("format").toObject();
    return {{"streams", streams}, {"format", QJsonObject{{"duration", format.value("duration")}, {"format_name", format.value("format_name")}}}};
}
}

AvatarGenerator::AvatarGenerator(QObject *parent) : QObject(parent)
{
#ifdef Q_OS_WIN
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) m_job = reinterpret_cast<quintptr>(job);
    else if (job) CloseHandle(job);
    m_process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) { arguments->flags |= CREATE_SUSPENDED; });
    connect(&m_process, &QProcess::started, this, [this] {
        const DWORD pid = DWORD(m_process.processId());
        HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, pid);
        const bool assigned = m_job && child && AssignProcessToJobObject(reinterpret_cast<HANDLE>(m_job), child);
        if (child) CloseHandle(child);
        bool resumed = false;
        HANDLE threads = assigned ? CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0) : INVALID_HANDLE_VALUE;
        THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
        if (threads != INVALID_HANDLE_VALUE) {
            if (Thread32First(threads, &entry)) do {
                if (entry.th32OwnerProcessID != pid) continue;
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
                if (thread) { resumed = ResumeThread(thread) != DWORD(-1); CloseHandle(thread); }
            } while (Thread32Next(threads, &entry));
            CloseHandle(threads);
        }
        if (!assigned || !resumed) { m_cancelled = true; stop(); }
    });
#endif
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &AvatarGenerator::consume);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] { m_process.readAllStandardError(); });
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &AvatarGenerator::completed);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_busy) fail(QStringLiteral("无法启动本地数字人程序。"));
    });
}
AvatarGenerator::~AvatarGenerator()
{
    disconnect(&m_process, nullptr, this, nullptr);
    stop(); m_process.waitForFinished(3000);
#ifdef Q_OS_WIN
    if (m_job) CloseHandle(reinterpret_cast<HANDLE>(m_job));
#endif
    if (m_busy) cleanup();
}
void AvatarGenerator::configure(const QString &ffmpeg, const QString &ffprobe, const QString &python, const QString &script,
                                const QString &runtimeRoot, const QString &outputRoot)
{
    if (m_busy) return;
    m_ffmpeg = ffmpeg; m_ffprobe = ffprobe; m_python = python; m_script = script;
    m_runtime = QDir::fromNativeSeparators(runtimeRoot); m_outputRoot = QDir::fromNativeSeparators(outputRoot);
}
void AvatarGenerator::stop()
{
#ifdef Q_OS_WIN
    if (m_job) TerminateJobObject(reinterpret_cast<HANDLE>(m_job), 1);
#endif
    if (m_process.state() != QProcess::NotRunning) m_process.kill();
}
void AvatarGenerator::cleanup()
{
    if (!m_output.isEmpty() && ordinary(m_output, true)) QFile::remove(m_output);
    if (!m_jobDirectory.isEmpty() && ordinary(m_jobDirectory, true)) {
        QDirIterator entries(m_jobDirectory, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (entries.hasNext()) if (!ordinary(entries.next(), true)) return;
        QDir(m_jobDirectory).removeRecursively();
    }
}
void AvatarGenerator::fail(const QString &message)
{
    stop(); cleanup(); m_output.clear(); m_jobDirectory.clear(); m_busy = false;
    Q_EMIT failed(message);
}
bool AvatarGenerator::unchanged() const
{
    return snapshot(m_image) == m_imageSnapshot && snapshot(m_audio) == m_audioSnapshot
        && !m_imageSnapshot.isEmpty() && !m_audioSnapshot.isEmpty();
}
void AvatarGenerator::generate(const QString &imagePath, const QString &audioPath)
{
    if (m_busy || m_process.state() != QProcess::NotRunning) { Q_EMIT failed(QStringLiteral("数字人任务仍在运行，请等待或取消后重试。")); return; }
    m_output.clear(); m_jobDirectory.clear();
    m_image = QDir::fromNativeSeparators(imagePath); m_audio = QDir::fromNativeSeparators(audioPath);
    m_imageSnapshot = snapshot(m_image); m_audioSnapshot = snapshot(m_audio);
    if (m_imageSnapshot.isEmpty() || m_audioSnapshot.isEmpty()) { fail(QStringLiteral("请选择真实的本机照片和音频文件，不能使用网络路径或链接。")); return; }
    QImageReader reader(m_image);
    const QSize size = reader.size();
    if (!reader.canRead() || reader.supportsAnimation() || size.width() <= 0 || size.height() <= 0
        || size.width() > 16384 || size.height() > 16384 || qint64(size.width()) * size.height() > 64000000) {
        fail(QStringLiteral("请选择尺寸有效的静态照片。")); return;
    }
    for (const auto &path : {m_ffmpeg, m_ffprobe, m_python, m_script}) {
        if (!ordinary(path) || !QFileInfo(path).isFile()) { fail(QStringLiteral("本地数字人工具和脚本尚未准备完整。")); return; }
    }
    if (!ordinary(m_runtime, true) || !ordinary(m_outputRoot, true) || !QFileInfo(m_runtime).isDir()
        || !QDir().mkpath(m_outputRoot) || !ordinary(m_outputRoot, true)) { fail(QStringLiteral("数字人运行和输出目录必须位于当前工作区的普通目录中。")); return; }
    m_busy = true; m_cancelled = false; m_reportedOk = false; m_overflow = false;
    probe(m_audio, 1);
}
void AvatarGenerator::cancel()
{
    if (!m_busy) return;
    m_cancelled = true; stop();
    if (m_process.state() == QProcess::NotRunning) fail(QStringLiteral("已取消数字人生成。"));
}
void AvatarGenerator::probe(const QString &path, int stage)
{
    if (!ordinary(m_ffprobe) || !ordinary(path, stage == 3) || !QFileInfo(path).isFile()) { fail(QStringLiteral("素材或输出路径发生变化。")); return; }
    m_stage = stage; m_stdout.clear(); m_received = 0;
    m_process.start(m_ffprobe, {"-v", "error", "-protocol_whitelist", "file,pipe", "-read_intervals", "%+31", "-count_frames", "-show_entries",
        "stream=codec_type,codec_name,width,height,avg_frame_rate,sample_rate,channels,duration,nb_frames,nb_read_frames:format=duration,format_name", "-of", "json", path});
}
void AvatarGenerator::consume()
{
    const QByteArray bytes = m_process.readAllStandardOutput(); m_received += bytes.size();
    if (m_received > (m_stage == 2 ? 1048576 : 131072)) { m_overflow = true; stop(); return; }
    m_stdout += bytes;
    if (m_stage != 2) return;
    int newline;
    while ((newline = m_stdout.indexOf('\n')) >= 0) {
        const auto object = QJsonDocument::fromJson(m_stdout.left(newline)).object(); m_stdout.remove(0, newline + 1);
        if (object.contains("ok")) {
            m_reportedOk = object.value("ok").toBool() && !m_output.isEmpty() && QDir::fromNativeSeparators(object.value("path").toString()) == m_output;
            if (!object.value("ok").toBool()) {
                const auto error = object.value("code").toString();
                if (error == "output_exists") m_output.clear();
                if (error == "job_exists") m_jobDirectory.clear();
            }
        }
        const auto value = object.value("progress");
        if (value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0 && value.toDouble() <= 100)
            Q_EMIT progress(QStringLiteral("正在本机生成照片口型视频：%1%").arg(qRound(value.toDouble())));
    }
    if (m_stdout.size() > 65536) { m_overflow = true; stop(); }
}
void AvatarGenerator::completed(int code, QProcess::ExitStatus status)
{
    if (!m_busy) return;
    consume();
    if (m_stage == 2 && !m_stdout.isEmpty()) {
        m_stdout += '\n';
        consume();
    }
    if (m_cancelled) { fail(QStringLiteral("已取消数字人生成。")); return; }
    if (m_overflow || code != 0 || status != QProcess::NormalExit) { fail(QStringLiteral("本地数字人程序未能完成，请检查照片、语音和运行环境。")); return; }
    if (!unchanged()) { fail(QStringLiteral("原始照片或语音在生成期间发生变化，已停止导入。")); return; }
    if (m_stage == 2) {
        if (!m_reportedOk) { fail(QStringLiteral("数字人程序没有返回有效结果。")); return; }
        probe(m_output, 3); return;
    }
    if (m_stage == 3 && (!ordinary(m_output, true) || !QFileInfo(m_output).isFile())) { fail(QStringLiteral("数字人输出路径发生变化。")); return; }
    const auto metadata = QJsonDocument::fromJson(m_stdout).object();
    const auto audio = stream(metadata, "audio");
    const double duration = number(metadata.value("format").toObject(), "duration");
    if (!audioValid(audio) || !std::isfinite(duration) || duration <= 0) { fail(QStringLiteral("素材或结果缺少可解码的有效音轨。")); return; }
    if (m_stage == 1) {
        if (duration < .12 || duration > 30) { fail(QStringLiteral("请选择 0.12 至 30 秒的语音用于照片口型生成。")); return; }
        m_duration = duration;
        if (!ordinary(m_runtime, true) || !ordinary(m_outputRoot, true) || !ordinary(m_script)
             || !ordinary(m_python) || !ordinary(m_ffmpeg)) { fail(QStringLiteral("本地生成目录发生变化。")); return; }
        m_output = QDir(m_outputRoot).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".mp4");
        QFile output(m_output);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { m_output.clear(); fail(QStringLiteral("无法创建独立的数字人成片文件。")); return; }
        output.close();
        m_jobDirectory = QDir(m_runtime).filePath("jobs/" + QFileInfo(m_output).completeBaseName());
        const QString cache = QDir(m_runtime).filePath("cache");
        if (!ordinary(cache, true) || !QDir().mkpath(cache)) { fail(QStringLiteral("无法使用本地生成缓存。")); return; }
        auto environment = QProcessEnvironment::systemEnvironment();
        for (const auto &key : {"TMP", "TEMP", "TMPDIR", "TORCH_HOME", "HF_HOME", "XDG_CACHE_HOME", "NUMBA_CACHE_DIR"}) environment.insert(key, cache);
        environment.remove("PYTHONPATH"); environment.remove("PYTHONHOME");
        environment.insert("PYTHONUTF8", "1"); environment.insert("PYTHONDONTWRITEBYTECODE", "1");
        environment.insert("HF_HUB_OFFLINE", "1"); environment.insert("TRANSFORMERS_OFFLINE", "1");
        m_process.setProcessEnvironment(environment); m_process.setWorkingDirectory(m_runtime);
        m_stage = 2; m_stdout.clear(); m_received = 0;
        m_process.start(m_python, {m_script, "--image", m_image, "--audio", m_audio, "--output", m_output,
            "--runtime-root", m_runtime, "--ffmpeg", m_ffmpeg, "--ffprobe", m_ffprobe});
        return;
    }
    const auto video = stream(metadata, "video");
    const auto rate = video.value("avg_frame_rate").toString().split('/');
    const double fps = rate.size() == 2 && rate[1].toDouble() > 0 ? rate[0].toDouble() / rate[1].toDouble() : 0;
    if (video.value("codec_name") != "h264" || video.value("width").toInt() != 256 || video.value("height").toInt() != 256
        || !std::isfinite(fps) || std::abs(fps - 25) > .001 || std::abs(number(video, "duration") - m_duration) > .041
        || std::abs(number(video, "nb_read_frames") - qRound64(m_duration * 25)) > 1 || std::abs(duration - m_duration) > .12
        || !std::isfinite(number(audio, "duration")) || std::abs(number(audio, "duration") - m_duration) > .12) {
        fail(QStringLiteral("数字人成片的画面、帧数或时长校验未通过。")); return;
    }
    m_busy = false;
    Q_EMIT finished(QJsonObject{{"operation", "generate_avatar"}, {"path", m_output}, {"metadata", cleanMetadata(metadata)}});
}
