/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "videogenerationclient.h"
#include "workspacepaths.h"
#include "ltxspacevideoprovider.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QTimer>
#include <QUuid>

namespace {
bool workspacePath(const QString &path)
{
    return WorkspacePaths::isInside(path);
}

bool taskIdValid(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]+$"));
    return pattern.match(id).hasMatch();
}

QNetworkRequest apiRequest(const QUrl &url, const QString &key, int timeout)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(timeout);
    return request;
}

bool retryable(int code, QNetworkReply::NetworkError error)
{
    return code == 429 || code == 408 || code >= 500 || (code == 0 && error != QNetworkReply::NoError);
}
}

struct VideoGenerationClient::State {
    QNetworkAccessManager network;
    QTimer poll, probeTimeout;
    QProcess probe;
    QPointer<QNetworkReply> reply;
    std::unique_ptr<QTemporaryFile> media;
    QString key, directory, ffprobe, id, status{QStringLiteral("unconnected")}, localPath;
    QJsonObject usage;
    quint64 generation{0};
    VideoGenerationProvider *provider{nullptr};
    QString providerId{QStringLiteral("seedance")};
    bool configured{false}, cancelRace{false}, providerPending{false};
};

VideoGenerationClient::VideoGenerationClient(QObject *parent) : QObject(parent), d(std::make_unique<State>())
{
    d->poll.setSingleShot(true);
    d->probeTimeout.setSingleShot(true);
    connect(&d->poll, &QTimer::timeout, this, &VideoGenerationClient::refresh);
}
VideoGenerationClient::~VideoGenerationClient() { reset(); }

void VideoGenerationClient::reset()
{
    ++d->generation;
    d->providerPending = false;
    if (d->provider) disconnect(d->provider, nullptr, this, nullptr);
    d->poll.stop();
    d->probeTimeout.stop();
    if (d->reply) d->reply->abort();
    d->reply.clear();
    disconnect(&d->probe, nullptr, this, nullptr);
    disconnect(&d->probeTimeout, nullptr, this, nullptr);
    if (d->probe.state() != QProcess::NotRunning) {
        d->probe.kill();
        d->probe.waitForFinished(1000);
    }
    d->media.reset();
    d->cancelRace = false;
}

void VideoGenerationClient::resetConfiguration(const QString &providerId)
{
    reset();
    if (d->provider) {
        auto *provider = d->provider;
        d->provider = nullptr;
        provider->cancelOrStopWaiting();
        provider->deleteLater();
    }
    d->providerId = providerId;
    d->key.clear();
    d->configured = false;
    d->id.clear();
    d->usage = {};
    d->localPath.clear();
}

bool VideoGenerationClient::configure(const QString &apiKey, const QString &storageDirectory, const QString &ffprobePath)
{
    resetConfiguration(QStringLiteral("seedance"));
    if (apiKey.trimmed().isEmpty() || apiKey.contains(QLatin1Char('\r')) || apiKey.contains(QLatin1Char('\n'))) {
        setStatus(QStringLiteral("unconnected"), tr("未连接：请输入服务 API Key。"));
        return false;
    }
    if (!configureStorage(storageDirectory, ffprobePath)) return false;
    d->key = apiKey.trimmed();
    d->configured = true;
    setStatus(QStringLiteral("idle"), tr("已配置服务密钥；模型权限和余额尚未验证。"));
    return true;
}

bool VideoGenerationClient::configureProvider(const QString &providerId, const QString &credential, const QString &storageDirectory, const QString &ffprobePath)
{
    if (providerId == QStringLiteral("seedance")) return configure(credential, storageDirectory, ffprobePath);
    const bool isLtx = providerId == QStringLiteral("ltx-space");
    resetConfiguration(isLtx ? providerId : QString());
    if (!isLtx) {
        setStatus(QStringLiteral("unconnected"), tr("不支持该视频生成服务。"));
        return false;
    }
    if (!configureStorage(storageDirectory, ffprobePath)) return false;
    auto provider = m_providerFactory ? m_providerFactory() : std::make_unique<LtxSpaceVideoProvider>();
    if (!provider->configure(credential)) {
        setStatus(QStringLiteral("unconnected"), tr("视频生成服务配置失败。"));
        return false;
    }
    provider->setParent(this);
    d->provider = provider.release();
    d->configured = true;
    setStatus(QStringLiteral("idle"), tr("已配置 LTX 共享服务。"));
    return true;
}

