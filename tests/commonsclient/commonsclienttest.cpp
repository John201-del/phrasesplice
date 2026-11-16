/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "../localcut_test_paths.hpp"
#include "../../src/assistant/commonsclient.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>
#include <cstring>

struct Response {
    QByteArray body;
    QString mime = QStringLiteral("application/json");
    int status = 200;
    QUrl redirect;
    qint64 length = -1;
    bool pause = false;
};
class FakeReply : public QNetworkReply
{
public:
    FakeReply(const QNetworkRequest &request, const Response &response, QObject *parent) : QNetworkReply(parent), m_body(response.body) {
        setRequest(request); setUrl(request.url()); open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.status);
        setHeader(QNetworkRequest::ContentTypeHeader, response.mime);
        setHeader(QNetworkRequest::ContentLengthHeader, response.length < 0 ? m_body.size() : response.length);
        if (response.redirect.isValid()) setAttribute(QNetworkRequest::RedirectionTargetAttribute, response.redirect);
        QTimer::singleShot(0, this, [this, response]() {
            Q_EMIT metaDataChanged(); Q_EMIT readyRead();
            if (!response.pause) { setFinished(true); Q_EMIT finished(); }
        });
    }
    void abort() override { setError(OperationCanceledError, QStringLiteral("canceled")); setFinished(true); Q_EMIT finished(); }
    qint64 bytesAvailable() const override { return m_body.size() - m_offset + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char *data, qint64 size) override {
        const qint64 count = qMin(size, qint64(m_body.size()) - m_offset);
        if (!count) return -1;
        std::memcpy(data, m_body.constData() + m_offset, size_t(count)); m_offset += count; return count;
    }
