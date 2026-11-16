#include "../../src/assistant/workbuddybridge.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QUuid>
#include <QtTest>
#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#endif

namespace {
QJsonArray tools()
{
    return {QJsonObject{{"type", "function"}, {"name", "read_project_state"}, {"description", "Read state"},
                       {"deferLoading", false}, {"inputSchema", QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}}}}};
}
QString socketName(const WorkBuddyBridge &bridge)
{
    return bridge.configuration().value("mcpServers").toObject().value("localcut").toObject().value("args").toArray()[1].toString();
}
QByteArray message(const QString &method, const QJsonValue &id = QJsonValue(QJsonValue::Undefined), const QJsonObject &params = {})
{
    QJsonObject result{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
    if (!id.isUndefined()) result.insert("id", id);
    return QJsonDocument(result).toJson(QJsonDocument::Compact) + '\n';
}
QJsonObject read(QIODevice &device)
{
    QElapsedTimer timer;
    timer.start();
    while (!device.canReadLine() && timer.elapsed() < 3000) QTest::qWait(5);
    return QJsonDocument::fromJson(device.readLine()).object();
}
bool initialize(QLocalSocket &socket, WorkBuddyBridge &bridge, const QString &version = QStringLiteral("2025-11-25"))
{
    socket.connectToServer(socketName(bridge));
    if (!socket.waitForConnected(1000)) return false;
    socket.write(message(QStringLiteral("initialize"), 1, {{"protocolVersion", version}, {"capabilities", QJsonObject{}},
        {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}}));
    const auto response = read(socket);
    if (!response.value("result").isObject()) return false;
    socket.write(message(QStringLiteral("notifications/initialized")));
    return true;
}
void call(QLocalSocket &socket, const QJsonValue &id)
{
    socket.write(message(QStringLiteral("tools/call"), id, {{"name", "read_project_state"}, {"arguments", QJsonObject{}}}));
}
}

class WorkBuddyBridgeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        const QString base = qEnvironmentVariable("LOCALCUT_WORKSPACE_ROOT", QCoreApplication::applicationDirPath());
        qputenv("LOCALCUT_WORKSPACE_ROOT", (base + QStringLiteral("/workbuddy-") + QUuid::createUuid().toString(QUuid::WithoutBraces)).toUtf8());
    }
    void listenerReady()
    {
        WorkBuddyBridge bridge;
        QSignalSpy ready(&bridge, &WorkBuddyBridge::ready);
        bridge.start({}, QStringLiteral("LocalCut test"));
        QVERIFY(bridge.isReady());
        QCOMPARE(ready.count(), 1);
        QVERIFY(!bridge.isBusy());
    }
    void stableExclusiveListener()
    {
        WorkBuddyBridge first;
        first.start(tools(), {});
        QVERIFY(first.isReady());
        const QString name = socketName(first);
        QVERIFY(name.startsWith(QStringLiteral("localcut-mcp-")));
        QCOMPARE(name.size(), 37);
        WorkBuddyBridge second;
        QSignalSpy failure(&second, &WorkBuddyBridge::failed);
        second.start(tools(), {});
        QVERIFY(!second.isReady());
        QCOMPARE(failure.count(), 1);
        QCOMPARE(socketName(second), name);
        first.stop();
        second.start(tools(), {});
        QVERIFY(second.isReady());
        QCOMPARE(socketName(second), name);
    }
