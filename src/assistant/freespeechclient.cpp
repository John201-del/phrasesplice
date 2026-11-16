/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "freespeechclient.h"
#include "workspacepaths.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <cmath>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
const QString host = QStringLiteral("https://k2-fsa-text-to-speech.hf.space");
const QString language = QStringLiteral("Chinese (Mandarin, 普通话)");
const QString model = QStringLiteral("csukuangfj/vits-zh-aishell3|174 speakers");
constexpr qint64 maximumAudioBytes = 20 * 1024 * 1024;
constexpr qint64 maximumEventBytes = 1024 * 1024;
bool safeDirectory(const QString &path)
{
    const auto clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    if (!WorkspacePaths::isInside(path)) return false;
    QFileInfo current(clean);
    while (!current.filePath().isEmpty()) {
#ifdef Q_OS_WIN
        const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(current.absoluteFilePath()).utf16()));
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
        if (current.isSymLink() || current.isJunction()) return false;
        const auto parent = current.absolutePath();
        if (parent == current.filePath()) break;
        current.setFile(parent);
    }
    return true;
}
QString endpoint(bool initialize)
{
    return host + QStringLiteral("/gradio_api/call/") + (initialize ? QStringLiteral("update_model_dropdown") : QStringLiteral("process"));
}
bool validFileUrl(const QUrl &url)
{
    return url.isValid() && url.scheme() == QStringLiteral("https") && url.host() == QUrl(host).host()
        && url.port(443) == 443 && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment()
        && url.path().startsWith(QStringLiteral("/gradio_api/file=/")) && url.path().endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive)
        && !url.path().contains(QStringLiteral("hint"), Qt::CaseInsensitive);
}
}

