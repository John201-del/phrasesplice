/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ltxspacevideoprovider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <algorithm>
#include <memory>

namespace {
constexpr auto spaceHost = "lightricks-ltx-video-distilled.hf.space";
constexpr auto callPath = "/call/text_to_video";

QNetworkRequest anonymousRequest(const QUrl &url, int timeout)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setTransferTimeout(timeout);
    return request;
}

int httpStatus(QNetworkReply *reply)
{
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

bool succeeded(QNetworkReply *reply)
{
    const int code = httpStatus(reply);
    return reply->error() == QNetworkReply::NoError && code >= 200 && code < 300;
}

QString networkFailure(QNetworkReply *reply)
{
    const int code = httpStatus(reply);
    const QString details = QStringLiteral("HTTP %1，Qt 网络错误 %2").arg(code).arg(int(reply->error()));
    if (code >= 500) {
        return QObject::tr("LTX 服务错误（%1），请稍后手动重试。").arg(details);
    }
    if (code >= 400) {
        return QObject::tr("LTX 服务拒绝请求（%1），请在官方页面确认访问状态后再手动重试。").arg(details);
    }
    return QObject::tr("LTX 请求结果未知（%1），远端任务可能已创建或继续运行；请先检查网络和服务状态，勿反复提交。").arg(details);
}

bool isOfficialVideo(const QUrl &url)
{
    const QString path = url.path(QUrl::FullyEncoded);
    const auto prefix = QStringLiteral("/gradio_api/file=");
    if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host() != QString::fromLatin1(spaceHost)
        || url.port(443) != 443 || url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@')) || !path.startsWith(prefix)
        || path.size() == prefix.size() || path.contains(QStringLiteral("%2f"), Qt::CaseInsensitive)
        || path.contains(QStringLiteral("%5c"), Qt::CaseInsensitive) || path.contains(QStringLiteral("%25"))) {
        return false;
    }
    const auto filePath = url.path(QUrl::FullyDecoded).mid(prefix.size());
    const auto segments = filePath.split(QLatin1Char('/'));
    return !segments.contains(QStringLiteral(".")) && !segments.contains(QStringLiteral(".."));
}

bool isQuotaError(const QByteArray &data)
{
    const auto object = QJsonDocument::fromJson(data).object();
    QString message = object.value(QStringLiteral("message")).toString();
    if (message.isEmpty()) {
        message = object.value(QStringLiteral("error")).toString();
    }
    if (message.isEmpty()) {
        message = QString::fromUtf8(data);
    }
    message = message.trimmed().toLower();
    return message == QStringLiteral("quota") || message == QStringLiteral("\"quota\"")
        || (message.contains(QStringLiteral("quota"))
            && (message.contains(QStringLiteral("exceeded")) || message.contains(QStringLiteral("exhausted"))));
}

struct EventStream {
    QByteArray pending;
    QByteArray event;
    QByteArray data;
};
}

LtxSpaceVideoProvider::LtxSpaceVideoProvider(QObject *parent)
    : VideoGenerationProvider(parent)
    , m_network(this)
    , m_overallTimer(this)
{
    m_overallTimer.setSingleShot(true);
    m_overallTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_overallTimer, &QTimer::timeout, this, [this] {
        fail(tr("LTX 已超过总等待时限，请稍后手动重试。"));
    });
}

QString LtxSpaceVideoProvider::id() const
{
    return QStringLiteral("ltx-space");
}

bool LtxSpaceVideoProvider::requiresCredential() const
{
    return false;
}

bool LtxSpaceVideoProvider::supportsResume() const
{
    return false;
}

bool LtxSpaceVideoProvider::configure(const QString &)
{
    return true;
}

