/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "localcut_test_paths.hpp"
#include "assistant/assistantcontroller.h"
#include "assistant/assistantpanel.h"
#include "assistant/assistantsession.h"
#include "assistant/assistantconnectiondialog.h"
#include "assistant/mediatasks.h"
#include "assistant/localmediaprocessor.h"
#include "assistant/editbridge.h"
#include "assistant/videogenerationclient.h"
#include "assistant/videogenerationprovider.h"
#include "bin/projectfolder.h"
#include "test_utils.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QProcess>
#include <QStandardPaths>

class VideoGenerationClientTest
{
public:
    static void useLocalEndpoint(VideoGenerationClient *client)
    {
        client->m_apiBase = QUrl(QStringLiteral("http://127.0.0.1:1/api/v3"));
    }

    static void setProviderFactory(VideoGenerationClient *client, std::function<std::unique_ptr<VideoGenerationProvider>()> factory)
    {
        client->m_providerFactory = std::move(factory);
    }
};

class AssistantControllerTest
{
public:
    static bool acceptsImportedAssetLoad(const QJsonObject &before, const QJsonObject &after, const QStringList &ids)
    {
        return AssistantController::acceptsImportedAssetLoad(before, after, ids);
    }
};

namespace {
struct ProviderCapture {
    QString credential;
    VideoGenerationRequest request;
    bool created{false};
};

class CapturingVideoProvider : public VideoGenerationProvider
{
public:
    explicit CapturingVideoProvider(ProviderCapture *capture)
        : m_capture(capture)
    {
    }