#ifdef Q_OS_WIN
    void pipeAclOnlyAllowsCurrentUser()
    {
        WorkBuddyBridge bridge;
        bridge.start(tools(), {});
        QLocalSocket socket;
        QVERIFY(initialize(socket, bridge));
        PACL acl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        QCOMPARE(GetSecurityInfo(reinterpret_cast<HANDLE>(socket.socketDescriptor()), SE_KERNEL_OBJECT,
                                 DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor), DWORD(ERROR_SUCCESS));
        auto releaseDescriptor = qScopeGuard([&]() { LocalFree(descriptor); });
        QVERIFY(acl != nullptr);
        QCOMPARE(acl->AceCount, WORD(1));
        void *ace = nullptr;
        QVERIFY(GetAce(acl, 0, &ace));
        QCOMPARE(static_cast<ACE_HEADER *>(ace)->AceType, BYTE(ACCESS_ALLOWED_ACE_TYPE));
        HANDLE token = nullptr;
        QVERIFY(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
        auto closeToken = qScopeGuard([&]() { CloseHandle(token); });
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        QByteArray user(int(size), '\0');
        QVERIFY(GetTokenInformation(token, TokenUser, user.data(), size, &size));
        QVERIFY(EqualSid(&static_cast<ACCESS_ALLOWED_ACE *>(ace)->SidStart, reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid));
    }
#endif
    void protocolVersions_data()
    {
        QTest::addColumn<QString>("requested");
        QTest::addColumn<QString>("negotiated");
        for (const auto &version : {"2025-11-25", "2025-06-18", "2024-11-05"}) QTest::newRow(version) << QString::fromLatin1(version) << QString::fromLatin1(version);
        QTest::newRow("unknown") << QStringLiteral("2099-01-01") << QStringLiteral("2025-11-25");
    }
    void protocolVersions()
    {
        QFETCH(QString, requested);
        QFETCH(QString, negotiated);
        WorkBuddyBridge bridge;
        bridge.start(tools(), QStringLiteral("Instructions"));
        QLocalSocket socket;
        socket.connectToServer(socketName(bridge));
        QVERIFY(socket.waitForConnected(1000));
        socket.write(message(QStringLiteral("initialize"), 1, {{"protocolVersion", requested}, {"capabilities", QJsonObject{}}, {"clientInfo", QJsonObject{}}}));
        const auto result = read(socket).value("result").toObject();
        QCOMPARE(result.value("protocolVersion").toString(), negotiated);
        QCOMPARE(result.value("instructions").toString(), QStringLiteral("Instructions"));
        QVERIFY(result.value("capabilities").toObject().contains("tools"));
        socket.write(message(QStringLiteral("tools/list"), 2));
        QCOMPARE(read(socket).value("error").toObject().value("code").toInt(), -32002);
        socket.write(message(QStringLiteral("notifications/initialized")));
        socket.write(message(QStringLiteral("tools/list"), 3));
        const auto catalog = read(socket).value("result").toObject().value("tools").toArray();
        QCOMPARE(catalog.size(), 1);
        const auto tool = catalog[0].toObject();
        QCOMPARE(tool.value("name").toString(), QStringLiteral("read_project_state"));
        QVERIFY(tool.value("inputSchema").isObject());
        QVERIFY(!tool.contains("deferLoading"));
        QVERIFY(!tool.contains("type"));
    }
    void synchronousToolReply()
    {
        WorkBuddyBridge bridge;
        bridge.start(tools(), {});
        QLocalSocket socket;
        QVERIFY(initialize(socket, bridge));
        QStringList order;
        connect(&bridge, &WorkBuddyBridge::toolStarted, &bridge, [&]() { order << "started"; });
        connect(&bridge, &WorkBuddyBridge::toolRequested, &bridge, [&](const QJsonValue &token, const QString &name, const QJsonObject &arguments) {
            order << "tool";
            QVERIFY(bridge.isBusy());
            QCOMPARE(name, QStringLiteral("read_project_state"));
            QVERIFY(arguments.isEmpty());
            QVERIFY(!QUuid(token.toString()).isNull());
            bridge.replyToTool(token, QJsonObject{{"revision", 3}}, true);
        });
        QSignalSpy finished(&bridge, &WorkBuddyBridge::turnFinished);
        call(socket, QStringLiteral("state-1"));
        const auto response = read(socket);
        QCOMPARE(response.value("id").toString(), QStringLiteral("state-1"));
        QCOMPARE(order, QStringList({"started", "tool"}));
        QVERIFY(!response.value("result").toObject().value("isError").toBool());
        QVERIFY(response.value("result").toObject().value("content").toArray()[0].toObject().value("text").toString().contains("revision"));
        QCOMPARE(finished.count(), 1);
        QVERIFY(!bridge.isBusy());
    }
    void rejectsInvalidAndConcurrentCalls()
    {
        WorkBuddyBridge bridge;
        bridge.start(tools(), {});
        QLocalSocket socket;
        QVERIFY(initialize(socket, bridge));
        QSignalSpy requested(&bridge, &WorkBuddyBridge::toolRequested);
        socket.write(message(QStringLiteral("tools/call"), 2, {{"name", "unknown"}, {"arguments", QJsonObject{}}}));
        QCOMPARE(read(socket).value("error").toObject().value("code").toInt(), -32602);
        socket.write(message(QStringLiteral("tools/call"), 3, {{"name", "read_project_state"}, {"arguments", QJsonArray{}}}));
        QCOMPARE(read(socket).value("error").toObject().value("code").toInt(), -32602);
        QCOMPARE(requested.count(), 0);
        call(socket, 4);
        QTRY_COMPARE(requested.count(), 1);
        call(socket, 5);
        QCOMPARE(read(socket).value("error").toObject().value("code").toInt(), -32000);
        QCOMPARE(requested.count(), 1);
        bridge.replyToTool(requested[0][0].value<QJsonValue>(), QJsonObject{}, true);
        QCOMPARE(read(socket).value("id").toInt(), 4);
        socket.write(message(QStringLiteral("ping"), 6));
        QVERIFY(read(socket).value("result").isObject());
        socket.write(message(QStringLiteral("ping"), 6));
        QCOMPARE(read(socket).value("error").toObject().value("code").toInt(), -32600);
        QTRY_COMPARE(socket.state(), QLocalSocket::UnconnectedState);
    }
    void cancellationAndStaleReplies()
    {
        WorkBuddyBridge bridge;
        bridge.start(tools(), {});
        QLocalSocket socket;
        QVERIFY(initialize(socket, bridge));
        QSignalSpy requested(&bridge, &WorkBuddyBridge::toolRequested);
        QSignalSpy cancelled(&bridge, &WorkBuddyBridge::cancelRequested);
        QSignalSpy finished(&bridge, &WorkBuddyBridge::turnFinished);
        connect(&bridge, &WorkBuddyBridge::cancelRequested, &bridge, &WorkBuddyBridge::cancel);
        call(socket, 2);
        QTRY_COMPARE(requested.count(), 1);
        const auto oldToken = requested[0][0].value<QJsonValue>();
        socket.write(message(QStringLiteral("notifications/cancelled"), QJsonValue(QJsonValue::Undefined), {{"requestId", 99}}));
        socket.write(message(QStringLiteral("ping"), 3));
        QVERIFY(read(socket).value("result").isObject());
        QVERIFY(bridge.isBusy());
        QCOMPARE(cancelled.count(), 0);
        socket.write(message(QStringLiteral("notifications/cancelled"), QJsonValue(QJsonValue::Undefined), {{"requestId", 2}}));
        QCOMPARE(read(socket).value("error").toObject().value("code").toInt(), -32800);
        QCOMPARE(cancelled.count(), 1);
        QCOMPARE(finished.count(), 1);
        QVERIFY(!bridge.isBusy());
        call(socket, 4);
        QTRY_COMPARE(requested.count(), 2);
        const auto newToken = requested[1][0].value<QJsonValue>();
        QVERIFY(oldToken != newToken);
        bridge.replyToTool(oldToken, QJsonObject{}, true);
        QVERIFY(bridge.isBusy());
        QCOMPARE(finished.count(), 1);
        bridge.replyToTool(newToken, QJsonObject{}, true);
        QCOMPARE(read(socket).value("id").toInt(), 4);
        QCOMPARE(finished.count(), 2);
    }
    void disconnectCancelsAndStopIsQuiet()
    {
        WorkBuddyBridge bridge;
        bridge.start(tools(), {});
        QLocalSocket socket;
        QVERIFY(initialize(socket, bridge));
        QSignalSpy requested(&bridge, &WorkBuddyBridge::toolRequested);
        QSignalSpy cancelled(&bridge, &WorkBuddyBridge::cancelRequested);
        QSignalSpy finished(&bridge, &WorkBuddyBridge::turnFinished);
        call(socket, 2);
        QTRY_COMPARE(requested.count(), 1);
        const auto oldToken = requested[0][0].value<QJsonValue>();
        socket.abort();
        QTRY_COMPARE(cancelled.count(), 1);
        QCOMPARE(finished.count(), 1);
        QVERIFY(!bridge.isBusy());
        QVERIFY(bridge.isReady());
        QVERIFY(initialize(socket, bridge));
        call(socket, 2);
        QTRY_COMPARE(requested.count(), 2);
        bridge.replyToTool(oldToken, QJsonObject{}, true);
        QVERIFY(bridge.isBusy());
        bridge.stop();
        QCOMPARE(cancelled.count(), 1);
        QCOMPARE(finished.count(), 1);
        QVERIFY(!bridge.isReady());
        QVERIFY(!bridge.isBusy());
    }
    void imagesAndOversizeReplies()
    {
        WorkBuddyBridge bridge;
        bridge.start(tools(), {});
        QLocalSocket socket;
        QVERIFY(initialize(socket, bridge));
        QSignalSpy requested(&bridge, &WorkBuddyBridge::toolRequested);
        call(socket, 2);
        QTRY_COMPARE(requested.count(), 1);
        bridge.replyToTool(requested[0][0].value<QJsonValue>(), QJsonArray{QJsonObject{{"type", "inputImage"}, {"imageUrl", "data:image/jpeg;base64,YWJj"}}}, true);
        const auto content = read(socket).value("result").toObject().value("content").toArray()[0].toObject();
        QCOMPARE(content.value("type").toString(), QStringLiteral("image"));
        QCOMPARE(content.value("mimeType").toString(), QStringLiteral("image/jpeg"));
        QCOMPARE(content.value("data").toString(), QStringLiteral("YWJj"));
        call(socket, 3);
        QTRY_COMPARE(requested.count(), 2);
        bridge.replyToTool(requested[1][0].value<QJsonValue>(), QJsonObject{{"oversize", QString(8 * 1024 * 1024, QLatin1Char('x'))}}, true);
        QCOMPARE(read(socket).value("error").toObject().value("code").toInt(), -32603);
        QVERIFY(!bridge.isBusy());
    }
    void frameLimitAndSecondConnection()
    {
        WorkBuddyBridge bridge;
        bridge.start(tools(), {});
        QLocalSocket first;
        QVERIFY(initialize(first, bridge));
        QLocalSocket second;
        second.connectToServer(socketName(bridge));
        QVERIFY(second.waitForConnected(1000));
        QTRY_COMPARE(second.state(), QLocalSocket::UnconnectedState);
        first.write(message(QStringLiteral("ping"), 2));
        QCOMPARE(read(first).value("id").toInt(), 2);
        first.write(QByteArray(8 * 1024 * 1024 + 1, 'x'));
        QCOMPARE(read(first).value("error").toObject().value("code").toInt(), -32600);
        QTRY_COMPARE(first.state(), QLocalSocket::UnconnectedState);
        QVERIFY(bridge.isReady());
    }
    void bundledStdioRelay()
    {
        WorkBuddyBridge bridge;
        bridge.start(tools(), {});
        const auto config = bridge.configuration().value("mcpServers").toObject().value("localcut").toObject();
        QProcess relay;
        relay.start(config.value("command").toString(), {QStringLiteral("--socket"), socketName(bridge)});
        QVERIFY(relay.waitForStarted(2000));
        relay.write(message(QStringLiteral("initialize"), 1, {{"protocolVersion", "2025-11-25"}, {"capabilities", QJsonObject{}}, {"clientInfo", QJsonObject{}}}));
        QCOMPARE(read(relay).value("id").toInt(), 1);
        relay.write(message(QStringLiteral("notifications/initialized")));
        QSignalSpy requested(&bridge, &WorkBuddyBridge::toolRequested);
        relay.write(message(QStringLiteral("tools/call"), 2, {{"name", "read_project_state"}, {"arguments", QJsonObject{}}}));
        QTRY_COMPARE(requested.count(), 1);
        bridge.replyToTool(requested[0][0].value<QJsonValue>(), QJsonObject{{"ok", true}}, true);
        QCOMPARE(read(relay).value("id").toInt(), 2);
        QCOMPARE(relay.readAllStandardError(), QByteArray{});
        relay.closeWriteChannel();
        QTRY_COMPARE(relay.state(), QProcess::NotRunning);
        QCOMPARE(relay.exitCode(), 0);
        QCOMPARE(relay.exitStatus(), QProcess::NormalExit);
    }
    void relayExitsWhenEditorStops()
    {
        WorkBuddyBridge bridge;
        bridge.start(tools(), {});
        const auto config = bridge.configuration().value("mcpServers").toObject().value("localcut").toObject();
        QProcess relay;
        relay.start(config.value("command").toString(), {QStringLiteral("--socket"), socketName(bridge)});
        QVERIFY(relay.waitForStarted(2000));
        relay.write(message(QStringLiteral("ping"), 1));
        QCOMPARE(read(relay).value("id").toInt(), 1);
        bridge.stop();
        QTRY_COMPARE(relay.state(), QProcess::NotRunning);
        QCOMPARE(relay.exitCode(), 1);
        QVERIFY(relay.readAllStandardOutput().isEmpty());
        QVERIFY(!relay.readAllStandardError().isEmpty());
    }
    void relayRejectsOversizeInput()
    {
        WorkBuddyBridge bridge;
        bridge.start(tools(), {});
        const auto config = bridge.configuration().value("mcpServers").toObject().value("localcut").toObject();
        QProcess relay;
        relay.start(config.value("command").toString(), {QStringLiteral("--socket"), socketName(bridge)});
        QVERIFY(relay.waitForStarted(2000));
        relay.write(QByteArray(8 * 1024 * 1024 + 1, 'x'));
        QTRY_COMPARE(relay.state(), QProcess::NotRunning);
        QCOMPARE(relay.exitCode(), 1);
        QVERIFY(relay.readAllStandardOutput().isEmpty());
        QVERIFY(relay.readAllStandardError().contains("8 MiB"));
    }
    void relayExitsWithBlockedStdout()
    {
        WorkBuddyBridge bridge;
        bridge.start(tools(), {});
        QProcess sink;
        QProcess relay;
        auto cleanup = qScopeGuard([&]() {
            if (relay.state() != QProcess::NotRunning) { relay.kill(); relay.waitForFinished(1000); }
            if (sink.state() != QProcess::NotRunning) { sink.kill(); sink.waitForFinished(1000); }
        });
        relay.setStandardOutputProcess(&sink);
        sink.start(QCoreApplication::applicationDirPath() + QStringLiteral("/workbuddy-output-sink"), {});
        QVERIFY(sink.waitForStarted(2000));
        const auto config = bridge.configuration().value("mcpServers").toObject().value("localcut").toObject();
        relay.start(config.value("command").toString(), {QStringLiteral("--socket"), socketName(bridge)});
        QVERIFY(relay.waitForStarted(2000));
        relay.write(message(QStringLiteral("initialize"), 1, {{"protocolVersion", "2025-11-25"}, {"capabilities", QJsonObject{}}, {"clientInfo", QJsonObject{}}}));
        relay.write(message(QStringLiteral("notifications/initialized")));
        QSignalSpy requested(&bridge, &WorkBuddyBridge::toolRequested);
        relay.write(message(QStringLiteral("tools/call"), 2, {{"name", "read_project_state"}, {"arguments", QJsonObject{}}}));
        QTRY_COMPARE(requested.count(), 1);
        bridge.replyToTool(requested[0][0].value<QJsonValue>(), QJsonObject{{"payload", QString(2 * 1024 * 1024, QLatin1Char('x'))}}, true);
        QTest::qWait(200);
        QCOMPARE(relay.state(), QProcess::Running);
        bridge.stop();
        QTRY_COMPARE_WITH_TIMEOUT(relay.state(), QProcess::NotRunning, 2000);
        QCOMPARE(relay.exitCode(), 1);
    }
};
QTEST_GUILESS_MAIN(WorkBuddyBridgeTest)
#include "workbuddybridgetest.moc"
