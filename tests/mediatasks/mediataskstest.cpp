#include "assistant/mediatasks.h"
#include "../localcut_test_paths.hpp"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QProcess>
#include <QDirIterator>
#include <QDateTime>
#include <QSignalSpy>
#include <QTest>

class MediaTasksTest : public QObject
{
    Q_OBJECT
    const QString ffmpeg = LocalCutTestPaths::executable("LOCALCUT_TEST_FFMPEG", QStringLiteral("ffmpeg"));
    const QString ffprobe = LocalCutTestPaths::executable("LOCALCUT_TEST_FFPROBE", QStringLiteral("ffprobe"));
    const QString root = LocalCutTestPaths::workspace(QStringLiteral("mediatasks"));
    const QString cache = LocalCutTestPaths::workspace(QStringLiteral("cache"));
    const QString python = LocalCutTestPaths::workspace(QStringLiteral("speech-venv/Scripts/python.exe"));
    const QString script = LocalCutTestPaths::source(QStringLiteral("data/scripts/whisper/whispertosrt.py"));
    QString video, silence, tone, speech, noAudio;
    void configure(MediaTasks &tasks, const QString &overrideScript = {})
    {
        tasks.configure(ffmpeg, ffprobe, overrideScript.isEmpty() ? python : LocalCutTestPaths::executable("LOCALCUT_TEST_PYTHON", QStringLiteral("python")),
                        overrideScript.isEmpty() ? script : overrideScript, cache);
    }
    bool generate(const QStringList &args)
    {
        QProcess process;
        process.start(ffmpeg, QStringList{"-hide_banner", "-loglevel", "error", "-y"} + args);
        return process.waitForFinished(20000) && process.exitCode() == 0;
    }
    QString fixture(const QString &name, const QByteArray &body)
    {
        QFile file(root + "/" + name + ".py");
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(body) != body.size()) {
            return {};
        }
        return file.fileName();
    }
    bool hasMarker(const QString &name)
    {
        QDirIterator iterator(cache, {name}, QDir::Files, QDirIterator::Subdirectories);
        return iterator.hasNext();
    }
