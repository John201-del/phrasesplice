/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../localcut_test_paths.hpp"
#include "../../src/assistant/videogenerationclient.h"
#include "../../src/assistant/videogenerationprovider.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <functional>

struct Request {
    QByteArray method, path, body;
    QMap<QByteArray, QByteArray> headers;
    QPointer<QTcpSocket> socket;
};

class HttpServer : public QTcpServer
{
public:
    QList<Request> requests;
    std::function<void(const Request &)> handle;
    HttpServer()
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
                    Request r;
                    r.method = start[0]; r.path = start[1]; r.socket = socket;
                    for (const auto &line : lines.mid(1)) {
                        const int colon = line.indexOf(':');
                        if (colon > 0) r.headers[line.left(colon).trimmed().toLower()] = line.mid(colon + 1).trimmed();
                    }
                    const int length = r.headers.value("content-length").toInt();
                    if (buffer->size() < split + 4 + length) return;
                    r.body = buffer->mid(split + 4, length);
                    socket->setProperty("handled", true);
                    requests.append(r);
                    if (handle) handle(r);
                });
            }
        });
    }
    QUrl base() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/api/v3").arg(serverPort())); }
    QUrl media() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/video.mp4").arg(serverPort())); }
    static void reply(const Request &r, int code, const QByteArray &body = {}, const QByteArray &extra = {})
    {
        if (!r.socket) return;
        r.socket->write("HTTP/1.1 " + QByteArray::number(code) + " Result\r\nContent-Length: " + QByteArray::number(body.size())
                        + "\r\nConnection: close\r\n" + extra + "\r\n" + body);
        r.socket->disconnectFromHost();
    }
    static void json(const Request &r, const QJsonObject &body, int code = 200) { reply(r, code, QJsonDocument(body).toJson(QJsonDocument::Compact)); }
};


class FakeVideoGenerationProvider : public VideoGenerationProvider
{
public:
    QList<VideoGenerationRequest> requests;
    QString configuredCredential;
    int stops{0};
    QString id() const override { return QStringLiteral("ltx-space"); }
    bool requiresCredential() const override { return false; }
    bool supportsResume() const override { return false; }
    bool configure(const QString &credential) override { configuredCredential = credential; return true; }
    void create(const VideoGenerationRequest &request) override
    {
        requests.append(request);
        Q_EMIT status(QStringLiteral("submitting"), {});
    }
    void cancelOrStopWaiting() override { ++stops; }
};

class VideoGenerationClientTest : public QObject
{
    Q_OBJECT
    QString probe{LocalCutTestPaths::executable("LOCALCUT_TEST_FFPROBE", QStringLiteral("ffprobe"))};
    QString ffmpeg{LocalCutTestPaths::executable("LOCALCUT_TEST_FFMPEG", QStringLiteral("ffmpeg"))};
    QTemporaryDir root{LocalCutTestPaths::workspace(QStringLiteral("video-generation-XXXXXX"))};
    QByteArray mp4;
    std::unique_ptr<HttpServer> server;
    std::unique_ptr<VideoGenerationClient> client;
    std::unique_ptr<QSignalSpy> errors, ready;
    FakeVideoGenerationProvider *fake{nullptr};
    bool configureFakeProvider(const QString &credential = {})
    {
        client->m_providerFactory = [this] {
            auto provider = std::make_unique<FakeVideoGenerationProvider>();
            fake = provider.get();
            return provider;
        };
        return client->configureProvider("ltx-space", credential, root.filePath(QString::fromLatin1(QTest::currentTestFunction())), probe);
    }

