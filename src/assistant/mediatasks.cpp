// SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#include "mediatasks.h"
#include "workspacepaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
bool safeOutput(const QString &path)
{
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    if (!WorkspacePaths::isInside(path)) {
        return false;
    }
    QString current = clean;
    while (!current.isEmpty()) {
#ifdef Q_OS_WIN
        const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(current.utf16()));
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return false;
        }
#else
        if (QFileInfo(current).isSymLink()) {
            return false;
        }
#endif
        const QString parent = QFileInfo(current).absolutePath();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return true;
}

bool hasStream(const QJsonObject &probe, const QString &type)
{
    for (const auto &stream : probe[QStringLiteral("streams")].toArray()) {
        if (stream.toObject()[QStringLiteral("codec_type")].toString() == type) {
            return true;
        }
    }
    return false;
}

double duration(const QJsonObject &probe)
{
    return probe[QStringLiteral("format")].toObject()[QStringLiteral("duration")].toString().toDouble();
}
}

MediaTasks::MediaTasks(QObject *parent) : QObject(parent) {}
MediaTasks::~MediaTasks() { cancel(); }

void MediaTasks::configure(const QString &ffmpegPath, const QString &ffprobePath, const QString &pythonPath, const QString &whisperScript,
                           const QString &cacheRoot)
{
    if (m_busy) {
        Q_EMIT failed(QStringLiteral("媒体分析正在运行，不能更改配置。"));
        return;
    }
    m_ffmpeg = ffmpegPath;
    m_ffprobe = ffprobePath;
    m_python = pythonPath;
    m_script = whisperScript;
    m_cache = QDir::cleanPath(QDir::fromNativeSeparators(cacheRoot));
}

bool MediaTasks::isBusy() const { return m_busy; }

bool MediaTasks::begin(const QString &sourcePath)
{
    if (m_busy) {
        Q_EMIT failed(QStringLiteral("已有媒体分析任务，请等待或取消。"));
        return false;
    }
    if (!QFileInfo(sourcePath).isFile()) {
        Q_EMIT failed(QStringLiteral("素材不是本地有效文件。"));
        return false;
    }
    if (!safeOutput(m_cache) || !QDir().mkpath(m_cache) || !safeOutput(m_cache)) {
        Q_EMIT failed(QStringLiteral("分析缓存必须位于当前工作区内且不能经过链接目录。"));
        return false;
    }
    m_job = m_cache + QStringLiteral("/media-job-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!safeOutput(m_job) || !QDir().mkdir(m_job)) {
        Q_EMIT failed(QStringLiteral("无法创建分析任务目录。"));
        return false;
    }
    ++m_generation;
    m_source = QFileInfo(sourcePath).absoluteFilePath();
    m_busy = true;
    return true;
}

void MediaTasks::cancel()
{
    ++m_generation;
    m_busy = false;
    if (m_process) {
        QProcess *process = m_process;
        m_process = nullptr;
        process->disconnect(this);
        process->kill();
        process->waitForFinished(1000);
        process->deleteLater();
    }
}

void MediaTasks::fail(const QString &message)
{
    cancel();
    Q_EMIT failed(message);
}

void MediaTasks::complete(const QJsonObject &result, const QByteArray &image)
{
    m_busy = false;
    Q_EMIT finished(result, image);
}

