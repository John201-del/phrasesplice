/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../src/assistant/ltxspacevideoprovider.h"

#include "../../src/assistant/workspacepaths.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <functional>
#include <memory>

struct LtxRequest {
    QByteArray method;
    QByteArray path;
    QByteArray body;
    QMap<QByteArray, QByteArray> headers;
    QPointer<QTcpSocket> socket;
};

class LtxHttpServer : public QTcpServer
{
public:
    QList<LtxRequest> requests;
    std::function<void(const LtxRequest &)> handle;

    LtxHttpServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    buffer->append(socket->readAll());
                    const int split = buffer->indexOf("\r\n\r\n");
                    if (split < 0 || socket->property("handled").toBool()) return;
                    const auto lines = buffer->left(split).split('\n');
                    const auto start = lines.first().trimmed().split(' ');
                    LtxRequest request;
                    request.method = start.value(0);
                    request.path = start.value(1);
                    request.socket = socket;
                    for (const auto &line : lines.mid(1)) {
                        const int colon = line.indexOf(':');
                        if (colon > 0) request.headers[line.left(colon).trimmed().toLower()] = line.mid(colon + 1).trimmed();
                    }
                    const int length = request.headers.value("content-length").toInt();
                    if (buffer->size() < split + 4 + length) return;
                    request.body = buffer->mid(split + 4, length);
                    socket->setProperty("handled", true);
                    requests.append(request);
                    if (handle) handle(request);
                });
            }
        });
    }

    QUrl base() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/gradio_api").arg(serverPort())); }

    static void reply(const LtxRequest &request, int code, const QByteArray &body, const QByteArray &contentType)
    {
        if (!request.socket) return;
        request.socket->write("HTTP/1.1 " + QByteArray::number(code) + " Result\r\nContent-Type: " + contentType
                              + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        request.socket->disconnectFromHost();
    }

    static void json(const LtxRequest &request, const QJsonObject &body)
    {
        reply(request, 200, QJsonDocument(body).toJson(QJsonDocument::Compact), "application/json");
    }

    static void sse(const LtxRequest &request, const QByteArray &body)
    {
        reply(request, 200, body, "text/event-stream");
    }
};

class LtxSpaceVideoProviderTest : public QObject
{
    Q_OBJECT
    std::unique_ptr<LtxHttpServer> server;
    std::unique_ptr<LtxSpaceVideoProvider> provider;
    std::unique_ptr<QSignalSpy> outputs;
    std::unique_ptr<QSignalSpy> errors;

    int count(const QByteArray &method) const
    {
        int result = 0;
        for (const auto &request : server->requests) {
            if (request.method == method) ++result;
        }
        return result;
    }

    static QByteArray completeEvent(const QString &url)
    {
        const QJsonObject file{{QStringLiteral("url"), url},
                               {QStringLiteral("orig_name"), QStringLiteral("output.mp4")},
                               {QStringLiteral("mime_type"), QStringLiteral("video/mp4")},
                               {QStringLiteral("meta"), QJsonObject{{QStringLiteral("_type"), QStringLiteral("gradio.FileData")}}}};
        const QJsonObject videoData{{QStringLiteral("video"), file}, {QStringLiteral("subtitles"), QJsonValue::Null}};
        return "event: complete\ndata: " + QJsonDocument(QJsonArray{videoData, 42}).toJson(QJsonDocument::Compact) + "\n\n";
    }

private Q_SLOTS:
    void init()
    {
        server = std::make_unique<LtxHttpServer>();
        QVERIFY(server->listen(QHostAddress::LocalHost));
        provider = std::make_unique<LtxSpaceVideoProvider>();
        provider->m_apiBase = server->base();
        provider->m_requestTimeoutMs = 500;
        outputs = std::make_unique<QSignalSpy>(provider.get(), &VideoGenerationProvider::outputReady);
        errors = std::make_unique<QSignalSpy>(provider.get(), &VideoGenerationProvider::failed);
        QVERIFY(provider->configure({}));
    }

    void cleanup()
    {
        provider.reset();
        server.reset();
    }

