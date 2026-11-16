/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/assistant/assistantsession.h"
#include "../../src/assistant/codexclient.h"
#include "../../src/assistant/chatcompletionsclient.h"
#include "../../src/assistant/workbuddybridge.h"
#include "../../src/assistant/workspacepaths.h"

#include <QDir>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

QJsonArray sessionTools()
{
    return {QJsonObject{{"type", "function"}, {"name", "read_project_state"}, {"description", "Read the actual project."},
        {"inputSchema", QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}, {"required", QJsonArray{}}, {"additionalProperties", false}}}}};
}

class Peer : public QLocalSocket
{
public:
    QList<QJsonObject> responses;
    QByteArray buffer;
    Peer()
    {
        connect(this, &QLocalSocket::readyRead, this, [this] {
            buffer += readAll();
            qsizetype newline;
            while ((newline = buffer.indexOf('\n')) >= 0) {
                responses.append(QJsonDocument::fromJson(buffer.left(newline)).object());
                buffer.remove(0, newline + 1);
            }
        });
    }
    void send(const QJsonObject &message) { write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n'); flush(); }
    QJsonObject response(int id) const
    {
        for (const auto &message : responses) if (message.value("id").toInt(-1) == id) return message;
        return {};
    }
};

class AssistantSessionTest : public QObject
{
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> workspace;
    void configure(AssistantSession &session)
    {
        session.startCompatible(QUrl("http://127.0.0.1:11434/v1/chat/completions"), "local-model", {}, sessionTools(), "Read before editing.");
    }
    void connectPeer(Peer &peer, const AssistantSession &session)
    {
        const auto config = session.externalConfiguration().value("mcpServers").toObject().value("localcut").toObject();
        const auto args = config.value("args").toArray();
        QCOMPARE(args.size(), 2); QCOMPARE(args[0].toString(), QStringLiteral("--socket"));
        peer.connectToServer(args[1].toString());
        QTRY_COMPARE(peer.state(), QLocalSocket::ConnectedState);
        peer.send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", QJsonObject{
            {"protocolVersion", "2025-11-25"}, {"capabilities", QJsonObject{}}, {"clientInfo", QJsonObject{{"name", "session-test"}, {"version", "1"}}}}}});
        QTRY_VERIFY(!peer.response(1).isEmpty());
        QVERIFY(peer.response(1).contains("result"));
        peer.send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    }
    void callTool(Peer &peer, int id)
    {
        peer.send({{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
            {"params", QJsonObject{{"name", "read_project_state"}, {"arguments", QJsonObject{}}}}});
    }
private Q_SLOTS:
    void initTestCase()
    {
        const QString root = qEnvironmentVariable("LOCALCUT_WORKSPACE_ROOT");
        QVERIFY2(!root.isEmpty() && QDir(root).exists(), "Set LOCALCUT_WORKSPACE_ROOT to an existing test output directory.");
        workspace = std::make_unique<QTemporaryDir>(QDir(root).filePath("session-XXXXXX"));
        QVERIFY(workspace->isValid());
        QString error;
        QVERIFY2(WorkspacePaths::initialize(workspace->path(), &error), qPrintable(error));
    }

    void compatibleConfigurationAndExternalSwitch()
    {
        AssistantSession session;
        QSignalSpy ready(&session, &AssistantSession::ready), errors(&session, &AssistantSession::failed);
        configure(session);
        QVERIFY(session.isReady()); QVERIFY(session.isCompatible()); QVERIFY(!session.isExternal());
        QCOMPARE(ready.size(), 1); QVERIFY(!session.isBusy());
        session.startExternal(sessionTools(), "Read before editing.");
        QVERIFY(session.isReady()); QVERIFY(session.isExternal()); QVERIFY(!session.isCompatible());
        QCOMPARE(ready.size(), 2); QCOMPARE(errors.size(), 0);
        QVERIFY(!session.externalConfiguration().isEmpty());
        auto *compatible = session.findChild<ChatCompletionsClient *>(); QVERIFY(compatible);
        QVERIFY(!compatible->isReady());
        session.stop();
        QVERIFY(!session.isReady()); QVERIFY(!session.isBusy()); QVERIFY(!session.isExternal());
    }

    void inactiveProvidersDoNotForwardSignals()
    {
        AssistantSession session; configure(session);
        auto *codex = session.findChild<CodexClient *>(); auto *external = session.findChild<WorkBuddyBridge *>();
        QVERIFY(codex); QVERIFY(external);
        QSignalSpy ready(&session, &AssistantSession::ready), text(&session, &AssistantSession::textDelta), errors(&session, &AssistantSession::failed),
            tools(&session, &AssistantSession::toolRequested), finished(&session, &AssistantSession::turnFinished),
            started(&session, &AssistantSession::externalToolStarted), cancelled(&session, &AssistantSession::externalCancelRequested);
        codex->ready(); codex->textDelta("old codex text"); codex->failed("old codex error");
        codex->toolRequested("old-codex-token", "read_project_state", {}); codex->turnFinished(false);
        external->ready(); external->textDelta("old external text"); external->failed("old external error");
        external->toolRequested("old-external-token", "read_project_state", {}); external->turnFinished(false);
        external->toolStarted(); external->cancelRequested();
        QCOMPARE(ready.size(), 0); QCOMPARE(text.size(), 0); QCOMPARE(errors.size(), 0); QCOMPARE(tools.size(), 0);
        QCOMPARE(finished.size(), 0); QCOMPARE(started.size(), 0); QCOMPARE(cancelled.size(), 0);
        QVERIFY(session.isReady()); QVERIFY(session.isCompatible());
    }

    void switchingAwaySuppressesOldHttpSignals()
    {
        AssistantSession session; configure(session);
        auto *compatible = session.findChild<ChatCompletionsClient *>(); QVERIFY(compatible);
        session.startExternal(sessionTools(), "Read before editing.");
        QSignalSpy text(&session, &AssistantSession::textDelta), errors(&session, &AssistantSession::failed),
            tools(&session, &AssistantSession::toolRequested), finished(&session, &AssistantSession::turnFinished);
        compatible->textDelta("old HTTP text"); compatible->failed("old HTTP error"); compatible->turnFinished(false);
        compatible->toolRequested("chat-old", "read_project_state", {});
        QCOMPARE(text.size(), 0); QCOMPARE(errors.size(), 0); QCOMPARE(tools.size(), 0); QCOMPARE(finished.size(), 0);
        QVERIFY(session.isReady()); QVERIFY(session.isExternal());
    }

    void stoppedSessionDoesNotForwardLateEvents()
    {
        AssistantSession session; configure(session);
        auto *codex = session.findChild<CodexClient *>(); auto *compatible = session.findChild<ChatCompletionsClient *>();
        auto *external = session.findChild<WorkBuddyBridge *>(); QVERIFY(codex); QVERIFY(compatible); QVERIFY(external);
        session.stop();
        QSignalSpy ready(&session, &AssistantSession::ready), text(&session, &AssistantSession::textDelta), errors(&session, &AssistantSession::failed),
            tools(&session, &AssistantSession::toolRequested), finished(&session, &AssistantSession::turnFinished),
            started(&session, &AssistantSession::externalToolStarted), cancelled(&session, &AssistantSession::externalCancelRequested);
        codex->ready(); codex->textDelta("old"); codex->failed("old"); codex->turnFinished(false); codex->toolRequested("old", "read_project_state", {});
        compatible->ready(); compatible->textDelta("old"); compatible->failed("old"); compatible->turnFinished(false); compatible->toolRequested("old", "read_project_state", {});
        external->ready(); external->textDelta("old"); external->failed("old"); external->turnFinished(false); external->toolRequested("old", "read_project_state", {});
        external->toolStarted(); external->cancelRequested();
        QCOMPARE(ready.size(), 0); QCOMPARE(text.size(), 0); QCOMPARE(errors.size(), 0); QCOMPARE(tools.size(), 0);
        QCOMPARE(finished.size(), 0); QCOMPARE(started.size(), 0); QCOMPARE(cancelled.size(), 0);
    }

    void staleOtherProviderReplyCannotFailNewCodexRoute()
    {
        AssistantSession session; configure(session);
        // The missing process fails asynchronously; a stale tool reply must not itself fail the new route.
        session.start(workspace->filePath("missing-codex.exe"), workspace->path(), sessionTools(), "Read before editing.");
        QSignalSpy errors(&session, &AssistantSession::failed);
        session.replyToTool("chat-old", QJsonObject{{"old", true}}, true);
        session.replyToTool("workbuddy-old", QJsonArray{}, false);
        QCOMPARE(errors.size(), 0);
        session.stop();
    }

    void externalRequestSupportsSynchronousToolReply()
    {
        AssistantSession session; session.startExternal(sessionTools(), "Read before editing."); QVERIFY(session.isReady());
        QStringList order;
        connect(&session, &AssistantSession::externalToolStarted, &session, [&] { order.append("start"); });
        connect(&session, &AssistantSession::toolRequested, &session, [&](const QJsonValue &token, const QString &name, const QJsonObject &) {
            order.append("tool"); QCOMPARE(name, QStringLiteral("read_project_state"));
            session.replyToTool(token, QJsonObject{{"revision", 12}}, true);
        });
        connect(&session, &AssistantSession::turnFinished, &session, [&](bool ok) { QVERIFY(ok); order.append("finish"); });
        Peer peer; connectPeer(peer, session); QVERIFY(peer.response(1).contains("result")); callTool(peer, 2);
        QTRY_VERIFY(!peer.response(2).isEmpty());
        QCOMPARE(order, QStringList({"start", "tool", "finish"}));
        QVERIFY(!session.isBusy()); QVERIFY(session.isReady());
        const auto result = peer.response(2).value("result").toObject();
        QVERIFY(!result.value("isError").toBool());
        const auto data = QJsonDocument::fromJson(result.value("content").toArray().first().toObject().value("text").toString().toUtf8()).object();
        QCOMPARE(data.value("revision").toInt(), 12);
        peer.disconnectFromServer();
    }

    void externalCancellationDoesNotRecurseAndOldReplyIsIgnored()
    {
        AssistantSession session; session.startExternal(sessionTools(), "Read before editing."); QVERIFY(session.isReady());
        QSignalSpy tools(&session, &AssistantSession::toolRequested), cancelled(&session, &AssistantSession::externalCancelRequested),
            finished(&session, &AssistantSession::turnFinished), errors(&session, &AssistantSession::failed);
        // This mirrors controller's cancellation callback into its stop routine.
        connect(&session, &AssistantSession::externalCancelRequested, &session, &AssistantSession::cancel);
        Peer peer; connectPeer(peer, session); QVERIFY(peer.response(1).contains("result")); callTool(peer, 2);
        QTRY_COMPARE(tools.size(), 1); QVERIFY(session.isBusy());
        const auto oldToken = tools.first().first().value<QJsonValue>();
        peer.send({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", QJsonObject{{"requestId", 2}}}});
        QTRY_COMPARE(cancelled.size(), 1); QTRY_COMPARE(finished.size(), 1); QVERIFY(!session.isBusy());
        QVERIFY(!finished.first().first().toBool());
        callTool(peer, 3); QTRY_COMPARE(tools.size(), 2);
        session.replyToTool(oldToken, QJsonObject{{"revision", 99}}, true);
        QCOMPARE(finished.size(), 1); QVERIFY(session.isBusy());
        session.replyToTool(tools.last().first().value<QJsonValue>(), QJsonObject{{"revision", 13}}, true);
        QTRY_VERIFY(!peer.response(3).isEmpty()); QCOMPARE(finished.size(), 2); QCOMPARE(errors.size(), 0);
        peer.disconnectFromServer();
    }

    void externalDisconnectRequestsJobCancellation()
    {
        AssistantSession session; session.startExternal(sessionTools(), "Read before editing."); QVERIFY(session.isReady());
        QSignalSpy tools(&session, &AssistantSession::toolRequested), cancelled(&session, &AssistantSession::externalCancelRequested),
            finished(&session, &AssistantSession::turnFinished);
        Peer peer; connectPeer(peer, session); QVERIFY(peer.response(1).contains("result")); callTool(peer, 2);
        QTRY_COMPARE(tools.size(), 1); QVERIFY(session.isBusy());
        peer.abort(); QTRY_COMPARE(cancelled.size(), 1); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!session.isBusy()); QVERIFY(!finished.first().first().toBool());
        session.replyToTool(tools.first().first().value<QJsonValue>(), QJsonObject{{"late", true}}, true);
        QCOMPARE(finished.size(), 1);
    }

    void activeCancelDoesNotEchoButLaterPeerCancellationStillForwards()
    {
        AssistantSession session; session.startExternal(sessionTools(), "Read before editing."); QVERIFY(session.isReady());
        QSignalSpy tools(&session, &AssistantSession::toolRequested), cancelled(&session, &AssistantSession::externalCancelRequested),
            finished(&session, &AssistantSession::turnFinished);
        connect(&session, &AssistantSession::externalCancelRequested, &session, &AssistantSession::cancel);
        Peer peer; connectPeer(peer, session); QVERIFY(peer.response(1).contains("result")); callTool(peer, 2);
        QTRY_COMPARE(tools.size(), 1); QVERIFY(session.isBusy());
        session.cancel();
        QTRY_COMPARE(finished.size(), 1); QTRY_VERIFY(!peer.response(2).isEmpty());
        QCOMPARE(peer.response(2).value("error").toObject().value("code").toInt(), -32800);
        QCOMPARE(cancelled.size(), 0);
        QVERIFY(!session.isBusy()); QVERIFY(session.isReady());

        callTool(peer, 3); QTRY_COMPARE(tools.size(), 2); QVERIFY(session.isBusy());
        peer.send({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", QJsonObject{{"requestId", 3}}}});
        QTRY_COMPARE(cancelled.size(), 1); QTRY_COMPARE(finished.size(), 2);
        QVERIFY(!session.isBusy()); QVERIFY(!finished.last().first().toBool());
        session.stop();
    }

    void stoppingActiveToolDoesNotEchoAndNewSessionCanReceiveCancellation()
    {
        AssistantSession session; session.startExternal(sessionTools(), "Read before editing."); QVERIFY(session.isReady());
        QSignalSpy tools(&session, &AssistantSession::toolRequested), cancelled(&session, &AssistantSession::externalCancelRequested),
            finished(&session, &AssistantSession::turnFinished);
        Peer first; connectPeer(first, session); QVERIFY(first.response(1).contains("result")); callTool(first, 2);
        QTRY_COMPARE(tools.size(), 1); QVERIFY(session.isBusy());
        session.stop();
        QTRY_COMPARE(first.state(), QLocalSocket::UnconnectedState);
        QCOMPARE(cancelled.size(), 0); QCOMPARE(finished.size(), 0);
        QVERIFY(!session.isReady()); QVERIFY(!session.isBusy());

        session.startExternal(sessionTools(), "Read before editing.");
        Peer second; connectPeer(second, session); QVERIFY(second.response(1).contains("result")); callTool(second, 2);
        QTRY_COMPARE(tools.size(), 2); QVERIFY(session.isBusy());
        second.send({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", QJsonObject{{"requestId", 2}}}});
        QTRY_COMPARE(cancelled.size(), 1); QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!session.isBusy());
        session.stop();
    }
};

QTEST_GUILESS_MAIN(AssistantSessionTest)
#include "test.moc"