void MediaTasks::run(const QString &program, const QStringList &arguments,
                     const std::function<void(QByteArray, QByteArray)> &next, bool capture)
{
    if (!m_busy) {
        return;
    }
    if (!safeOutput(m_job)) {
        fail(QStringLiteral("分析输出目录已变为链接，任务停止。"));
        return;
    }
    auto *process = new QProcess(this);
#ifdef Q_OS_WIN
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    m_process = process;
    const quint64 generation = m_generation;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString scratch = m_job + QStringLiteral("/tmp");
    if (!QDir().mkpath(scratch) || !safeOutput(scratch)) {
        fail(QStringLiteral("无法建立任务临时目录。"));
        return;
    }
    for (const QString &key : {QStringLiteral("TEMP"), QStringLiteral("TMP"), QStringLiteral("TORCH_HOME"), QStringLiteral("HF_HOME"),
                               QStringLiteral("NUMBA_CACHE_DIR")}) {
        env.insert(key, scratch);
    }
    env.insert(QStringLiteral("XDG_CACHE_HOME"), m_cache);
    env.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    env.insert(QStringLiteral("HF_HUB_OFFLINE"), QStringLiteral("1"));
    env.insert(QStringLiteral("TRANSFORMERS_OFFLINE"), QStringLiteral("1"));
    env.insert(QStringLiteral("PATH"), QFileInfo(m_ffmpeg).absolutePath() + QDir::listSeparator() + env.value(QStringLiteral("PATH")));
    process->setProcessEnvironment(env);
    process->setWorkingDirectory(m_job);
    auto out = std::make_shared<QByteArray>();
    auto err = std::make_shared<QByteArray>();
    connect(process, &QProcess::readyReadStandardOutput, this, [process, out, capture]() {
        const auto data = process->readAllStandardOutput();
        if (capture) {
            out->append(data);
        }
    });
    connect(process, &QProcess::readyReadStandardError, this, [process, err, capture]() {
        const auto data = process->readAllStandardError();
        if (capture) {
            err->append(data);
        }
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, generation](QProcess::ProcessError error) {
        if (generation == m_generation && process == m_process && error == QProcess::FailedToStart) {
            fail(QStringLiteral("无法启动本地分析程序，请检查工具路径。"));
        }
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, generation, next, out, err, capture](int code, QProcess::ExitStatus status) {
        if (generation != m_generation || process != m_process) {
            return;
        }
        if (capture) {
            out->append(process->readAllStandardOutput());
            err->append(process->readAllStandardError());
        }
        m_process = nullptr;
        process->deleteLater();
        if (code != 0 || status != QProcess::NormalExit) {
            fail(QStringLiteral("本地分析程序执行失败，未生成可用结果。"));
            return;
        }
        next(*out, *err);
    });
    process->start(program, arguments);
}

void MediaTasks::probe(const QString &path, const std::function<void(QJsonObject)> &next)
{
    run(m_ffprobe, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-protocol_whitelist"), QStringLiteral("file,pipe"),
                    QStringLiteral("-show_streams"), QStringLiteral("-show_format"),
                    QStringLiteral("-of"), QStringLiteral("json"), path}, [this, next](QByteArray out, QByteArray) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(out, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            fail(QStringLiteral("无法读取媒体信息。"));
            return;
        }
        next(document.object());
    });
}