    int count(const QByteArray &method) const
    {
        int n = 0; for (const auto &r : server->requests) if (r.method == method) ++n; return n;
    }
    QJsonObject task(const QString &state, const QString &id = QStringLiteral("task-1")) const
    {
        return {{"id", id}, {"status", state}};
    }
private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(root.isValid());
        QProcess process;
        process.start(ffmpeg, {"-v", "error", "-f", "lavfi", "-i", "color=c=red:s=32x32:r=5", "-t", "0.4", "-an", "-c:v", "libx264", "-pix_fmt", "yuv420p", root.filePath("fixture.mp4")});
        QVERIFY(process.waitForFinished(20000));
        QCOMPARE(process.exitCode(), 0);
        QFile file(root.filePath("fixture.mp4")); QVERIFY(file.open(QIODevice::ReadOnly)); mp4 = file.readAll(); QVERIFY(!mp4.isEmpty());
    }
    void init()
    {
        QTest::failOnWarning();
        server = std::make_unique<HttpServer>(); QVERIFY(server->listen(QHostAddress::LocalHost));
        client = std::make_unique<VideoGenerationClient>();
        client->m_apiBase = server->base(); client->m_pollIntervalMs = 20; client->m_retryIntervalMs = 20; client->m_requestTimeoutMs = 200;
        errors = std::make_unique<QSignalSpy>(client.get(), &VideoGenerationClient::failed);
        ready = std::make_unique<QSignalSpy>(client.get(), &VideoGenerationClient::ready);
        client->configure("local-test-key", root.filePath(QString::fromLatin1(QTest::currentTestFunction())), probe);
    }
    void cleanup() { client.reset(); server.reset(); }
    void configurationActivatesClient()
    {
        QVERIFY(client->configure("local-test-key", root.filePath("configured"), probe));
        QCOMPARE(client->currentStatus(), QStringLiteral("idle"));
    }
    void emptyKeyNeverSends()
    {
        QVERIFY(!client->configure({}, root.filePath("empty"), probe));
        client->create("test"); client->resume("x"); client->refresh();
        QTest::qWait(80); QCOMPARE(server->requests.size(), 0); QCOMPARE(client->currentStatus(), QStringLiteral("unconnected"));
    }
    void rejectsStorageOutsideWorkspace()
    {
        QVERIFY(!client->configure("key", LocalCutTestPaths::workspace() + QStringLiteral("-outside/video"), probe));
        client->create("test"); QTest::qWait(50); QCOMPARE(server->requests.size(), 0);
    }
    void rejectsHeaderLineBreaks()
    {
        QVERIFY(!client->configure("key\r\nInjected: value", root.filePath("bad-header"), probe));
        client->create("test"); QTest::qWait(50); QCOMPARE(server->requests.size(), 0);
    }
    void createFieldsAndAtomicRecord()
    {
        server->handle = [this](const Request &r) { if (r.method == "POST") HttpServer::json(r, {{"id", "task-1"}}); else HttpServer::json(r, task("queued")); };
        QFile image(root.filePath("chosen.png")); QVERIFY(image.open(QIODevice::WriteOnly)); image.write(QByteArray::fromHex("89504e470d0a1a0a")); image.close();
        client->create(QString::fromUtf8("风吹树叶"), image.fileName());
        QTRY_COMPARE(client->taskId(), QStringLiteral("task-1"));
        QCOMPARE(count("POST"), 1); const auto &r = server->requests.first();
        QCOMPARE(r.path, QByteArray("/api/v3/contents/generations/tasks")); QCOMPARE(r.headers.value("authorization"), QByteArray("Bearer local-test-key"));
        const auto json = QJsonDocument::fromJson(r.body).object();
        QCOMPARE(json.value("model").toString(), QStringLiteral("doubao-seedance-2-5-260628"));
        QCOMPARE(json.value("resolution").toString(), QStringLiteral("720p")); QCOMPARE(json.value("ratio").toString(), QStringLiteral("16:9"));
        QCOMPARE(json.value("duration").toInt(), 5); QCOMPARE(json.value("output_format").toString(), QStringLiteral("mp4"));
        QCOMPARE(json.value("generate_audio").toBool(true), false); QCOMPARE(json.value("watermark").toBool(true), false);
        const auto content = json.value("content").toArray(); QCOMPARE(content.size(), 2); QCOMPARE(content[0].toObject().value("text").toString(), QString::fromUtf8("风吹树叶"));
        QCOMPARE(content[1].toObject().value("role").toString(), QStringLiteral("first_frame"));
        QVERIFY(content[1].toObject().value("image_url").toObject().value("url").toString().startsWith("data:image/png;base64,"));
        QFile record(client->taskRecordPath()); QVERIFY(record.open(QIODevice::ReadOnly)); const auto saved = record.readAll();
        QVERIFY(QJsonDocument::fromJson(saved).isObject()); QVERIFY(!saved.contains("local-test-key")); QVERIFY(!saved.contains("base64")); QVERIFY(!saved.contains("chosen.png"));
    }
    void queuedRunningSuccessDownloadsAndProbes()
    {
        int gets = 0;
        server->handle = [this, &gets](const Request &r) {
            if (r.method == "POST") HttpServer::json(r, {{"id", "task-1"}});
            else if (r.path == "/video.mp4") HttpServer::reply(r, 200, mp4);
            else { auto result = task(++gets == 1 ? "queued" : gets == 2 ? "running" : "succeeded"); result["content"] = QJsonObject{{"video_url", server->media().toString()}}; result["usage"] = QJsonObject{{"total_tokens", 123}}; HttpServer::json(r, result); }
        };
        QSignalSpy statuses(client.get(), &VideoGenerationClient::status); client->create("test");
        QTRY_COMPARE_WITH_TIMEOUT(ready->size(), 1, 5000); QCOMPARE(errors->size(), 0);
        const QString path = ready->first().first().toString(); QVERIFY(path.endsWith(".mp4")); QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), mp4);
        QStringList states; for (const auto &args : statuses) states << args[0].toString(); QVERIFY(states.contains("queued")); QVERIFY(states.contains("running")); QCOMPARE(client->currentStatus(), QStringLiteral("ready"));
        for (const auto &r : server->requests) { if (r.path == "/video.mp4") QVERIFY(!r.headers.contains("authorization")); else QCOMPARE(r.headers.value("authorization"), QByteArray("Bearer local-test-key")); }
        QFile record(client->taskRecordPath()); QVERIFY(record.open(QIODevice::ReadOnly)); QCOMPARE(QJsonDocument::fromJson(record.readAll()).object().value("usage").toObject().value("total_tokens").toInt(), 123);
    }
    void terminalStates_data() { QTest::addColumn<QString>("state"); QTest::newRow("failed") << "failed"; QTest::newRow("expired") << "expired"; QTest::newRow("cancelled") << "cancelled"; }
    void terminalStates()
    {
        QFETCH(QString, state); server->handle = [this, state](const Request &r) { HttpServer::json(r, task(state)); };
        client->resume("task-1"); QTRY_COMPARE(client->currentStatus(), state); QTest::qWait(80); QCOMPARE(count("GET"), 1); QCOMPARE(ready->size(), 0);
    }
    void post429IsNotRetried()
    {
        server->handle = [](const Request &r) { HttpServer::reply(r, 429, "rate limit"); };
        client->create("test"); QTRY_COMPARE(errors->size(), 1); QTest::qWait(150); QCOMPARE(count("POST"), 1); QCOMPARE(client->currentStatus(), QStringLiteral("failed"));
    }
    void remoteErrorsCannotEchoCredentials()
    {
        server->handle = [](const Request &r) { HttpServer::reply(r, 403, "Authorization: Bearer local-test-key"); };
        QSignalSpy statuses(client.get(), &VideoGenerationClient::status); client->create("test"); QTRY_COMPARE(errors->size(), 1);
        QVERIFY(!errors->first().first().toString().contains("local-test-key"));
        for (const auto &args : statuses) QVERIFY(!args[1].toString().contains("local-test-key"));
    }
    void lostPostResponseIsUnknownAndNeverReposted()
    {
        server->handle = [](const Request &) {}; client->create("test");
        QTRY_COMPARE_WITH_TIMEOUT(client->currentStatus(), QStringLiteral("submission_unknown"), 2000); QTest::qWait(150); QCOMPARE(count("POST"), 1); QCOMPARE(errors->size(), 1);
    }
    void disconnectedPostIsNeverReposted()
    {
        server->handle = [](const Request &r) { if (r.socket) r.socket->abort(); }; client->create("test");
        QTRY_COMPARE(client->currentStatus(), QStringLiteral("submission_unknown")); QTest::qWait(150); QCOMPARE(count("POST"), 1);
    }
    void queuedCancelRaceDoesNotClaimCancellation()
    {
        bool deleting = false;
        server->handle = [this, &deleting](const Request &r) {
            if (r.method == "DELETE") { deleting = true; HttpServer::reply(r, 409); }
            else HttpServer::json(r, task(deleting ? "running" : "queued"));
        };
        client->resume("task-1"); QTRY_COMPARE(client->currentStatus(), QStringLiteral("queued")); client->cancelOrStopWaiting();
        QTRY_COMPARE(client->currentStatus(), QStringLiteral("stopped")); QCOMPARE(count("DELETE"), 1); QCOMPARE(client->taskId(), QStringLiteral("task-1"));
    }
    void queuedCancelSuccess()
    {
        server->handle = [this](const Request &r) { if (r.method == "DELETE") HttpServer::reply(r, 204); else HttpServer::json(r, task("queued")); };
        client->resume("task-1"); QTRY_COMPARE(client->currentStatus(), QStringLiteral("queued")); client->cancelOrStopWaiting(); QTRY_COMPARE(client->currentStatus(), QStringLiteral("cancelled"));
    }
    void runningStopThenResume()
    {
        client->m_requestTimeoutMs = 5000;
        int gets = 0;
        server->handle = [this, &gets](const Request &r) { if (++gets != 2) HttpServer::json(r, task("running")); };
        client->resume("task-1"); QTRY_COMPARE(count("GET"), 2);
        QCOMPARE(client->currentStatus(), QStringLiteral("running")); client->cancelOrStopWaiting();
        QCOMPARE(client->currentStatus(), QStringLiteral("stopped")); const int requests = count("GET"); QTest::qWait(100); QCOMPARE(count("GET"), requests); QCOMPARE(count("DELETE"), 0);
        client->resume("task-1"); QTRY_COMPARE(client->currentStatus(), QStringLiteral("running")); QVERIFY(count("GET") > requests);
    }
    void stopSubmittingIsUnknown()
    {
        server->handle = [](const Request &) {}; client->create("test"); QTRY_COMPARE(count("POST"), 1); client->cancelOrStopWaiting();
        QCOMPARE(client->currentStatus(), QStringLiteral("submission_unknown")); QCOMPARE(count("DELETE"), 0);
    }
    void oldResponseCannotReplaceNewTask()
    {
        Request held;
        server->handle = [this, &held](const Request &r) { if (r.path.endsWith("old")) held = r; else HttpServer::json(r, task("running", "new")); };
        client->resume("old"); QTRY_COMPARE(count("GET"), 1); client->resume("new"); QTRY_COMPARE(client->currentStatus(), QStringLiteral("running"));
        HttpServer::json(held, task("failed", "old")); QTest::qWait(60); QCOMPARE(client->taskId(), QStringLiteral("new")); QCOMPARE(client->currentStatus(), QStringLiteral("running")); QCOMPARE(errors->size(), 0);
    }
    void invalidMediaNeverBecomesReady()
    {
        server->handle = [this](const Request &r) { if (r.path == "/video.mp4") HttpServer::reply(r, 200, "not a video"); else { auto result = task("succeeded"); result["content"] = QJsonObject{{"video_url", server->media().toString()}}; HttpServer::json(r, result); } };
        client->resume("task-1"); QTRY_COMPARE_WITH_TIMEOUT(errors->size(), 1, 5000); QCOMPARE(ready->size(), 0);
        const QDir directory(root.filePath(QString::fromLatin1(QTest::currentTestFunction()))); QCOMPARE(directory.entryList({"*.mp4", "*.part*"}, QDir::Files).size(), 0);
    }
    void apiRedirectDoesNotLeakAuthorization()
    {
        HttpServer other; QVERIFY(other.listen(QHostAddress::LocalHost));
        server->handle = [&other](const Request &r) { HttpServer::reply(r, 302, {}, "Location: " + other.media().toEncoded() + "\r\n"); };
        client->resume("task-1"); QTRY_COMPARE(errors->size(), 1); QTest::qWait(80); QCOMPARE(other.requests.size(), 0);
    }
    void getRetriesAreBounded()
    {
        server->handle = [](const Request &r) { HttpServer::reply(r, 503); }; client->resume("task-1"); QTRY_COMPARE(errors->size(), 1); QCOMPARE(count("GET"), 3); QTest::qWait(100); QCOMPARE(count("GET"), 3);
    }
    void downloadRetriesAreBounded()
    {
        int downloads = 0;
        server->handle = [this, &downloads](const Request &r) { if (r.path == "/video.mp4") { ++downloads; HttpServer::reply(r, 503); } else { auto result = task("succeeded"); result["content"] = QJsonObject{{"video_url", server->media().toString()}}; HttpServer::json(r, result); } };
        client->resume("task-1"); QTRY_COMPARE(errors->size(), 1); QCOMPARE(downloads, 3); QCOMPARE(ready->size(), 0);
    }
    void get404IsNotRetried()
    {
        server->handle = [](const Request &r) { HttpServer::reply(r, 404); }; client->resume("task-1"); QTRY_COMPARE(errors->size(), 1); QTest::qWait(100); QCOMPARE(count("GET"), 1);
    }
    void manualRefreshInvalidatesDelayedRetry()
    {
        client->m_retryIntervalMs = 250;
        int gets = 0;
        server->handle = [this, &gets](const Request &r) { if (++gets == 1) HttpServer::reply(r, 503); else HttpServer::json(r, task("cancelled")); };
        client->resume("task-1"); QTRY_COMPARE(count("GET"), 1); QTest::qWait(30); client->refresh();
        QTRY_COMPARE(client->currentStatus(), QStringLiteral("cancelled")); QTest::qWait(350); QCOMPARE(count("GET"), 2);
    }
    void newResumeCanBeStoppedAfterPreviousReady()
    {
        server->handle = [this](const Request &r) { if (r.path == "/video.mp4") HttpServer::reply(r, 200, mp4); else { auto result = task("succeeded"); result["content"] = QJsonObject{{"video_url", server->media().toString()}}; HttpServer::json(r, result); } };
        client->resume("task-1"); QTRY_COMPARE_WITH_TIMEOUT(ready->size(), 1, 5000);
        server->handle = [](const Request &) {};
        client->resume("task-2"); QTRY_COMPARE(count("GET"), 3); client->cancelOrStopWaiting();
        QCOMPARE(client->currentStatus(), QStringLiteral("stopped")); QCOMPARE(client->taskId(), QStringLiteral("task-2")); QCOMPARE(ready->size(), 1);
    }
    void stoppingDownloadRemovesPartialFile()
    {
        server->handle = [this](const Request &r) {
            if (r.path == "/video.mp4") { r.socket->write("HTTP/1.1 200 OK\r\nContent-Length: 100000\r\n\r\npartial"); }
            else { auto result = task("succeeded"); result["content"] = QJsonObject{{"video_url", server->media().toString()}}; HttpServer::json(r, result); }
        };
        client->resume("task-1"); QTRY_COMPARE(client->currentStatus(), QStringLiteral("downloading")); client->cancelOrStopWaiting(); QTest::qWait(80);
        QCOMPARE(client->currentStatus(), QStringLiteral("stopped")); QCOMPARE(ready->size(), 0);
        const QDir directory(root.filePath(QString::fromLatin1(QTest::currentTestFunction()))); QCOMPARE(directory.entryList({"*.mp4", "*.part*"}, QDir::Files).size(), 0);
    }

    void legacyConfigureDefaultsToSeedance()
    {
        QVERIFY(configureFakeProvider());
        client->create("test");
        auto *previous = fake;
        QVERIFY(client->configure("local-test-key", root.filePath("legacy-configure"), probe));
        Q_EMIT previous->outputReady("stale-1", server->media(), {});
        QCOMPARE(previous->stops, 1);
        QVERIFY(client->taskId().isEmpty());
        QVERIFY(server->requests.isEmpty());
        QCOMPARE(client->providerId(), QStringLiteral("seedance"));
        QVERIFY(client->supportsResume());
        QCOMPARE(client->currentStatus(), QStringLiteral("idle"));
    }
    void seedanceResolutionDefaultsTo720pAndAcceptsExplicit1080p()
    {
        server->handle = [](const Request &r) { HttpServer::reply(r, 400); };
        QVERIFY(client->configureProvider("seedance", "local-test-key", root.filePath("seedance-resolution"), probe));
        client->create("test");
        QTRY_COMPARE(errors->size(), 1);
        client->create("test", {}, "16:9", 5, "1080p");
        QTRY_COMPARE(errors->size(), 2);
        client->create("test", {}, "16:9", 5, "space-default");
        QTRY_COMPARE(errors->size(), 3);
        QCOMPARE(server->requests.size(), 2);
        QCOMPARE(count("POST"), 2);
        QCOMPARE(QJsonDocument::fromJson(server->requests[0].body).object().value("resolution").toString(), QStringLiteral("720p"));
        QCOMPARE(QJsonDocument::fromJson(server->requests[1].body).object().value("resolution").toString(), QStringLiteral("1080p"));
    }
    void unknownProviderNeverSends()
    {
        QVERIFY(!client->configureProvider("unknown", "local-test-key", root.filePath("unknown-provider"), probe));
        client->create("test");
        client->resume("task-1");
        client->refresh();
        QTest::qWait(80);
        QCOMPARE(server->requests.size(), 0);
    }
    void anonymousProviderAcceptsEmptyCredential()
    {
        QVERIFY(client->configureProvider("ltx-space", {}, root.filePath("anonymous-provider"), probe));
        QCOMPARE(client->providerId(), QStringLiteral("ltx-space"));
        QCOMPARE(client->currentStatus(), QStringLiteral("idle"));
        QVERIFY(!client->supportsResume());
        QVERIFY(server->requests.isEmpty());
    }
    void ltxRejectsUnsupportedAspectRatios_data()
    {
        QTest::addColumn<QString>("ratio");
        QTest::addColumn<bool>("supported");
        QTest::newRow("unsupported-4:3") << QStringLiteral("4:3") << false;
        QTest::newRow("unsupported-3:4") << QStringLiteral("3:4") << false;
        QTest::newRow("unsupported-21:9") << QStringLiteral("21:9") << false;
        QTest::newRow("unsupported-adaptive") << QStringLiteral("adaptive") << false;
        QTest::newRow("supported-16:9") << QStringLiteral("16:9") << true;
        QTest::newRow("supported-9:16") << QStringLiteral("9:16") << true;
        QTest::newRow("supported-1:1") << QStringLiteral("1:1") << true;
    }
    void ltxRejectsUnsupportedAspectRatios()
    {
        QFETCH(QString, ratio);
        QFETCH(bool, supported);
        QVERIFY(configureFakeProvider());
        client->create("test", {}, ratio, 5, "space-default");
        QCOMPARE(fake->requests.size(), supported ? 1 : 0);
        QCOMPARE(errors->size(), supported ? 0 : 1);
        if (supported) QCOMPARE(fake->requests.first().ratio, ratio);
        QCOMPARE(server->requests.size(), 0);
    }
    void providerOutputUsesExistingDownloadAndProbe()
    {
        QVERIFY(configureFakeProvider());
        HttpServer other;
        QVERIFY(other.listen(QHostAddress::LocalHost));
        auto target = other.media();
        target.setHost(QStringLiteral("localhost"));
        const auto cookie = QByteArray("Set-Cookie: media_token=unexpected; Path=/\r\n");
        server->handle = [this, cookie](const Request &r) { HttpServer::reply(r, 200, mp4, cookie); };
        QSignalSpy statuses(client.get(), &VideoGenerationClient::status);
        client->create("test", {}, "16:9", 5, "space-default");
        QCOMPARE(fake->requests.size(), 1);
        QCOMPARE(fake->requests.first().resolution, QStringLiteral("space-default"));
        Q_EMIT fake->outputReady("ltx-1", server->media(), {});
        Q_EMIT fake->outputReady("duplicate", server->media(), {});
        QTRY_COMPARE_WITH_TIMEOUT(ready->size(), 1, 5000);
        QCOMPARE(client->taskId(), QStringLiteral("ltx-1"));
        QCOMPARE(errors->size(), 0);
        QFile file(ready->first().first().toString());
        QVERIFY(file.fileName().endsWith(".mp4"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), mp4);
        QStringList states;
        for (const auto &args : statuses) states << args[0].toString();
        QVERIFY(states.contains("downloading"));
        QVERIFY(states.contains("validating"));

        server->handle = [target](const Request &r) { HttpServer::reply(r, 302, {}, "Location: " + target.toEncoded() + "\r\n"); };
        client->create("redirect", {}, "16:9", 5, "space-default");
        Q_EMIT fake->outputReady("ltx-2", server->media(), {});
        QTRY_COMPARE(errors->size(), 1);
        QTest::qWait(80);
        QCOMPARE(ready->size(), 1);
        QCOMPARE(count("POST"), 0);
        QCOMPARE(count("GET"), 2);
        QCOMPARE(other.requests.size(), 0);
        for (const auto &r : server->requests) {
            QCOMPARE(r.path, QByteArray("/video.mp4"));
            QVERIFY(!r.headers.contains("authorization"));
            QVERIFY(!r.headers.contains("cookie"));
        }

        server->handle = [](const Request &r) { r.socket->write("HTTP/1.1 200 OK\r\nContent-Length: 100000\r\n\r\npartial"); };
        client->create("stop download", {}, "16:9", 5, "space-default");
        Q_EMIT fake->outputReady("ltx-3", server->media(), {});
        QTRY_COMPARE(count("GET"), 3);
        client->cancelOrStopWaiting();
        Q_EMIT fake->outputReady("late", server->media(), {});
        QTest::qWait(80);
        QCOMPARE(client->currentStatus(), QStringLiteral("stopped"));
        QCOMPARE(ready->size(), 1);
        QCOMPARE(count("GET"), 3);
        const QDir directory(root.filePath(QString::fromLatin1(QTest::currentTestFunction())));
        QCOMPARE(directory.entryList({"*.mp4"}, QDir::Files).size(), 1);
        QVERIFY(directory.entryList({"*.part*"}, QDir::Files).isEmpty());
    }
    void providerFailureNeverFallsBackToSeedance()
    {
        QVERIFY(configureFakeProvider());
        client->create("test", {}, "16:9", 5, "space-default");
        QCOMPARE(fake->requests.size(), 1);
        Q_EMIT fake->failed("provider failed");
        QTRY_COMPARE(errors->size(), 1);
        QTest::qWait(80);
        QCOMPARE(client->currentStatus(), QStringLiteral("failed"));
        QCOMPARE(client->providerId(), QStringLiteral("ltx-space"));
        QCOMPARE(fake->requests.size(), 1);
        QCOMPARE(server->requests.size(), 0);
    }
    void taskRecordStoresProviderWithoutCredential()
    {
        QVERIFY(configureFakeProvider("private-test-credential"));
        QCOMPARE(fake->configuredCredential, QStringLiteral("private-test-credential"));
        server->handle = [this](const Request &r) { HttpServer::reply(r, 200, mp4); };
        client->create("test", {}, "16:9", 5, "space-default");
        Q_EMIT fake->outputReady("ltx-1", server->media(), {{"total_tokens", 123}});
        QTRY_COMPARE_WITH_TIMEOUT(ready->size(), 1, 5000);
        QFile record(client->taskRecordPath());
        QVERIFY(record.open(QIODevice::ReadOnly));
        const auto saved = record.readAll();
        const auto json = QJsonDocument::fromJson(saved).object();
        QCOMPARE(json.value("provider").toString(), QStringLiteral("ltx-space"));
        QCOMPARE(json.value("usage").toObject().value("total_tokens").toInt(), 123);
        QVERIFY(!saved.contains("private-test-credential"));
        QVERIFY(!saved.contains("credential"));
        record.close();

        const auto folder = root.filePath(QString::fromLatin1(QTest::currentTestFunction()));
        QVERIFY(client->configure("local-test-key", folder, probe));
        client->resume("ltx-1");
        QTRY_COMPARE(errors->size(), 1);
        client->refresh();
        QTest::qWait(80);
        QCOMPARE(server->requests.size(), 1);

        QVERIFY(record.open(QIODevice::WriteOnly | QIODevice::Truncate));
        auto legacy = json;
        legacy.remove("provider");
        QVERIFY(record.write(QJsonDocument(legacy).toJson()) > 0);
        record.close();
        server->handle = [this](const Request &r) { HttpServer::json(r, task("cancelled", "ltx-1")); };
        client->resume("ltx-1");
        QTRY_COMPARE(client->currentStatus(), QStringLiteral("cancelled"));
        QCOMPARE(server->requests.size(), 2);
        QCOMPARE(server->requests.last().headers.value("authorization"), QByteArray("Bearer local-test-key"));
    }
    void unsupportedResumeNeverSends()
    {
        QVERIFY(configureFakeProvider());
        QVERIFY(!client->supportsResume());
        client->resume("ltx-1");
        client->refresh();
        QTest::qWait(80);
        QVERIFY(client->taskId().isEmpty());
        QCOMPARE(fake->requests.size(), 0);
        QCOMPARE(server->requests.size(), 0);

        server->handle = [](const Request &r) { HttpServer::reply(r, 503); };
        client->m_retryIntervalMs = 250;
        client->create("stop retry");
        Q_EMIT fake->outputReady("ltx-1", server->media(), {});
        QTRY_COMPARE(count("GET"), 1);
        client->resume("ignored");
        client->refresh();
        client->cancelOrStopWaiting();
        QTest::qWait(350);
        QCOMPARE(client->taskId(), QStringLiteral("ltx-1"));
        QCOMPARE(client->currentStatus(), QStringLiteral("stopped"));
        QCOMPARE(count("GET"), 1);
        QCOMPARE(fake->requests.size(), 1);
        QCOMPARE(ready->size(), 0);
    }

    void badInputDoesNotSend()
    {
        client->create("test", {}, "bad", 5); client->create("test", {}, "16:9", 31); client->create("test", root.filePath("missing.png")); client->create({});
        QTest::qWait(80); QCOMPARE(server->requests.size(), 0); QCOMPARE(errors->size(), 4);
    }
};

QTEST_GUILESS_MAIN(VideoGenerationClientTest)
#include "videogenerationclienttest.moc"