FreeSpeechClient::FreeSpeechClient(const QString &outputDirectory, const QString &ffprobe, QObject *parent, QNetworkAccessManager *network)
    : QObject(parent), m_directory(QDir::cleanPath(QDir::fromNativeSeparators(outputDirectory))), m_ffprobe(ffprobe),
      m_network(network ? network : new QNetworkAccessManager(this))
{
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("免费配音等待超时，任务状态未确认；为避免重复提交，已停止等待。"));
    });
}
FreeSpeechClient::~FreeSpeechClient()
{
    QPointer<QProcess> process = m_probe;
    clear();
    if (process && process->state() != QProcess::NotRunning) process->waitForFinished(1000);
}
bool FreeSpeechClient::isBusy() const { return m_busy; }
void FreeSpeechClient::clear()
{
    ++m_generation;
    m_deadline.stop();
    if (m_reply) {
        auto *reply = m_reply.data(); m_reply.clear();
        disconnect(reply, nullptr, this, nullptr); reply->abort(); reply->deleteLater();
    }
    if (m_probe) {
        auto *process = m_probe.data(); m_probe.clear();
        disconnect(process, nullptr, this, nullptr);
        if (process->state() == QProcess::NotRunning) process->deleteLater();
        else {
            connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process, &QObject::deleteLater);
            process->kill();
        }
    }
    if (m_file) { m_file->cancelWriting(); m_file.reset(); }
    if (!m_output.isEmpty() && safeDirectory(m_directory)) QFile::remove(m_output + QStringLiteral(".pending"));
    m_hash.reset(); m_output.clear(); m_text.clear(); m_session.clear(); m_busy = false;
}
void FreeSpeechClient::fail(const QString &message) { clear(); Q_EMIT failed(message); }
void FreeSpeechClient::cancel()
{
    if (m_busy) fail(QStringLiteral("已取消免费云配音。"));
}
void FreeSpeechClient::generate(const QString &text)
{
    if (m_busy) return;
    if (text.trimmed().isEmpty() || text.toUcs4().size() > 200) {
        fail(QStringLiteral("请输入 1–200 字的配音文本。")); return;
    }
    if (!safeDirectory(m_directory) || !QDir().mkpath(m_directory) || !safeDirectory(m_directory)) {
        fail(QStringLiteral("配音保存目录必须位于当前工作区内的普通目录。")); return;
    }
    if (!QFileInfo(m_ffprobe).isFile()) { fail(QStringLiteral("配音音频检查程序尚未准备完整。")); return; }
    ++m_generation; m_busy = true; m_text = text;
    m_session = QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'));
    m_output = QDir(m_directory).filePath(QStringLiteral("speech-") + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".wav"));
    if (QFile::exists(m_output) || QFile::exists(m_output + QStringLiteral(".pending"))) {
        fail(QStringLiteral("无法为配音创建独立文件。")); return;
    }
    m_deadline.start(180000);
    const auto token = m_generation;
    Q_EMIT progress(QStringLiteral("正在连接免费中文配音服务。"));
    if (token == m_generation) submit(true);
}
QNetworkReply *FreeSpeechClient::request(const QUrl &url, const QByteArray &payload)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setTransferTimeout(60000);
    if (payload.isEmpty()) m_reply = m_network->get(request);
    else { request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json")); m_reply = m_network->post(request, payload); }
    return m_reply;
}
bool FreeSpeechClient::responseOk(QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 429) fail(QStringLiteral("免费配音服务当前额度不足或请求过多，请稍后手动重试。"));
    else if (status == 403) fail(QStringLiteral("免费配音服务拒绝访问，请稍后手动重试。"));
    else if (status >= 300 || reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid())
        fail(QStringLiteral("免费配音服务暂时不可用，请稍后手动重试。"));
    else if (reply->error() != QNetworkReply::NoError || status != 200)
        fail(QStringLiteral("免费配音连接中断，任务状态未确认；为避免重复提交，已停止等待。"));
    else return true;
    return false;
}
void FreeSpeechClient::submit(bool initialize)
{
    const auto token = m_generation;
    const QJsonArray data = initialize ? QJsonArray{language} : QJsonArray{language, model, m_text, QStringLiteral("0"), 1};
    auto *reply = request(QUrl(endpoint(initialize)), QJsonDocument(QJsonObject{{QStringLiteral("data"), data},
                                                                                           {QStringLiteral("session_hash"), m_session}}).toJson(QJsonDocument::Compact));
    auto bytes = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, token, bytes] {
        if (token != m_generation) return;
        if (bytes->size() + reply->bytesAvailable() > maximumEventBytes) { fail(QStringLiteral("免费配音服务返回了无效响应。")); return; }
        bytes->append(reply->readAll());
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, token, bytes, initialize] {
        if (token != m_generation || !responseOk(reply)) return;
        bytes->append(reply->readAll());
        const auto event = QJsonDocument::fromJson(*bytes).object().value(QStringLiteral("event_id")).toString();
        if (bytes->size() > maximumEventBytes || !QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,128}$")).match(event).hasMatch()) {
            fail(QStringLiteral("免费配音服务没有确认任务，已停止提交。")); return;
        }
        m_reply.clear(); reply->deleteLater(); events(initialize);
    });
}
void FreeSpeechClient::events(bool initialize)
{
    const auto token = m_generation;
    // This Space keys its call queue by session_hash when a session is supplied to POST.
    auto *reply = request(QUrl(endpoint(initialize) + QLatin1Char('/') + m_session));
    auto bytes = std::make_shared<QByteArray>();
    auto kind = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, token, bytes, kind, initialize] {
        if (token != m_generation || !responseOk(reply)) return;
        if (bytes->size() + reply->bytesAvailable() > maximumEventBytes) { fail(QStringLiteral("免费配音服务返回了无效响应。")); return; }
        bytes->append(reply->readAll());
        int newline;
        while ((newline = bytes->indexOf('\n')) >= 0) {
            const auto line = bytes->left(newline).trimmed(); bytes->remove(0, newline + 1);
            if (line.startsWith("event:")) *kind = line.mid(6).trimmed();
            else if (line.startsWith("data:") && (*kind == "complete" || *kind == "error")) {
                if (*kind == "error") { fail(QStringLiteral("免费配音服务未能完成请求，请稍后手动重试。")); return; }
                const auto data = line.mid(5).trimmed();
                m_reply.clear(); disconnect(reply, nullptr, this, nullptr); reply->abort(); reply->deleteLater();
                terminal(initialize, data); return;
            }
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, token] {
        if (token != m_generation || m_reply != reply || !responseOk(reply)) return;
        fail(QStringLiteral("免费配音连接中断，任务状态未确认；为避免重复提交，已停止等待。"));
    });
}
void FreeSpeechClient::terminal(bool initialize, const QByteArray &data)
{
    const auto result = QJsonDocument::fromJson(data).array();
    if (initialize) {
        const auto choices = result.isEmpty() ? QJsonArray() : result[0].toObject().value(QStringLiteral("choices")).toArray();
        bool found = false;
        for (const auto &choice : choices) if (choice.toArray().contains(model) || choice.toString() == model) found = true;
        if (!found) { fail(QStringLiteral("免费服务当前没有已验证的中文配音模型。")); return; }
        const auto token = m_generation;
        Q_EMIT progress(QStringLiteral("正在生成免费中文配音。"));
        if (token == m_generation) submit(false);
        return;
    }
    const auto html = result.size() > 1 ? result[1].toString() : QString();
    if (!html.contains(QStringLiteral("result_item_success")) || !html.contains(QStringLiteral("Wave duration"))
        || html.contains(QStringLiteral("result_item_error"))) { fail(QStringLiteral("免费配音服务没有返回成功的语音结果。")); return; }
    const auto file = result[0].toObject();
    const auto mime = file.value(QStringLiteral("mime_type")).toString();
    if (!mime.isEmpty() && !mime.startsWith(QStringLiteral("audio/"))) {
        fail(QStringLiteral("免费配音服务没有返回有效的音频文件。")); return;
    }
    auto url = QUrl(file.value(QStringLiteral("url")).toString());
    if (url.isEmpty()) url = QUrl(host + QStringLiteral("/gradio_api/file=") + file.value(QStringLiteral("path")).toString());
    else url = QUrl(host).resolved(url);
    if (!validFileUrl(url)) { fail(QStringLiteral("免费配音服务返回了不受信任的音频地址。")); return; }
    download(url);
}
void FreeSpeechClient::download(const QUrl &url)
{
    if (!safeDirectory(m_directory)) { fail(QStringLiteral("配音保存目录发生变化，已停止保存。")); return; }
    m_file = std::make_unique<QSaveFile>(m_output + QStringLiteral(".pending"));
    if (!m_file->open(QIODevice::WriteOnly)) { fail(QStringLiteral("无法保存免费配音音频。")); return; }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    const auto token = m_generation;
    Q_EMIT progress(QStringLiteral("正在下载并检查配音音频。"));
    if (token != m_generation) return;
    auto *reply = request(url);
    auto size = std::make_shared<qint64>(0);
    auto consume = [this, reply, token, size] {
        if (token != m_generation || !responseOk(reply)) return;
        if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > maximumAudioBytes) {
            fail(QStringLiteral("配音音频超过允许大小，已停止下载。")); return;
        }
        while (reply->bytesAvailable() > 0) {
            const auto chunk = reply->read(65536); *size += chunk.size();
            if (*size > maximumAudioBytes) { fail(QStringLiteral("配音音频超过允许大小，已停止下载。")); return; }
            if (m_file->write(chunk) != chunk.size()) { fail(QStringLiteral("无法保存免费配音音频。")); return; }
            m_hash->addData(chunk);
        }
    };
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply, token] {
        if (token == m_generation && responseOk(reply) && reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > maximumAudioBytes)
            fail(QStringLiteral("配音音频超过允许大小，已停止下载。"));
    });
    connect(reply, &QNetworkReply::readyRead, this, consume);
    connect(reply, &QNetworkReply::finished, this, [this, reply, token, size, consume] {
        if (token != m_generation || !responseOk(reply)) return;
        consume(); if (token != m_generation) return;
        if (*size == 0 || m_hash->result().toHex() == QByteArrayLiteral("77ce75e0e929d5705124e2cfb4d13ee49fd97124a0186e19eae5dd038f7d182c")) {
            fail(QStringLiteral("免费服务返回了提示音或空音频，未保存为配音。")); return;
        }
        if (!safeDirectory(m_directory) || !m_file->commit()) { fail(QStringLiteral("无法保存免费配音音频。")); return; }
        m_file.reset(); m_reply.clear(); reply->deleteLater(); probe();
    });
}
void FreeSpeechClient::probe()
{
    const auto token = m_generation;
    auto *process = new QProcess(this); m_probe = process;
    auto bytes = std::make_shared<QByteArray>();
    process->setWorkingDirectory(m_directory);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, token, bytes] {
        if (token != m_generation) return;
        bytes->append(process->readAllStandardOutput());
        if (bytes->size() > maximumEventBytes) fail(QStringLiteral("配音结果不是有效的 WAV 音频。"));
    });
    connect(process, &QProcess::readyReadStandardError, this, [process] { process->readAllStandardError(); });
    connect(process, &QProcess::errorOccurred, this, [this, token](QProcess::ProcessError error) {
        if (token == m_generation && error == QProcess::FailedToStart) fail(QStringLiteral("无法启动配音音频检查程序。"));
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, process, token, bytes](int code, QProcess::ExitStatus status) {
        if (token != m_generation) return;
        bytes->append(process->readAllStandardOutput());
        const auto metadata = QJsonDocument::fromJson(*bytes).object();
        const auto format = metadata.value(QStringLiteral("format")).toObject();
        const auto streams = metadata.value(QStringLiteral("streams")).toArray();
        const auto audio = streams.size() == 1 ? streams[0].toObject() : QJsonObject();
        const double duration = format.value(QStringLiteral("duration")).toVariant().toDouble();
        if (code != 0 || status != QProcess::NormalExit || format.value(QStringLiteral("format_name")).toString() != QStringLiteral("wav")
            || audio.value(QStringLiteral("codec_type")).toString() != QStringLiteral("audio")
            || audio.value(QStringLiteral("codec_name")).toString() != QStringLiteral("pcm_s16le")
            || audio.value(QStringLiteral("sample_rate")).toVariant().toInt() < 8000 || !std::isfinite(duration) || duration <= 0 || duration > 300) {
            fail(QStringLiteral("配音结果不是有效的 WAV 音频。")); return;
        }
        if (!safeDirectory(m_directory) || !QFile::rename(m_output + QStringLiteral(".pending"), m_output)) {
            fail(QStringLiteral("无法保存免费配音音频。")); return;
        }
        const auto output = m_output; clear(); Q_EMIT ready(output);
    });
    process->start(m_ffprobe, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
                             QStringLiteral("stream=codec_type,codec_name,sample_rate:format=format_name,duration"),
                             QStringLiteral("-of"), QStringLiteral("json"), m_output + QStringLiteral(".pending")});
}