void MediaTasks::sampleFrame(const QString &sourcePath, double seconds)
{
    if (m_busy) {
        Q_EMIT failed(QStringLiteral("已有媒体分析任务，请等待或取消。"));
        return;
    }
    if (!std::isfinite(seconds) || seconds < 0) {
        Q_EMIT failed(QStringLiteral("取帧时间必须为非负有限秒数。"));
        return;
    }
    if (!begin(sourcePath)) {
        return;
    }
    const quint64 generation = m_generation;
    Q_EMIT progress(QStringLiteral("正在读取素材画面信息。"));
    if (!m_busy || generation != m_generation) {
        return;
    }
    probe(m_source, [this, seconds](QJsonObject info) {
        const auto format = info["format"].toObject()["format_name"].toString();
        if (format == QStringLiteral("png_pipe") || format == QStringLiteral("jpeg_pipe")) {
            const auto streams = info["streams"].toArray();
            const auto stream = streams.isEmpty() ? QJsonObject() : streams.first().toObject();
            const int width = stream["width"].toInt(), height = stream["height"].toInt();
            if (width < 1 || height < 1) {
                fail(QStringLiteral("无法读取照片的像素尺寸。"));
                return;
            }
            complete({{"kind", "image_info"}, {"width", width}, {"height", height}, {"view", "source"},
                      {"message", QStringLiteral("静态照片的编码像素尺寸。修图后以导出结果返回的显示尺寸为准。")}});
            return;
        }
        const double length = duration(info);
        if (!hasStream(info, QStringLiteral("video")) || !std::isfinite(length) || length <= 0 || seconds >= length) {
            fail(QStringLiteral("素材没有视频，或取帧时间超出时长。"));
            return;
        }
        const QString output = m_job + QStringLiteral("/frame.jpg");
        run(m_ffmpeg, {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-ss", QString::number(seconds, 'f', 6),
                       "-protocol_whitelist", "file,pipe", "-i", m_source,
                       "-map", "0:v:0", "-frames:v", "1", "-vf", "scale=640:640:force_original_aspect_ratio=decrease", "-q:v", "3", output},
            [this, output, seconds](QByteArray, QByteArray) {
            if (!safeOutput(output) || QFileInfo(output).size() <= 0) {
                fail(QStringLiteral("没有提取到有效视频帧。"));
                return;
            }
            probe(output, [this, output, seconds](QJsonObject info) {
                const auto streams = info["streams"].toArray();
                const auto stream = streams.isEmpty() ? QJsonObject() : streams[0].toObject();
                QFile file(output);
                if (stream["codec_name"].toString() != "mjpeg" || stream["width"].toInt() <= 0 || stream["height"].toInt() <= 0
                    || stream["width"].toInt() > 640 || stream["height"].toInt() > 640 || !safeOutput(output) || !file.open(QIODevice::ReadOnly)) {
                    fail(QStringLiteral("提取的 JPEG 无效。"));
                    return;
                }
                const QByteArray image = file.readAll();
                if (!image.startsWith(QByteArray::fromHex("ffd8")) || !image.endsWith(QByteArray::fromHex("ffd9"))) {
                    fail(QStringLiteral("提取的 JPEG 不完整。"));
                    return;
                }
                complete({{"kind", "frame"}, {"timeSeconds", seconds}, {"mimeType", "image/jpeg"},
                          {"width", stream["width"]}, {"height", stream["height"]}, {"view", "source"}}, image);
            });
        });
    });
}

bool MediaTasks::prepareAudio(const QJsonObject &info)
{
    m_duration = duration(info);
    if (!std::isfinite(m_duration) || m_duration <= 0) {
        return false;
    }
    const double formatStart = info["format"].toObject()["start_time"].toString().toDouble();
    for (const auto &entry : info["streams"].toArray()) {
        const auto stream = entry.toObject();
        if (stream["codec_type"].toString() != "audio") {
            continue;
        }
        bool validStart = false, validDuration = false;
        const double start = stream["start_time"].toString().toDouble(&validStart);
        const double offset = validStart ? start - formatStart : 0;
        const double length = stream["duration"].toString().toDouble(&validDuration);
        const double end = validDuration ? std::min(m_duration, offset + length) : m_duration;
        m_audioOffset = std::max(0.0, offset);
        m_audioTrim = std::max(0.0, -offset);
        m_audioDuration = end - m_audioOffset;
        m_audioStreamIndex = stream["index"].toInt(-1);
        return std::isfinite(offset) && std::isfinite(m_audioDuration) && m_audioDuration > 0 && m_audioStreamIndex >= 0;
    }
    return false;
}

QString MediaTasks::audioFilter() const
{
    // Decode clocks can include container offsets and AAC padding. Analyze only
    // the playable audio range at zero, then map results back to source time.
    return QStringLiteral("asetpts=PTS-STARTPTS,atrim=start=%1:duration=%2,asetpts=PTS-STARTPTS")
        .arg(QString::number(m_audioTrim, 'f', 6), QString::number(m_audioDuration, 'f', 6));
}

QJsonObject MediaTasks::audioResult(const QString &kind) const
{
    return {{"kind", kind}, {"duration", m_duration}, {"timeReference", "source"}, {"audioStart", m_audioOffset},
            {"audioEnd", m_audioOffset + m_audioDuration}, {"audioStreamIndex", m_audioStreamIndex}};
}