private:
    QByteArray m_body;
    qint64 m_offset = 0;
};
class FakeNetwork : public QNetworkAccessManager
{
public:
    QList<Response> responses;
    QList<QNetworkRequest> requests;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
        requests.append(request);
        return new FakeReply(request, responses.isEmpty() ? Response{} : responses.takeFirst(), this);
    }
};
static QByteArray imageBytes() {
    QImage image(8, 8, QImage::Format_RGB32); image.fill(Qt::green);
    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG"); return bytes;
}
static QJsonObject page(int id, const QString &license = QStringLiteral("CC BY 4.0")) {
    QJsonObject metadata{{"LicenseShortName", QJsonObject{{"value", license}}},
        {"LicenseUrl", QJsonObject{{"value", "https://creativecommons.org/licenses/by/4.0/"}}},
        {"Artist", QJsonObject{{"value", "<a href='https://example.org'>Example Artist</a>"}}},
        {"Credit", QJsonObject{{"value", "<b>Example collection</b>"}}}};
    if (license == QStringLiteral("CC0")) metadata["LicenseUrl"] = QJsonObject{{"value", "https://creativecommons.org/publicdomain/zero/1.0/"}};
    if (license == QStringLiteral("CC BY-SA 4.0")) metadata["LicenseUrl"] = QJsonObject{{"value", "https://creativecommons.org/licenses/by-sa/4.0/"}};
    if (license == QStringLiteral("Public domain")) { metadata["Copyrighted"] = QJsonObject{{"value", "False"}}; metadata.remove("LicenseUrl"); }
    return {{"pageid", id}, {"title", QStringLiteral("File:Example %1.png").arg(id)},
        {"imageinfo", QJsonArray{QJsonObject{{"url", QStringLiteral("https://upload.wikimedia.org/wikipedia/commons/a/ab/example%1.png").arg(id)},
            {"descriptionurl", QStringLiteral("https://commons.wikimedia.org/wiki/File:Example_%1.png").arg(id)},
            {"mime", "image/png"}, {"width", 8}, {"height", 8}, {"size", 1024}, {"extmetadata", metadata}}}}};
}
static QByteArray searchBody(QJsonArray pages = QJsonArray{page(10)}) {
    return QJsonDocument(QJsonObject{{"query", QJsonObject{{"pages", pages}}}}).toJson(QJsonDocument::Compact);
}
class CommonsClientTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void searchUsesFixedApiAndSanitizesLicenses() {
        QTemporaryDir directory(LocalCutTestPaths::workspace(QStringLiteral("commons-XXXXXX"))); QVERIFY(directory.isValid());
        FakeNetwork network;
        network.responses.append({searchBody({page(10), page(11, "Public domain"), page(12, "All rights reserved")})});
        CommonsClient client(directory.path(), nullptr, &network);
        QSignalSpy results(&client, &CommonsClient::searchFinished);
        QVERIFY(client.search(QStringLiteral("山 & forest")));
        QTRY_COMPARE(results.count(), 1);
        const auto assets = results.first().first().toJsonArray(); QCOMPARE(assets.size(), 2);
        QCOMPARE(assets.first().toObject()["author"].toString(), QStringLiteral("Example Artist"));
        QCOMPARE(assets.first().toObject()["credit"].toString(), QStringLiteral("Example collection"));
        QCOMPARE(assets.first().toObject()["assetId"].toString(), QStringLiteral("10"));
        QCOMPARE(network.requests.first().url().host(), QStringLiteral("commons.wikimedia.org"));
        const QUrlQuery query(network.requests.first().url());
        QCOMPARE(query.queryItemValue("gsrsearch", QUrl::FullyDecoded), QStringLiteral("山 & forest"));
        QCOMPARE(query.queryItemValue("gsrnamespace"), QStringLiteral("6"));
        QVERIFY(network.requests.first().rawHeader("User-Agent").contains("LocalCut"));
    }
    void acceptsEachWhitelistedLicense_data() {
        QTest::addColumn<QString>("license");
        QTest::newRow("PD") << QStringLiteral("Public domain"); QTest::newRow("CC0") << QStringLiteral("CC0");
        QTest::newRow("BY") << QStringLiteral("CC BY 4.0"); QTest::newRow("BY-SA") << QStringLiteral("CC BY-SA 4.0");
    }
    void acceptsEachWhitelistedLicense() {
        QFETCH(QString, license);
        QTemporaryDir directory(LocalCutTestPaths::workspace(QStringLiteral("commons-XXXXXX"))); FakeNetwork network;
        network.responses.append({searchBody({page(10, license)})}); CommonsClient client(directory.path(), nullptr, &network);
        QSignalSpy results(&client, &CommonsClient::searchFinished); QVERIFY(client.search("forest")); QTRY_COMPARE(results.count(), 1);
        QCOMPARE(results.first().first().toJsonArray().size(), 1);
        QCOMPARE(results.first().first().toJsonArray().first().toObject()["licenseName"].toString(), license);
    }
    void filtersUnsafeSourceUrls() {
        QTemporaryDir directory(LocalCutTestPaths::workspace(QStringLiteral("commons-XXXXXX"))); FakeNetwork network;
        auto malicious = page(10); auto info = malicious["imageinfo"].toArray().first().toObject(); info["url"] = "https://evil.example/image.png";
        malicious["imageinfo"] = QJsonArray{info}; network.responses.append({searchBody({malicious})});
        CommonsClient client(directory.path(), nullptr, &network); QSignalSpy results(&client, &CommonsClient::searchFinished);
        QVERIFY(client.search("forest")); QTRY_COMPARE(results.count(), 1); QVERIFY(results.first().first().toJsonArray().isEmpty());
    }
    void downloadsImageAndArchivesAttribution() {
        QTemporaryDir directory(LocalCutTestPaths::workspace(QStringLiteral("commons-XXXXXX"))); QVERIFY(directory.isValid());
        FakeNetwork network; network.responses.append({searchBody()}); network.responses.append({imageBytes(), "image/png"});
        CommonsClient client(directory.path(), nullptr, &network);
        QSignalSpy results(&client, &CommonsClient::searchFinished), downloaded(&client, &CommonsClient::downloaded), failed(&client, &CommonsClient::failed);
        QVERIFY(client.search("forest")); QTRY_COMPARE(results.count(), 1);
        QVERIFY(client.download("10")); QTRY_VERIFY(downloaded.count() || failed.count());
        QVERIFY2(failed.isEmpty(), qPrintable(failed.isEmpty() ? QString() : failed.first().first().toString())); QCOMPARE(downloaded.count(), 1);
        const QString path = downloaded.first().at(0).toString(); QFile media(path); QVERIFY(media.open(QIODevice::ReadOnly)); QCOMPARE(media.readAll(), imageBytes());
        QFile sidecar(path + ".json"); QVERIFY(sidecar.open(QIODevice::ReadOnly));
        const auto metadata = QJsonDocument::fromJson(sidecar.readAll()).object();
        QCOMPARE(metadata["author"].toString(), QStringLiteral("Example Artist"));
        QCOMPARE(metadata["licenseName"].toString(), QStringLiteral("CC BY 4.0"));
        QVERIFY(metadata["sourceUrl"].toString().startsWith("https://commons.wikimedia.org/wiki/File:"));
        QCOMPARE(metadata["sha256"].toString(), QString::fromLatin1(QCryptographicHash::hash(imageBytes(), QCryptographicHash::Sha256).toHex()));
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 2);
    }
    void rejectsUnsearchedIds() {
        QTemporaryDir directory(LocalCutTestPaths::workspace(QStringLiteral("commons-XXXXXX"))); FakeNetwork network;
        CommonsClient client(directory.path(), nullptr, &network); QSignalSpy failed(&client, &CommonsClient::failed);
        QVERIFY(!client.download("https://evil.example/file.png")); QCOMPARE(failed.count(), 1); QCOMPARE(network.requests.size(), 0);
    }
    void rejectsRedirectOversizeAndWrongMime_data() {
        QTest::addColumn<int>("kind"); QTest::newRow("cross-host redirect") << 0; QTest::newRow("100MB exceeded") << 1; QTest::newRow("html instead of image") << 2; QTest::newRow("invalid image bytes") << 3;
    }
    void rejectsRedirectOversizeAndWrongMime() {
        QFETCH(int, kind);
        QTemporaryDir directory(LocalCutTestPaths::workspace(QStringLiteral("commons-XXXXXX"))); FakeNetwork network;
        network.responses.append({searchBody()}); Response response{imageBytes(), "image/png"};
        if (kind == 0) { response.status = 302; response.redirect = QUrl("https://evil.example/image.png"); }
        if (kind == 1) response.length = 100LL * 1024 * 1024 + 1;
        if (kind == 2) { response.mime = "text/html"; response.body = "<html>denied</html>"; }
        if (kind == 3) response.body = "not an image";
        network.responses.append(response); CommonsClient client(directory.path(), nullptr, &network);
        QSignalSpy results(&client, &CommonsClient::searchFinished), failed(&client, &CommonsClient::failed), downloaded(&client, &CommonsClient::downloaded);
        QVERIFY(client.search("forest")); QTRY_COMPARE(results.count(), 1); QVERIFY(client.download("10")); QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(downloaded.count(), 0); QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 0); QCOMPARE(network.requests.size(), 2);
    }
    void cancellationRemovesPartialFile() {
        QTemporaryDir directory(LocalCutTestPaths::workspace(QStringLiteral("commons-XXXXXX"))); FakeNetwork network;
        network.responses.append({searchBody()}); Response response{imageBytes(), "image/png"}; response.pause = true; network.responses.append(response);
        CommonsClient client(directory.path(), nullptr, &network); QSignalSpy results(&client, &CommonsClient::searchFinished), canceled(&client, &CommonsClient::canceled), downloaded(&client, &CommonsClient::downloaded);
        QVERIFY(client.search("forest")); QTRY_COMPARE(results.count(), 1); QVERIFY(client.download("10"));
        QTRY_COMPARE(network.requests.size(), 2); QTest::qWait(10); client.cancel(); QCOMPARE(canceled.count(), 1); QCOMPARE(downloaded.count(), 0);
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 0);
    }
    void liveSearchAndDownload() {
        if (!qEnvironmentVariableIsSet("COMMONS_LIVE")) QSKIP("Explicit live GET opt-in only");
        CommonsClient client(LocalCutTestPaths::workspace(QStringLiteral("commons-live")));
        QSignalSpy results(&client, &CommonsClient::searchFinished), failed(&client, &CommonsClient::failed), downloaded(&client, &CommonsClient::downloaded);
        QVERIFY(client.search(QStringLiteral("intitle:\"Rembrandt van Rijn - Self-Portrait - Google Art ProjectFXD\"")));
        QTRY_VERIFY_WITH_TIMEOUT(results.count() || failed.count(), 60000);
        QVERIFY2(failed.isEmpty(), "Real Commons search failed; inspect fixed code in signal");
        const auto assets = results.first().first().toJsonArray();
        QVERIFY2(!assets.isEmpty(), "No licensed image returned from real Commons API");
        QVERIFY(client.download(assets.first().toObject()["assetId"].toString()));
        QTRY_VERIFY_WITH_TIMEOUT(downloaded.count() || failed.count(), 120000);
        QVERIFY2(failed.isEmpty(), "Real Commons download failed; do not report online success");
        QVERIFY(QFile::exists(downloaded.first().at(0).toString()));
        qInfo().noquote() << "COMMONS_LIVE_RESULT" << QJsonDocument(downloaded.first().at(1).toJsonObject()).toJson(QJsonDocument::Compact);
    }
    void malformedSearchFails() {
        QTemporaryDir directory(LocalCutTestPaths::workspace(QStringLiteral("commons-XXXXXX"))); FakeNetwork network;
        network.responses.append({"not json"}); CommonsClient client(directory.path(), nullptr, &network); QSignalSpy failed(&client, &CommonsClient::failed);
        QVERIFY(client.search("forest")); QTRY_COMPARE(failed.count(), 1);
    }
};
QTEST_MAIN(CommonsClientTest)
#include "commonsclienttest.moc"