bool VideoGenerationClient::configureStorage(const QString &storageDirectory, const QString &ffprobePath)
{
    if (!workspacePath(storageDirectory) || !QDir().mkpath(storageDirectory) || !workspacePath(storageDirectory)) {
        setStatus(QStringLiteral("unconnected"), tr("素材目录必须位于当前工作区，且可写入。"));
        return false;
    }
    if (!QFileInfo(ffprobePath).isFile() || !QFileInfo(ffprobePath).isExecutable()) {
        setStatus(QStringLiteral("unconnected"), tr("未找到可执行的 ffprobe。"));
        return false;
    }
    d->directory = QDir(storageDirectory).canonicalPath();
    d->ffprobe = QFileInfo(ffprobePath).absoluteFilePath();
    return true;
}

QString VideoGenerationClient::providerId() const { return d->providerId; }
bool VideoGenerationClient::supportsResume() const { return d->providerId == QStringLiteral("seedance"); }
QString VideoGenerationClient::taskId() const { return d->id; }
QString VideoGenerationClient::currentStatus() const { return d->status; }
QString VideoGenerationClient::taskRecordPath() const { return taskRecordPath(d->id); }
QString VideoGenerationClient::taskRecordPath(const QString &taskId) const
{
    if (taskId.isEmpty() || d->directory.isEmpty()) return {};
    const QByteArray hash = QCryptographicHash::hash(taskId.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(d->directory).filePath(QString::fromLatin1(hash) + QStringLiteral(".json"));
}
void VideoGenerationClient::setStatus(const QString &state, const QString &message)
{
    d->status = state;
    Q_EMIT status(state, message);
}
void VideoGenerationClient::fail(const QString &message)
{
    reset();
    const auto generation = d->generation;
    setStatus(QStringLiteral("failed"), message);
    if (generation == d->generation) Q_EMIT failed(message);
}
bool VideoGenerationClient::persist()
{
    if (d->id.isEmpty()) return true;
    QSaveFile file(taskRecordPath());
    const QByteArray bytes = QJsonDocument(QJsonObject{{"id", d->id}, {"status", d->status}, {"usage", d->usage}, {"local_path", d->localPath}, {"provider", d->providerId}}).toJson();
    if (!workspacePath(d->directory) || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        fail(tr("任务记录保存失败，请保留任务 ID 后在服务控制台查询。"));
        return false;
    }
    return true;
}

void VideoGenerationClient::create(const QString &text, const QString &selectedFirstFramePath, const QString &ratio, int duration, const QString &resolution)
{
    if (!d->configured) { setStatus(QStringLiteral("unconnected"), tr("未连接：请先配置服务与素材目录。")); return; }
    if (auto *provider = d->provider) {
        if (ratio != QStringLiteral("16:9") && ratio != QStringLiteral("9:16") && ratio != QStringLiteral("1:1")) {
            fail(tr("LTX 只支持 16:9、9:16 或 1:1。"));
            return;
        }
        reset();
        d->id.clear();
        d->usage = {};
        d->localPath.clear();
        const auto generation = d->generation;
        d->providerPending = true;
        connect(provider, &VideoGenerationProvider::status, this, [this, generation](const QString &state, const QString &message) {
            if (generation == d->generation && d->providerPending && state != QStringLiteral("failed")) setStatus(state, message);
        });
        connect(provider, &VideoGenerationProvider::failed, this, [this, generation](const QString &message) {
            if (generation == d->generation && d->providerPending) fail(message);
        });
        connect(provider, &VideoGenerationProvider::outputReady, this, [this, generation](const QString &id, const QUrl &url, const QJsonObject &usage) {
            if (generation != d->generation || !d->providerPending) return;
            d->providerPending = false;
            if (!taskIdValid(id)) { fail(tr("服务返回的任务 ID 无效。")); return; }
            d->id = id;
            d->usage = usage;
            d->status = QStringLiteral("succeeded");
            if (persist()) download(url);
        });
        provider->create({text, selectedFirstFramePath, ratio, duration, resolution});
        return;
    }
    if (resolution != QStringLiteral("720p") && resolution != QStringLiteral("1080p")) {
        fail(tr("请选择 720p 或 1080p。"));
        return;
    }
    const QStringList ratios{QStringLiteral("16:9"), QStringLiteral("4:3"), QStringLiteral("1:1"), QStringLiteral("3:4"), QStringLiteral("9:16"), QStringLiteral("21:9"), QStringLiteral("adaptive")};
    if (text.trimmed().isEmpty() || !ratios.contains(ratio) || duration < 4 || duration > 30) {
        fail(tr("请填写提示词，选择有效宽高比和 4–30 秒时长。"));
        return;
    }
    QJsonArray content{QJsonObject{{"type", "text"}, {"text", text}}};
    if (!selectedFirstFramePath.isEmpty()) {
        QFile image(selectedFirstFramePath);
        const QString mime = QMimeDatabase().mimeTypeForFile(selectedFirstFramePath, QMimeDatabase::MatchContent).name();
        if (!mime.startsWith(QStringLiteral("image/")) || !image.open(QIODevice::ReadOnly)) { fail(tr("无法读取所选首帧图片。")); return; }
        const QString data = QStringLiteral("data:%1;base64,%2").arg(mime, QString::fromLatin1(image.readAll().toBase64()));
        content.append(QJsonObject{{"type", "image_url"}, {"role", "first_frame"}, {"image_url", QJsonObject{{"url", data}}}});
    }
    reset();
    d->id.clear();
    d->usage = {};
    d->localPath.clear();
    const auto generation = d->generation;
    setStatus(QStringLiteral("submitting"), tr("正在提交生成任务。"));
    if (generation != d->generation) return;
    const QJsonObject payload{{"model", "doubao-seedance-2-5-260628"}, {"content", content}, {"resolution", resolution}, {"ratio", ratio},
                              {"duration", duration}, {"generate_audio", false}, {"output_format", "mp4"}, {"watermark", false}};
    auto *reply = d->network.post(apiRequest(QUrl(m_apiBase.toString() + QStringLiteral("/contents/generations/tasks")), d->key, m_requestTimeoutMs), QJsonDocument(payload).toJson(QJsonDocument::Compact));
    d->reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        reply->deleteLater();
        if (generation != d->generation) return;
        d->reply.clear();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString id = code >= 200 && code < 300 && reply->isReadable()
            ? QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("id")).toString() : QString();
        if (code >= 200 && code < 300 && reply->error() == QNetworkReply::NoError && taskIdValid(id)) {
            d->id = id;
            d->status = QStringLiteral("queued");
            if (!persist()) return;
            setStatus(QStringLiteral("queued"), tr("任务已提交，等待服务处理。"));
            if (generation == d->generation) query();
        } else if (code == 0 || code == 408 || code >= 500 || (code >= 200 && code < 300)) {
            const QString message = tr("提交结果未知，可能已产生任务和费用。不会自动重新提交；请先在服务控制台确认。");
            setStatus(QStringLiteral("submission_unknown"), message);
            if (generation == d->generation) Q_EMIT failed(message);
        } else fail(tr("服务拒绝创建任务（HTTP %1），请检查权限、余额或限流。不会自动重新提交。").arg(code));
    });
}

