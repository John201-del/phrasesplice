/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../localcut_test_paths.hpp"
#include "../../src/assistant/freespeechclient.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <cstring>

namespace {
const QString host = QStringLiteral("https://k2-fsa-text-to-speech.hf.space");
const QString model = QStringLiteral("csukuangfj/vits-zh-aishell3|174 speakers");
const QString successHtml = QStringLiteral("<div class='result_item result_item_success'>Wave duration : 4.844 s</div>");
struct Response { QByteArray bytes; int status{200}; QUrl redirect; bool hold{false}; qint64 declaredSize{-1}; };
class Reply : public QNetworkReply
{
public:
    Reply(const QNetworkRequest &request, const Response &response, QObject *parent) : QNetworkReply(parent), m_bytes(response.bytes)
    {
        setRequest(request); setUrl(request.url()); open(ReadOnly | Unbuffered);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.status);
        if (!response.redirect.isEmpty()) setAttribute(QNetworkRequest::RedirectionTargetAttribute, response.redirect);
        if (response.declaredSize >= 0) setHeader(QNetworkRequest::ContentLengthHeader, response.declaredSize);
        if (!response.hold) QTimer::singleShot(0, this, [this] { Q_EMIT metaDataChanged(); Q_EMIT readyRead(); setFinished(true); Q_EMIT finished(); });
    }
    void abort() override { setError(OperationCanceledError, QString()); setFinished(true); Q_EMIT finished(); }
    qint64 bytesAvailable() const override { return m_bytes.size() - m_offset + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char *data, qint64 max) override {
        const auto amount = qMin(max, qint64(m_bytes.size()) - m_offset);
        if (amount <= 0) return -1;
        memcpy(data, m_bytes.constData() + m_offset, size_t(amount)); m_offset += amount; return amount;
    }
private:
    QByteArray m_bytes; qint64 m_offset{0};
};
class Network : public QNetworkAccessManager
{
public:
    QList<Response> responses;
    QList<QNetworkRequest> requests;
    QList<QByteArray> bodies;
    QList<Operation> operations;
protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *body) override {
        requests << request; operations << operation; bodies << (body ? body->readAll() : QByteArray());
        return new Reply(request, responses.isEmpty() ? Response{{}, 500} : responses.takeFirst(), this);
    }
};
QByteArray sse(const QJsonArray &data) { return "event: complete\ndata: " + QJsonDocument(data).toJson(QJsonDocument::Compact) + "\n\n"; }
QList<Response> successful(const QByteArray &wav) {
    return {{"{\"event_id\":\"init-event\"}"}, {sse({QJsonObject{{"choices", QJsonArray{QJsonArray{model,model}}}, {"__type__", "update"}}})},
            {"{\"event_id\":\"speech-event\"}"}, {sse({QJsonObject{{"url", host + "/gradio_api/file=/tmp/gradio/result/audio.wav"}, {"path", "/tmp/gradio/result/audio.wav"}}, successHtml})}, {wav}};
}
}

