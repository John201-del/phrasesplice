#include "../../src/assistant/codexclient.h"
#include "../../src/assistant/workspacepaths.h"
#include <QDir>
#include <QSignalSpy>
#include <QtTest>

class CodexClientTest : public QObject
{
    Q_OBJECT
    void startPeer(CodexClient &client, const QString &scenario, const QString &model = QStringLiteral("offline-model"))
    {
        const QJsonArray tools{QJsonObject{{"type", "function"}, {"name", "timeline.read"},
            {"description", "Read timeline"}, {"inputSchema", QJsonObject{{"type", "object"}}}}};
        client.start(QString::fromUtf8(PYTHON_PATH), WorkspacePaths::root(), tools,
            QStringLiteral("仅允许剪辑工具"), model, {QString::fromUtf8(FAKE_CODEX_PATH), scenario});
    }
private Q_SLOTS:
    void missingModelUsesVisibleCatalogDefault_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::newRow("single-page") << "catalog-default";
        QTest::newRow("paginated") << "catalog-pages";
    }
    void missingModelUsesVisibleCatalogDefault()
    {
        QFETCH(QString, scenario);
        CodexClient client;
        QSignalSpy ready(&client, &CodexClient::ready), failed(&client, &CodexClient::failed);
        QSignalSpy done(&client, &CodexClient::turnFinished);
        startPeer(client, scenario, {});
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(failed.count(), 0);
        client.submit(QStringLiteral("剪辑请求"));
        QTRY_COMPARE(done.count(), 1);
        QVERIFY(done.first().first().toBool());
    }
    void unusableModelCatalogFailsBeforeReady_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::newRow("empty") << "catalog-empty";
        QTest::newRow("ambiguous-default") << "catalog-ambiguous";
        QTest::newRow("repeated-cursor") << "catalog-loop";
    }
    void unusableModelCatalogFailsBeforeReady()
    {
        QFETCH(QString, scenario);
        CodexClient client;
        QSignalSpy ready(&client, &CodexClient::ready), failed(&client, &CodexClient::failed);
        startPeer(client, scenario, {});
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(ready.count(), 0);
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("可用模型")));
        QVERIFY(!client.isReady());
    }
    void failedTurnHasSafeActionableCategory_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<QString>("hint");
        QTest::newRow("model") << "turn-error-model" << QStringLiteral("模型");
        QTest::newRow("quota") << "turn-error-quota" << QStringLiteral("额度");
        QTest::newRow("auth") << "turn-error-auth" << QStringLiteral("登录");
        QTest::newRow("http-quota") << "turn-error-http-quota" << QStringLiteral("额度");
        QTest::newRow("network") << "turn-error-network" << QStringLiteral("网络");
        QTest::newRow("context") << "turn-error-context" << QStringLiteral("上下文");
        QTest::newRow("overload") << "turn-error-overload" << QStringLiteral("繁忙");
        QTest::newRow("unknown") << "turn-error-unknown" << QStringLiteral("处理失败");
    }
    void failedTurnHasSafeActionableCategory()
    {
        QFETCH(QString, scenario);
        QFETCH(QString, hint);
        CodexClient client;
        QSignalSpy ready(&client, &CodexClient::ready), failed(&client, &CodexClient::failed);
        QSignalSpy done(&client, &CodexClient::turnFinished);
        startPeer(client, scenario);
        QTRY_COMPARE(ready.count(), 1);
        client.submit(QStringLiteral("剪辑请求"));
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(done.count(), 1);
        QVERIFY(!done.first().first().toBool());
        const QString message = failed.first().first().toString();
        QVERIFY(message.contains(hint));
        QVERIFY(!message.contains(QStringLiteral("private-token")));
        QVERIFY(!message.contains(QStringLiteral("personal prompt")));
        QVERIFY(!client.isBusy());
        QVERIFY(!client.isReady());
    }
    void handshakeAndFragmentedChinese()
    {
        CodexClient client;
        QSignalSpy ready(&client, &CodexClient::ready), text(&client, &CodexClient::textDelta);
        QSignalSpy done(&client, &CodexClient::turnFinished), failed(&client, &CodexClient::failed);
        startPeer(client, "text");
        QTRY_COMPARE(ready.count(), 1);
        QVERIFY(client.isReady());
        client.submit(QStringLiteral("剪辑请求"));
        QVERIFY(client.isBusy());
        QTRY_COMPARE(done.count(), 1);
        QCOMPARE(text.at(0).at(0).toString(), QStringLiteral("你好，剪辑"));
        QCOMPARE(done.at(0).at(0).toBool(), true);
        QTest::qWait(60);
        QCOMPARE(done.count(), 1);
        QCOMPARE(failed.count(), 0);
        QVERIFY(!client.isBusy());
    }
    void toolRoundTrip()
    {
        CodexClient client;
        QSignalSpy ready(&client, &CodexClient::ready), tool(&client, &CodexClient::toolRequested);
        QSignalSpy done(&client, &CodexClient::turnFinished);
        startPeer(client, "tool");
        QTRY_COMPARE(ready.count(), 1);
        client.submit(QStringLiteral("剪辑请求"));
        QTRY_COMPARE(tool.count(), 1);
        QCOMPARE(tool.at(0).at(1).toString(), "timeline.read");
        QCOMPARE(qvariant_cast<QJsonObject>(tool.at(0).at(2)).value(QStringLiteral("中文")).toString(), QStringLiteral("值"));
        client.replyToTool(qvariant_cast<QJsonValue>(tool.at(0).at(0)), QJsonObject{{"ok", true}}, true);
        QTRY_COMPARE(done.count(), 1);
        QVERIFY(done.at(0).at(0).toBool());
    }
    void toolImageRoundTrip()
    {
        CodexClient client;
        QSignalSpy ready(&client, &CodexClient::ready), tool(&client, &CodexClient::toolRequested);
        QSignalSpy done(&client, &CodexClient::turnFinished), failed(&client, &CodexClient::failed);
        startPeer(client, "tool-image");
        QTRY_COMPARE(ready.count(), 1);
        client.submit(QStringLiteral("剪辑请求"));
        QTRY_COMPARE(tool.count(), 1);
        const auto id = qvariant_cast<QJsonValue>(tool.at(0).at(0));
        client.replyToTool(id, QJsonArray{QJsonObject{{"type", "unsupported"}}}, true);
        QCOMPARE(failed.count(), 1);
        client.replyToTool(id, QJsonArray{QJsonObject{{"type", "inputImage"},
            {"imageUrl", "data:image/png;base64,AA=="}}}, true);
        QTRY_COMPARE(done.count(), 1);
        QVERIFY(done.at(0).at(0).toBool());
    }
    void visibleRejections()
    {
        CodexClient client;
        QSignalSpy failed(&client, &CodexClient::failed), ready(&client, &CodexClient::ready);
        client.submit("before ready");
        QCOMPARE(failed.count(), 1);
        startPeer(client, "hold");
        QTRY_COMPARE(ready.count(), 1);
        client.submit(QStringLiteral("剪辑请求"));
        client.submit("parallel");
        QCOMPARE(failed.count(), 2);
        QVERIFY(client.isBusy());
    }
    void cancellation_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::newRow("known-turn") << "cancel";
        QTest::newRow("pending-start") << "cancel-pending";
    }
    void cancellation()
    {
        QFETCH(QString, scenario);
        CodexClient client;
        QSignalSpy ready(&client, &CodexClient::ready), done(&client, &CodexClient::turnFinished);
        startPeer(client, scenario);
        QTRY_COMPARE(ready.count(), 1);
        client.submit(QStringLiteral("剪辑请求"));
        client.cancel();
        QTRY_COMPARE(done.count(), 1);
        QVERIFY(!done.at(0).at(0).toBool());
        QVERIFY(!client.isBusy());
    }
    void cancellationRecoversWhenCompletionNeverArrives()
    {
        CodexClient client;
        QSignalSpy ready(&client, &CodexClient::ready), done(&client, &CodexClient::turnFinished);
        QSignalSpy failed(&client, &CodexClient::failed);
        startPeer(client, "cancel-no-completion");
        QTRY_COMPARE(ready.count(), 1);
        client.submit(QStringLiteral("剪辑请求"));
        client.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 7000);
        QVERIFY(!done.at(0).at(0).toBool());
        QVERIFY(!client.isBusy());
        QVERIFY(!client.isReady());
        QCOMPARE(failed.count(), 1);
        startPeer(client, "text");
        QTRY_COMPARE(ready.count(), 2);
        client.submit(QStringLiteral("剪辑请求"));
        QTRY_COMPARE(done.count(), 2);
        QVERIFY(done.at(1).at(0).toBool());
    }
    void processFailure_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::newRow("exit") << "exit";
        QTest::newRow("rpc-error") << "error";
        QTest::newRow("malformed-json") << "malformed";
        QTest::newRow("unknown-request") << "unknown";
    }
    void processFailure()
    {
        QFETCH(QString, scenario);
        CodexClient client;
        QSignalSpy ready(&client, &CodexClient::ready), done(&client, &CodexClient::turnFinished);
        QSignalSpy failed(&client, &CodexClient::failed);
        startPeer(client, scenario);
        QTRY_COMPARE(ready.count(), 1);
        client.submit(QStringLiteral("剪辑请求"));
        QTRY_COMPARE(done.count(), 1);
        QVERIFY(!done.at(0).at(0).toBool());
        QVERIFY(!client.isBusy());
        QTRY_VERIFY(!failed.isEmpty());
        for (const auto &event : failed) {
            QVERIFY(!event.at(0).toString().contains("private-token"));
            QVERIFY(!event.at(0).toString().contains("secret personal"));
            QVERIFY(!event.at(0).toString().contains(QStringLiteral("个人提示词")));
        }
    }
    void isolationFailsClosed()
    {
        CodexClient client;
        QSignalSpy ready(&client, &CodexClient::ready), failed(&client, &CodexClient::failed);
        startPeer(client, "unsafe");
        QTRY_VERIFY(!failed.isEmpty());
        QCOMPARE(ready.count(), 0);
        QVERIFY(!client.isReady());
    }
    void inheritedMcpIsDisabledBeforeThread()
    {
        CodexClient client;
        QSignalSpy ready(&client, &CodexClient::ready), failed(&client, &CodexClient::failed);
        startPeer(client, "inherited");
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(failed.count(), 0);
    }
    void stopClearsOldCallbacks()
    {
        CodexClient client;
        QSignalSpy ready(&client, &CodexClient::ready), done(&client, &CodexClient::turnFinished);
        QSignalSpy tool(&client, &CodexClient::toolRequested), failed(&client, &CodexClient::failed);
        startPeer(client, "tool");
        QTRY_COMPARE(ready.count(), 1);
        client.submit(QStringLiteral("剪辑请求"));
        QTRY_COMPARE(tool.count(), 1);
        const auto oldId = qvariant_cast<QJsonValue>(tool.at(0).at(0));
        client.stop();
        QVERIFY(!client.isReady());
        QVERIFY(!client.isBusy());
        QCOMPARE(done.count(), 1);
        startPeer(client, "text");
        QTRY_COMPARE(ready.count(), 2);
        client.replyToTool(oldId, QJsonObject{{"ok", true}}, true);
        QVERIFY(!failed.isEmpty());
        client.submit(QStringLiteral("剪辑请求"));
        QTRY_COMPARE(done.count(), 2);
        QVERIFY(done.at(1).at(0).toBool());
    }
    void reusedServerIdsDoNotAcceptOldToolReplies()
    {
        CodexClient client;
        QSignalSpy ready(&client, &CodexClient::ready), tool(&client, &CodexClient::toolRequested);
        QSignalSpy done(&client, &CodexClient::turnFinished), failed(&client, &CodexClient::failed);
        startPeer(client, "tool");
        QTRY_COMPARE(ready.count(), 1);
        client.submit(QStringLiteral("剪辑请求"));
        QTRY_COMPARE(tool.count(), 1);
        const auto oldId = qvariant_cast<QJsonValue>(tool.at(0).at(0));
        client.stop();
        startPeer(client, "tool");
        QTRY_COMPARE(ready.count(), 2);
        client.submit(QStringLiteral("剪辑请求"));
        QTRY_COMPARE(tool.count(), 2);
        const auto newId = qvariant_cast<QJsonValue>(tool.at(1).at(0));
        QVERIFY(oldId != newId);
        client.replyToTool(oldId, QJsonObject{{"ok", true}}, true);
        QCOMPARE(failed.count(), 1);
        QVERIFY(client.isBusy());
        client.replyToTool(newId, QJsonObject{{"ok", true}}, true);
        QTRY_COMPARE(done.count(), 2);
        QVERIFY(done.at(1).at(0).toBool());
    }
};
QTEST_GUILESS_MAIN(CodexClientTest)
#include "codexclienttest.moc"
