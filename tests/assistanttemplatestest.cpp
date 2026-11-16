/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "localcut_test_paths.hpp"
#include "assistant/editbridge.h"
#include "assistant/edittemplates.h"
#include "catch.hpp"
#include "test_utils.hpp"
#include "bin/projectfolder.h"
#include "bin/model/subtitlemodel.hpp"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "effects/effectstack/model/effectitemmodel.hpp"
#include "transitions/transitionsrepository.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

static void assignTestDocumentId(KdenliveDoc &document)
{
    static qint64 id = QDateTime::currentMSecsSinceEpoch();
    document.setDocumentProperty(QStringLiteral("documentid"), QString::number(++id));
}

TEST_CASE("Template recipes allocate exact integer frames", "[AssistantTemplates]")
{
    CHECK(EditTemplates::catalog()["templates"].toArray().size() == 8);
    for (int total : {125, 300, 1001}) {
        const auto result = EditTemplates::allocate(total, 3, "product_three_shot_v1");
        REQUIRE(result.size() == 3);
        CHECK(result[0] + result[1] + result[2] == total);
        CHECK(result[0] == total / 4);
        CHECK(result[1] == total / 4);
    }
    CHECK(EditTemplates::allocate(1, 3, "photo_story_v1").isEmpty());
    CHECK(EditTemplates::find("untrusted_script", 1).isEmpty());
    CHECK(EditTemplates::find("photo_story_v1", 2).isEmpty());
}