void MediaTasks::detectSilence(const QString &sourcePath)
{
    if (!begin(sourcePath)) {
        return;
    }
    const quint64 generation = m_generation;
    Q_EMIT progress(QStringLiteral("正在检测静音区间。"));
    if (!m_busy || generation != m_generation) {
        return;
    }
    probe(m_source, [this](QJsonObject info) {
        if (!prepareAudio(info)) {
            fail(QStringLiteral("素材没有有效音轨。"));
            return;
        }
        run(m_ffmpeg, {"-hide_banner", "-nostdin", "-nostats", "-protocol_whitelist", "file,pipe", "-i", m_source,
                       "-map", "0:a:0", "-vn", "-af", audioFilter() + ",silencedetect=noise=-35dB:d=0.35",
                       "-f", "null", "-"}, [this](QByteArray, QByteArray err) {
            const QRegularExpression re(QStringLiteral("silence_(start|end):\\s*(-?\\d+(?:\\.\\d+)?(?:[eE][+-]?\\d+)?)"));
            auto matches = re.globalMatch(QString::fromUtf8(err));
            QJsonArray intervals;
            if (m_audioOffset >= .35) {
                intervals.append(QJsonObject{{"kind", "no_audio"}, {"start", 0.0}, {"end", m_audioOffset}});
            }
            double start = -1;
            double previousEnd = 0;
            while (matches.hasNext()) {
                const auto match = matches.next();
                const double time = match.captured(2).toDouble();
                if (!std::isfinite(time) || time < 0 || time > m_audioDuration + .1) {
                    fail(QStringLiteral("静音检测返回无效时间。"));
                    return;
                }
                if (match.captured(1) == "start") {
                    if (start >= 0 || time < previousEnd) {
                        fail(QStringLiteral("静音区间顺序无效。"));
                        return;
                    }
                    start = std::min(time, m_audioDuration);
                } else {
                    if (start < 0 || time < start) {
                        fail(QStringLiteral("静音区间无效。"));
                        return;
                    }
                    previousEnd = std::min(time, m_audioDuration);
                    if (previousEnd > start) {
                        intervals.append(QJsonObject{{"kind", "silence"}, {"start", start + m_audioOffset}, {"end", previousEnd + m_audioOffset}});
                    }
                    start = -1;
                }
            }
            if (start >= 0 && start < m_audioDuration) {
                intervals.append(QJsonObject{{"kind", "silence"}, {"start", start + m_audioOffset}, {"end", m_audioOffset + m_audioDuration}});
            }
            const double audioEnd = m_audioOffset + m_audioDuration;
            if (m_duration - audioEnd >= .35) {
                intervals.append(QJsonObject{{"kind", "no_audio"}, {"start", audioEnd}, {"end", m_duration}});
            }
            auto result = audioResult(QStringLiteral("silence"));
            result.insert(QStringLiteral("intervals"), intervals);
            complete(result);
        });
    });
}

void MediaTasks::transcribe(const QString &sourcePath, const QString &language)
{
    if (!begin(sourcePath)) {
        return;
    }
    if (m_job.contains(QRegularExpression(QStringLiteral("\\s")))
        || (!language.isEmpty() && !QRegularExpression(QStringLiteral("^[A-Za-z-]+$")).match(language).hasMatch())
        || !QFileInfo(m_script).isFile() || !safeOutput(m_cache + "/whisper/small.pt") || !QFileInfo::exists(m_cache + "/whisper/small.pt")) {
        fail(QStringLiteral("本地转写需要无空格缓存目录、有效语音语言和已安装的 small 模型。"));
        return;
    }
    const quint64 generation = m_generation;
    Q_EMIT progress(QStringLiteral("正在提取语音。"));
    if (!m_busy || generation != m_generation) {
        return;
    }
    probe(m_source, [this, language](QJsonObject info) {
        if (!prepareAudio(info)) {
            fail(QStringLiteral("素材没有有效音轨。"));
            return;
        }
        const QString wave = m_job + "/audio.wav";
        run(m_ffmpeg, {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-protocol_whitelist", "file,pipe", "-i", m_source,
                       "-map", "0:a:0", "-vn", "-af", audioFilter(),
                       "-ar", "16000", "-ac", "1", "-c:a", "pcm_s16le", wave}, [this, wave, language](QByteArray, QByteArray) {
            const quint64 generation = m_generation;
            Q_EMIT progress(QStringLiteral("正在本地识别语音。"));
            if (!m_busy || generation != m_generation) {
                return;
            }
            QStringList args{m_script, wave, "small", "device=cpu", "task=transcribe", "fp16=False", "word_timestamps=True"};
            if (!language.isEmpty()) {
                args.append("language=" + language);
            }
            run(m_python, args, [this](QByteArray, QByteArray) { readTranscript(); }, false);
        });
    });
}