void VideoGenerationClient::resume(const QString &taskId)
{
    if (!d->configured) { setStatus(QStringLiteral("unconnected"), tr("未连接：请先配置服务密钥。")); return; }
    if (!supportsResume()) return;
    if (!taskIdValid(taskId)) { fail(tr("任务 ID 无效。")); return; }
    QFile record(taskRecordPath(taskId));
    if (record.open(QIODevice::ReadOnly)) {
        const auto savedProvider = QJsonDocument::fromJson(record.readAll()).object().value(QStringLiteral("provider")).toString(QStringLiteral("seedance"));
        if (savedProvider != d->providerId) { fail(tr("该任务不支持通过当前服务恢复。")); return; }
    }
    reset();
    d->id = taskId;
    d->usage = {};
    d->localPath.clear();
    const auto generation = d->generation;
    setStatus(QStringLiteral("querying"), tr("正在查询服务任务。"));
    if (generation == d->generation) query();
}
void VideoGenerationClient::refresh()
{
    if (d->configured && supportsResume() && !d->id.isEmpty() && !d->reply && d->probe.state() == QProcess::NotRunning && d->status != QStringLiteral("ready")) {
        d->poll.stop();
        reset();
        query();
    }
}
void VideoGenerationClient::query(int attempt)
{
    const auto generation = d->generation;
    auto *reply = d->network.get(apiRequest(QUrl(m_apiBase.toString() + QStringLiteral("/contents/generations/tasks/") + d->id), d->key, m_requestTimeoutMs));
    d->reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, attempt] {
        reply->deleteLater();
        if (generation != d->generation) return;
        d->reply.clear();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code < 200 || code >= 300 || reply->error() != QNetworkReply::NoError) {
            if (attempt < 2 && retryable(code, reply->error())) QTimer::singleShot(m_retryIntervalMs, this, [this, generation, attempt] { if (generation == d->generation) query(attempt + 1); });
            else fail(tr("任务查询失败（HTTP %1），已停止自动查询，可凭任务 ID 恢复。").arg(code));
            return;
        }
        const auto result = QJsonDocument::fromJson(reply->readAll()).object();
        if (result.value(QStringLiteral("id")).toString() != d->id) { fail(tr("服务返回的任务 ID 不匹配。")); return; }
        const auto usage = result.value(QStringLiteral("usage")).toObject();
        for (const QString &name : {QStringLiteral("completion_tokens"), QStringLiteral("total_tokens")}) if (usage.value(name).isDouble()) d->usage[name] = usage.value(name);
        const QString state = result.value(QStringLiteral("status")).toString();
        if (state == QStringLiteral("queued") || state == QStringLiteral("running")) {
            if (d->cancelRace) {
                d->cancelRace = false;
                d->status = QStringLiteral("stopped");
                if (persist()) setStatus(QStringLiteral("stopped"), tr("服务尚未确认取消，已停止本地等待。任务可能仍在运行并计费，可凭 ID 恢复。"));
            } else {
                d->status = state;
                if (!persist()) return;
                setStatus(state, state == QStringLiteral("queued") ? tr("服务排队中。") : tr("服务生成中。"));
                if (generation == d->generation) d->poll.start(m_pollIntervalMs);
            }
        } else if (state == QStringLiteral("succeeded")) {
            d->status = state;
            if (persist()) download(QUrl(result.value(QStringLiteral("content")).toObject().value(QStringLiteral("video_url")).toString()));
        } else if (state == QStringLiteral("failed") || state == QStringLiteral("expired") || state == QStringLiteral("cancelled")) {
            d->status = state;
            if (!persist()) return;
            setStatus(state, state == QStringLiteral("cancelled") ? tr("服务任务已取消。") : tr("服务任务未成功，请在控制台查看原因。"));
            if (generation == d->generation && state != QStringLiteral("cancelled")) Q_EMIT failed(tr("服务任务%1。").arg(state));
        } else fail(tr("服务返回了未知任务状态，已停止等待。"));
    });
}