    void anonymousCreatePostsVerifiedParameterOrder_data()
    {
        QTest::addColumn<QString>("ratio");
        QTest::addColumn<int>("duration");
        QTest::addColumn<int>("height");
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("frames");
        QTest::newRow("16:9-default") << QStringLiteral("16:9") << 5 << 432 << 768 << 121;
        QTest::newRow("9:16-minimum") << QStringLiteral("9:16") << 4 << 768 << 432 << 97;
        QTest::newRow("1:1-maximum") << QStringLiteral("1:1") << 8 << 512 << 512 << 193;
    }

    void anonymousCreatePostsVerifiedParameterOrder()
    {
        QFETCH(QString, ratio);
        QFETCH(int, duration);
        QFETCH(int, height);
        QFETCH(int, width);
        QFETCH(int, frames);
        server->handle = [](const LtxRequest &request) {
            if (request.method == "POST") {
                request.socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                      "Set-Cookie: ltx-session=fixture; Path=/\r\nContent-Length: 22\r\nConnection: close\r\n\r\n"
                                      "{\"event_id\":\"event-1\"}");
                request.socket->disconnectFromHost();
            } else {
                LtxHttpServer::sse(request, "event: error\ndata: null\n\n");
            }
        };

        QCOMPARE(provider->id(), QStringLiteral("ltx-space"));
        QVERIFY(!provider->requiresCredential());
        QVERIFY(!provider->supportsResume());
        provider->create({QStringLiteral("prompt"), {}, ratio, duration, QStringLiteral("720p")});