void MediaTasks::readTranscript()
{
    const QString output = m_job + "/audio.srt";
    QFile file(output);
    if (!safeOutput(output) || !file.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("转写未产生本次任务字幕。"));
        return;
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString text = decoder(file.readAll());
    if (decoder.hasError()) {
        fail(QStringLiteral("字幕不是有效 UTF-8。"));
        return;
    }
    text.remove(QChar(0xfeff));
    text.replace("\r\n", "\n");
    QJsonArray segments;
    const QRegularExpression entry(QStringLiteral("^\\d+\\n(\\d{2,}):(\\d{2}):(\\d{2}),(\\d{3}) --> (\\d{2,}):(\\d{2}):(\\d{2}),(\\d{3})\\n([\\s\\S]+)$"));
    double previousStart = 0;
    for (const auto &block : text.trimmed().split(QRegularExpression(QStringLiteral("\\n\\s*\\n")), Qt::SkipEmptyParts)) {
        const auto match = entry.match(block);
        auto time = [&match](int offset) {
            return match.captured(offset).toDouble() * 3600 + match.captured(offset + 1).toDouble() * 60
                + match.captured(offset + 2).toDouble() + match.captured(offset + 3).toDouble() / 1000;
        };
        const double start = time(1), end = time(5);
        const QString content = match.captured(9).trimmed();
        if (!match.hasMatch() || start < previousStart || !std::isfinite(end) || end <= start || end > m_audioDuration + .1 || content.isEmpty()
            || match.captured(2).toInt() > 59 || match.captured(3).toInt() > 59 || match.captured(6).toInt() > 59 || match.captured(7).toInt() > 59) {
            fail(QStringLiteral("字幕内容或时间范围无效。"));
            return;
        }
        previousStart = start;
        const double clippedEnd = std::min(end, m_audioDuration);
        if (clippedEnd > start) {
            segments.append(QJsonObject{{"start", start + m_audioOffset}, {"end", clippedEnd + m_audioOffset}, {"text", content}});
        }
    }
    if (segments.isEmpty()) {
        fail(QStringLiteral("没有识别到有效字幕。"));
        return;
    }
    auto result = audioResult(QStringLiteral("transcript"));
    result.insert(QStringLiteral("segments"), segments);
    const QString wordPath = m_job + QStringLiteral("/audio.words.json");
    if (QFileInfo::exists(wordPath)) {
        QFile wordFile(wordPath);
        if (!safeOutput(wordPath) || !wordFile.open(QIODevice::ReadOnly)) {
            fail(QStringLiteral("无法读取本次识别的词级时间。")); return;
        }
        QStringDecoder wordDecoder(QStringDecoder::Utf8);
        const QString wordText = wordDecoder(wordFile.readAll());
        const auto document = QJsonDocument::fromJson(wordText.toUtf8());
        if (wordDecoder.hasError() || !document.isArray()) {
            fail(QStringLiteral("词级识别结果格式无效。")); return;
        }
        QJsonArray words;
        double previousWord = 0;
        for (const auto &value : document.array()) {
            const auto word = value.toObject();
            const double start = word["start"].toDouble(-1), end = word["end"].toDouble(-1);
            const QString content = word["text"].toString().trimmed();
            if (!word["start"].isDouble() || !word["end"].isDouble() || !std::isfinite(start) || !std::isfinite(end)
                || start < previousWord || end < start || end > m_audioDuration + .1 || content.isEmpty()) {
                fail(QStringLiteral("词级识别时间或文字无效。")); return;
            }
            previousWord = start;
            const double clippedEnd = std::min(end, m_audioDuration);
            if (clippedEnd > start) words.append(QJsonObject{{"start", start + m_audioOffset},
                {"end", clippedEnd + m_audioOffset}, {"text", content}});
        }
        result.insert(QStringLiteral("words"), words);
    }
    complete(result);
}
