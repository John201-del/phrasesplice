/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "localcut_test_paths.hpp"
#include "jobs/transcodetask.h"
#include "kdenlivesettings.h"
#include "test_utils.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>

class TranscodeTaskTest
{
public:
    static QString destination(const QString &source, ClipType::ProducerType type, const QString &suffix, int binId = -1)
    {
        TranscodeSeek::TranscodeInfo info;
        info.url = source;
        info.type = type;
        TranscodeTask task(ObjectId(KdenliveObjectType::NoItem, binId, QUuid()), suffix, {}, QStringLiteral("%1.mp4"), info, -1, -1, false, nullptr, false);
        return task.destinationPath(QStringLiteral(".mp4"));
    }

    static int runProcess(const QString &source, ClipType::ProducerType type, const QString &suffix, const QString &parameters = QStringLiteral("%1.mp4"))
    {
        pCore->taskManager.unBlock();
        TranscodeSeek::TranscodeInfo info;
        info.url = source;
        info.type = type;
        auto *task = new TranscodeTask(ObjectId(KdenliveObjectType::NoItem, -1, QUuid()), suffix, {}, parameters, info, -1, -1,
                                      false, nullptr, false);
        task->m_jobProcess = nullptr;
        task->run();
        REQUIRE(task->m_jobProcess);
        return task->m_jobProcess->exitStatus() == QProcess::NormalExit ? task->m_jobProcess->exitCode() : -1;
    }
};

TEST_CASE("Transcodes from any source stay in the validated persistent workspace data", "[TranscodeStorage]")
{
    const QString directory = LocalCutTestPaths::workspace(QStringLiteral("transcoded"));
    for (auto type : {ClipType::AV, ClipType::Video, ClipType::Audio, ClipType::Playlist, ClipType::SlideShow}) {
        const auto path = TranscodeTaskTest::destination(LocalCutTestPaths::workspace() + QStringLiteral("-outside/中文 照片或视频.mov"), type, QStringLiteral("-25fps"));
        REQUIRE(QFileInfo(path).absolutePath() == QDir::cleanPath(directory));
        REQUIRE(QFileInfo(path).fileName() == QStringLiteral("中文 照片或视频-25fps.mp4"));
    }
}

TEST_CASE("Transcodes never select a pre-existing output file", "[TranscodeStorage]")
{
    const QString suffix = QStringLiteral("-collision-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString source = LocalCutTestPaths::workspace(QStringLiteral("media/原件.mov"));
    const QString path = TranscodeTaskTest::destination(source, ClipType::AV, suffix);
    REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile existing(path);
    REQUIRE(existing.open(QIODevice::WriteOnly));
    REQUIRE(existing.write("keep-existing-output") > 0);
    existing.close();
    const QString next = TranscodeTaskTest::destination(source, ClipType::AV, suffix);
    REQUIRE(next != path);
    REQUIRE(QFileInfo(next).absolutePath() == QFileInfo(path).absolutePath());
    REQUIRE(QFileInfo(next).fileName() == QStringLiteral("原件0001%1.mp4").arg(suffix));
    REQUIRE(existing.open(QIODevice::ReadOnly));
    REQUIRE(existing.readAll() == QByteArray("keep-existing-output"));
    existing.close();
    REQUIRE(existing.remove());
}

TEST_CASE("Editable title names cannot redirect transcode output", "[TranscodeStorage]")
{
    const auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    const auto closeDocument = qScopeGuard([&]() {
        pCore->projectManager()->closeCurrentDocument(false, false);
    });
    QDateTime documentDate = QDateTime::currentDateTime();
    KdenliveTests::updateTimeline(false, QString(), QString(), documentDate, 0);
    pCore->projectManager()->testSetActiveTimeline(document.getTimeline(document.uuid()));
    const QString binId = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", binModel, 25);
    const auto clip = binModel->getClipByBinID(binId);
    REQUIRE(clip);
    const QString directory = LocalCutTestPaths::workspace(QStringLiteral("transcoded"));
    for (const auto &name : {QStringLiteral("D:/outside/中文标题"), QStringLiteral("../中文标题"), QStringLiteral("C:\\outside\\中文标题")}) {
        clip->setName(name);
        const QString path = TranscodeTaskTest::destination({}, ClipType::Text, QStringLiteral("-title"), binId.toInt());
        REQUIRE(QFileInfo(path).absolutePath() == QDir::cleanPath(directory));
        REQUIRE_FALSE(QFileInfo(path).fileName().contains(QLatin1Char(':')));
        REQUIRE_FALSE(QFileInfo(path).fileName().contains(QLatin1Char('\\')));
        REQUIRE(clip->name() == name);
    }
}

TEST_CASE("Failed transcoders discard their partial output in both backends", "[TranscodeFailure]")
{
    const QString stub = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("transcode_failure_stub.exe"));
    REQUIRE(QFile::exists(stub));
    const QString oldFfmpeg = KdenliveSettings::ffmpegpath();
    const QString oldMelt = KdenliveSettings::meltpath();
    KdenliveSettings::setFfmpegpath(stub);
    KdenliveSettings::setMeltpath(stub);
    const auto restore = qScopeGuard([&]() {
        KdenliveSettings::setFfmpegpath(oldFfmpeg);
        KdenliveSettings::setMeltpath(oldMelt);
    });
    REQUIRE(QDir().mkpath(LocalCutTestPaths::workspace(QStringLiteral("transcode"))));
    QTemporaryDir sourceDir(LocalCutTestPaths::workspace(QStringLiteral("transcode/transcode-source-XXXXXX")));
    REQUIRE(sourceDir.isValid());
    const QString source = sourceDir.filePath(QStringLiteral("中文 原件.mov"));
    QFile original(source);
    REQUIRE(original.open(QIODevice::WriteOnly));
    REQUIRE(original.write("unchanged-source") > 0);
    original.close();
    SECTION("FFmpeg") {
        const QString path = TranscodeTaskTest::destination(source, ClipType::AV, QStringLiteral("-failure"));
        REQUIRE(TranscodeTaskTest::runProcess(source, ClipType::AV, QStringLiteral("-failure")) == 17);
        REQUIRE_FALSE(QFile::exists(path));
    }
    SECTION("MLT") {
        const QString path = TranscodeTaskTest::destination(source, ClipType::Playlist, QStringLiteral("-failure"));
        REQUIRE(TranscodeTaskTest::runProcess(source, ClipType::Playlist, QStringLiteral("-failure")) == 17);
        REQUIRE_FALSE(QFile::exists(path));
    }
    REQUIRE(original.open(QIODevice::ReadOnly));
    REQUIRE(original.readAll() == QByteArray("unchanged-source"));
}