TEST_CASE("All eight templates apply to native media at 25 and 30000 over 1001 fps", "[AssistantTemplates]")
{
    const int numerator = GENERATE(25, 30000);
    const QString id = GENERATE(QStringLiteral("photo_story_v1"), QStringLiteral("knowledge_steps_v1"),
        QStringLiteral("product_three_shot_v1"), QStringLiteral("travel_chapters_v1"), QStringLiteral("event_recap_v1"),
        QStringLiteral("before_after_v1"), QStringLiteral("vertical_teaser_v1"), QStringLiteral("talking_clean_v1"));
    const int denominator = numerator == 25 ? 1 : 1001;
    INFO(id.toStdString() << " " << numerator << "/" << denominator);
    auto &profile = pCore->getProjectProfile();
    struct RestoreProfile {
        Mlt::Profile &profile;
        int numerator, denominator, width, height;
        ~RestoreProfile() {
            profile.set_frame_rate(numerator, denominator);
            profile.set_width(width); profile.set_height(height);
        }
    } restoreProfile{profile, profile.frame_rate_num(), profile.frame_rate_den(), profile.width(), profile.height()};
    profile.set_frame_rate(numerator, denominator);
    if (id == "vertical_teaser_v1") { profile.set_width(720); profile.set_height(1280); }
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    assignTestDocumentId(document);
    pCore->projectManager()->testSetDocument(&document);
    struct CloseDocument { ~CloseDocument() { pCore->projectManager()->closeCurrentDocument(false, false); } } closeDocument;
    REQUIRE(KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0));
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const int video = timeline->getTrackIndexFromPosition(2), mirror = timeline->getMirrorTrackId(video);
    const auto add = [&](const QString &path, int length) {
        REQUIRE(QFile::exists(path));
        auto producer = std::make_shared<Mlt::Producer>(profile, path.toUtf8().constData());
        REQUIRE(producer->is_valid());
        if (length > 0) { producer->set("length", length); producer->set_in_and_out(0, length - 1); }
        QString bin;
        Fun undo = []() { return true; }, redo = []() { return true; };
        REQUIRE(binModel->requestAddBinClip(bin, producer, binModel->getRootFolder()->clipId(), undo, redo));
        REQUIRE(binModel->getClipByBinID(bin)->clipStatus() == FileStatus::StatusReady);
        return bin;
    };
    const bool photo = id == "photo_story_v1", talking = id == "talking_clean_v1";
    const int fiveSeconds = qRound(5.0 * numerator / denominator);
    const QString source = photo
        ? add(LocalCutTestPaths::fixture(QStringLiteral("language-effects-20261107/运镜测试.png")), fiveSeconds)
        : add(LocalCutTestPaths::fixture(QStringLiteral("media/中文 空格 样片.mp4")), 0);
    const auto asset = binModel->getClipByBinID(source);
    if (photo) {
        REQUIRE(asset->clipType() == ClipType::Image);
        REQUIRE(asset->getFramePlaytime() == fiveSeconds);
    } else {
        REQUIRE(asset->hasAudio()); REQUIRE(asset->hasVideo());
        REQUIRE(asset->getFramePlaytime() >= 100);
    }
    EditBridge bridge;
    bridge.bindContext(document.uuid().toString(), timeline, undoStack);
    const bool muted = id == "travel_chapters_v1";
    const int count = talking ? 2 : EditTemplates::find(id, 1)["minInputs"].toInt();
    const int total = photo ? 1001 : talking ? 45 : 101;
    QJsonArray inputs, texts;
    QVector<int> sourceIns;
    const auto allocated = talking ? QVector<int>{20, 25} : EditTemplates::allocate(total, count, id);
    for (int i = 0; i < count && !talking; ++i) {
        const int sourceIn = photo ? 0 : 7 + i * 3;
        sourceIns.append(sourceIn);
        inputs.append(QJsonObject{{"binId", source}, {"sourceInFrames", sourceIn},
            {"sourceDurationFrames", photo ? total : asset->getFramePlaytime() - sourceIn}});
        texts.append(QStringLiteral("第%1段已有文字").arg(i + 1));
        if (photo) REQUIRE(allocated[i] > fiveSeconds * 2);
    }
    QJsonObject parameters{{"totalFrames", total}, {"originalAudio", muted ? "mute" : "keep"}, {"texts", texts}};
    QJsonObject target{{"videoTrackId", video}};
    int originalClip = -1;
    QString music;
    int musicTrack = -1;
    if (talking) {
        REQUIRE(timeline->requestClipInsertion(source, video, 0, originalClip));
        REQUIRE(timeline->getClipSplitPartner(originalClip) >= 0);
        sourceIns = {5, 40};
        parameters["texts"] = QJsonArray{"用户确认的完整口播字幕"};
        parameters["ranges"] = QJsonArray{QJsonObject{{"start", 5}, {"end", 25}}, QJsonObject{{"start", 40}, {"end", 65}}};
    }
    if (id == "before_after_v1") {
        for (int track : timeline->getTracksIds(true)) if (track != mirror) { musicTrack = track; break; }
        REQUIRE(musicTrack >= 0);
        music = add(LocalCutTestPaths::fixture(QStringLiteral("avatar-templates-20261107/template-music-six-seconds.wav")), 0);
        REQUIRE(binModel->getClipByBinID(music)->getFramePlaytime() >= total);
        parameters["bgmBinId"] = music; parameters["bgmDb"] = -18;
        target["audioTrackId"] = musicTrack;
    }
    const auto state = bridge.readState();
    REQUIRE(state["frameRate"].toObject()["num"].toInt() == numerator);
    REQUIRE(state["frameRate"].toObject()["den"].toInt() == denominator);
    QJsonObject request{{"sessionId", state["sessionId"]}, {"projectId", state["projectId"]}, {"sequenceId", state["sequenceId"]},
        {"expectedRevision", state["revision"]}, {"requestId", "all-templates-locked"}, {"templateId", id}, {"templateVersion", 1},
        {"parameters", parameters}, {"target", target}};
    if (talking) request["clipId"] = originalClip; else request["inputs"] = inputs;
    const int initialHistory = undoStack->count(), initialClips = timeline->getClipsCount();
    const auto initialClipState = state["clips"];
    timeline->setTrackProperty(video, QStringLiteral("kdenlive:locked_track"), QStringLiteral("1"));
    request["expectedRevision"] = bridge.readState()["revision"];
    CHECK_FALSE(bridge.applyEditTemplate(request)["ok"].toBool());
    CHECK(undoStack->count() == initialHistory);
    CHECK(bridge.readState()["clips"] == initialClipState);
    timeline->setTrackProperty(video, QStringLiteral("kdenlive:locked_track"), QStringLiteral("0"));
    if (!photo && !muted) {
        timeline->setTrackProperty(mirror, QStringLiteral("kdenlive:locked_track"), QStringLiteral("1"));
        request["requestId"] = "all-templates-audio-locked";
        request["expectedRevision"] = bridge.readState()["revision"];
        CHECK_FALSE(bridge.applyEditTemplate(request)["ok"].toBool());
        CHECK(undoStack->count() == initialHistory);
        CHECK(bridge.readState()["clips"] == initialClipState);
        timeline->setTrackProperty(mirror, QStringLiteral("kdenlive:locked_track"), QStringLiteral("0"));
    }
    request["requestId"] = "all-templates-normal";
    request["expectedRevision"] = bridge.readState()["revision"];
    int publishedDuration = -1;
    const auto durationConnection = QObject::connect(timeline.get(), &TimelineModel::durationUpdated, timeline.get(),
        [&]() { publishedDuration = timeline->duration(); });
    const auto result = bridge.applyEditTemplate(request);
    INFO(QJsonDocument(result).toJson().constData());
    REQUIRE(result["ok"].toBool());
    CHECK(result["appliedFrames"].toInt() == total);
    CHECK(result["frameRate"].toObject()["num"].toInt() == numerator);
    CHECK(result["frameRate"].toObject()["den"].toInt() == denominator);
    CHECK(timeline->duration() == total);
    CHECK(publishedDuration == total);
    CHECK(undoStack->count() == initialHistory + 1);
    REQUIRE(timeline->getSubtitleModel());
    CHECK(timeline->getSubtitleModel()->getAllSubtitles().size() == (talking ? 1 : count));
    int videoCount = 0, sourceAudioCount = 0, musicCount = 0;
    for (const auto &value : bridge.readState()["clips"].toArray()) {
        const auto clip = value.toObject();
        const int clipId = clip["id"].toInt(), track = timeline->getClipTrackId(clipId);
        if (timeline->getClipBinId(clipId) == music && !music.isEmpty()) {
            ++musicCount;
            CHECK(track == musicTrack); CHECK(track != mirror);
            CHECK(timeline->getClipPosition(clipId) == 0);
            CHECK(timeline->getClipInOut(clipId).first == 0);
            CHECK(timeline->getClipPlaytime(clipId) == total);
            CHECK(clip["effectIds"].toArray().contains(QStringLiteral("volume")));
            const auto stack = timeline->getClipEffectStackModel(clipId);
            REQUIRE(stack->rowCount() == 1);
            const auto gain = std::static_pointer_cast<EffectItemModel>(stack->getEffectStackRow(0));
            CHECK(QString::fromUtf8(gain->filter().get("level")) == QStringLiteral("-18dB"));
            continue;
        }
        if (timeline->isAudioTrack(track)) { ++sourceAudioCount; CHECK(track == mirror); continue; }
        ++videoCount;
        const int position = timeline->getClipPosition(clipId);
        int slot = 0, expectedPosition = 0;
        while (slot < allocated.size() && expectedPosition != position) expectedPosition += allocated[slot++];
        REQUIRE(slot < allocated.size());
        CHECK(timeline->getClipPlaytime(clipId) == allocated[slot]);
        CHECK(timeline->getClipInOut(clipId).first == sourceIns[slot]);
        CHECK(timeline->getClipInOut(clipId).second == sourceIns[slot] + allocated[slot] - 1);
        const int partner = timeline->getClipSplitPartner(clipId);
        if (photo || muted) CHECK(partner == -1);
        else {
            REQUIRE(partner >= 0);
            CHECK(timeline->getClipInOut(partner) == timeline->getClipInOut(clipId));
            CHECK(timeline->getClipPosition(partner) == position);
            CHECK(timeline->getClipPlaytime(partner) == allocated[slot]);
        }
    }
    CHECK(videoCount == count);
    CHECK(sourceAudioCount == (photo || muted ? 0 : count));
    CHECK(musicCount == (music.isEmpty() ? 0 : 1));
    const int completedClips = timeline->getClipsCount();
    undoStack->undo();
    CHECK(timeline->getClipsCount() == initialClips);
    CHECK(bridge.readState()["clips"] == initialClipState);
    undoStack->redo();
    CHECK(timeline->getClipsCount() == completedClips);
    CHECK(timeline->duration() == total);
    QObject::disconnect(durationConnection);
}

