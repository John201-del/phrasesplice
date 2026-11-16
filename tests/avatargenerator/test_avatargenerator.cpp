#include "../../src/assistant/avatargenerator.h"
#include "../../src/assistant/workspacepaths.h"
#include <QtTest>
#include <QSignalSpy>
#include <QDir>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QUuid>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
class AvatarTest : public QObject {
    Q_OBJECT
    QString root, image, audio;
    QString ffmpeg, ffprobe;
    QString python;
    QString script;
    void configure(AvatarGenerator &job) {
        job.configure(ffmpeg, ffprobe, python, script, root, root + "/outputs");
    }
private Q_SLOTS:
    void initTestCase() {
        root = WorkspacePaths::root() + "/avatar-tests/run-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(QDir().mkpath(root));
        ffmpeg = qEnvironmentVariable("AVATAR_TEST_FFMPEG", QStandardPaths::findExecutable(QStringLiteral("ffmpeg")));
        ffprobe = qEnvironmentVariable("AVATAR_TEST_FFPROBE", QStandardPaths::findExecutable(QStringLiteral("ffprobe")));
        python = qEnvironmentVariable("AVATAR_TEST_PYTHON", QStandardPaths::findExecutable(QStringLiteral("python3")));
        QVERIFY(QFileInfo(ffmpeg).isFile()); QVERIFY(QFileInfo(ffprobe).isFile()); QVERIFY(QFileInfo(python).isFile());
        script = QStringLiteral(AVATAR_FIXTURE_PATH);
        image = root + "/face.png"; audio = root + "/voice.wav";
        QImage face(64, 64, QImage::Format_RGB32); face.fill(Qt::gray); QVERIFY(face.save(image));
        QProcess make;
        make.start(ffmpeg, {"-v", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=1", "-y", audio});
        QVERIFY(make.waitForFinished(15000)); QCOMPARE(make.exitCode(), 0);
    }
    void rejectsNetworkInput() {
        AvatarGenerator job; QSignalSpy failure(&job, &AvatarGenerator::failed);
        job.generate(QStringLiteral("https://example.invalid/face.png"), QStringLiteral("D:/missing.wav"));
        QCOMPARE(failure.size(), 1); QVERIFY(!job.isBusy());
    }
    void rejectsMissingRuntime() {
        AvatarGenerator job; QSignalSpy failure(&job, &AvatarGenerator::failed);
        job.configure("C:/invalid", "C:/invalid", "C:/invalid", "C:/invalid", "C:/invalid", "C:/invalid");
        job.generate(image, audio);
        QCOMPARE(failure.size(), 1); QVERIFY(!job.isBusy());
    }
    void rejectsOutsideOutput() {
        AvatarGenerator job; QSignalSpy failure(&job, &AvatarGenerator::failed);
        const QString outside = QFileInfo(WorkspacePaths::root()).dir().filePath("avatar-outside-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        QVERIFY(!WorkspacePaths::isInside(outside));
        QVERIFY(!QFileInfo::exists(outside));
        job.configure(ffmpeg, ffprobe, python, script, root, outside);
        job.generate(image, audio); QCOMPARE(failure.size(), 1); QVERIFY(!job.isBusy());
        QVERIFY(failure.first().first().toString().contains(QStringLiteral("运行和输出目录")));
        QVERIFY(!QFileInfo::exists(outside));
    }
    void acceptsPackagedToolsOutsideDataRoot() {
        for (const auto &path : {ffmpeg, ffprobe, python, script}) {
            QVERIFY(QFileInfo(path).isAbsolute());
            QVERIFY(QFileInfo(path).isFile());
            QVERIFY2(!WorkspacePaths::isInside(path), qUtf8Printable(path));
        }
        qputenv("AVATAR_TEST_MODE", "valid");
        AvatarGenerator job; configure(job);
        QSignalSpy failure(&job, &AvatarGenerator::failed), done(&job, &AvatarGenerator::finished);
        job.generate(image, audio);
        QTRY_VERIFY_WITH_TIMEOUT(!job.isBusy(), 30000);
        QCOMPARE(failure.size(), 0); QCOMPARE(done.size(), 1);
        const auto result = done.first().first().toJsonObject();
        const QString output = result.value("path").toString();
        QVERIFY(WorkspacePaths::isInside(output));
        QCOMPARE(QFileInfo(output).absolutePath(), root + "/outputs");
        QVERIFY(QFileInfo(output).size() > 0);
        const auto metadata = result.value("metadata").toObject();
        bool video = false, audioStream = false;
        for (const auto &entry : metadata.value("streams").toArray()) {
            const auto stream = entry.toObject();
            if (stream.value("codec_type").toString() == QStringLiteral("video")) {
                QCOMPARE(stream.value("width").toInt(), 256);
                QCOMPARE(stream.value("height").toInt(), 256);
                video = true;
            }
            audioStream |= stream.value("codec_type").toString() == QStringLiteral("audio");
        }
        QVERIFY(video); QVERIFY(audioStream);
    }
    void rejectsVideoAsImage() {
        AvatarGenerator job; configure(job); QSignalSpy failure(&job, &AvatarGenerator::failed);
        job.generate(audio, audio); QCOMPARE(failure.size(), 1); QVERIFY(!job.isBusy());
    }
    void protocol_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("success");
        QTest::newRow("real media") << QString("valid") << true;
        QTest::newRow("fake ok") << QString("bad") << false;
        QTest::newRow("wrong geometry") << QString("geometry") << false;
        QTest::newRow("short video") << QString("duration") << false;
        QTest::newRow("missing audio") << QString("silent") << false;
        QTest::newRow("short audio track") << QString("shortaudio") << false;
        QTest::newRow("changed source") << QString("mutate") << false;
        QTest::newRow("flood stdout") << QString("flood") << false;
    }
    void protocol() {
        QFETCH(QString, mode); QFETCH(bool, success);
        qputenv("AVATAR_TEST_MODE", mode.toUtf8());
        AvatarGenerator job; configure(job);
        QVERIFY(QDir().mkpath(root + "/outputs"));
        QFile sentinel(root + "/outputs/existing.mp4"); QVERIFY(sentinel.open(QIODevice::WriteOnly));
        QCOMPARE(sentinel.write("existing"), qint64(8)); sentinel.close();
        QSignalSpy failure(&job, &AvatarGenerator::failed), done(&job, &AvatarGenerator::finished);
        job.generate(image, audio);
        QTRY_VERIFY_WITH_TIMEOUT(!job.isBusy(), 30000);
        QCOMPARE(done.size(), success ? 1 : 0); QCOMPARE(failure.size(), success ? 0 : 1);
        QVERIFY(sentinel.open(QIODevice::ReadOnly)); QCOMPARE(sentinel.readAll(), QByteArray("existing"));
        if (success) {
            const auto result = done.first().first().toJsonObject();
            QCOMPARE(result.value("operation").toString(), QString("generate_avatar"));
            QVERIFY(QFileInfo::exists(result.value("path").toString()));
            QVERIFY(!result.value("metadata").toObject().value("format").toObject().contains("tags"));
        }
    }
    void rejectsLongAudio() {
        const QString longAudio = root + "/long.wav";
        QProcess make; make.start(ffmpeg, {"-v", "error", "-f", "lavfi", "-i", "sine=duration=30.1", "-y", longAudio});
        QVERIFY(make.waitForFinished(15000)); QCOMPARE(make.exitCode(), 0);
        AvatarGenerator job; configure(job); QSignalSpy failure(&job, &AvatarGenerator::failed);
        job.generate(image, longAudio); QTRY_VERIFY_WITH_TIMEOUT(!job.isBusy(), 10000); QCOMPARE(failure.size(), 1);
    }
    void preservesConflictingFiles() {
        AvatarGenerator job; configure(job);
        QSignalSpy failure(&job, &AvatarGenerator::failed);
        qputenv("AVATAR_TEST_MODE", "output_conflict");
        job.generate(image, audio);
        QTRY_VERIFY_WITH_TIMEOUT(!job.isBusy(), 10000); QCOMPARE(failure.size(), 1);
        const QDir outputs(root + "/outputs");
        bool retained = false;
        for (const auto &name : outputs.entryList({"*.mp4"}, QDir::Files)) {
            QFile file(outputs.filePath(name));
            retained |= file.open(QIODevice::ReadOnly) && file.readAll() == QByteArray("external output");
        }
        QVERIFY(retained);
        qputenv("AVATAR_TEST_MODE", "job_conflict");
        job.generate(image, audio);
        QTRY_VERIFY_WITH_TIMEOUT(!job.isBusy(), 10000); QCOMPARE(failure.size(), 2);
        const QDir jobs(root + "/jobs");
        const auto children = jobs.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(children.size(), 1);
        QVERIFY(QFileInfo::exists(jobs.filePath(children.first() + "/keep.txt")));
    }
    void cancel() {
        const QDir jobs(root + "/jobs");
        const auto previous = jobs.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        qputenv("AVATAR_TEST_MODE", "cancel"); AvatarGenerator job; configure(job);
        QSignalSpy progress(&job, &AvatarGenerator::progress), failure(&job, &AvatarGenerator::failed);
        job.generate(image, audio); QTRY_VERIFY_WITH_TIMEOUT(progress.size() > 0, 10000);
        job.cancel(); QTRY_VERIFY_WITH_TIMEOUT(!job.isBusy(), 10000); QCOMPARE(failure.size(), 1);
#ifdef Q_OS_WIN
        QFile pidFile(root + "/child.pid"); QVERIFY(pidFile.open(QIODevice::ReadOnly));
        HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, pidFile.readAll().toUInt());
        if (child) { QCOMPARE(WaitForSingleObject(child, 5000), DWORD(WAIT_OBJECT_0)); CloseHandle(child); }
#endif
        QCOMPARE(jobs.entryList(QDir::Dirs | QDir::NoDotAndDotDot), previous);
    }
};
QTEST_GUILESS_MAIN(AvatarTest)
#include "test_avatargenerator.moc"