TEST_CASE("Native transcodes preserve originals and existing outputs with or without audio", "[TranscodeSuccess]")
{
    const QString ffmpeg = LocalCutTestPaths::executable("LOCALCUT_TEST_FFMPEG", QStringLiteral("ffmpeg"));
    const QString ffprobe = LocalCutTestPaths::executable("LOCALCUT_TEST_FFPROBE", QStringLiteral("ffprobe"));
    const QString input = LocalCutTestPaths::fixture(QStringLiteral("media/中文测试-25fps-双声道.mp4"));
    REQUIRE(QFile::exists(ffmpeg));
    REQUIRE(QFile::exists(ffprobe));
    REQUIRE(QFile::exists(input));
    const QString oldFfmpeg = KdenliveSettings::ffmpegpath();
    KdenliveSettings::setFfmpegpath(ffmpeg);
    const auto restore = qScopeGuard([&]() { KdenliveSettings::setFfmpegpath(oldFfmpeg); });
    REQUIRE(QDir().mkpath(LocalCutTestPaths::workspace(QStringLiteral("transcode"))));
    for (bool withAudio : {true, false}) {
        INFO((withAudio ? "source has audio" : "source has no audio"));
        QTemporaryDir sourceDir(LocalCutTestPaths::workspace(QStringLiteral("transcode/transcode-success-XXXXXX")));
        REQUIRE(sourceDir.isValid());
        QString source = input;
        if (!withAudio) {
            source = sourceDir.filePath(QStringLiteral("中文 无音轨.mp4"));
            QProcess makeSilent;
            makeSilent.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"), input, QStringLiteral("-an"),
                                      QStringLiteral("-c:v"), QStringLiteral("copy"), source});
            REQUIRE(makeSilent.waitForFinished(30000));
            REQUIRE(makeSilent.exitCode() == 0);
        }
        QFile original(source);
        REQUIRE(original.open(QIODevice::ReadOnly));
        const auto originalHash = QCryptographicHash::hash(original.readAll(), QCryptographicHash::Sha256);
        original.close();
        const QString suffix = QStringLiteral("-success-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        const QString existingPath = TranscodeTaskTest::destination(source, ClipType::AV, suffix);
        QFile existing(existingPath);
        REQUIRE(existing.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        REQUIRE(existing.write("existing-output-must-not-change") > 0);
        existing.close();
        const QString output = TranscodeTaskTest::destination(source, ClipType::AV, suffix);
        const auto cleanup = qScopeGuard([&]() { QFile::remove(output); QFile::remove(existingPath); });
        REQUIRE(output != existingPath);
        const QString parameters = QStringLiteral("-c:v libx264 -preset ultrafast -crf 28 -pix_fmt yuv420p -threads 1 -c:a aac %1.mp4");
        REQUIRE(TranscodeTaskTest::runProcess(source, ClipType::AV, suffix, parameters) == 0);
        REQUIRE(QFileInfo(output).size() > 0);
        QProcess probe;
        probe.start(ffprobe, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
                              QStringLiteral("stream=codec_type,codec_name,width,height"), QStringLiteral("-of"), QStringLiteral("json"), output});
        REQUIRE(probe.waitForFinished(30000));
        REQUIRE(probe.exitCode() == 0);
        const auto streams = QJsonDocument::fromJson(probe.readAllStandardOutput()).object().value(QStringLiteral("streams")).toArray();
        bool hasVideo = false;
        bool hasAudio = false;
        for (const auto &item : streams) {
            const auto stream = item.toObject();
            if (stream.value(QStringLiteral("codec_type")).toString() == QStringLiteral("video")) {
                hasVideo = true;
                REQUIRE(stream.value(QStringLiteral("codec_name")).toString() == QStringLiteral("h264"));
                REQUIRE(stream.value(QStringLiteral("width")).toInt() == 640);
                REQUIRE(stream.value(QStringLiteral("height")).toInt() == 360);
            }
            hasAudio |= stream.value(QStringLiteral("codec_type")).toString() == QStringLiteral("audio");
        }
        REQUIRE(hasVideo);
        REQUIRE(hasAudio == withAudio);
        REQUIRE(original.open(QIODevice::ReadOnly));
        REQUIRE(QCryptographicHash::hash(original.readAll(), QCryptographicHash::Sha256) == originalHash);
        REQUIRE(existing.open(QIODevice::ReadOnly));
        REQUIRE(existing.readAll() == QByteArray("existing-output-must-not-change"));
    }
}