TEST_CASE("Template story uses native source ranges and one undo transaction", "[AssistantTemplates]")
{
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    assignTestDocumentId(document);
    pCore->projectManager()->testSetDocument(&document);
    struct CloseDocument { ~CloseDocument() { pCore->projectManager()->closeCurrentDocument(false, false); } } closeDocument;
    KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const int track = timeline->getTrackIndexFromPosition(2);
    const QString path = LocalCutTestPaths::fixture(QStringLiteral("language-effects-20261107/运镜测试.png"));
    REQUIRE(QFile::exists(path));
    auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), path.toUtf8().constData());
    REQUIRE(producer->is_valid());
    producer->set("length", 500);
    producer->set_in_and_out(0, 499);
    QString bin;
    Fun undo = []() { return true; }, redo = []() { return true; };
    REQUIRE(binModel->requestAddBinClip(bin, producer, binModel->getRootFolder()->clipId(), undo, redo));
    EditBridge bridge;
    bridge.bindContext(document.uuid().toString(), timeline, undoStack);
    const auto listed = bridge.listEditTemplates()["availableInputs"].toArray();
    REQUIRE(listed.size() == 1);
    CHECK(listed.first().toObject()["binId"].toString() == bin);
    CHECK(listed.first().toObject()["isImage"].toBool());
    CHECK(listed.first().toObject()["durationFrames"].toInt() == 0);
    CHECK_FALSE(listed.first().toObject().contains("resource"));
    const auto state = bridge.readState();
    QJsonObject request{{"sessionId", state["sessionId"]}, {"projectId", state["projectId"]}, {"sequenceId", state["sequenceId"]},
        {"expectedRevision", state["revision"]}, {"requestId", "template-native"}, {"templateId", "before_after_v1"}, {"templateVersion", 1},
        {"inputs", QJsonArray{QJsonObject{{"binId", bin}, {"sourceInFrames", 0}, {"sourceDurationFrames", 100}},
                              QJsonObject{{"binId", bin}, {"sourceInFrames", 0}, {"sourceDurationFrames", 100}}}},
        {"parameters", QJsonObject{{"totalFrames", 101}, {"originalAudio", "mute"}, {"texts", QJsonArray{"原始", "调整"}}}},
        {"target", QJsonObject{{"videoTrackId", track}}}};
    const int history = undoStack->count();
    SECTION("success and replay") {
        const auto result = bridge.applyEditTemplate(request);
        INFO(QJsonDocument(result).toJson().constData());
        REQUIRE(result["ok"].toBool());
        CHECK(result["createdClipIds"].toArray().size() == 2);
        CHECK(timeline->duration() == 101);
        CHECK(undoStack->count() == history + 1);
        CHECK(bridge.applyEditTemplate(request)["ok"].toBool());
        CHECK(timeline->getClipsCount() == 2);
        auto &profile = pCore->getProjectProfile();
        struct RestoreRate {
            Mlt::Profile &profile;
            int numerator, denominator;
            ~RestoreRate() { profile.set_frame_rate(numerator, denominator); }
        } restoreRate{profile, profile.frame_rate_num(), profile.frame_rate_den()};
        profile.set_frame_rate(restoreRate.numerator == 30000 && restoreRate.denominator == 1001 ? 25 : 30000,
                               restoreRate.numerator == 30000 && restoreRate.denominator == 1001 ? 1 : 1001);
        const int replayHistory = undoStack->count();
        CHECK(bridge.applyEditTemplate(request) == result);
        CHECK(undoStack->count() == replayHistory);
        profile.set_frame_rate(restoreRate.numerator, restoreRate.denominator);
        auto conflicting = request;
        conflicting["templateId"] = "photo_story_v1";
        CHECK(bridge.applyEditTemplate(conflicting)["code"].toString() == "request_id_conflict");
        undoStack->undo();
        CHECK(timeline->getClipsCount() == 0);
        undoStack->redo();
        CHECK(timeline->getClipsCount() == 2);
        CHECK(timeline->duration() == 101);
    }
    SECTION("short source rejects without history") {
        auto inputs = request["inputs"].toArray();
        auto input = inputs[1].toObject(); input["sourceDurationFrames"] = 1; inputs[1] = input; request["inputs"] = inputs;
        CHECK_FALSE(bridge.applyEditTemplate(request)["ok"].toBool());
        CHECK(timeline->getClipsCount() == 0);
        CHECK(undoStack->count() == history);
    }
    SECTION("stale revision rejects without history") {
        request["expectedRevision"] = -1;
        CHECK(bridge.applyEditTemplate(request)["code"].toString() == "stale_state");
        CHECK(timeline->getClipsCount() == 0);
    }
    SECTION("unknown and mutually exclusive fields reject without history") {
        request["clipId"] = 0;
        CHECK(bridge.applyEditTemplate(request)["code"].toString() == "invalid_template_parameters");
        request.remove("clipId");
        request["execute"] = "untrusted";
        CHECK(bridge.applyEditTemplate(request)["code"].toString() == "invalid_template_parameters");
        CHECK(timeline->getClipsCount() == 0);
        CHECK(undoStack->count() == history);
    }
    SECTION("composition without clips is not an empty timeline") {
        QString transition;
        for (const auto &entry : TransitionsRepository::get()->getNames()) {
            if (TransitionsRepository::get()->isComposition(entry.first)) { transition = entry.first; break; }
        }
        REQUIRE_FALSE(transition.isEmpty());
        int composition = -1;
        REQUIRE(timeline->requestCompositionInsertion(transition, track, 10, 30, nullptr, composition));
        REQUIRE(timeline->getClipsCount() == 0);
        REQUIRE(timeline->getCompositionsCount() == 1);
        request["expectedRevision"] = bridge.readState()["revision"];
        const int compositionHistory = undoStack->count();
        const int compositionTrack = timeline->getItemTrackId(composition);
        const int compositionStart = timeline->getCompositionPosition(composition);
        const int compositionLength = timeline->getCompositionPlaytime(composition);
        CHECK(bridge.applyEditTemplate(request)["code"].toString() == "template_requires_empty_timeline");
        CHECK(timeline->getClipsCount() == 0);
        CHECK(timeline->getCompositionsCount() == 1);
        CHECK(timeline->getItemTrackId(composition) == compositionTrack);
        CHECK(timeline->getCompositionPosition(composition) == compositionStart);
        CHECK(timeline->getCompositionPlaytime(composition) == compositionLength);
        CHECK(undoStack->count() == compositionHistory);
    }
    SECTION("unready source rejects without insertion") {
        binModel->getClipByBinID(bin)->setClipStatus(FileStatus::StatusWaiting);
        CHECK(bridge.applyEditTemplate(request)["code"].toString() == "invalid_template_source");
        CHECK(timeline->getClipsCount() == 0);
        CHECK(undoStack->count() == history);
    }
    SECTION("short BGM rejects without insertion") {
        const QString audioPath = LocalCutTestPaths::fixture(QStringLiteral("avatar-20261107/speech2.wav"));
        REQUIRE(QFile::exists(audioPath));
        auto audio = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), audioPath.toUtf8().constData());
        REQUIRE(audio->is_valid());
        QString music;
        Fun addUndo = []() { return true; }, addRedo = []() { return true; };
        REQUIRE(binModel->requestAddBinClip(music, audio, binModel->getRootFolder()->clipId(), addUndo, addRedo));
        auto parameters = request["parameters"].toObject(); parameters["bgmBinId"] = music; parameters["bgmDb"] = -18;
        request["parameters"] = parameters;
        auto target = request["target"].toObject(); target["audioTrackId"] = timeline->getMirrorTrackId(track); request["target"] = target;
        request["expectedRevision"] = bridge.readState()["revision"];
        CHECK(bridge.applyEditTemplate(request)["code"].toString() == "invalid_template_bgm");
        CHECK(timeline->getClipsCount() == 0);
        CHECK(undoStack->count() == history);
    }
    SECTION("private operation cannot be called through batch") {
        auto operation = request; operation["type"] = "template_story";
        request["operations"] = QJsonArray{operation};
        CHECK(bridge.applyBatch(request)["code"].toString() == "unsupported_operation");
        CHECK(timeline->getClipsCount() == 0);
    }
    SECTION("mid operation effect rejection rolls back inserted image") {
        request["templateId"] = "photo_story_v1";
        auto inputs = request["inputs"].toArray(); inputs.append(inputs.first()); request["inputs"] = inputs;
        auto parameters = request["parameters"].toObject();
        parameters["totalFrames"] = 3;
        parameters["texts"] = QJsonArray{"一", "二", "三"};
        request["parameters"] = parameters;
        CHECK_FALSE(bridge.applyEditTemplate(request)["ok"].toBool());
        CHECK(timeline->getClipsCount() == 0);
        CHECK(timeline->duration() == 0);
        CHECK(undoStack->count() == history);
    }
    SECTION("finite media retains nonzero source in and native AV partners") {
        const QString media = LocalCutTestPaths::fixture(QStringLiteral("media/中文 空格 样片.mp4"));
        REQUIRE(QFile::exists(media));
        auto finite = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), media.toUtf8().constData());
        REQUIRE(finite->is_valid());
        finite->set("length", 100); finite->set_in_and_out(0, 99);
        QString source;
        Fun addUndo = []() { return true; }, addRedo = []() { return true; };
        REQUIRE(binModel->requestAddBinClip(source, finite, binModel->getRootFolder()->clipId(), addUndo, addRedo));
        REQUIRE(binModel->getClipByBinID(source)->hasAudio());
        REQUIRE(binModel->getClipByBinID(source)->hasVideo());
        bool listedSource = false;
        for (const auto &value : bridge.listEditTemplates()["availableInputs"].toArray()) {
            const auto asset = value.toObject();
            if (asset["binId"].toString() != source) continue;
            listedSource = true;
            CHECK(asset["durationFrames"].toInt() == binModel->getClipByBinID(source)->getFramePlaytime());
            CHECK(asset["durationFrames"].toInt() == 100);
            CHECK(asset["ready"].toBool());
        }
        REQUIRE(listedSource);
        request["inputs"] = QJsonArray{QJsonObject{{"binId", source}, {"sourceInFrames", 20}, {"sourceDurationFrames", 80}},
                                        QJsonObject{{"binId", source}, {"sourceInFrames", 40}, {"sourceDurationFrames", 60}}};
        auto parameters = request["parameters"].toObject(); parameters["originalAudio"] = "keep"; request["parameters"] = parameters;
        request["expectedRevision"] = bridge.readState()["revision"];
        const auto result = bridge.applyEditTemplate(request);
        INFO(QJsonDocument(result).toJson().constData());
        REQUIRE(result["ok"].toBool());
        CHECK(result["createdClipIds"].toArray().size() == 4);
        for (const auto &value : bridge.readState()["clips"].toArray()) {
            const int id = value.toObject()["id"].toInt();
            const int partner = timeline->getClipSplitPartner(id);
            REQUIRE(partner >= 0);
            CHECK(timeline->getClipInOut(partner) == timeline->getClipInOut(id));
            CHECK(timeline->getClipPosition(partner) == timeline->getClipPosition(id));
            CHECK(timeline->getClipInOut(id).first == (timeline->getClipPosition(id) == 0 ? 20 : 40));
        }
    }
}