void VideoGenerationClient::cancelOrStopWaiting()
{
    if (auto *provider = d->provider) {
        if (!d->providerPending && d->status != QStringLiteral("downloading") && d->status != QStringLiteral("validating")) return;
        reset();
        provider->cancelOrStopWaiting();
        d->status = QStringLiteral("stopped");
        if (persist()) setStatus(QStringLiteral("stopped"), tr("已停止本地等待，远程生成可能仍在运行。"));
        return;
    }
    if (d->status == QStringLiteral("submitting")) {
        reset();
        const auto generation = d->generation;
        const QString message = tr("已停止等待提交结果，但服务可能已创建任务并计费。请在服务控制台确认，勿直接重复提交。");
        setStatus(QStringLiteral("submission_unknown"), message);
        if (generation == d->generation) Q_EMIT failed(message);
        return;
    }
    if (d->id.isEmpty() || d->status == QStringLiteral("ready") || d->status == QStringLiteral("cancelled")) return;
    const bool queued = d->status == QStringLiteral("queued");
    reset();
    if (!queued) {
        d->status = QStringLiteral("stopped");
        if (persist()) setStatus(QStringLiteral("stopped"), tr("已停止本地等待。服务任务未确认取消，仍可能计费；可凭任务 ID 恢复。"));
        return;
    }
    const auto generation = d->generation;
    setStatus(QStringLiteral("cancelling"), tr("正在请求取消排队任务。"));
    if (generation != d->generation) return;
    auto *reply = d->network.deleteResource(apiRequest(QUrl(m_apiBase.toString() + QStringLiteral("/contents/generations/tasks/") + d->id), d->key, m_requestTimeoutMs));
    d->reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        reply->deleteLater();
        if (generation != d->generation) return;
        d->reply.clear();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code >= 200 && code < 300 && reply->error() == QNetworkReply::NoError) {
            d->status = QStringLiteral("cancelled");
            if (persist()) setStatus(QStringLiteral("cancelled"), tr("服务已确认取消排队任务。"));
        } else { d->cancelRace = true; query(); }
    });
}