class FreeSpeechClientTest : public QObject
{
    Q_OBJECT
    QString m_ffprobe;
    QByteArray m_wav;
private Q_SLOTS:
    void initTestCase() {
        m_ffprobe = LocalCutTestPaths::executable("LOCALCUT_TEST_FFPROBE", QStringLiteral("ffprobe"));
        QVERIFY(QFile::exists(m_ffprobe));
        QFile wav(LocalCutTestPaths::fixture(QStringLiteral("local-extension-20261107/cloud/k2-cloud-test.wav")));
        QVERIFY(wav.open(QIODevice::ReadOnly)); m_wav = wav.readAll(); QVERIFY(m_wav.startsWith("RIFF"));
    }
    void initializesSameSessionAndValidatesWav() {
        QTemporaryDir directory(LocalCutTestPaths::workspace(QStringLiteral("speech-XXXXXX"))); QVERIFY(directory.isValid());
        Network network; network.responses = successful(m_wav);
        FreeSpeechClient client(directory.path(), m_ffprobe, nullptr, &network);
        QSignalSpy ready(&client, &FreeSpeechClient::ready), failed(&client, &FreeSpeechClient::failed);
        client.generate(QStringLiteral("这是一段测试配音。"));
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 10000);
        QCOMPARE(failed.size(), 0); QVERIFY(!client.isBusy()); QCOMPARE(network.requests.size(), 5);
        const auto initial = QJsonDocument::fromJson(network.bodies[0]).object();
        const auto generation = QJsonDocument::fromJson(network.bodies[2]).object();
        QVERIFY(network.requests[0].url().path().endsWith("/update_model_dropdown"));
        QCOMPARE(initial["data"].toArray(), QJsonArray{QStringLiteral("Chinese (Mandarin, 普通话)")});
        QCOMPARE(initial["session_hash"], generation["session_hash"]);
        const auto session = initial["session_hash"].toString(); QVERIFY(!session.isEmpty());
        QVERIFY(network.requests[1].url().path().endsWith('/' + session));
        QVERIFY(network.requests[3].url().path().endsWith('/' + session));
        QCOMPARE(generation["data"].toArray(), (QJsonArray{QStringLiteral("Chinese (Mandarin, 普通话)"), model, QStringLiteral("这是一段测试配音。"), "0", 1}));
        QFile output(ready[0][0].toString()); QVERIFY(output.open(QIODevice::ReadOnly)); QCOMPARE(output.readAll(), m_wav);
        QVERIFY(output.fileName().startsWith(directory.path() + '/'));
    }
    void rejectsInvalidTextWithoutNetwork() {
        Network network; FreeSpeechClient client(LocalCutTestPaths::workspace(), m_ffprobe, nullptr, &network);
        QSignalSpy failed(&client, &FreeSpeechClient::failed);
        client.generate(QString()); client.generate(QString(201, QLatin1Char('a')));
        QCOMPARE(failed.size(), 2); QVERIFY(network.requests.isEmpty());
    }
    void rejectsDirectoryOutsideWorkspace() {
        Network network; FreeSpeechClient client(LocalCutTestPaths::workspace() + QStringLiteral("-outside/speech"), m_ffprobe, nullptr, &network);
        QSignalSpy failed(&client, &FreeSpeechClient::failed); client.generate(QStringLiteral("测试"));
        QCOMPARE(failed.size(),1); QVERIFY(network.requests.isEmpty());
    }
    void rejectsMissingModelBeforeGeneration() {
        Network network; network.responses=successful(m_wav);
        network.responses[1].bytes=sse({QJsonObject{{"choices", QJsonArray{QJsonArray{"different", "different"}}}}});
        FreeSpeechClient client(LocalCutTestPaths::workspace(),m_ffprobe,nullptr,&network);
        QSignalSpy failed(&client,&FreeSpeechClient::failed); client.generate(QStringLiteral("测试"));
        QTRY_COMPARE(failed.size(),1); QCOMPARE(network.operations.count(QNetworkAccessManager::PostOperation),1);
    }
    void rejectsProviderErrorHtml() {
        QTemporaryDir directory(LocalCutTestPaths::workspace(QStringLiteral("speech-XXXXXX")));
        Network network; network.responses=successful(m_wav);
        network.responses[3].bytes=sse({QJsonObject{{"url",host + "/gradio_api/file=/tmp/gradio/result/audio.wav"}}, "<div class='result_item_error'>private error</div>"});
        FreeSpeechClient client(directory.path(),m_ffprobe,nullptr,&network);
        QSignalSpy failed(&client,&FreeSpeechClient::failed); client.generate(QStringLiteral("测试"));
        QTRY_COMPARE(failed.size(),1); QCOMPARE(network.requests.size(),4);
        QVERIFY(!failed[0][0].toString().contains("private"));
    }
    void rejectsProviderNonAudioMime() {
        Network network; network.responses=successful(m_wav);
        network.responses[3].bytes=sse({QJsonObject{{"url",host + "/gradio_api/file=/tmp/gradio/result/audio.wav"}, {"mime_type","text/html"}}, successHtml});
        FreeSpeechClient client(LocalCutTestPaths::workspace(),m_ffprobe,nullptr,&network);
        QSignalSpy failed(&client,&FreeSpeechClient::failed); client.generate(QStringLiteral("测试"));
        QTRY_COMPARE(failed.size(),1); QCOMPARE(network.requests.size(),4);
    }
    void serviceFailureNeverResubmits_data() {
        QTest::addColumn<int>("status"); QTest::addColumn<QByteArray>("terminal");
        QTest::newRow("quota") << 429 << QByteArray();
        QTest::newRow("forbidden") << 403 << QByteArray();
        QTest::newRow("service-error") << 200 << QByteArray("event: error\ndata: null\n\n");
        QTest::newRow("disconnected") << 200 << QByteArray("event: generating\ndata: null\n\n");
    }
    void serviceFailureNeverResubmits() {
        QFETCH(int,status); QFETCH(QByteArray,terminal);
        QTemporaryDir directory(LocalCutTestPaths::workspace(QStringLiteral("speech-XXXXXX")));
        Network network; network.responses = successful(m_wav);
        network.responses[2].status = status; network.responses[3].bytes = terminal;
        FreeSpeechClient client(directory.path(),m_ffprobe,nullptr,&network); QSignalSpy failed(&client,&FreeSpeechClient::failed), ready(&client,&FreeSpeechClient::ready);
        client.generate(QStringLiteral("测试")); QTRY_COMPARE(failed.size(),1); QTest::qWait(30);
        QCOMPARE(ready.size(),0); QCOMPARE(network.operations.count(QNetworkAccessManager::PostOperation),2);
        QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }
    void rejectsUnsafeOutput_data() {
        QTest::addColumn<QString>("url"); QTest::addColumn<QByteArray>("bytes"); QTest::addColumn<int>("status"); QTest::addColumn<qint64>("size");
        const auto valid = host + "/gradio_api/file=/tmp/gradio/result/audio.wav";
        QTest::newRow("cross-host") << QStringLiteral("https://example.com/audio.wav") << m_wav << 200 << qint64(-1);
        QTest::newRow("hint") << host + "/gradio_api/file=/tmp/gradio/hint.wav" << m_wav << 200 << qint64(-1);
        QFile hint(LocalCutTestPaths::fixture(QStringLiteral("local-extension-20261107/cloud/k2-official-hint.wav")));
        QVERIFY(hint.open(QIODevice::ReadOnly));
        QTest::newRow("hint-disguised-name") << valid << hint.readAll() << 200 << qint64(-1);
        QTest::newRow("html") << valid << QByteArray("<html>maintenance</html>") << 200 << qint64(-1);
        QTest::newRow("too-large") << valid << m_wav << 200 << qint64(21 * 1024 * 1024);
        QTest::newRow("oversized-body") << valid << QByteArray(20 * 1024 * 1024 + 1, 'x') << 200 << qint64(-1);
        QTest::newRow("redirect") << valid << m_wav << 302 << qint64(-1);
    }
    void rejectsUnsafeOutput() {
        QFETCH(QString,url); QFETCH(QByteArray,bytes); QFETCH(int,status); QFETCH(qint64,size);
        QTemporaryDir directory(LocalCutTestPaths::workspace(QStringLiteral("speech-XXXXXX")));
        Network network; network.responses=successful(bytes);
        network.responses[3].bytes=sse({QJsonObject{{"url",url},{"path","/tmp/gradio/result/audio.wav"}}, successHtml});
        network.responses[4].status=status; network.responses[4].declaredSize=size;
        if(status==302) network.responses[4].redirect=QUrl(QStringLiteral("https://example.com/audio.wav"));
        FreeSpeechClient client(directory.path(),m_ffprobe,nullptr,&network); QSignalSpy failed(&client,&FreeSpeechClient::failed), ready(&client,&FreeSpeechClient::ready);
        client.generate(QStringLiteral("测试")); QTRY_COMPARE_WITH_TIMEOUT(failed.size(),1,10000); QCOMPARE(ready.size(),0);
        QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }
    void cancellationDoesNotContaminateNextRequest() {
        QTemporaryDir directory(LocalCutTestPaths::workspace(QStringLiteral("speech-XXXXXX")));
        Network network; network.responses={{"",200,{},true}};
        FreeSpeechClient client(directory.path(),m_ffprobe,nullptr,&network); QSignalSpy failed(&client,&FreeSpeechClient::failed), ready(&client,&FreeSpeechClient::ready);
        client.generate(QStringLiteral("取消测试")); QVERIFY(client.isBusy()); client.cancel(); QVERIFY(!client.isBusy());
        network.responses=successful(m_wav); client.generate(QStringLiteral("下一条测试"));
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,10000); QCOMPARE(failed.size(),1);
        QCOMPARE(QDir(directory.path()).entryList({"*.wav"},QDir::Files).size(),1);
    }
    void cancellationFromProgressDoesNotSubmit() {
        QTemporaryDir directory(LocalCutTestPaths::workspace(QStringLiteral("speech-XXXXXX")));
        Network network; FreeSpeechClient client(directory.path(),m_ffprobe,nullptr,&network);
        QSignalSpy failed(&client,&FreeSpeechClient::failed);
        connect(&client,&FreeSpeechClient::progress,&client,&FreeSpeechClient::cancel);
        client.generate(QStringLiteral("测试")); QCOMPARE(failed.size(),1); QVERIFY(!client.isBusy());
        QVERIFY(network.requests.isEmpty());
    }
    void cancellationWhileDownloadingRemovesPartial() {
        QTemporaryDir directory(LocalCutTestPaths::workspace(QStringLiteral("speech-XXXXXX")));
        Network network; network.responses=successful(m_wav); network.responses[4].hold=true;
        FreeSpeechClient client(directory.path(),m_ffprobe,nullptr,&network); QSignalSpy ready(&client,&FreeSpeechClient::ready);
        client.generate(QStringLiteral("测试")); QTRY_COMPARE(network.requests.size(),5); client.cancel(); QTest::qWait(20);
        QVERIFY(!client.isBusy()); QCOMPARE(ready.size(),0);
        QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }
};
QTEST_GUILESS_MAIN(FreeSpeechClientTest)
#include "freespeechclienttest.moc"