    QString id() const override { return QStringLiteral("ltx-space"); }
    bool requiresCredential() const override { return false; }
    bool supportsResume() const override { return false; }
    bool configure(const QString &credential) override
    {
        m_capture->credential = credential;
        return credential.isEmpty();
    }
    void create(const VideoGenerationRequest &request) override
    {
        m_capture->request = request;
        m_capture->created = true;
        Q_EMIT failed(QStringLiteral("captured without network"));
    }
    void cancelOrStopWaiting() override {}

private:
    ProviderCapture *m_capture;
};

bool waitFor(const std::function<bool()> &condition, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    return condition();
}

TEST_CASE("Ended Codex turns cancel in-flight media analysis", "[.][AssistantLifecycle]")
{
    AssistantPanel panel;
    AssistantController controller(&panel);
    auto *client = controller.findChild<AssistantSession *>();
    auto *media = controller.findChild<MediaTasks *>();
    REQUIRE(client);
    REQUIRE(media);
    media->configure(LocalCutTestPaths::executable("LOCALCUT_TEST_FFMPEG", QStringLiteral("ffmpeg")),
                     LocalCutTestPaths::executable("LOCALCUT_TEST_FFPROBE", QStringLiteral("ffprobe")), {}, {},
                     LocalCutTestPaths::workspace(QStringLiteral("lifecycle-cache")));
    media->sampleFrame(LocalCutTestPaths::fixture(QStringLiteral("media/中文测试-25fps-双声道.mp4")), 0);
    REQUIRE(media->isBusy());
    SECTION("Connection failure") { Q_EMIT client->failed(QStringLiteral("模拟连接中断")); }
    SECTION("Turn completion") { Q_EMIT client->turnFinished(false); }
    REQUIRE_FALSE(media->isBusy());
}

TEST_CASE("Ended Codex turns cancel local processing", "[.][AssistantLifecycle]")
{
    AssistantPanel panel;
    AssistantController controller(&panel);
    auto *processor = controller.findChild<LocalMediaProcessor *>();
    auto *client = controller.findChild<AssistantSession *>();
    REQUIRE(processor);
    REQUIRE(client);
    const QString root = LocalCutTestPaths::workspace(QStringLiteral("lifecycle"));
    REQUIRE(QDir().mkpath(root + "/models"));
    REQUIRE(QDir().mkpath(root + "/runtime"));
    QFile script(root + "/slow.py");
    REQUIRE(script.open(QIODevice::WriteOnly));
    REQUIRE(script.write("import time\ntime.sleep(60)\n") > 0);
    script.close();
    processor->configure(LocalCutTestPaths::executable("LOCALCUT_TEST_FFMPEG", QStringLiteral("ffmpeg")),
        LocalCutTestPaths::executable("LOCALCUT_TEST_FFPROBE", QStringLiteral("ffprobe")),
        LocalCutTestPaths::executable("LOCALCUT_TEST_PYTHON", QStringLiteral("python")), script.fileName(),
        root + "/models", root + "/runtime", root + "/processed");
    processor->process(LocalCutTestPaths::fixture(QStringLiteral("media/中文测试-25fps-双声道.mp4")), QStringLiteral("denoise_audio"));
    REQUIRE(processor->isBusy());
    SECTION("Turn completion") { Q_EMIT client->turnFinished(false); }
    SECTION("Connection failure") { Q_EMIT client->failed(QStringLiteral("模拟连接中断")); }
    REQUIRE(waitFor([&]() { return !processor->isBusy(); }, 3000));
}

TEST_CASE("Own asynchronous asset loading preserves the turn but external changes do not", "[.][AssistantLifecycle]")
{
    QJsonObject before{{"sessionId", "s"}, {"projectId", "p"}, {"sequenceId", "q"}, {"revision", 3},
        {"assets", QJsonArray{QJsonObject{{"binId", "new"}, {"name", "new.mov"}, {"resource", LocalCutTestPaths::workspace(QStringLiteral("new.mov"))},
            {"hasAudio", false}, {"hasVideo", false}}}}, {"clips", QJsonArray{}}, {"selection", QJsonArray{}}};
    auto loaded = before;
    loaded["revision"] = 4;
    auto asset = before["assets"].toArray()[0].toObject(); asset["hasAudio"] = true; asset["hasVideo"] = true;
    loaded["assets"] = QJsonArray{asset};
    REQUIRE(AssistantControllerTest::acceptsImportedAssetLoad(before, loaded, {"new"}));
    REQUIRE(AssistantControllerTest::acceptsImportedAssetLoad(loaded, loaded, {"new"}));
    REQUIRE_FALSE(AssistantControllerTest::acceptsImportedAssetLoad(before, loaded, {"other"}));
    SECTION("A source path change") { asset["resource"] = LocalCutTestPaths::workspace(QStringLiteral("other.mov")); loaded["assets"] = QJsonArray{asset}; }
    SECTION("A manual edit") { loaded["clips"] = QJsonArray{QJsonObject{{"id", 9}}}; }
    SECTION("An extra transient edit") { loaded["revision"] = 5; }
    SECTION("A changed selection") { loaded["selection"] = QJsonArray{9}; }
    REQUIRE_FALSE(AssistantControllerTest::acceptsImportedAssetLoad(before, loaded, {"new"}));
}

TEST_CASE("Derived media loads without an automatic proxy undo command", "[.][AssistantLifecycle]")
{
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    struct CloseDocument {
        ~CloseDocument() { pCore->projectManager()->closeCurrentDocument(false, false); }
    } closeDocument;
    QDateTime date = QDateTime::currentDateTime();
    KdenliveTests::updateTimeline(false, QString(), QString(), date, 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    EditBridge bridge;
    bridge.bindContext(document.uuid().toString(), timeline, undoStack);
    document.setDocumentProperty(QStringLiteral("enableproxy"), QStringLiteral("1"));
    document.setDocumentProperty(QStringLiteral("generateproxy"), QStringLiteral("1"));
    document.setDocumentProperty(QStringLiteral("proxyminsize"), QStringLiteral("1"));
    const QString path = LocalCutTestPaths::fixture(QStringLiteral("local-extension-20261107/native-import-alpha-fixture.mov"));
    REQUIRE(QFile::exists(path));
    auto xml = ClipCreator::getXmlFromUrl(path);
    QString id;
    Fun undo = []() { return true; }, redo = []() { return true; };
    REQUIRE(binModel->requestAddBinClip(id, xml.documentElement(), binModel->getRootFolder()->clipId(), undo, redo));
    binModel->getClipByBinID(id)->setProducerProperty(QStringLiteral("kdenlive:proxy"), QStringLiteral("-"));
    pCore->pushUndo(undo, redo, QStringLiteral("测试导入副本"));
    const int history = undoStack->index();
    const auto loadingState = bridge.readState();
    REQUIRE(waitFor([&]() {
        auto clip = binModel->getClipByBinID(id);
        return clip && clip->clipStatus() == FileStatus::StatusReady;
    }, 20000));
    auto clip = binModel->getClipByBinID(id);
    const auto loadedState = bridge.readState();
    INFO(QString::fromUtf8(QJsonDocument(loadingState).toJson(QJsonDocument::Compact)).toStdString());
    INFO(QString::fromUtf8(QJsonDocument(loadedState).toJson(QJsonDocument::Compact)).toStdString());
    REQUIRE(AssistantControllerTest::acceptsImportedAssetLoad(loadingState, loadedState, {id}));
    REQUIRE(clip->getProducerProperty(QStringLiteral("kdenlive:proxy")) == QStringLiteral("-"));
    REQUIRE(QFileInfo(clip->url()).absoluteFilePath() == QFileInfo(path).absoluteFilePath());
    REQUIRE(undoStack->index() == history);
    undoStack->undo();
    REQUIRE_FALSE(binModel->getClipByBinID(id));
}

TEST_CASE("Local undo and redo do not depend on Codex login", "[.][AssistantLifecycle]")
{
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    struct CloseDocument {
        ~CloseDocument() { pCore->projectManager()->closeCurrentDocument(false, false); }
    } closeDocument;
    QDateTime date = QDateTime::currentDateTime();
    KdenliveTests::updateTimeline(false, QString(), QString(), date, 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const QString binId = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", binModel, 100, false);
    int clip = -1;
    REQUIRE(timeline->requestClipInsertion(binId, timeline->getTrackIndexFromPosition(2), 0, clip));
    AssistantPanel panel;
    AssistantController controller(&panel);
    auto *undo = panel.findChild<QPushButton *>(QStringLiteral("undoButton"));
    REQUIRE(undo);
    undo->click();
    REQUIRE(timeline->getClipsCount() == 0);
    auto *redo = panel.findChild<QPushButton *>(QStringLiteral("redoButton"));
    REQUIRE(redo);
    redo->click();
    REQUIRE(timeline->getClipsCount() == 1);
    REQUIRE(timeline->getClipPlaytime(clip) == 100);
}

TEST_CASE("Stopping a pending assistant connection restores local controls", "[.][AssistantLifecycle]")
{
    AssistantPanel panel;
    AssistantController controller(&panel);
    auto *client = controller.findChild<AssistantSession *>();
    auto *stop = panel.findChild<QPushButton *>(QStringLiteral("stopButton"));
    auto *undo = panel.findChild<QPushButton *>(QStringLiteral("undoButton"));
    REQUIRE((client && stop && undo));
    REQUIRE_FALSE(client->isBusy());
    bool selectedCodex = false;
    QTimer::singleShot(0, &panel, [&]() {
        auto *dialog = panel.findChild<AssistantConnectionDialog *>();
        if (!dialog) return;
        auto *provider = dialog->findChild<QComboBox *>(QStringLiteral("connectionProvider"));
        if (!provider) { dialog->reject(); return; }
        provider->setCurrentIndex(provider->findData(QStringLiteral("codex")));
        selectedCodex = true;
        dialog->accept();
    });
    controller.beginConnection();
    REQUIRE(selectedCodex);
    REQUIRE(stop->isEnabled());
    REQUIRE_FALSE(undo->isEnabled());
    stop->click();
    REQUIRE_FALSE(stop->isEnabled());
    REQUIRE(undo->isEnabled());
    REQUIRE_FALSE(client->isReady());
}

TEST_CASE("Assistant progress uses Chinese product messages", "[.][AssistantLifecycle]")
{
    AssistantPanel panel;
    AssistantController controller(&panel);
    auto *client = controller.findChild<AssistantSession *>();
    auto *transcript = panel.findChild<QPlainTextEdit *>(QStringLiteral("transcript"));
    REQUIRE((client && transcript));
    Q_EMIT client->statusChanged(QStringLiteral("Working"));
    REQUIRE(transcript->toPlainText().contains(QStringLiteral("正在")));
    REQUIRE_FALSE(transcript->toPlainText().contains(QStringLiteral("Working")));
    const auto before = transcript->toPlainText();
    Q_EMIT client->statusChanged(QStringLiteral("Ready"));
    REQUIRE(transcript->toPlainText() == before);
}

TEST_CASE("Controller forwards the selected video provider and quality without cloud traffic", "[.][AssistantLifecycle]")
{
    AssistantPanel panel;
    AssistantController controller(&panel);
    auto *video = controller.findChild<VideoGenerationClient *>();
    REQUIRE(video);
    VideoGenerationClientTest::useLocalEndpoint(video);

    ProviderCapture capture;
    VideoGenerationClientTest::setProviderFactory(video, [&capture]() {
        return std::make_unique<CapturingVideoProvider>(&capture);
    });
    Q_EMIT panel.videoRequested(QStringLiteral("a calm lake"), {}, QStringLiteral("16:9"), 4, {}, QStringLiteral("ltx-space"),
                                QStringLiteral("space-default"));
    REQUIRE(video->providerId() == QStringLiteral("ltx-space"));
    REQUIRE(capture.credential.isEmpty());
    REQUIRE(capture.created);
    REQUIRE(capture.request.prompt == QStringLiteral("a calm lake"));
    REQUIRE(capture.request.ratio == QStringLiteral("16:9"));
    REQUIRE(capture.request.duration == 4);
    REQUIRE(capture.request.resolution == QStringLiteral("space-default"));

    Q_EMIT panel.videoRequested(QStringLiteral("quality sentinel"), {}, QStringLiteral("16:9"), 4, QStringLiteral("unit-test-key"),
                                QStringLiteral("seedance"), QStringLiteral("controller-resolution-sentinel"));
    REQUIRE(video->providerId() == QStringLiteral("seedance"));
    REQUIRE(video->currentStatus() == QStringLiteral("failed"));

    Q_EMIT panel.videoRequested(QStringLiteral("unknown provider"), {}, QStringLiteral("16:9"), 4, {}, QStringLiteral("unknown-provider"),
                                QStringLiteral("720p"));
    REQUIRE(video->providerId().isEmpty());
    REQUIRE(video->currentStatus() == QStringLiteral("unconnected"));
}

TEST_CASE("Controller keeps the selected service when preparing a video form", "[.][AssistantLifecycle]")
{
    AssistantPanel panel;
    AssistantController controller(&panel);
    auto *video = controller.findChild<VideoGenerationClient *>();
    auto *provider = panel.findChild<QComboBox *>(QStringLiteral("videoProvider"));
    auto *quality = panel.findChild<QComboBox *>(QStringLiteral("videoQuality"));
    REQUIRE(video);
    REQUIRE(provider);
    REQUIRE(quality);
    provider->setCurrentIndex(provider->findData(QStringLiteral("ltx-space")));

    panel.prepareVideo(QStringLiteral("prepared only"), QStringLiteral("9:16"), 5);

    REQUIRE(provider->currentData().toString() == QStringLiteral("ltx-space"));
    REQUIRE(quality->currentData().toString() == QStringLiteral("space-default"));
    REQUIRE(video->currentStatus() == QStringLiteral("unconnected"));
}

TEST_CASE("Controller rejects resume for providers without resume support", "[.][AssistantLifecycle]")
{
    AssistantPanel panel;
    AssistantController controller(&panel);
    auto *video = controller.findChild<VideoGenerationClient *>();
    auto *status = panel.findChild<QLabel *>(QStringLiteral("videoStatus"));
    REQUIRE(video);
    REQUIRE(status);
    VideoGenerationClientTest::useLocalEndpoint(video);

    Q_EMIT panel.resumeVideoRequested({}, QStringLiteral("ltx-space"));

    REQUIRE(video->providerId() == QStringLiteral("seedance"));
    REQUIRE(video->currentStatus() == QStringLiteral("unconnected"));
    REQUIRE(status->text().contains(QStringLiteral("不支持")));
}

TEST_CASE("Controller resume skips newer LTX records and accepts legacy Seedance records", "[.][AssistantLifecycle]")
{
    const QString folder = LocalCutTestPaths::workspace(QStringLiteral("generated"));
    REQUIRE(QDir().mkpath(folder));
    const QString ltxPath = QDir(folder).filePath(QStringLiteral("controller-test-newer-ltx.json"));
    const QString seedancePath = QDir(folder).filePath(QStringLiteral("controller-test-older-seedance.json"));
    struct Cleanup {
        QStringList paths;
        ~Cleanup()
        {
            for (const auto &path : paths) QFile::remove(path);
        }
    } cleanup{{ltxPath, seedancePath}};

    const auto writeRecord = [](const QString &path, const QJsonObject &record, const QDateTime &modified) {
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        REQUIRE(file.write(QJsonDocument(record).toJson(QJsonDocument::Compact)) > 0);
        REQUIRE(file.setFileTime(modified, QFileDevice::FileModificationTime));
    };
    writeRecord(ltxPath, QJsonObject{{QStringLiteral("id"), QStringLiteral("newer-ltx")}, {QStringLiteral("provider"), QStringLiteral("ltx-space")}},
                QDateTime::currentDateTime().addYears(20));
    writeRecord(seedancePath, QJsonObject{{QStringLiteral("id"), QStringLiteral("invalid legacy seedance id")}},
                QDateTime::currentDateTime().addYears(19));

    AssistantPanel panel;
    AssistantController controller(&panel);
    auto *video = controller.findChild<VideoGenerationClient *>();
    REQUIRE(video);
    VideoGenerationClientTest::useLocalEndpoint(video);

    Q_EMIT panel.resumeVideoRequested(QStringLiteral("unit-test-key"), QStringLiteral("seedance"));

    REQUIRE(video->providerId() == QStringLiteral("seedance"));
    REQUIRE(video->currentStatus() == QStringLiteral("failed"));
}

TEST_CASE("Assistant exports Chinese media to a verified Chinese MP4", "[.][AssistantExport]")
{
    bool contextChanged = false;
    SECTION("Unchanged timeline") {}
    SECTION("Timeline changed after the export snapshot") { contextChanged = true; }
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    struct CloseDocument {
        ~CloseDocument() { pCore->projectManager()->closeCurrentDocument(false, false); }
    } closeDocument;
    QDateTime date = QDateTime::currentDateTime();
    KdenliveTests::updateTimeline(false, QString(), QString(), date, 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const QString source = LocalCutTestPaths::fixture(QStringLiteral("media/中文 空格 样片.mp4"));
    REQUIRE(QFile::exists(source));
    auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), source.toUtf8().constData());
    REQUIRE(producer->is_valid());
    producer->set("length", 100);
    producer->set_in_and_out(0, 99);
    QString binId;
    Fun undo = []() { return true; }, redo = []() { return true; };
    REQUIRE(binModel->requestAddBinClip(binId, producer, binModel->getRootFolder()->clipId(), undo, redo));
    int clip = -1;
    REQUIRE(timeline->requestClipInsertion(binId, timeline->getTrackIndexFromPosition(2), 0, clip));
    AssistantPanel panel;
    AssistantController controller(&panel);
    auto *exportButton = panel.findChild<QPushButton *>(QStringLiteral("exportButton"));
    auto *stopButton = panel.findChild<QPushButton *>(QStringLiteral("stopButton"));
    auto *transcript = panel.findChild<QPlainTextEdit *>(QStringLiteral("transcript"));
    auto *assistantStatus = panel.findChild<QLabel *>(QStringLiteral("assistantStatus"));
    REQUIRE(exportButton);
    REQUIRE(stopButton);
    REQUIRE(assistantStatus);
    exportButton->click();
    if (contextChanged) {
        undoStack->undo();
        REQUIRE(timeline->getClipsCount() == 0);
    }
    REQUIRE(waitFor([&]() { return !stopButton->isEnabled(); }, 90000));
    INFO(transcript->toPlainText().toStdString());
    if (contextChanged) {
        REQUIRE(transcript->toPlainText().contains(QStringLiteral("工程随后有改动")));
        REQUIRE(assistantStatus->text().contains(QStringLiteral("工程随后有改动")));
    } else {
        REQUIRE(transcript->toPlainText().contains(QStringLiteral("MP4 已导出并通过视频校验")));
    }
    const QDir exports(LocalCutTestPaths::workspace(QStringLiteral("exports")));
    const auto files = exports.entryInfoList({QStringLiteral("成片*.mp4")}, QDir::Files, QDir::Time);
    REQUIRE_FALSE(files.isEmpty());
    REQUIRE(files.first().size() > 0);
    QProcess probe;
    probe.start(LocalCutTestPaths::executable("LOCALCUT_TEST_FFPROBE", QStringLiteral("ffprobe")), {"-v", "error", "-show_streams", "-of", "json", files.first().filePath()});
    REQUIRE(probe.waitForFinished(15000));
    REQUIRE(probe.exitCode() == 0);
    bool audio = false;
    for (const auto &value : QJsonDocument::fromJson(probe.readAllStandardOutput()).object()["streams"].toArray()) {
        const auto stream = value.toObject();
        if (stream["codec_type"] == "audio") {
            REQUIRE(stream["codec_name"] == "aac");
            REQUIRE(stream["channels"].toInt() == 2);
            audio = true;
        }
    }
    REQUIRE(audio);
}
}

TEST_CASE("Chinese assistant splits a real native clip with one undo", "[.][AssistantLive]")
{
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    struct CloseDocument {
        ~CloseDocument() { pCore->projectManager()->closeCurrentDocument(false, false); }
    } closeDocument;
    QDateTime documentDate = QDateTime::currentDateTime();
    KdenliveTests::updateTimeline(false, QString(), QString(), documentDate, 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const int track = timeline->getTrackIndexFromPosition(2);
    const QString binId = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", binModel, 100, false);
    int clip = -1;
    REQUIRE(timeline->requestClipInsertion(binId, track, 0, clip));
    const int history = undoStack->count();

    AssistantPanel panel;
    AssistantController controller(&panel);
    auto *connectButton = panel.findChild<QPushButton *>(QStringLiteral("connectButton"));
    auto *sendButton = panel.findChild<QPushButton *>(QStringLiteral("sendButton"));
    auto *input = panel.findChild<QPlainTextEdit *>(QStringLiteral("messageInput"));
    auto *transcript = panel.findChild<QPlainTextEdit *>(QStringLiteral("transcript"));
    REQUIRE(connectButton);
    REQUIRE(sendButton);
    REQUIRE(input);
    REQUIRE(transcript);
    input->setPlainText(QStringLiteral("把指定片段（clipId=%1）在第50帧分为两段，其它不变。请读取真实工程后直接完成。").arg(clip));
    connectButton->click();
    REQUIRE(waitFor([&]() { return sendButton->isEnabled(); }, 60000));
    sendButton->click();
    const bool edited = waitFor([&]() { return timeline->getClipsCount() == 2; }, 120000);
    INFO(transcript->toPlainText().toStdString());
    REQUIRE(edited);
    REQUIRE(timeline->getClipPlaytime(clip) == 50);
    REQUIRE(undoStack->count() == history + 1);
    undoStack->undo();
    REQUIRE(timeline->getClipsCount() == 1);
    REQUIRE(timeline->getClipPlaytime(clip) == 100);
    undoStack->redo();
    REQUIRE(timeline->getClipsCount() == 2);
    REQUIRE(timeline->getClipPlaytime(clip) == 50);
}
