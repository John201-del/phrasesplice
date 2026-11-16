#include "../../src/assistant/localmediaprocessor.h"
#include "../localcut_test_paths.hpp"
#include <QtTest>
#include <QSignalSpy>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QThread>
#include <QCryptographicHash>
#include <QTemporaryDir>
class ProcessorTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void realPhotoExportChecksPngAndKeepsExistingFile() {
        const QString product = LocalCutTestPaths::workspace();
        const QString source = LocalCutTestPaths::fixture(QStringLiteral("avatar-20261107/portrait.png"));
        QVERIFY2(QFileInfo(source).isFile(), "Provide LOCALCUT_NATIVE_FIXTURES/avatar-20261107/portrait.png");
        QVERIFY2(QFileInfo(product + "/local-ai/python/python.exe").isFile(), "Provide LOCALCUT_WORKSPACE_ROOT/local-ai/python/python.exe");
        QVERIFY2(QFileInfo(product + "/local-ai/models/rvm_mobilenetv3_fp32.onnx").isFile(), "Provide LOCALCUT_WORKSPACE_ROOT/local-ai/models/rvm_mobilenetv3_fp32.onnx");
        QTemporaryDir directory(LocalCutTestPaths::workspace(QStringLiteral("photo-qt-XXXXXX")));
        QVERIFY(directory.isValid());
        LocalMediaProcessor task;
        task.configure(LocalCutTestPaths::executable("LOCALCUT_TEST_FFMPEG", QStringLiteral("ffmpeg")), LocalCutTestPaths::executable("LOCALCUT_TEST_FFPROBE", QStringLiteral("ffprobe")),
            product + "/local-ai/python/python.exe", LocalCutTestPaths::source(QStringLiteral("data/scripts/local_ai/retouch_photo.py")),
            product + "/local-ai/models", product + "/local-ai", directory.path());
        const QString output = directory.filePath(QStringLiteral("透明证件照.png"));
        const QJsonObject options{{"background", QJsonObject{{"mode", "transparent"}}}};
        QSignalSpy done(&task, &LocalMediaProcessor::finished), failed(&task, &LocalMediaProcessor::failed);
        task.retouchPhoto(source, output, options);
        QTRY_COMPARE_WITH_TIMEOUT(done.count() + failed.count(), 1, 30000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        const auto result = done.first().first().toJsonObject();
        QCOMPARE(result["path"].toString(), output);
        QVERIFY(result["alpha"].toBool());
        QCOMPARE(result["width"].toInt(), 480);
        QCOMPARE(result["height"].toInt(), 480);
        QFile saved(output); QVERIFY(saved.open(QIODevice::ReadOnly));
        const auto original = saved.readAll(); saved.close();
        QVERIFY(original.startsWith(QByteArray::fromHex("89504e470d0a1a0a")));
        task.retouchPhoto(source, output, options);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(done.count(), 1);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), original);
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden), QStringList{QFileInfo(output).fileName()});
    }

    void realMovingFaceVideoKeepsAudioAndFrameCount() {
        const QString product = LocalCutTestPaths::workspace();
        const QString fixtures = LocalCutTestPaths::fixture(QStringLiteral("photo-retouch"));
        QVERIFY2(QFileInfo(fixtures + QStringLiteral("/验收移动人脸.mp4")).isFile(), "Provide LOCALCUT_NATIVE_FIXTURES/photo-retouch/验收移动人脸.mp4");
        QVERIFY2(QFileInfo(fixtures + QStringLiteral("/验收卡通青蛙.png")).isFile(), "Provide LOCALCUT_NATIVE_FIXTURES/photo-retouch/验收卡通青蛙.png");
        QVERIFY2(QFileInfo(product + "/local-ai/python/python.exe").isFile(), "Provide LOCALCUT_WORKSPACE_ROOT/local-ai/python/python.exe");
        QTemporaryDir directory(LocalCutTestPaths::workspace(QStringLiteral("face-qt-XXXXXX")));
        QVERIFY(directory.isValid());
        LocalMediaProcessor task;
        task.configure(LocalCutTestPaths::executable("LOCALCUT_TEST_FFMPEG", QStringLiteral("ffmpeg")), LocalCutTestPaths::executable("LOCALCUT_TEST_FFPROBE", QStringLiteral("ffprobe")),
            product + "/local-ai/python/python.exe", LocalCutTestPaths::source(QStringLiteral("data/scripts/local_ai/process_media.py")),
            product + "/local-ai/models", product + "/local-ai", directory.path());
        QSignalSpy done(&task, &LocalMediaProcessor::finished), failed(&task, &LocalMediaProcessor::failed);
        task.process(fixtures + QStringLiteral("/验收移动人脸.mp4"), QStringLiteral("cover_face"), fixtures + QStringLiteral("/验收卡通青蛙.png"));
        QTRY_COMPARE_WITH_TIMEOUT(done.count() + failed.count(), 1, 30000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        const auto result = done.first().first().toJsonObject();
        QVERIFY(QFileInfo(result["path"].toString()).size() > 0);
        const auto streams = result["outputMetadata"].toObject()["streams"].toArray();
        QCOMPARE(streams.size(), 2);
        QCOMPARE(streams.at(0).toObject()["nb_frames"].toString(), QStringLiteral("12"));
        QCOMPARE(streams.at(1).toObject()["codec_name"].toString(), QStringLiteral("aac"));
    }

    void realRvmCancellation() {
        const QString product = LocalCutTestPaths::workspace();
        const QString root = LocalCutTestPaths::workspace(QStringLiteral("local-extension-20261107/processor-real-cancel"));
        const QString source = LocalCutTestPaths::fixture(QStringLiteral("local-extension-20261107/portrait-test.mp4"));
        QVERIFY2(QFileInfo(source).isFile(), "Provide LOCALCUT_NATIVE_FIXTURES/local-extension-20261107/portrait-test.mp4");
        QVERIFY2(QFileInfo(product + "/local-ai/python/python.exe").isFile(), "Provide LOCALCUT_WORKSPACE_ROOT/local-ai/python/python.exe");
        QVERIFY2(QFileInfo(product + "/local-ai/models/rvm_mobilenetv3_fp32.onnx").isFile(), "Provide LOCALCUT_WORKSPACE_ROOT/local-ai/models/rvm_mobilenetv3_fp32.onnx");
        QFile input(source); QVERIFY(input.open(QIODevice::ReadOnly));
        const auto original = QCryptographicHash::hash(input.readAll(), QCryptographicHash::Sha256); input.close();
        QVERIFY(QDir().mkpath(root));
        const auto existing = QDir(root).entryList(QDir::Files);
        LocalMediaProcessor task;
        task.configure(LocalCutTestPaths::executable("LOCALCUT_TEST_FFMPEG", QStringLiteral("ffmpeg")), LocalCutTestPaths::executable("LOCALCUT_TEST_FFPROBE", QStringLiteral("ffprobe")),
            product + "/local-ai/python/python.exe", LocalCutTestPaths::source(QStringLiteral("data/scripts/local_ai/process_media.py")),
            product + "/local-ai/models", product + "/local-ai", root);
        QSignalSpy done(&task, &LocalMediaProcessor::finished), failed(&task, &LocalMediaProcessor::failed), progress(&task, &LocalMediaProcessor::progress);
        task.process(source, QStringLiteral("remove_background"));
        QTRY_VERIFY_WITH_TIMEOUT(progress.count() > 1 || !failed.isEmpty() || !done.isEmpty(), 60000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QVERIFY(task.isBusy());
        task.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QCOMPARE(done.count(), 0); QVERIFY(!task.isBusy());
        QCOMPARE(failed.first().first().toString(), QStringLiteral("已取消本地处理。"));
        QTest::qWait(1000);
        QCOMPARE(QDir(root).entryList(QDir::Files), existing);
        QVERIFY(input.open(QIODevice::ReadOnly));
        QCOMPARE(QCryptographicHash::hash(input.readAll(), QCryptographicHash::Sha256), original);
    }
    void rejectsUnknown() {
        LocalMediaProcessor task;
        QSignalSpy failed(&task, &LocalMediaProcessor::failed);
        task.process(LocalCutTestPaths::workspace(QStringLiteral("nonexistent")), QStringLiteral("shell"));
        QCOMPARE(failed.count(), 1);
        QVERIFY(!task.isBusy());
    }
    void rejectsUnsafeStorage() {
        const QString source = LocalCutTestPaths::workspace(QStringLiteral("local-extension-20261107/unsafe-source.wav"));
        QVERIFY(QDir().mkpath(QFileInfo(source).absolutePath()));
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("source"); file.close();
        const auto executable = QCoreApplication::applicationFilePath();
        LocalMediaProcessor task;
        const QString outside = LocalCutTestPaths::workspace() + QStringLiteral("-outside");
        task.configure(executable, executable, executable, executable, outside + "/models", outside + "/runtime", outside + "/processed");
        QSignalSpy failed(&task, &LocalMediaProcessor::failed);
        task.process(source, QStringLiteral("denoise_audio"));
        QCOMPARE(failed.count(), 1); QCOMPARE(failed.first().first().toString(), QStringLiteral("本地处理目录必须位于当前工作区内的普通目录。")); QVERIFY(!task.isBusy());
        QVERIFY(file.remove());
    }
    void childFixtureWorks() {
        const QString marker = LocalCutTestPaths::workspace(QStringLiteral("local-extension-20261107/child-baseline"));
        QVERIFY(QDir().mkpath(QFileInfo(marker).absolutePath()));
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--child-marker"), marker});
        QVERIFY(child.waitForFinished(5000)); QCOMPARE(child.exitCode(), 0);
        QFile file(marker); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("alive")); file.close(); QVERIFY(file.remove());
    }
    void subprocesses_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("success"); QTest::addColumn<QString>("operation");
        for (const auto &mode : {QStringLiteral("valid"), QStringLiteral("bad-media"), QStringLiteral("exit-error"), QStringLiteral("cancel"), QStringLiteral("wrong-duration"), QStringLiteral("no-ok")})
            QTest::newRow(qPrintable(mode)) << mode << (mode == QStringLiteral("valid")) << QStringLiteral("denoise_audio");
        QTest::newRow("alpha") << QStringLiteral("valid") << true << QStringLiteral("remove_background");
        QTest::newRow("missing-alpha") << QStringLiteral("no-alpha") << false << QStringLiteral("remove_background");
        QTest::newRow("video") << QStringLiteral("valid") << true << QStringLiteral("enhance_video");
        QTest::newRow("two-stems") << QStringLiteral("valid") << true << QStringLiteral("separate_vocals");
        QTest::newRow("rotated-bad-output") << QStringLiteral("rotated") << false << QStringLiteral("remove_background");
        QTest::newRow("rotated-upright-output") << QStringLiteral("rotated-upright") << true << QStringLiteral("remove_background");
        QTest::newRow("non-square-pixels") << QStringLiteral("non-square") << false << QStringLiteral("remove_background");
        QTest::newRow("mirrored-matrix") << QStringLiteral("mirrored") << false << QStringLiteral("remove_background");
    }
    void subprocesses() {
        QFETCH(QString, mode); QFETCH(bool, success); QFETCH(QString, operation);
        const bool video = operation == QStringLiteral("remove_background") || operation == QStringLiteral("enhance_video");
        const QByteArray original = mode.startsWith(QStringLiteral("rotated")) ? QByteArray("sourcevideo-rotated")
            : mode == QStringLiteral("non-square") ? QByteArray("sourcevideo-sar")
            : mode == QStringLiteral("mirrored") ? QByteArray("sourcevideo-mirrored") : video ? QByteArray("sourcevideo") : QByteArray("source");
        const QString root = LocalCutTestPaths::workspace(QStringLiteral("local-extension-20261107/processor-fixture"));
        QDir().mkpath(root);
        QDir().mkpath(root + QStringLiteral("/processed"));
        QFile existing(root + QStringLiteral("/processed/existing.wav")); QVERIFY(existing.open(QIODevice::WriteOnly)); existing.write("keep"); existing.close();
        QFile input(root + QStringLiteral("/input.wav")); QVERIFY(input.open(QIODevice::WriteOnly)); input.write(original); input.close();
        QFile script(root + QStringLiteral("/mode")); QVERIFY(script.open(QIODevice::WriteOnly)); script.write(mode.toUtf8()); script.close();
        LocalMediaProcessor task;
        const auto executable = QCoreApplication::applicationFilePath();
        task.configure(executable, executable, executable, script.fileName(), root, root, root + QStringLiteral("/processed"));
        QSignalSpy done(&task, &LocalMediaProcessor::finished), failed(&task, &LocalMediaProcessor::failed), progress(&task, &LocalMediaProcessor::progress);
        task.process(input.fileName(), operation);
        if (mode == QStringLiteral("cancel")) { QTRY_COMPARE_WITH_TIMEOUT(progress.count(), 1, 10000); task.cancel(); }
        QTRY_VERIFY_WITH_TIMEOUT(done.count() + failed.count() == 1, 10000);
        QCOMPARE(done.count(), success ? 1 : 0); QVERIFY(!task.isBusy());
        QVERIFY(input.open(QIODevice::ReadOnly)); QCOMPARE(input.readAll(), original);
        if (success) {
            const auto result = done.first().first().toJsonObject(); QVERIFY(QFileInfo::exists(result.value("path").toString())); QFile::remove(result.value("path").toString());
            if (operation == QStringLiteral("separate_vocals")) { QVERIFY(QFileInfo::exists(result.value("backgroundPath").toString())); QFile::remove(result.value("backgroundPath").toString()); }
        }
        QVERIFY(existing.open(QIODevice::ReadOnly)); QCOMPARE(existing.readAll(), QByteArray("keep")); existing.close();
        QCOMPARE(QDir(root + QStringLiteral("/processed")).entryList(QDir::Files).size(), 1);
        QVERIFY(existing.remove());
        if (mode == QStringLiteral("cancel")) { QTest::qWait(1500); QVERIFY(!QFileInfo::exists(root + QStringLiteral("/child-marker"))); }
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.contains(QStringLiteral("--child-marker"))) {
        QThread::sleep(1);
        QFile marker(args.last()); if (!marker.open(QIODevice::WriteOnly)) return 9; marker.write("alive"); return 0;
    }
    if (args.contains(QStringLiteral("-show_streams"))) {
        QFile file(args.last()); if (!file.open(QIODevice::ReadOnly)) return 9;
        const auto contents = file.readAll();
        if (contents.startsWith("sourcevideo") || contents == QByteArray("video") || contents.startsWith("alpha")) {
            QJsonObject videoStream{{"codec_type", "video"}, {"codec_name", contents.startsWith("alpha") ? "qtrle" : "h264"}, {"pix_fmt", contents.startsWith("alpha") ? "argb" : "yuv420p"}, {"width", contents == QByteArray("alpha-upright") ? 360 : 640}, {"height", contents == QByteArray("alpha-upright") ? 640 : 360}, {"avg_frame_rate", "25/1"}};
            if (contents == QByteArray("sourcevideo-rotated")) videoStream["side_data_list"] = QJsonArray{QJsonObject{{"side_data_type", "Display Matrix"}, {"rotation", 90}, {"displaymatrix", "\n00000000: 0 -65536 0\n00000001: 65536 0 0\n00000002: 0 0 1073741824\n"}}};
            if (contents == QByteArray("sourcevideo-mirrored")) videoStream["side_data_list"] = QJsonArray{QJsonObject{{"side_data_type", "Display Matrix"}, {"rotation", 180}, {"displaymatrix", "\n00000000: -65536 0 0\n00000001: 0 65536 0\n00000002: 0 0 1073741824\n"}}};
            if (contents == QByteArray("sourcevideo-sar")) videoStream["sample_aspect_ratio"] = "4:3";
            const QJsonObject metadata{{"format", QJsonObject{{"duration", "1.0"}}}, {"streams", QJsonArray{videoStream, QJsonObject{{"codec_type", "audio"}, {"codec_name", "aac"}, {"sample_rate", "44100"}, {"channels", 2}}}}};
            printf("%s", QJsonDocument(metadata).toJson(QJsonDocument::Compact).constData()); return 0;
        }
        if (contents == QByteArray("invalid")) { printf("{}"); return 0; }
        if (contents == QByteArray("wrong-duration")) { printf("{\"format\":{\"duration\":\"2.0\"},\"streams\":[{\"codec_type\":\"audio\",\"codec_name\":\"pcm_s16le\",\"sample_rate\":\"44100\",\"channels\":2}]}"); return 0; }
        printf("{\"format\":{\"duration\":\"1.0\"},\"streams\":[{\"codec_type\":\"audio\",\"codec_name\":\"pcm_s16le\",\"sample_rate\":\"44100\",\"channels\":2}]}"); return 0;
    }
    if (args.contains(QStringLiteral("--operation"))) {
        QFile script(args.at(1)); if (!script.open(QIODevice::ReadOnly)) return 9; const auto mode = script.readAll();
        if (mode == QByteArray("cancel")) {
            if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), {QStringLiteral("--child-marker"), QFileInfo(script.fileName()).absolutePath() + QStringLiteral("/child-marker")})) return 9;
            printf("{\"progress\":0.1}\n"); fflush(stdout); QThread::sleep(10);
        }
        const auto output = args.at(args.indexOf(QStringLiteral("--output")) + 1);
        const auto operation = args.at(args.indexOf(QStringLiteral("--operation")) + 1);
        QFile file(output); if (!file.open(QIODevice::WriteOnly)) return 9;
        file.write(mode == QByteArray("bad-media") ? "invalid" : mode == QByteArray("wrong-duration") ? "wrong-duration" : mode == QByteArray("rotated-upright") ? "alpha-upright" : operation == QStringLiteral("remove_background") && mode != QByteArray("no-alpha") ? "alpha" : operation == QStringLiteral("remove_background") || operation == QStringLiteral("enhance_video") ? "video" : "media"); file.close();
        if (args.contains(QStringLiteral("--background-output"))) { QFile background(args.at(args.indexOf(QStringLiteral("--background-output")) + 1)); if (!background.open(QIODevice::WriteOnly)) return 9; background.write("media"); }
        if (mode == QByteArray("no-ok")) return 0;
        const auto json = QJsonDocument(QJsonObject{{"ok", true}, {"path", output}}).toJson(QJsonDocument::Compact);
        printf("%s\n", json.constData()); return mode == QByteArray("exit-error") ? 7 : 0;
    }
    ProcessorTest test; return QTest::qExec(&test, argc, argv);
}
#include "localmediaprocessortest.moc"