        QTRY_COMPARE_WITH_TIMEOUT(server->requests.size(), 2, 1500);
        QCOMPARE(count("POST"), 1);
        const auto request = server->requests.first();
        QCOMPARE(request.path, QByteArray("/gradio_api/call/text_to_video"));
        QVERIFY(!request.headers.contains("authorization"));
        QVERIFY(!request.headers.contains("cookie"));
        const auto get = server->requests.at(1);
        QCOMPARE(get.method, QByteArray("GET"));
        QCOMPARE(get.path, QByteArray("/gradio_api/call/text_to_video/event-1"));
        QVERIFY(!get.headers.contains("authorization"));
        QVERIFY(!get.headers.contains("cookie"));
        const QJsonArray expected{QStringLiteral("prompt"),
                                  QStringLiteral("worst quality, inconsistent motion, blurry, jittery, distorted"),
                                  QJsonValue::Null,
                                  QJsonValue::Null,
                                  height,
                                  width,
                                  QStringLiteral("text-to-video"),
                                  duration,
                                  frames,
                                  42,
                                  true,
                                  1.0,
                                  true};
        QCOMPARE(QJsonDocument::fromJson(request.body).object().value(QStringLiteral("data")).toArray(), expected);
    }

    void completeEventReturnsOnlyOfficialHttpsVideo()
    {
        const QString url = QStringLiteral("https://lightricks-ltx-video-distilled.hf.space/gradio_api/file=/tmp/gradio/hash/output.mp4");
        LtxRequest held;
        server->handle = [&held](const LtxRequest &request) {
            if (request.method == "POST") LtxHttpServer::json(request, {{QStringLiteral("event_id"), QStringLiteral("event-2")}});
            else held = request;
        };

        provider->create({QStringLiteral("prompt")});
        QTRY_VERIFY_WITH_TIMEOUT(held.socket, 1500);

        held.socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
                           ": heartbeat\r\n\r\nevent: generating\r\ndata: null\r\n\r\nevent: comp");
        QTest::qWait(20);
        held.socket->write("lete\r");
        QTest::qWait(20);
        const auto data = completeEvent(url).split('\n').at(1).mid(6);
        held.socket->write("\ndata: [\r\ndata: " + data.mid(1) + "\r\n");
        QTest::qWait(20);
        QCOMPARE(outputs->size(), 0);
        held.socket->write("\r\n");

        // Process the complete SSE frame without waiting for the server to close the response.
        QTRY_COMPARE_WITH_TIMEOUT(outputs->size(), 1, 1500);
        QCOMPARE(errors->size(), 0);
        QCOMPARE(count("POST"), 1);
        QCOMPARE(outputs->first().at(0).toString(), QStringLiteral("event-2"));
        QCOMPARE(outputs->first().at(1).toUrl(), QUrl(url));
        QCOMPARE(outputs->first().at(2).toJsonObject().value(QStringLiteral("seed")).toInt(), 42);
    }

    void errorEventFailsWithoutSecondPost_data()
    {
        QTest::addColumn<QByteArray>("eventData");
        QTest::addColumn<bool>("quota");
        QTest::newRow("generic") << QByteArray("null") << false;
        QTest::newRow("quota") << QByteArray("{\"message\":\"quota\"}") << true;
    }

    void errorEventFailsWithoutSecondPost()
    {
        QFETCH(QByteArray, eventData);
        QFETCH(bool, quota);
        server->handle = [eventData](const LtxRequest &request) {
            if (request.method == "POST") LtxHttpServer::json(request, {{QStringLiteral("event_id"), QStringLiteral("event-3")}});
            else LtxHttpServer::sse(request, "event: error\ndata: " + eventData + "\n\n");
        };

        provider->create({QStringLiteral("prompt")});

        QTRY_COMPARE_WITH_TIMEOUT(errors->size(), 1, 1500);
        QTest::qWait(50);
        QCOMPARE(count("POST"), 1);
        QCOMPARE(outputs->size(), 0);
        QCOMPARE(errors->size(), 1);
        const auto message = errors->first().at(0).toString();
        QCOMPARE(message.contains(QStringLiteral("共享额度")), quota);
        QVERIFY(message.contains(QStringLiteral("手动重试")));
    }

    void tooManyRequestsReportsSharedQuota_data()
    {
        QTest::addColumn<bool>("onPost");
        QTest::newRow("post") << true;
        QTest::newRow("event-stream") << false;
    }

    void tooManyRequestsReportsSharedQuota()
    {
        QFETCH(bool, onPost);
        server->handle = [onPost](const LtxRequest &request) {
            if (onPost || request.method == "GET") LtxHttpServer::reply(request, 429, "rate limited", "text/plain");
            else LtxHttpServer::json(request, {{QStringLiteral("event_id"), QStringLiteral("event-quota")}});
        };
        provider->create({QStringLiteral("prompt")});
        QTRY_COMPARE_WITH_TIMEOUT(errors->size(), 1, 1500);
        QTest::qWait(50);
        QCOMPARE(count("POST"), 1);
        QCOMPARE(count("GET"), onPost ? 0 : 1);
        QCOMPARE(errors->size(), 1);
        QCOMPARE(outputs->size(), 0);
        const auto message = errors->first().at(0).toString();
        QVERIFY(message.contains(QStringLiteral("共享额度")));
        QVERIFY(message.contains(QStringLiteral("手动重试")));
    }

    void networkFailuresAreDiagnosableAndSanitized_data()
    {
        QTest::addColumn<bool>("onPost");
        QTest::addColumn<int>("httpCode");
        QTest::addColumn<QString>("category");
        QTest::newRow("post-access-denied") << true << 403 << QStringLiteral("拒绝");
        QTest::newRow("post-service-error") << true << 503 << QStringLiteral("服务错误");
        QTest::newRow("post-disconnected") << true << 0 << QStringLiteral("结果未知");
        QTest::newRow("stream-access-denied") << false << 403 << QStringLiteral("拒绝");
        QTest::newRow("stream-service-error") << false << 503 << QStringLiteral("服务错误");
        QTest::newRow("stream-disconnected") << false << 0 << QStringLiteral("结果未知");
    }

    void networkFailuresAreDiagnosableAndSanitized()
    {
        QFETCH(bool, onPost);
        QFETCH(int, httpCode);
        QFETCH(QString, category);
        server->handle = [onPost, httpCode](const LtxRequest &request) {
            if (!onPost && request.method == "POST") {
                LtxHttpServer::json(request, {{QStringLiteral("event_id"), QStringLiteral("event-diagnostic")}});
            } else if (httpCode == 0) {
                request.socket->abort();
            } else {
                LtxHttpServer::reply(request, httpCode, "private-token personal-prompt https://private.example", "text/plain");
            }
        };
        provider->create({QStringLiteral("prompt")});
        QTRY_COMPARE_WITH_TIMEOUT(errors->size(), 1, 1500);
        QTest::qWait(50);
        QCOMPARE(count("POST"), 1);
        if (!onPost && httpCode == 0) QVERIFY(count("GET") >= 1);
        else QCOMPARE(count("GET"), onPost ? 0 : 1);
        const int finalGets = count("GET");
        QTest::qWait(50);
        QCOMPARE(count("GET"), finalGets);
        QCOMPARE(outputs->size(), 0);
        QCOMPARE(errors->size(), 1);
        const auto message = errors->first().at(0).toString();
        QVERIFY(message.contains(category));
        QVERIFY(message.contains(QStringLiteral("HTTP %1").arg(httpCode)));
        QVERIFY(QRegularExpression(QStringLiteral("Qt 网络错误 [0-9]+")).match(message).hasMatch());
        QVERIFY(!message.contains(QStringLiteral("private-token")));
        QVERIFY(!message.contains(QStringLiteral("personal-prompt")));
        QVERIFY(!message.contains(QStringLiteral("private.example")));
    }

    void stalledRequestFailsOnce_data()
    {
        QTest::addColumn<bool>("onPost");
        QTest::newRow("post") << true;
        QTest::newRow("event-stream") << false;
    }

    void stalledRequestFailsOnce()
    {
        QFETCH(bool, onPost);
        provider->m_requestTimeoutMs = 150;
        server->handle = [onPost](const LtxRequest &request) {
            if (!onPost && request.method == "POST") {
                LtxHttpServer::json(request, {{QStringLiteral("event_id"), QStringLiteral("event-stalled")}});
            }
        };
        provider->create({QStringLiteral("prompt")});
        QTRY_COMPARE_WITH_TIMEOUT(errors->size(), 1, 1500);
        QTest::qWait(200);
        QCOMPARE(count("POST"), 1);
        QCOMPARE(count("GET"), onPost ? 0 : 1);
        QCOMPARE(errors->size(), 1);
        QCOMPARE(outputs->size(), 0);
    }

    void heartbeatCannotKeepTaskAliveForever()
    {
        provider->m_requestTimeoutMs = 150;
        provider->m_overallTimeoutMs = 350;
        LtxRequest held;
        int heartbeats = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] {
            if (held.socket) {
                held.socket->write(": heartbeat\r\n\r\n");
                ++heartbeats;
            }
        });
        server->handle = [&](const LtxRequest &request) {
            if (request.method == "POST") {
                LtxHttpServer::json(request, {{QStringLiteral("event_id"), QStringLiteral("event-heartbeat")}});
            } else {
                held = request;
                held.socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
                heartbeat.start(25);
            }
        };
        provider->create({QStringLiteral("prompt")});
        QTRY_COMPARE_WITH_TIMEOUT(errors->size(), 1, 1500);
        QVERIFY(heartbeats >= 2);
        QVERIFY(errors->first().at(0).toString().contains(QStringLiteral("总等待时限")));
        QTest::qWait(200);
        QCOMPARE(count("POST"), 1);
        QCOMPARE(count("GET"), 1);
        QCOMPARE(errors->size(), 1);
        QCOMPARE(outputs->size(), 0);
    }

    void reentrantStatusDoesNotPublishStaleFailure()
    {
        const auto url = QStringLiteral("https://lightricks-ltx-video-distilled.hf.space/gradio_api/file=next.mp4");
        server->handle = [&](const LtxRequest &request) {
            if (request.method == "POST") {
                const auto id = count("POST") == 1 ? QStringLiteral("event-old") : QStringLiteral("event-new");
                LtxHttpServer::json(request, {{QStringLiteral("event_id"), id}});
            } else if (request.path.endsWith("event-old")) {
                LtxHttpServer::sse(request, "event: error\ndata: null\n\n");
            } else {
                LtxHttpServer::sse(request, completeEvent(url));
            }
        };
        bool restarted = false;
        connect(provider.get(), &VideoGenerationProvider::status, this, [&](const QString &state) {
            if (state == QStringLiteral("failed") && !restarted) {
                restarted = true;
                provider->create({QStringLiteral("next prompt")});
            }
        });
        provider->create({QStringLiteral("first prompt")});
        QTRY_COMPARE_WITH_TIMEOUT(outputs->size(), 1, 1500);
        QVERIFY(restarted);
        QCOMPARE(outputs->first().at(0).toString(), QStringLiteral("event-new"));
        QCOMPARE(count("POST"), 2);
        QCOMPARE(errors->size(), 0);
    }

    void malformedCompleteDataFails()
    {
        server->handle = [](const LtxRequest &request) {
            if (request.method == "POST") LtxHttpServer::json(request, {{QStringLiteral("event_id"), QStringLiteral("event-4")}});
            else LtxHttpServer::sse(request, "event: complete\ndata: {\"video\":\"not-an-array\"}\n\n");
        };

        provider->create({QStringLiteral("prompt")});

        QTRY_COMPARE_WITH_TIMEOUT(errors->size(), 1, 1500);
        QCOMPARE(outputs->size(), 0);
        QCOMPARE(count("POST"), 1);
    }

    void foreignOrHttpDownloadUrlIsRejected_data()
    {
        QTest::addColumn<QString>("url");
        QTest::newRow("http") << QStringLiteral("http://lightricks-ltx-video-distilled.hf.space/gradio_api/file=output.mp4");
        QTest::newRow("foreign") << QStringLiteral("https://example.com/gradio_api/file=output.mp4");
        QTest::newRow("alternate-port") << QStringLiteral("https://lightricks-ltx-video-distilled.hf.space:444/gradio_api/file=output.mp4");
        QTest::newRow("userinfo") << QStringLiteral("https://user@lightricks-ltx-video-distilled.hf.space/gradio_api/file=output.mp4");
        QTest::newRow("wrong-prefix") << QStringLiteral("https://lightricks-ltx-video-distilled.hf.space/gradio_api/fileish=output.mp4");
        QTest::newRow("dot-segments") << QStringLiteral("https://lightricks-ltx-video-distilled.hf.space/gradio_api/file=/../output.mp4");
        QTest::newRow("relative-dot-segment") << QStringLiteral("https://lightricks-ltx-video-distilled.hf.space/gradio_api/file=../output.mp4");
        QTest::newRow("encoded-dot-segments") << QStringLiteral("https://lightricks-ltx-video-distilled.hf.space/gradio_api/file=/%2e%2E/output.mp4");
        QTest::newRow("encoded-slash") << QStringLiteral("https://lightricks-ltx-video-distilled.hf.space/gradio_api/file=/tmp%2foutput.mp4");
        QTest::newRow("encoded-backslash") << QStringLiteral("https://lightricks-ltx-video-distilled.hf.space/gradio_api/file=/tmp%5Coutput.mp4");
        QTest::newRow("double-encoded-dot-segments") << QStringLiteral("https://lightricks-ltx-video-distilled.hf.space/gradio_api/file=/%252e%252e/output.mp4");
        QTest::newRow("double-encoded-slash") << QStringLiteral("https://lightricks-ltx-video-distilled.hf.space/gradio_api/file=/tmp%252foutput.mp4");
        QTest::newRow("double-encoded-backslash") << QStringLiteral("https://lightricks-ltx-video-distilled.hf.space/gradio_api/file=/tmp%255coutput.mp4");
        QTest::newRow("backslash") << QStringLiteral("https://lightricks-ltx-video-distilled.hf.space/gradio_api/file=/tmp\\output.mp4");
        QTest::newRow("empty-userinfo") << QStringLiteral("https://@lightricks-ltx-video-distilled.hf.space/gradio_api/file=output.mp4");
    }

    void foreignOrHttpDownloadUrlIsRejected()
    {
        QFETCH(QString, url);
        server->handle = [url](const LtxRequest &request) {
            if (request.method == "POST") LtxHttpServer::json(request, {{QStringLiteral("event_id"), QStringLiteral("event-5")}});
            else LtxHttpServer::sse(request, completeEvent(url));
        };

        provider->create({QStringLiteral("prompt")});

        QTRY_COMPARE_WITH_TIMEOUT(errors->size(), 1, 1500);
        QCOMPARE(outputs->size(), 0);
        QCOMPARE(count("POST"), 1);
    }

    void cancelStopsLocalWaitAndLateEventIsIgnored()
    {
        LtxRequest held;
        server->handle = [&held](const LtxRequest &request) {
            if (request.method == "POST") LtxHttpServer::json(request, {{QStringLiteral("event_id"), QStringLiteral("event-6")}});
            else held = request;
        };
        QSignalSpy statuses(provider.get(), &VideoGenerationProvider::status);
        provider->create({QStringLiteral("prompt")});
        QTRY_VERIFY_WITH_TIMEOUT(held.socket, 1500);

        provider->cancelOrStopWaiting();

        QVERIFY(!statuses.isEmpty());
        QCOMPARE(statuses.last().at(0).toString(), QStringLiteral("stopped"));
        LtxHttpServer::sse(held, completeEvent(QStringLiteral("https://lightricks-ltx-video-distilled.hf.space/gradio_api/file=late.mp4")));
        QTest::qWait(80);
        QCOMPARE(outputs->size(), 0);
        QCOMPARE(errors->size(), 0);
        QCOMPARE(count("POST"), 1);
        QCOMPARE(count("DELETE"), 0);
    }

    void progressAndStopMessagesUseChinese()
    {
        server->handle = [](const LtxRequest &request) {
            if (request.method == "POST") {
                LtxHttpServer::json(request, {{QStringLiteral("event_id"), QStringLiteral("event-progress")}});
            }
        };
        QSignalSpy statuses(provider.get(), &VideoGenerationProvider::status);
        provider->create({QStringLiteral("prompt")});

        QVERIFY(!statuses.isEmpty());
        QCOMPARE(statuses.first().at(0).toString(), QStringLiteral("submitting"));
        QVERIFY(statuses.first().at(1).toString().contains(QStringLiteral("提交")));
        QTRY_COMPARE_WITH_TIMEOUT(statuses.size(), 2, 1500);
        QCOMPARE(statuses.last().at(0).toString(), QStringLiteral("running"));
        QVERIFY(statuses.last().at(1).toString().contains(QStringLiteral("等待")));

        provider->cancelOrStopWaiting();
        QCOMPARE(statuses.last().at(0).toString(), QStringLiteral("stopped"));
        QVERIFY(statuses.last().at(1).toString().contains(QStringLiteral("停止")));
        QVERIFY(statuses.last().at(1).toString().contains(QStringLiteral("远端任务可能")));
    }

    void unsupportedDurationIsRejectedWithoutRequest_data()
    {
        QTest::addColumn<int>("duration");
        QTest::newRow("below-minimum") << 3;
        QTest::newRow("above-maximum") << 9;
        QTest::newRow("paid-maximum") << 30;
    }

    void unsupportedDurationIsRejectedWithoutRequest()
    {
        QFETCH(int, duration);
        provider->create({QStringLiteral("prompt"), {}, QStringLiteral("16:9"), duration, QStringLiteral("space-default")});

        QTRY_COMPARE_WITH_TIMEOUT(errors->size(), 1, 1500);
        QCOMPARE(server->requests.size(), 0);
        QCOMPARE(outputs->size(), 0);
        QVERIFY(errors->first().at(0).toString().contains(QStringLiteral("4")));
        QVERIFY(errors->first().at(0).toString().contains(QStringLiteral("8")));
    }

    void imageInputIsRejectedUntilUploadIsImplemented()
    {
        provider->create({QStringLiteral("prompt"), (WorkspacePaths::root() + QStringLiteral("/frame.png"))});

        QTRY_COMPARE(errors->size(), 1);
        QCOMPARE(server->requests.size(), 0);
        QCOMPARE(outputs->size(), 0);
    }
};

QTEST_GUILESS_MAIN(LtxSpaceVideoProviderTest)
#include "ltxspacevideoprovidertest.moc"