void LtxSpaceVideoProvider::clearReply()
{
    ++m_generation;
    m_overallTimer.stop();
    if (m_reply) {
        auto *reply = m_reply;
        m_reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

void LtxSpaceVideoProvider::fail(const QString &message)
{
    clearReply();
    const auto generation = m_generation;
    const QPointer<LtxSpaceVideoProvider> guard(this);
    Q_EMIT status(QStringLiteral("failed"), message);
    if (guard && generation == m_generation) {
        Q_EMIT failed(message);
    }
}

void LtxSpaceVideoProvider::failQuota()
{
    fail(tr("LTX 免费共享额度暂不可用，请稍后手动重试。"));
}

void LtxSpaceVideoProvider::cancelOrStopWaiting()
{
    clearReply();
    Q_EMIT status(QStringLiteral("stopped"), tr("已停止本地等待；远端任务可能继续运行。"));
}

void LtxSpaceVideoProvider::create(const VideoGenerationRequest &request)
{
    clearReply();
    if (request.duration < 4 || request.duration > 8) {
        fail(tr("LTX 免费通道仅支持 4–8 秒，请调整时长后重试。"));
        return;
    }
    if (!request.firstFramePath.isEmpty()) {
        fail(tr("LTX 免费通道暂不支持首帧图片，请清除首帧后重试。"));
        return;
    }

    int width = 768;
    int height = 432;
    if (request.ratio == QStringLiteral("9:16")) {
        width = 432;
        height = 768;
    } else if (request.ratio == QStringLiteral("1:1")) {
        width = height = 512;
    }
    const int frames = int(std::clamp(qint64(request.duration) * 24 + 1, qint64(9), qint64(257)));
    const QJsonArray data{request.prompt,
                          QStringLiteral("worst quality, inconsistent motion, blurry, jittery, distorted"),
                          QJsonValue::Null,
                          QJsonValue::Null,
                          height,
                          width,
                          QStringLiteral("text-to-video"),
                          request.duration,
                          frames,
                          42,
                          true,
                          1.0,
                          true};
    const QByteArray endpoint = m_apiBase.toEncoded() + callPath;
    auto networkRequest = anonymousRequest(QUrl::fromEncoded(endpoint), m_requestTimeoutMs);
    networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    auto *reply = m_network.post(networkRequest, QJsonDocument(QJsonObject{{QStringLiteral("data"), data}}).toJson(QJsonDocument::Compact));
    m_reply = reply;
    const quint64 generation = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        if (generation != m_generation) {
            return;
        }
        if (httpStatus(reply) == 429) {
            failQuota();
            return;
        }
        if (!succeeded(reply)) {
            fail(networkFailure(reply));
            return;
        }
        const QString taskId = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("event_id")).toString();
        static const QRegularExpression eventId(QStringLiteral("\\A[A-Za-z0-9_-]+\\z"));
        if (!eventId.match(taskId).hasMatch()) {
            fail(tr("LTX 返回的任务 ID 无效。"));
            return;
        }
        m_reply = nullptr;
        reply->deleteLater();
        waitForEvent(taskId, generation);
    });
    m_overallTimer.start(m_overallTimeoutMs);
    Q_EMIT status(QStringLiteral("submitting"), tr("正在提交到 LTX 免费通道。"));
}

void LtxSpaceVideoProvider::waitForEvent(const QString &taskId, quint64 generation)
{
    const QByteArray endpoint = m_apiBase.toEncoded() + callPath + '/' + taskId.toLatin1();
    auto request = anonymousRequest(QUrl::fromEncoded(endpoint), m_requestTimeoutMs);
    request.setRawHeader("Accept", "text/event-stream");
    auto *reply = m_network.get(request);
    m_reply = reply;
    auto stream = std::make_shared<EventStream>();
    const auto readEvents = [this, reply, generation, taskId, stream] {
        if (generation != m_generation || !succeeded(reply)) {
            return;
        }
        stream->pending += reply->readAll();
        qsizetype end;
        while ((end = stream->pending.indexOf('\n')) >= 0) {
            QByteArray line = stream->pending.left(end);
            stream->pending.remove(0, end + 1);
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            if (line.startsWith("event:")) {
                stream->event = line.mid(6).trimmed();
            } else if (line.startsWith("data:")) {
                if (!stream->data.isEmpty()) {
                    stream->data += '\n';
                }
                stream->data += line.mid(5).trimmed();
            } else if (line.isEmpty()) {
                if (stream->event == "error") {
                    if (isQuotaError(stream->data)) {
                        failQuota();
                    } else {
                        fail(tr("LTX 生成失败，请稍后手动重试。"));
                    }
                    return;
                }
                if (stream->event == "complete") {
                    const auto output = QJsonDocument::fromJson(stream->data).array();
                    if (output.size() != 2 || !output.at(1).isDouble()) {
                        fail(tr("LTX 返回的视频结果无效。"));
                        return;
                    }
                    const auto video = output.at(0).toObject().value(QStringLiteral("video")).toObject();
                    const QUrl url(video.value(QStringLiteral("url")).toString(), QUrl::StrictMode);
                    if (!isOfficialVideo(url)) {
                        fail(tr("LTX 返回的视频结果无效。"));
                        return;
                    }
                    const QJsonObject usage{{QStringLiteral("seed"), output.at(1)}};
                    clearReply();
                    Q_EMIT outputReady(taskId, url, usage);
                    return;
                }
                stream->event.clear();
                stream->data.clear();
            }
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, readEvents);
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, readEvents] {
        if (generation != m_generation) {
            return;
        }
        if (httpStatus(reply) == 429) {
            failQuota();
            return;
        }
        if (!succeeded(reply)) {
            fail(networkFailure(reply));
            return;
        }
        const QPointer<LtxSpaceVideoProvider> guard(this);
        readEvents();
        if (guard && generation == m_generation) {
            fail(tr("LTX 连接结束但未收到视频，请稍后手动重试。"));
        }
    });
    Q_EMIT status(QStringLiteral("running"), tr("正在等待 LTX 免费通道，可能需要排队。"));
}
