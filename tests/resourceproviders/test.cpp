#include "../../src/onlineresources/providermodel.hpp"
#include "kdenlive_debug.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

Q_LOGGING_CATEGORY(KDENLIVE_LOG, "localcut.tests.resourceproviders")

class ResourceProviderTest : public QObject
{
    Q_OBJECT
    QMap<QByteArray, QByteArray> previous;
    QString base;
    const QList<QByteArray> variables{"LOCALCUT_FREESOUND_API_KEY", "LOCALCUT_PEXELS_API_KEY", "LOCALCUT_PIXABAY_API_KEY"};
    QString profile(QTemporaryDir &directory, quint16 port, const QString &key)
    {
        const QJsonObject request{{"path", "/search"}, {"method", "GET"},
            {"header", QJsonArray{QJsonObject{{"key", "Authorization"}, {"value", "%clientkey%"}}}}};
        const QJsonObject operation{{"req", request}, {"res", QJsonObject{{"format", "json"}, {"list", "items"}, {"resultCount", "count"}}}};
        const QJsonObject object{{"name", "Test provider"}, {"homepage", "https://example.invalid"}, {"type", "image"},
            {"integration", "buildin"}, {"clientkey", key}, {"api", QJsonObject{{"root", QStringLiteral("http://127.0.0.1:%1").arg(port)},
            {"search", operation}, {"downloadUrls", operation}}}};
        const QString path = directory.filePath(QStringLiteral("provider.json"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return {};
        file.write(QJsonDocument(object).toJson());
        return path;
    }
private Q_SLOTS:
    void initTestCase()
    {
        base = qEnvironmentVariable("LOCALCUT_WORKSPACE_ROOT", QCoreApplication::applicationDirPath()) + QStringLiteral("/resource-provider-tests");
        QVERIFY(QDir().mkpath(base));
    }
    void init()
    {
        for (const auto &name : variables) {
            if (qEnvironmentVariableIsSet(name.constData())) previous.insert(name, qgetenv(name.constData()));
            qunsetenv(name.constData());
        }
    }
    void cleanup()
    {
        for (const auto &name : variables) {
            if (previous.contains(name)) qputenv(name.constData(), previous.value(name));
            else qunsetenv(name.constData());
        }
        previous.clear();
    }
    void missingKey_data()
    {
        QTest::addColumn<QString>("placeholder");
        QTest::addColumn<QByteArray>("variable");
        QTest::newRow("freesound") << QStringLiteral("%freesound_apikey%") << variables[0];
        QTest::newRow("pexels") << QStringLiteral("%pexels_apikey%") << variables[1];
        QTest::newRow("pixabay") << QStringLiteral("%pixabay_apikey%") << variables[2];
    }
    void missingKey()
    {
        QFETCH(QString, placeholder);
        QFETCH(QByteArray, variable);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir directory(base + QStringLiteral("/case-XXXXXX"));
        QVERIFY(directory.isValid());
        ProviderModel provider(profile(directory, server.serverPort(), placeholder));
        QVERIFY(provider.is_valid());
        QSignalSpy errors(&provider, &ProviderModel::searchError);
        QSignalSpy connections(&server, &QTcpServer::newConnection);
        provider.slotStartSearch(QStringLiteral("test"), 1);
        QCOMPARE(errors.count(), 1);
        QVERIFY(errors[0][0].toString().contains(QString::fromLatin1(variable)));
        provider.slotFetchFiles(QStringLiteral("1"));
        provider.authorize();
        provider.refreshAccessToken();
        QCOMPARE(errors.count(), 4);
        QTest::qWait(20);
        QCOMPARE(connections.count(), 0);
    }
    void usesRecipientKey_data()
    {
        missingKey_data();
    }
    void usesRecipientKey()
    {
        QFETCH(QString, placeholder);
        QFETCH(QByteArray, variable);
        qputenv(variable.constData(), "test-resource-key");
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir directory(base + QStringLiteral("/case-XXXXXX"));
        QVERIFY(directory.isValid());
        ProviderModel provider(profile(directory, server.serverPort(), placeholder));
        QSignalSpy errors(&provider, &ProviderModel::searchError);
        provider.slotStartSearch(QStringLiteral("test"), 1);
        QTRY_VERIFY(server.hasPendingConnections());
        auto *socket = server.nextPendingConnection();
        QByteArray request;
        QTRY_VERIFY((request += socket->readAll()).contains("\r\n\r\n"));
        // Boolean assertions deliberately never print credential-bearing headers.
        QVERIFY(request.contains("Authorization: test-resource-key\r\n"));
        QVERIFY(!request.contains("%clientkey%"));
        QCOMPARE(errors.count(), 0);
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 22\r\nConnection: close\r\n\r\n{\"items\":[],\"count\":0}");
        socket->disconnectFromHost();
    }
    void preservesCustomAndKeylessProfiles_data()
    {
        QTest::addColumn<QString>("key");
        QTest::newRow("custom-key") << QStringLiteral("test-private-profile-key");
        QTest::newRow("keyless") << QString();
    }
    void preservesCustomAndKeylessProfiles()
    {
        QFETCH(QString, key);
        qputenv("LOCALCUT_PEXELS_API_KEY", "test-environment-key");
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTemporaryDir directory(base + QStringLiteral("/case-XXXXXX"));
        QVERIFY(directory.isValid());
        ProviderModel provider(profile(directory, server.serverPort(), key));
        QSignalSpy errors(&provider, &ProviderModel::searchError);
        provider.slotStartSearch(QStringLiteral("test"), 1);
        QTRY_VERIFY(server.hasPendingConnections());
        auto *socket = server.nextPendingConnection();
        QByteArray request;
        QTRY_VERIFY((request += socket->readAll()).contains("\r\n\r\n"));
        if (!key.isEmpty()) QVERIFY(request.contains(key.toUtf8()));
        QVERIFY(!request.contains("test-environment-key"));
        QCOMPARE(errors.count(), 0);
    }
};
QTEST_GUILESS_MAIN(ResourceProviderTest)
#include "test.moc"
