/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/assistant/chatcompletionsclient.h"
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <functional>
#include <memory>

struct Request {
    QJsonObject body;
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
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
                    *bytes += socket->readAll();
                    const auto split = bytes->indexOf("\r\n\r\n");
                    if (split < 0 || socket->property("handled").toBool()) return;
                    Request request;
                    request.socket = socket;
                    for (const auto &line : bytes->left(split).split('\n').mid(1)) {
                        const auto colon = line.indexOf(':');
                        if (colon > 0) request.headers[line.left(colon).toLower().trimmed()] = line.mid(colon + 1).trimmed();
                    }
                    const int length = request.headers.value("content-length").toInt();
                    if (bytes->size() < split + 4 + length) return;
                    request.body = QJsonDocument::fromJson(bytes->mid(split + 4, length)).object();
                    socket->setProperty("handled", true);
                    requests.append(request);
                    if (handle) handle(request);
                });
            }
        });
    }
    QUrl endpoint() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(serverPort())); }
    static void reply(const Request &request, const QByteArray &body, int status = 200, const QByteArray &extra = {})
    {
        if (!request.socket) return;
        request.socket->write("HTTP/1.1 " + QByteArray::number(status) + " Result\r\nContent-Type: application/json\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\nConnection: close\r\n" + extra + "\r\n" + body);
        request.socket->disconnectFromHost();
    }
    static void json(const Request &request, const QJsonObject &body) { reply(request, QJsonDocument(body).toJson(QJsonDocument::Compact)); }
};

QJsonArray definitions()
{
    QJsonArray tools;
    for (const auto &name : {"read_project_state", "apply_edit_batch"})
        tools.append(QJsonObject{{"type", "function"}, {"name", name}, {"description", "Use the actual project."}, {"deferLoading", false},
            {"inputSchema", QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}, {"additionalProperties", false}}}});
    return tools;
}

QJsonObject call(const QString &id, const QString &name = QStringLiteral("read_project_state"), const QString &arguments = QStringLiteral("{}"))
{
    return {{"id", id}, {"type", "function"}, {"function", QJsonObject{{"name", name}, {"arguments", arguments}}}};
}

QJsonObject response(const QString &text = QStringLiteral("完成"), const QJsonArray &calls = {}, const QString &reasoning = {})
{
    QJsonObject message{{"role", "assistant"}, {"content", text}};
    if (!calls.isEmpty()) message.insert("tool_calls", calls);
    if (!reasoning.isEmpty()) message.insert("reasoning_content", reasoning);
    return {{"choices", QJsonArray{QJsonObject{{"finish_reason", calls.isEmpty() ? "stop" : "tool_calls"}, {"message", message}}}}};
}

class ChatCompletionsClientTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void configurationDoesNotCallTheProvider()
    {
        ChatCompletionsClient client;
        QSignalSpy ready(&client, &ChatCompletionsClient::ready);
        client.start(QUrl("http://127.0.0.1:11434/v1/chat/completions"), "local-model", {}, {}, "Edit safely.");
        QVERIFY(client.isReady());
        QVERIFY(!client.isBusy());
        QCOMPARE(ready.count(), 1);
    }

    void endpointValidation_data()
    {
        QTest::addColumn<QString>("url"); QTest::addColumn<bool>("accepted");
        QTest::newRow("https") << "https://api.example.com/v1/chat/completions" << true;
        QTest::newRow("localhost") << "http://localhost:11434/v1/chat/completions" << true;
        QTest::newRow("ipv6") << "http://[::1]:11434/v1/chat/completions" << true;
        QTest::newRow("remote-http") << "http://api.example.com/v1/chat/completions" << false;
        QTest::newRow("private-http") << "http://192.168.1.3/v1/chat/completions" << false;
        QTest::newRow("userinfo") << "https://name:secret@api.example.com/v1/chat/completions" << false;
        QTest::newRow("query") << "https://api.example.com/v1/chat/completions?api_key=secret" << false;
        QTest::newRow("fragment") << "https://api.example.com/v1/chat/completions#secret" << false;
        QTest::newRow("wrong-path") << "https://api.example.com/v1/models" << false;
    }
    void endpointValidation()
    {
        QFETCH(QString, url); QFETCH(bool, accepted);
        ChatCompletionsClient client; QSignalSpy errors(&client, &ChatCompletionsClient::failed);
        client.start(QUrl(url), "model", {}, definitions(), "Edit safely.");
        QCOMPARE(client.isReady(), accepted);
        QCOMPARE(errors.size(), accepted ? 0 : 1);
        if (!errors.isEmpty()) QVERIFY(!errors.first().first().toString().contains("secret"));
    }

    void actualHttpRequestAndTextReply()
    {
        HttpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        server.handle = [](const Request &request) { HttpServer::json(request, response()); };
        ChatCompletionsClient client;
        QSignalSpy finished(&client, &ChatCompletionsClient::turnFinished), text(&client, &ChatCompletionsClient::textDelta);
        client.start(server.endpoint(), "configured-model", "test-credential", definitions(), "Edit safely.");
        QCOMPARE(server.requests.size(), 0);
        client.submit("把视频剪开");
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.first().first().toBool());
        QCOMPARE(text.first().first().toString(), QStringLiteral("完成"));
        QCOMPARE(server.requests.size(), 1);
        const auto request = server.requests.first();
        QCOMPARE(request.headers.value("authorization"), QByteArray("Bearer test-credential"));
        QCOMPARE(request.body.value("model").toString(), QStringLiteral("configured-model"));
        QCOMPARE(request.body.value("stream"), QJsonValue(false));
        const auto messages = request.body.value("messages").toArray();
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messages.last().toObject().value("content").toString(), QStringLiteral("把视频剪开"));
        const auto tool = request.body.value("tools").toArray().first().toObject();
        QVERIFY(!tool.contains("deferLoading"));
        QCOMPARE(tool.value("function").toObject().value("parameters").toObject().value("type").toString(), QStringLiteral("object"));
        QVERIFY(client.isReady()); QVERIFY(!client.isBusy());
    }

    void serialToolsRoundTripPreservesReasoning()
    {
        HttpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        server.handle = [&](const Request &request) {
            HttpServer::json(request, server.requests.size() == 1 ? response({}, {call("a"), call("b", "apply_edit_batch", "{\"frame\":50}")}, "private reasoning") : response());
        };
        ChatCompletionsClient client;
        QSignalSpy tools(&client, &ChatCompletionsClient::toolRequested), finished(&client, &ChatCompletionsClient::turnFinished);
        client.start(server.endpoint(), "model", {}, definitions(), "Edit safely."); client.submit("剪开");
        QTRY_COMPARE(tools.size(), 1);
        QCOMPARE(server.requests.size(), 1);
        const auto firstToken = tools[0][0].value<QJsonValue>();
        client.replyToTool(firstToken, QJsonObject{{"revision", 1}}, true);
        QTRY_COMPARE(tools.size(), 2);
        QCOMPARE(tools[1][2].value<QJsonObject>().value("frame").toInt(), 50);
        client.replyToTool(firstToken, QJsonObject{{"stale", true}}, true);
        QCOMPARE(server.requests.size(), 1);
        client.replyToTool(tools[1][0].value<QJsonValue>(), QJsonObject{{"applied", true}}, true);
        QTRY_COMPARE(finished.size(), 1);
        const auto messages = server.requests.last().body.value("messages").toArray();
        QCOMPARE(messages.size(), 5);
        QCOMPARE(messages[2].toObject().value("reasoning_content").toString(), QStringLiteral("private reasoning"));
        QCOMPARE(messages[3].toObject().value("tool_call_id").toString(), QStringLiteral("a"));
        QCOMPARE(messages[4].toObject().value("tool_call_id").toString(), QStringLiteral("b"));
        QVERIFY(!messages[3].toObject().value("content").toString().contains("stale"));
    }

    void invalidBatchExecutesNothing_data()
    {
        QTest::addColumn<QJsonArray>("calls");
        QTest::newRow("unknown-second") << QJsonArray{call("a"), call("b", "arbitrary_shell")};
        QTest::newRow("duplicate-id") << QJsonArray{call("a"), call("a")};
        QTest::newRow("broken-json") << QJsonArray{call("a"), call("b", "apply_edit_batch", "{")};
        QTest::newRow("array-arguments") << QJsonArray{call("a", "read_project_state", "[]")};
        QTest::newRow("missing-id") << QJsonArray{call("")};
    }
    void invalidBatchExecutesNothing()
    {
        QFETCH(QJsonArray, calls);
        HttpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        server.handle = [&](const Request &request) { HttpServer::json(request, response({}, calls)); };
        ChatCompletionsClient client;
        QSignalSpy tools(&client, &ChatCompletionsClient::toolRequested), errors(&client, &ChatCompletionsClient::failed);
        client.start(server.endpoint(), "model", {}, definitions(), "Edit safely."); client.submit("剪开");
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(tools.size(), 0); QVERIFY(!client.isBusy());
    }

    void cancellationDiscardsLateNetworkReply()
    {
        HttpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        server.handle = [&](const Request &request) { if (server.requests.size() == 2) HttpServer::json(request, response("新的回复")); };
        ChatCompletionsClient client;
        QSignalSpy finished(&client, &ChatCompletionsClient::turnFinished), text(&client, &ChatCompletionsClient::textDelta);
        client.start(server.endpoint(), "model", {}, definitions(), "Edit safely."); client.submit("旧要求");
        QTRY_COMPARE(server.requests.size(), 1);
        client.cancel(); QVERIFY(client.isReady()); QVERIFY(!client.isBusy());
        client.submit("新要求");
        QTRY_COMPARE(finished.size(), 2);
        HttpServer::json(server.requests.first(), response("迟到回复"));
        QTest::qWait(25);
        QCOMPARE(text.size(), 1); QCOMPARE(text.first().first().toString(), QStringLiteral("新的回复"));
        const auto messages = server.requests.last().body.value("messages").toArray();
        QCOMPARE(messages.size(), 2); QCOMPARE(messages.last().toObject().value("content").toString(), QStringLiteral("新要求"));
    }

    void oldToolTokenCannotAffectNewSession()
    {
        HttpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        server.handle = [&](const Request &request) { HttpServer::json(request, response({}, {call(QString::number(server.requests.size()))})); };
        ChatCompletionsClient client; QSignalSpy tools(&client, &ChatCompletionsClient::toolRequested), errors(&client, &ChatCompletionsClient::failed);
        client.start(server.endpoint(), "model", {}, definitions(), "Edit safely."); client.submit("old");
        QTRY_COMPARE(tools.size(), 1); const auto oldToken = tools.first().first().value<QJsonValue>();
        client.start(server.endpoint(), "model", {}, definitions(), "Edit safely."); client.submit("new");
        QTRY_COMPARE(tools.size(), 2);
        QVERIFY(oldToken != tools.last().first().value<QJsonValue>());
        client.replyToTool(oldToken, QJsonObject{{"stale", true}}, true);
        QTest::qWait(25); QCOMPARE(server.requests.size(), 2); QCOMPARE(errors.size(), 0); QVERIFY(client.isBusy());
        client.cancel();
    }

    void reconnectFromTextCallbackDoesNotFinishTheNewTurn()
    {
        HttpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        server.handle = [&](const Request &request) {
            if (server.requests.size() == 1) HttpServer::json(request, response("first reply"));
        };
        ChatCompletionsClient client;
        QSignalSpy finished(&client, &ChatCompletionsClient::turnFinished);
        connect(&client, &ChatCompletionsClient::textDelta, &client, [&](const QString &) {
            client.start(server.endpoint(), "model", {}, definitions(), "Edit safely.");
            client.submit("new turn");
        });
        client.start(server.endpoint(), "model", {}, definitions(), "Edit safely."); client.submit("old turn");
        QTRY_COMPARE(server.requests.size(), 2);
        QVERIFY(client.isBusy());
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.first().first().toBool());
        client.cancel();
    }

    void errorsNeverExposeProviderBody_data()
    {
        QTest::addColumn<int>("status");
        for (int status : {401, 403, 429, 500}) QTest::newRow(qPrintable(QString::number(status))) << status;
    }
    void errorsNeverExposeProviderBody()
    {
        QFETCH(int, status);
        HttpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        server.handle = [&](const Request &request) { HttpServer::reply(request, "provider secret and private prompt", status); };
        ChatCompletionsClient client; QSignalSpy errors(&client, &ChatCompletionsClient::failed);
        client.start(server.endpoint(), "model", "private-key", definitions(), "Edit safely."); client.submit("private prompt");
        QTRY_COMPARE(errors.size(), 1);
        const auto message = errors.first().first().toString();
        QVERIFY(!message.contains("private")); QVERIFY(!message.contains("secret")); QCOMPARE(server.requests.size(), 1);
    }

    void redirectIsNotFollowed()
    {
        HttpServer server, target; QVERIFY(server.listen(QHostAddress::LocalHost)); QVERIFY(target.listen(QHostAddress::LocalHost));
        server.handle = [&](const Request &request) { HttpServer::reply(request, {}, 302, "Location: " + target.endpoint().toEncoded() + "\r\n"); };
        ChatCompletionsClient client; QSignalSpy errors(&client, &ChatCompletionsClient::failed);
        client.start(server.endpoint(), "model", "private-key", definitions(), "Edit safely."); client.submit("test");
        QTRY_COMPARE(errors.size(), 1); QCOMPARE(target.requests.size(), 0);
    }

    void timeoutStopsWithoutRetry()
    {
        HttpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        ChatCompletionsClient client; QSignalSpy errors(&client, &ChatCompletionsClient::failed);
        client.start(server.endpoint(), "model", {}, definitions(), "Edit safely.");
        const auto timer = client.findChild<QTimer *>(); QVERIFY(timer); QCOMPARE(timer->interval(), 180000); timer->setInterval(40);
        client.submit("test"); QTRY_COMPARE(errors.size(), 1); QCOMPARE(server.requests.size(), 1); QVERIFY(!client.isBusy());
    }

    void roundsAreBounded()
    {
        HttpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        server.handle = [&](const Request &request) { HttpServer::json(request, response({}, {call(QString::number(server.requests.size()))})); };
        ChatCompletionsClient client; QSignalSpy errors(&client, &ChatCompletionsClient::failed);
        connect(&client, &ChatCompletionsClient::toolRequested, &client, [&](const QJsonValue &token) { client.replyToTool(token, QJsonObject{}, true); });
        client.start(server.endpoint(), "model", {}, definitions(), "Edit safely."); client.submit("test");
        QTRY_COMPARE(errors.size(), 1); QCOMPARE(server.requests.size(), 16); QVERIFY(!client.isBusy());
    }

    void imageFollowsAllToolResultsAndAudioReturnsError()
    {
        HttpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        server.handle = [&](const Request &request) { HttpServer::json(request, server.requests.size() == 1 ? response({}, {call("a"), call("b")}) : response()); };
        ChatCompletionsClient client; QSignalSpy tools(&client, &ChatCompletionsClient::toolRequested), finished(&client, &ChatCompletionsClient::turnFinished);
        client.start(server.endpoint(), "model", {}, definitions(), "Edit safely."); client.submit("test");
        QTRY_COMPARE(tools.size(), 1);
        client.replyToTool(tools[0][0].value<QJsonValue>(), QJsonArray{QJsonObject{{"type", "inputImage"}, {"imageUrl", "data:image/png;base64,aGVsbG8="}}}, true);
        QTRY_COMPARE(tools.size(), 2);
        client.replyToTool(tools[1][0].value<QJsonValue>(), QJsonArray{QJsonObject{{"type", "inputAudio"}, {"audioUrl", "data:audio/wav;base64,aGVsbG8="}}}, true);
        QTRY_COMPARE(finished.size(), 1);
        const auto messages = server.requests.last().body.value("messages").toArray();
        QCOMPARE(messages.size(), 6);
        QCOMPARE(messages[3].toObject().value("role").toString(), QStringLiteral("tool"));
        QCOMPARE(messages[4].toObject().value("role").toString(), QStringLiteral("tool"));
        const auto audioResult = QJsonDocument::fromJson(messages[4].toObject().value("content").toString().toUtf8()).object();
        QCOMPARE(audioResult.value("ok").toBool(), false); QVERIFY(audioResult.value("text").toString().contains(QStringLiteral("不支持音频")));
        QCOMPARE(messages[5].toObject().value("role").toString(), QStringLiteral("user"));
        QCOMPARE(messages[5].toObject().value("content").toArray().last().toObject().value("type").toString(), QStringLiteral("image_url"));
    }

    void malformedAndOversizedResponses_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("invalid-json") << QByteArray("not json");
        QTest::newRow("empty-choices") << QByteArray("{\"choices\":[]}");
        QTest::newRow("too-large") << QByteArray(8 * 1024 * 1024 + 1, 'x');
        QTest::newRow("truncated-tools") << QByteArray("{\"choices\":[{\"finish_reason\":\"length\",\"message\":{\"role\":\"assistant\",\"tool_calls\":[]}}]}");
    }
    void malformedAndOversizedResponses()
    {
        QFETCH(QByteArray, body);
        HttpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        server.handle = [&](const Request &request) { HttpServer::reply(request, body); };
        ChatCompletionsClient client; QSignalSpy errors(&client, &ChatCompletionsClient::failed), tools(&client, &ChatCompletionsClient::toolRequested);
        client.start(server.endpoint(), "model", {}, definitions(), "Edit safely."); client.submit("test");
        QTRY_COMPARE(errors.size(), 1); QCOMPARE(tools.size(), 0); QVERIFY(!client.isBusy());
    }
};
QTEST_GUILESS_MAIN(ChatCompletionsClientTest)
#include "test.moc"