void VideoGenerationClient::download(const QUrl &url, int attempt)
{
    const bool localTest = m_apiBase.host() == QStringLiteral("127.0.0.1") && url.host() == QStringLiteral("127.0.0.1") && url.scheme() == QStringLiteral("http");
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || (url.scheme() != QStringLiteral("https") && !localTest)) { fail(tr("服务未返回有效的安全视频下载地址。")); return; }
    d->media = std::make_unique<QTemporaryFile>(QDir(d->directory).filePath(QStringLiteral("video-XXXXXX.part")));
    if (!workspacePath(d->directory) || !d->media->open()) { fail(tr("无法创建视频下载临时文件。")); return; }
    const auto generation = d->generation;
    setStatus(QStringLiteral("downloading"), tr("正在下载视频。"));
    if (generation != d->generation) return;
    QNetworkRequest request(url);
    const bool anonymous = d->provider != nullptr;
    if (anonymous) {
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    } else {
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    }
    request.setTransferTimeout(m_requestTimeoutMs);
    // CDN requests deliberately have no Authorization header.
    auto *reply = d->network.get(request);
    d->reply = reply;
    connect(reply, &QNetworkReply::downloadProgress, this, [this, generation](qint64 received, qint64 total) { if (generation == d->generation) Q_EMIT progress(received, total); });
    const auto writeData = [this, reply, generation] {
        if (generation != d->generation || !d->media) return false;
        const QByteArray bytes = reply->readAll();
        if (d->media->write(bytes) != bytes.size()) { fail(tr("视频下载写入失败。")); return false; }
        return true;
    };
    connect(reply, &QNetworkReply::readyRead, this, writeData);
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, url, attempt, writeData, anonymous] {
        reply->deleteLater();
        if (generation != d->generation) return;
        d->reply.clear();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code < 200 || code >= 300 || reply->error() != QNetworkReply::NoError) {
            d->media.reset();
            if (attempt < 2 && retryable(code, reply->error())) QTimer::singleShot(m_retryIntervalMs, this, [this, generation, url, attempt] { if (generation == d->generation) download(url, attempt + 1); });
            else fail((anonymous ? tr("视频下载失败（HTTP %1）。") : tr("视频下载失败（HTTP %1），可凭任务 ID 恢复。")).arg(code));
            return;
        }
        if (!writeData()) return;
        if (!d->media->flush()) { fail(tr("视频下载保存失败。")); return; }
        d->media->close();
        validate();
    });
}

void VideoGenerationClient::validate()
{
    const auto generation = d->generation;
    setStatus(QStringLiteral("validating"), tr("正在校验下载的视频。"));
    if (generation != d->generation) return;
    connect(&d->probeTimeout, &QTimer::timeout, this, [this, generation] { if (generation == d->generation) fail(tr("视频校验超时，未保存为成片。")); });
    connect(&d->probe, &QProcess::errorOccurred, this, [this, generation](QProcess::ProcessError error) { if (generation == d->generation && error == QProcess::FailedToStart) fail(tr("无法启动 ffprobe，未保存为成片。")); });
    connect(&d->probe, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, generation](int code, QProcess::ExitStatus exitStatus) {
        if (generation != d->generation) return;
        d->probeTimeout.stop();
        bool valid = false;
        const auto streams = QJsonDocument::fromJson(d->probe.readAllStandardOutput()).object().value(QStringLiteral("streams")).toArray();
        for (const auto &stream : streams) {
            const auto item = stream.toObject();
            if (item.value(QStringLiteral("codec_type")).toString() == QStringLiteral("video") && item.value(QStringLiteral("width")).toInt() > 0 && item.value(QStringLiteral("height")).toInt() > 0) valid = true;
        }
        if (code != 0 || exitStatus != QProcess::NormalExit || !valid) { fail(tr("下载内容不是有效视频，未保存为成片。")); return; }
        const QString finalPath = QDir(d->directory).filePath(QStringLiteral("generated-%1.mp4").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (!workspacePath(d->directory) || !d->media->rename(finalPath)) { fail(tr("视频原子保存失败。")); return; }
        d->media->setAutoRemove(false);
        d->localPath = finalPath;
        d->status = QStringLiteral("ready");
        if (!persist()) { QFile::remove(finalPath); return; }
        setStatus(QStringLiteral("ready"), tr("本地视频已下载并校验，可导入当前工程。"));
        if (generation == d->generation) Q_EMIT ready(finalPath);
    });
    d->probe.start(d->ffprobe, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"), QStringLiteral("stream=codec_type,width,height"), QStringLiteral("-of"), QStringLiteral("json"), d->media->fileName()});
    d->probeTimeout.start(m_probeTimeoutMs);
}