private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(QDir().mkpath(root));
        video = root + QStringLiteral("/中文 空格 视频.mp4");
        silence = root + "/silence.wav";
        tone = root + "/tone.wav";
        noAudio = root + "/no-audio.mp4";
        speech = LocalCutTestPaths::fixture(QStringLiteral("media/字幕识别-中文.wav"));
        QVERIFY2(QFileInfo(speech).isFile(), "Provide LOCALCUT_NATIVE_FIXTURES/media/字幕识别-中文.wav");
        const QString sourceVideo = LocalCutTestPaths::fixture(QStringLiteral("media/中文测试-25fps-双声道.mp4"));
        QVERIFY2(QFileInfo(sourceVideo).isFile(), "Provide LOCALCUT_NATIVE_FIXTURES/media/中文测试-25fps-双声道.mp4");
        QVERIFY(QFile::copy(sourceVideo, video) || QFile::exists(video));
        QVERIFY(generate({"-f", "lavfi", "-i", "anullsrc=r=16000:cl=mono", "-t", "2", silence}));
        QVERIFY(generate({"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=16000", "-t", "2", tone}));
        QVERIFY(generate({"-i", video, "-map", "0:v:0", "-c:v", "copy", "-an", noAudio}));
    }
    void actualFrame()
    {
        MediaTasks tasks;
        configure(tasks);
        QSignalSpy done(&tasks, &MediaTasks::finished);
        QSignalSpy errors(&tasks, &MediaTasks::failed);
        tasks.sampleFrame(video, 0.5);
        QVERIFY(tasks.isBusy());
        QVERIFY(done.wait(15000));
        QCOMPARE(errors.count(), 0);
        const auto result = done[0][0].toJsonObject();
        QCOMPARE(result["kind"].toString(), "frame");
        QCOMPARE(result["timeSeconds"].toDouble(), 0.5);
        QCOMPARE(result["mimeType"].toString(), "image/jpeg");
        QCOMPARE(result["width"].toInt(), 640);
        QCOMPARE(result["height"].toInt(), 360);
        QCOMPARE(result["view"].toString(), "source");
        QVERIFY(done[0][1].toByteArray().startsWith(QByteArray::fromHex("ffd8")));
        QVERIFY(done[0][1].toByteArray().endsWith(QByteArray::fromHex("ffd9")));
        QVERIFY(!tasks.isBusy());
    }
    void stillPhotoReturnsDimensionsWithoutImageBytes()
    {
        MediaTasks tasks;
        configure(tasks);
        QSignalSpy done(&tasks, &MediaTasks::finished);
        QSignalSpy errors(&tasks, &MediaTasks::failed);
        const QString photo = LocalCutTestPaths::fixture(QStringLiteral("avatar-20261107/portrait.png"));
        QVERIFY2(QFileInfo(photo).isFile(), "Provide LOCALCUT_NATIVE_FIXTURES/avatar-20261107/portrait.png");
        tasks.sampleFrame(photo, 0);
        QTRY_COMPARE_WITH_TIMEOUT(done.count() + errors.count(), 1, 15000);
        QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));
        const auto result = done[0][0].toJsonObject();
        QCOMPARE(result["kind"].toString(), "image_info");
        QCOMPARE(result["width"].toInt(), 480);
        QCOMPARE(result["height"].toInt(), 480);
        QVERIFY(done[0][1].toByteArray().isEmpty());
    }
    void actualSilenceAndNoSilence()
    {
        MediaTasks tasks;
        configure(tasks);
        QSignalSpy done(&tasks, &MediaTasks::finished);
        tasks.detectSilence(silence);
        QVERIFY(done.wait(15000));
        const auto result = done[0][0].toJsonObject();
        QCOMPARE(result["kind"].toString(), "silence");
        QVERIFY(qAbs(result["duration"].toDouble() - 2) < .01);
        const auto intervals = result["intervals"].toArray();
        QCOMPARE(intervals.size(), 1);
        QVERIFY(intervals[0].toObject()["start"].toDouble() < .01);
        QVERIFY(qAbs(intervals[0].toObject()["end"].toDouble() - 2) < .01);
        done.clear();
        tasks.detectSilence(tone);
        QVERIFY(done.wait(15000));
        QCOMPARE(done[0][0].toJsonObject()["intervals"].toArray().size(), 0);
    }
    void usesSourceClockWhenVideoIsLonger()
    {
        const QString mixed = root + "/long-video-short-audio.mp4";
        QVERIFY(generate({"-f", "lavfi", "-i", "color=c=blue:s=64x64:r=5:d=4", "-i", silence,
                          "-map", "0:v:0", "-map", "1:a:0", "-c:v", "mpeg4", "-c:a", "aac", mixed}));
        MediaTasks tasks;
        configure(tasks);
        QSignalSpy done(&tasks, &MediaTasks::finished);
        tasks.detectSilence(mixed);
        QVERIFY(done.wait(15000));
        const auto result = done[0][0].toJsonObject();
        QVERIFY(qAbs(result["duration"].toDouble() - 4) < .01);
        QCOMPARE(result["timeReference"].toString(), "source");
        QCOMPARE(result["audioStart"].toDouble(), 0.0);
        QVERIFY(qAbs(result["audioEnd"].toDouble() - 2) < .01);
        const auto intervals = result["intervals"].toArray();
        QCOMPARE(intervals.size(), 2);
        QCOMPARE(intervals[0].toObject()["kind"].toString(), "silence");
        QCOMPARE(intervals[1].toObject()["kind"].toString(), "no_audio");
        QVERIFY(qAbs(intervals[1].toObject()["start"].toDouble() - 2) < .01);
        QCOMPARE(intervals[1].toObject()["end"].toDouble(), 4.0);
    }
    void delayedAudioSilenceUsesSourceClock()
    {
        MediaTasks tasks;
        configure(tasks);
        QSignalSpy done(&tasks, &MediaTasks::finished);
        const QString source = LocalCutTestPaths::fixture(QStringLiteral("review-media-offset/offset-audio.mp4"));
        QVERIFY2(QFileInfo(source).isFile(), "Provide LOCALCUT_NATIVE_FIXTURES/review-media-offset/offset-audio.mp4");
        tasks.detectSilence(source);
        QVERIFY(done.wait(15000));
        auto result = done[0][0].toJsonObject();
        QCOMPARE(result["duration"].toDouble(), 4.0);
        QCOMPARE(result["timeReference"].toString(), "source");
        QVERIFY(qAbs(result["audioStart"].toDouble() - 1.936) < .001);
        QCOMPARE(result["audioEnd"].toDouble(), 4.0);
        QCOMPARE(result["audioStreamIndex"].toInt(), 1);
        auto intervals = result["intervals"].toArray();
        QCOMPARE(intervals.size(), 2);
        QCOMPARE(intervals[0].toObject()["kind"].toString(), "no_audio");
        QCOMPARE(intervals[0].toObject()["start"].toDouble(), 0.0);
        QVERIFY(qAbs(intervals[0].toObject()["end"].toDouble() - 1.936) < .001);
        QCOMPARE(intervals[1].toObject()["kind"].toString(), "silence");
        QVERIFY(qAbs(intervals[1].toObject()["start"].toDouble() - 1.936) < .001);
        QCOMPARE(intervals[1].toObject()["end"].toDouble(), 4.0);

        const QString delayedTone = root + "/delayed-tone.mp4";
        QVERIFY(generate({"-f", "lavfi", "-i", "color=c=blue:s=64x64:r=5:d=4", "-itsoffset", "2", "-i", tone,
                          "-map", "0:v:0", "-map", "1:a:0", "-c:v", "mpeg4", "-c:a", "aac", delayedTone}));
        done.clear();
        tasks.detectSilence(delayedTone);
        QVERIFY(done.wait(15000));
        result = done[0][0].toJsonObject();
        intervals = result["intervals"].toArray();
        QCOMPARE(intervals.size(), 1);
        QCOMPARE(intervals[0].toObject()["kind"].toString(), "no_audio");
        QVERIFY(qAbs(intervals[0].toObject()["end"].toDouble() - result["audioStart"].toDouble()) < .001);
    }
    void delayedTranscriptRestoresOffsetAndClipsPadding()
    {
        QVERIFY2(QFileInfo(cache + "/whisper/small.pt").isFile(), "Provide LOCALCUT_WORKSPACE_ROOT/cache/whisper/small.pt");
        const QString source = LocalCutTestPaths::fixture(QStringLiteral("review-media-offset/offset-audio.mp4"));
        QVERIFY2(QFileInfo(source).isFile(), "Provide LOCALCUT_NATIVE_FIXTURES/review-media-offset/offset-audio.mp4");
        MediaTasks tasks;
        const QString path = fixture("offset-transcript",
            "import sys,pathlib,wave\n"
            "with wave.open(sys.argv[1]) as w: assert abs(w.getnframes()/w.getframerate()-2.064)<0.001\n"
            "pathlib.Path(sys.argv[1]).with_suffix('.srt').write_text('1\\n00:00:00,250 --> 00:00:01,000\\ncontrolled\\n\\n2\\n00:00:01,500 --> 00:00:02,112\\npadding\\n',encoding='utf-8')\n");
        QVERIFY(!path.isEmpty());
        configure(tasks, path);
        QSignalSpy done(&tasks, &MediaTasks::finished);
        tasks.transcribe(source);
        QVERIFY(done.wait(15000));
        const auto result = done[0][0].toJsonObject();
        QCOMPARE(result["duration"].toDouble(), 4.0);
        QCOMPARE(result["timeReference"].toString(), "source");
        QVERIFY(qAbs(result["audioStart"].toDouble() - 1.936) < .001);
        QCOMPARE(result["audioEnd"].toDouble(), 4.0);
        QCOMPARE(result["audioStreamIndex"].toInt(), 1);
        const auto segments = result["segments"].toArray();
        QCOMPARE(segments.size(), 2);
        QVERIFY(qAbs(segments[0].toObject()["start"].toDouble() - 2.186) < .001);
        QVERIFY(qAbs(segments[0].toObject()["end"].toDouble() - 2.936) < .001);
        QVERIFY(qAbs(segments[1].toObject()["start"].toDouble() - 3.436) < .001);
        QCOMPARE(segments[1].toObject()["end"].toDouble(), 4.0);
    }
    void transcriptWordsUseSourceClockAndRejectInvalidTiming()
    {
        QVERIFY2(QFileInfo(cache + "/whisper/small.pt").isFile(), "Provide LOCALCUT_WORKSPACE_ROOT/cache/whisper/small.pt");
        const QString source = LocalCutTestPaths::fixture(QStringLiteral("review-media-offset/offset-audio.mp4"));
        QVERIFY2(QFileInfo(source).isFile(), "Provide LOCALCUT_NATIVE_FIXTURES/review-media-offset/offset-audio.mp4");
        MediaTasks tasks;
        QSignalSpy done(&tasks, &MediaTasks::finished);
        QSignalSpy errors(&tasks, &MediaTasks::failed);
        const QByteArray base =
            "import sys,pathlib\n"
            "p=pathlib.Path(sys.argv[1])\n"
            "p.with_suffix('.srt').write_text('1\\n00:00:00,250 --> 00:00:01,000\\ncontrolled\\n',encoding='utf-8')\n";
        const QList<QByteArray> words{
            "[{\"start\":0.25,\"end\":0.8,\"text\":\"first\"},{\"start\":1.5,\"end\":2.112,\"text\":\"second\"}]",
            "[{\"start\":0.25,\"end\":99,\"text\":\"invalid\"}]",
            "[{\"start\":0.8,\"end\":1,\"text\":\"first\"},{\"start\":0.2,\"end\":0.4,\"text\":\"backward\"}]"};
        for (int i = 0; i < words.size(); ++i) {
            const QString path = fixture(QStringLiteral("word-timing-%1").arg(i),
                base + "p.with_suffix('.words.json').write_text('" + words[i] + "',encoding='utf-8')\n");
            configure(tasks, path);
            done.clear(); errors.clear();
            tasks.transcribe(source);
            if (i == 0) {
                QVERIFY(done.wait(15000));
                const auto actual = done[0][0].toJsonObject()["words"].toArray();
                QCOMPARE(actual.size(), 2);
                QVERIFY(qAbs(actual[0].toObject()["start"].toDouble() - 2.186) < .001);
                QVERIFY(qAbs(actual[0].toObject()["end"].toDouble() - 2.736) < .001);
                QCOMPARE(actual[1].toObject()["end"].toDouble(), 4.0);
            } else {
                QVERIFY(errors.wait(15000));
                QCOMPARE(done.count(), 0);
            }
        }
    }
    void invalidInputsAndNoVideo()
    {
        MediaTasks tasks;
        configure(tasks);
        QSignalSpy errors(&tasks, &MediaTasks::failed);
        QSignalSpy done(&tasks, &MediaTasks::finished);
        tasks.sampleFrame(video, -1);
        QTRY_COMPARE(errors.count(), 1);
        tasks.sampleFrame(video, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 2, 15000);
        tasks.sampleFrame(tone, 0);
        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 3, 15000);
        tasks.detectSilence(root + "/missing.wav");
        QTRY_COMPARE(errors.count(), 4);
        tasks.detectSilence(noAudio);
        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 5, 15000);
        tasks.transcribe(noAudio);
        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 6, 15000);
        QCOMPARE(done.count(), 0);
    }
    void rejectsOutsideAndLinkedCache()
    {
        MediaTasks tasks;
        QSignalSpy errors(&tasks, &MediaTasks::failed);
        tasks.configure(ffmpeg, ffprobe, python, script, LocalCutTestPaths::workspace() + QStringLiteral("-outside/cache"));
        tasks.sampleFrame(video, 0);
        QTRY_COMPARE(errors.count(), 1);
        const QString link = root + "/junction";
        if (!QFile::exists(link)) {
            QProcess process;
            process.start(LocalCutTestPaths::executable("COMSPEC", QStringLiteral("cmd")), {"/c", "mklink", "/J", QDir::toNativeSeparators(link), QDir::toNativeSeparators(cache)});
            QVERIFY(process.waitForFinished(5000));
            QCOMPARE(process.exitCode(), 0);
        }
        tasks.configure(ffmpeg, ffprobe, python, script, link+"/child");
        tasks.sampleFrame(video, 0);
        QTRY_COMPARE(errors.count(), 2);
        QVERIFY(!QDir(link+"/child").exists());
    }
    void busyCancelAndRestart()
    {
        MediaTasks tasks;
        configure(tasks);
        QSignalSpy errors(&tasks, &MediaTasks::failed);
        QSignalSpy done(&tasks, &MediaTasks::finished);
        tasks.sampleFrame(video, 0.5);
        QVERIFY(tasks.isBusy());
        tasks.detectSilence(silence);
        QCOMPARE(errors.count(), 1);
        QVERIFY(tasks.isBusy());
        tasks.cancel();
        QVERIFY(!tasks.isBusy());
        QTest::qWait(200);
        QCOMPARE(done.count(), 0);
        tasks.detectSilence(tone);
        QVERIFY(done.wait(15000));
        QCOMPARE(done.count(), 1);
        QCOMPARE(done[0][0].toJsonObject()["kind"].toString(), "silence");
    }
    void restartFromProgressInvalidatesOldContinuation()
    {
        MediaTasks tasks;
        configure(tasks);
        QSignalSpy done(&tasks, &MediaTasks::finished);
        QSignalSpy errors(&tasks, &MediaTasks::failed);
        bool restarted = false;
        connect(&tasks, &MediaTasks::progress, &tasks, [&]() {
            if (!restarted) {
                restarted = true;
                tasks.cancel();
                tasks.detectSilence(tone);
            }
        });
        tasks.sampleFrame(video, 0);
        QVERIFY(done.wait(15000));
        QCOMPARE(done.count(), 1);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(done[0][0].toJsonObject()["kind"].toString(), "silence");
        QTest::qWait(100);
        QCOMPARE(done.count(), 1);
    }
    void frameAndSilenceWithoutSpeechDependencies()
    {
        MediaTasks tasks;
        tasks.configure(ffmpeg, ffprobe, root+"/missing.exe", root+"/missing.py", root+"/cache-no-model");
        QSignalSpy done(&tasks, &MediaTasks::finished);
        QSignalSpy errors(&tasks, &MediaTasks::failed);
        tasks.sampleFrame(video, 0);
        QVERIFY(done.wait(15000));
        tasks.detectSilence(tone);
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 2, 15000);
        tasks.transcribe(speech);
        QTRY_COMPARE(errors.count(), 1);
        QVERIFY(!tasks.isBusy());
    }
    void closesTrailingSilence()
    {
        MediaTasks tasks;
        tasks.configure(QCoreApplication::applicationDirPath()+"/media-slow-tool.exe", ffprobe, python, script, cache);
        QSignalSpy done(&tasks, &MediaTasks::finished);
        tasks.detectSilence(silence);
        QVERIFY(done.wait(15000));
        const auto interval = done[0][0].toJsonObject()["intervals"].toArray()[0].toObject();
        QCOMPARE(interval["start"].toDouble(), .5);
        QCOMPARE(interval["end"].toDouble(), 2.0);
    }
    void cancelExtractionAndRecognition()
    {
        QVERIFY2(QFileInfo(cache + "/whisper/small.pt").isFile(), "Provide LOCALCUT_WORKSPACE_ROOT/cache/whisper/small.pt");
        MediaTasks tasks;
        const QString extractMarker = "extract-running";
        QDirIterator old(cache, {extractMarker}, QDir::Files, QDirIterator::Subdirectories);
        while (old.hasNext()) {
            QFile::remove(old.next());
        }
        tasks.configure(QCoreApplication::applicationDirPath()+"/media-slow-tool.exe", ffprobe, python, script, cache);
        QSignalSpy done(&tasks, &MediaTasks::finished);
        QSignalSpy errors(&tasks, &MediaTasks::failed);
        tasks.transcribe(speech);
        QTRY_VERIFY_WITH_TIMEOUT(hasMarker(extractMarker), 15000);
        tasks.cancel();
        QVERIFY(!tasks.isBusy());
        const QString marker = "recognition-" + QString::number(QDateTime::currentMSecsSinceEpoch());
        const QString sleepScript = fixture("sleep", "import os,time,pathlib\npathlib.Path('" + marker.toUtf8() + "').write_text('running')\ntime.sleep(15)\n");
        QVERIFY(!sleepScript.isEmpty());
        configure(tasks, sleepScript);
        tasks.transcribe(speech);
        QTRY_VERIFY_WITH_TIMEOUT(hasMarker(marker), 15000);
        tasks.cancel();
        QVERIFY(!tasks.isBusy());
        QTest::qWait(250);
        QCOMPARE(done.count(), 0);
        QCOMPARE(errors.count(), 0);
        configure(tasks);
        tasks.sampleFrame(video, 0);
        QVERIFY(done.wait(15000));
        QCOMPARE(done.count(), 1);
    }
    void rejectsMissingInvalidAndFailedSubtitles()
    {
        QVERIFY2(QFileInfo(cache + "/whisper/small.pt").isFile(), "Provide LOCALCUT_WORKSPACE_ROOT/cache/whisper/small.pt");
        MediaTasks tasks;
        QSignalSpy done(&tasks, &MediaTasks::finished);
        QSignalSpy errors(&tasks, &MediaTasks::failed);
        const QByteArray base =
            "import os,sys,pathlib,wave\n"
            "root=pathlib.Path(os.environ['LOCALCUT_WORKSPACE_ROOT']).resolve()\n"
            "assert all(pathlib.Path(os.environ[k]).resolve().is_relative_to(root) for k in ['TEMP','TMP','XDG_CACHE_HOME','TORCH_HOME','HF_HOME','NUMBA_CACHE_DIR'])\n"
            "assert os.environ['PYTHONDONTWRITEBYTECODE']=='1'\n"
            "assert 'fp16=False' in sys.argv and not any(a.startswith('language=') for a in sys.argv)\n"
            "with wave.open(sys.argv[1]) as w: assert w.getnchannels()==1 and w.getframerate()==16000\n";
        const QString successScript = fixture("controlled-valid", base + "pathlib.Path(sys.argv[1]).with_suffix('.srt').write_text('1\\n00:00:00,000 --> 00:00:01,000\\ncontrolled\\n',encoding='utf-8')\n");
        QVERIFY(!successScript.isEmpty());
        configure(tasks, successScript);
        tasks.transcribe(speech);
        QVERIFY(done.wait(15000));
        QCOMPARE(done[0][0].toJsonObject()["segments"].toArray().size(), 1);
        const QList<QByteArray> bodies{
            base,
            base + "pathlib.Path(sys.argv[1]).with_suffix('.srt').write_text('1\\n00:00:00,000 --> 00:10:00,000\\nprivate\\n',encoding='utf-8')\n",
            base + "pathlib.Path(sys.argv[1]).with_suffix('.srt').write_text('1\\n00:00:00,000 --> 00:00:01,000\\nprivate\\n',encoding='utf-8')\nprint('private')\nsys.exit(1)\n",
            base + "pathlib.Path(sys.argv[1]).with_suffix('.srt').write_bytes(b'\\xff')\n"};
        for (int i=0; i<bodies.size(); ++i) {
            const QString path = fixture("controlled-"+QString::number(i), bodies[i]);
            QVERIFY(!path.isEmpty());
            configure(tasks, path);
            tasks.transcribe(speech);
            QTRY_COMPARE_WITH_TIMEOUT(errors.count(), i+1, 15000);
            QCOMPARE(done.count(), 1);
            QVERIFY(!errors.last()[0].toString().contains("private"));
            QVERIFY(!tasks.isBusy());
        }
        tasks.configure(ffmpeg, root+"/missing.exe", python, script, cache);
        tasks.sampleFrame(video, 0);
        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), bodies.size()+1, 15000);
        QCOMPARE(done.count(), 1);
    }
    void realChineseTranscript()
    {
        QVERIFY2(QFileInfo(python).isFile(), "Provide LOCALCUT_WORKSPACE_ROOT/speech-venv/Scripts/python.exe");
        QVERIFY2(QFileInfo(cache + "/whisper/small.pt").isFile(), "Provide LOCALCUT_WORKSPACE_ROOT/cache/whisper/small.pt");
        MediaTasks tasks;
        configure(tasks);
        QSignalSpy done(&tasks, &MediaTasks::finished);
        QSignalSpy errors(&tasks, &MediaTasks::failed);
        tasks.transcribe(speech);
        QVERIFY(tasks.isBusy());
        QVERIFY(done.wait(300000));
        QCOMPARE(errors.count(), 0);
        const auto result = done[0][0].toJsonObject();
        QCOMPARE(result["kind"].toString(), "transcript");
        const auto segments = result["segments"].toArray();
        QVERIFY(segments.size() >= 2);
        QString text;
        double last = 0;
        for (const auto &entry : segments) {
            auto segment = entry.toObject();
            QVERIFY(segment["start"].toDouble() >= last);
            QVERIFY(segment["end"].toDouble() > segment["start"].toDouble());
            QVERIFY(segment["end"].toDouble() <= 14.654);
            text += segment["text"].toString();
            last = segment["start"].toDouble();
        }
        QVERIFY(text.contains(QStringLiteral("视频")));
    }
};
QTEST_GUILESS_MAIN(MediaTasksTest)
#include "mediataskstest.moc"
