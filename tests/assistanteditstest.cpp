/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "localcut_test_paths.hpp"
#include "assistant/editbridge.h"
#include "catch.hpp"
#include "test_utils.hpp"
#include "bin/model/subtitlemodel.hpp"
#include "bin/projectfolder.h"
#include "effects/effectsrepository.hpp"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "effects/effectstack/model/effectitemmodel.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <limits>

namespace {
QJsonObject batch(const QJsonObject &state, const QString &requestId, const QJsonArray &operations)
{
    return {{QStringLiteral("sessionId"), state.value(QStringLiteral("sessionId"))},
            {QStringLiteral("projectId"), state.value(QStringLiteral("projectId"))},
            {QStringLiteral("sequenceId"), state.value(QStringLiteral("sequenceId"))},
            {QStringLiteral("expectedRevision"), state.value(QStringLiteral("revision"))},
            {QStringLiteral("requestId"), requestId},
            {QStringLiteral("operations"), operations}};
}

QJsonObject effectParameters(const QJsonObject &state, int clipId, const QString &effectId)
{
    for (const auto &clip : state["clips"].toArray()) {
        if (clip.toObject()["id"].toInt() != clipId) continue;
        for (const auto &effect : clip.toObject()["effectDetails"].toArray()) {
            if (effect.toObject()["id"].toString() == effectId) return effect.toObject()["parameters"].toObject();
        }
    }
    return {};
}
}

TEST_CASE("Assistant finalizes a still image insertion and publishes its real duration", "[AssistantEffects][AssistantInsertion]")
{
    auto mode = TimelineMode::NormalEdit;
    SECTION("Normal edit") {}
    SECTION("Insert edit") { mode = TimelineMode::InsertEdit; }
    SECTION("Overwrite edit") { mode = TimelineMode::OverwriteEdit; }
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    struct CloseDocument {
        ~CloseDocument() { pCore->projectManager()->closeCurrentDocument(false, false); }
    } closeDocument;
    KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    timeline->setEditMode(mode);
    const int track = timeline->getTrackIndexFromPosition(2);
    const QString sourcePath = LocalCutTestPaths::fixture(QStringLiteral("language-effects-20261107/运镜测试.png"));
    REQUIRE(QFile::exists(sourcePath));
    auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), sourcePath.toUtf8().constData());
    REQUIRE(producer->is_valid());
    producer->set("length", 125);
    producer->set_in_and_out(0, 124);
    QString binId;
    Fun addUndo = []() { return true; }, addRedo = []() { return true; };
    REQUIRE(binModel->requestAddBinClip(binId, producer, binModel->getRootFolder()->clipId(), addUndo, addRedo));
    REQUIRE(binModel->getClipByBinID(binId)->clipType() == ClipType::Image);
    REQUIRE(timeline->duration() == 0);
    int publishedDuration = 0;
    const auto connection = QObject::connect(timeline.get(), &TimelineModel::durationUpdated, timeline.get(),
                                            [&]() { publishedDuration = timeline->duration(); });
    EditBridge bridge;
    bridge.bindContext(document.uuid().toString(), timeline, undoStack);
    const int history = undoStack->count(), tracks = timeline->rowCount();
    const QJsonObject insert{{"type", "insert"}, {"binId", binId}, {"trackId", track}, {"frame", 0}};
    const auto result = bridge.applyBatch(batch(bridge.readState(), QStringLiteral("still-insert"), {insert}));
    REQUIRE(result["ok"].toBool());
    REQUIRE(result["createdClipIds"].toArray().size() == 1);
    const int clip = result["createdClipIds"].toArray().first().toInt();
    REQUIRE(timeline->getClipPlaytime(clip) == 125);
    REQUIRE(timeline->duration() == 125);
    CHECK(publishedDuration == 125);
    CHECK(timeline->data(timeline->makeClipIndexFromID(clip), TimelineModel::FakeTrackIdRole).toInt() == -1);
    CHECK(timeline->rowCount() == tracks);
    CHECK(undoStack->count() == history + 1);
    undoStack->undo();
    REQUIRE(timeline->getClipsCount() == 0);
    CHECK(publishedDuration == 0);
    undoStack->redo();
    REQUIRE(timeline->getClipsCount() == 1);
    REQUIRE(timeline->duration() == 125);
    CHECK(publishedDuration == 125);
    CHECK(timeline->data(timeline->makeClipIndexFromID(clip), TimelineModel::FakeTrackIdRole).toInt() == -1);
    auto secondInsert = insert;
    secondInsert["frame"] = 150;
    const QJsonObject invalid{{"type", "move"}, {"clipId", -1}, {"trackId", track}, {"frame", 0}};
    REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("still-rollback"), {secondInsert, invalid}))["ok"].toBool());
    REQUIRE(timeline->getClipsCount() == 1);
    REQUIRE(timeline->duration() == 125);
    CHECK(publishedDuration == 125);
    CHECK(undoStack->count() == history + 1);
    CHECK(timeline->rowCount() == tracks);
    QObject::disconnect(connection);
}

TEST_CASE("Assistant reads and edits the native timeline transactionally", "[Assistant]")
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
    EditBridge bridge;
    bridge.bindContext(document.uuid().toString(), timeline, undoStack);
    const auto state = bridge.readState();
    REQUIRE_FALSE(state.value(QStringLiteral("sessionId")).toString().isEmpty());
    REQUIRE(state.value(QStringLiteral("clips")).toArray().size() == 1);
    const QJsonObject split{{QStringLiteral("type"), QStringLiteral("split")},
                            {QStringLiteral("clipId"), clip}, {QStringLiteral("frame"), 50}};

    SECTION("A successful batch has one native undo and a replay cannot duplicate it")
    {
        const int history = undoStack->count();
        const auto request = batch(state, QStringLiteral("split-1"), {split});
        const auto result = bridge.applyBatch(request);
        REQUIRE(result.value(QStringLiteral("ok")).toBool());
        REQUIRE(timeline->getClipsCount() == 2);
        REQUIRE(timeline->getClipPlaytime(clip) == 50);
        REQUIRE(undoStack->count() == history + 1);
        REQUIRE(bridge.applyBatch(request).value(QStringLiteral("ok")).toBool());
        REQUIRE(undoStack->count() == history + 1);
        undoStack->undo();
        REQUIRE(timeline->getClipsCount() == 1);
        REQUIRE(timeline->getClipPlaytime(clip) == 100);
        undoStack->redo();
        REQUIRE(timeline->getClipsCount() == 2);
    }
    SECTION("Failure after an earlier successful operation rolls back the whole batch")
    {
        const int history = undoStack->count();
        const QJsonObject overlappingMove{{QStringLiteral("type"), QStringLiteral("move")},
                                          {QStringLiteral("clipId"), clip}, {QStringLiteral("trackId"), track},
                                          {QStringLiteral("frame"), 60}};
        const auto result = bridge.applyBatch(batch(state, QStringLiteral("rollback-1"), {split, overlappingMove}));
        REQUIRE_FALSE(result.value(QStringLiteral("ok")).toBool());
        REQUIRE(timeline->getClipsCount() == 1);
        REQUIRE(timeline->getClipPosition(clip) == 0);
        REQUIRE(timeline->getClipPlaytime(clip) == 100);
        REQUIRE(undoStack->count() == history);
    }
    SECTION("A manual edit invalidates an earlier assistant state")
    {
        REQUIRE(timeline->requestClipMove(clip, track, 100));
        const auto result = bridge.applyBatch(batch(state, QStringLiteral("stale-1"), {split}));
        REQUIRE_FALSE(result.value(QStringLiteral("ok")).toBool());
        REQUIRE(result.value(QStringLiteral("code")).toString() == QStringLiteral("stale_state"));
        REQUIRE(timeline->getClipPosition(clip) == 100);
        REQUIRE(timeline->getClipsCount() == 1);
    }
    SECTION("Missing or malformed state fields are invalid requests without changing the project")
    {
        const int history = undoStack->count();
        const auto request = batch(state, QStringLiteral("invalid-context"), {split});
        const auto checkInvalid = [&](const QJsonObject &candidate, const QString &field) {
            const auto result = bridge.applyBatch(candidate);
            REQUIRE_FALSE(result["ok"].toBool());
            REQUIRE(result["code"].toString() == QStringLiteral("invalid_request"));
            REQUIRE(result["message"].toString().contains(field));
            REQUIRE(result["message"].toString().contains(QStringLiteral("read_project_state")));
            REQUIRE(bridge.readState() == state);
            REQUIRE(undoStack->count() == history);
        };
        for (const auto &field : {QStringLiteral("sessionId"), QStringLiteral("projectId"), QStringLiteral("sequenceId")}) {
            auto candidate = request;
            candidate.remove(field);
            checkInvalid(candidate, field);
            for (const auto &value : QJsonArray{QJsonValue::Null, "", "  ", 1, true, QJsonArray{}, QJsonObject{}}) {
                candidate[field] = value;
                checkInvalid(candidate, field);
            }
        }
        auto candidate = request;
        candidate.remove(QStringLiteral("expectedRevision"));
        checkInvalid(candidate, QStringLiteral("expectedRevision"));
        for (const auto &value : QJsonArray{QJsonValue::Null, "2", true, QJsonArray{}, QJsonObject{}, 2.5, -1, 1e100}) {
            candidate["expectedRevision"] = value;
            checkInvalid(candidate, QStringLiteral("expectedRevision"));
        }
    }
    SECTION("Well formed but different state identifiers and revisions remain stale")
    {
        const auto request = batch(state, QStringLiteral("different-context"), {split});
        for (const auto &field : {QStringLiteral("sessionId"), QStringLiteral("projectId"), QStringLiteral("sequenceId"),
                                  QStringLiteral("expectedRevision")}) {
            auto candidate = request;
            candidate[field] = field == QStringLiteral("expectedRevision") ? QJsonValue(state["revision"].toInteger() + 1)
                                                                            : QJsonValue(QUuid::createUuid().toString());
            const auto result = bridge.applyBatch(candidate);
            REQUIRE_FALSE(result["ok"].toBool());
            REQUIRE(result["code"].toString() == QStringLiteral("stale_state"));
            REQUIRE(bridge.readState() == state);
        }
    }
    SECTION("Reusing a request id for different edits is rejected")
    {
        REQUIRE(bridge.applyBatch(batch(state, QStringLiteral("unique-1"), {split})).value(QStringLiteral("ok")).toBool());
        const auto current = bridge.readState();
        const QJsonObject remove{{QStringLiteral("type"), QStringLiteral("delete")}, {QStringLiteral("clipId"), clip}};
        REQUIRE_FALSE(bridge.applyBatch(batch(current, QStringLiteral("unique-1"), {remove})).value(QStringLiteral("ok")).toBool());
        REQUIRE(timeline->getClipsCount() == 2);
    }
    SECTION("Undo invalidates state even when the timeline returns to the same contents")
    {
        REQUIRE(timeline->requestClipMove(clip, track, 100));
        undoStack->undo();
        REQUIRE(timeline->getClipPosition(clip) == 0);
        REQUIRE_FALSE(bridge.applyBatch(batch(state, QStringLiteral("before-undo"), {split})).value(QStringLiteral("ok")).toBool());
    }
    SECTION("Trimming and deleting remain native undoable operations")
    {
        const QJsonObject trim{{QStringLiteral("type"), QStringLiteral("trim")}, {QStringLiteral("clipId"), clip},
                               {QStringLiteral("side"), QStringLiteral("right")}, {QStringLiteral("duration"), 75}};
        REQUIRE(bridge.applyBatch(batch(state, QStringLiteral("trim-1"), {trim})).value(QStringLiteral("ok")).toBool());
        REQUIRE(timeline->getClipPlaytime(clip) == 75);
        const QJsonObject remove{{QStringLiteral("type"), QStringLiteral("delete")}, {QStringLiteral("clipId"), clip}};
        REQUIRE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("delete-1"), {remove})).value(QStringLiteral("ok")).toBool());
        REQUIRE(timeline->getClipsCount() == 0);
        undoStack->undo();
        REQUIRE(timeline->getClipPlaytime(clip) == 75);
        undoStack->undo();
        REQUIRE(timeline->getClipPlaytime(clip) == 100);
    }
    SECTION("Moving a group preserves its relative timing and one undo restores it")
    {
        int secondClip = -1;
        REQUIRE(timeline->requestClipInsertion(binId, track, 150, secondClip));
        REQUIRE(timeline->requestClipsGroup({clip, secondClip}) > -1);
        const auto groupedState = bridge.readState();
        const int history = undoStack->count();
        const QJsonObject move{{QStringLiteral("type"), QStringLiteral("move")}, {QStringLiteral("clipId"), clip},
                               {QStringLiteral("trackId"), track}, {QStringLiteral("frame"), 25}};
        REQUIRE(bridge.applyBatch(batch(groupedState, QStringLiteral("group-move"), {move})).value(QStringLiteral("ok")).toBool());
        REQUIRE(timeline->getClipPosition(clip) == 25);
        REQUIRE(timeline->getClipPosition(secondClip) == 175);
        REQUIRE(undoStack->count() == history + 1);
        undoStack->undo();
        REQUIRE(timeline->getClipPosition(clip) == 0);
        REQUIRE(timeline->getClipPosition(secondClip) == 150);
        REQUIRE(timeline->isInGroup(clip));
    }
    SECTION("A locked track is not partially edited")
    {
        timeline->setTrackProperty(track, QStringLiteral("kdenlive:locked_track"), QStringLiteral("1"));
        const auto lockedState = bridge.readState();
        REQUIRE_FALSE(bridge.applyBatch(batch(lockedState, QStringLiteral("locked-1"), {split})).value(QStringLiteral("ok")).toBool());
        REQUIRE(timeline->getClipsCount() == 1);
    }
    SECTION("Stopping makes previously prepared edits stale")
    {
        bridge.cancelPending();
        REQUIRE_FALSE(bridge.applyBatch(batch(state, QStringLiteral("cancelled-1"), {split})).value(QStringLiteral("ok")).toBool());
        REQUIRE(timeline->getClipsCount() == 1);
    }
    SECTION("An unknown operation does not mutate the timeline")
    {
        const QJsonObject unknown{{QStringLiteral("type"), QStringLiteral("not-supported")}};
        REQUIRE_FALSE(bridge.applyBatch(batch(state, QStringLiteral("unknown-1"), {unknown})).value(QStringLiteral("ok")).toBool());
        REQUIRE(timeline->getClipsCount() == 1);
    }
    SECTION("Invalid frame values cannot be silently truncated")
    {
        auto invalidSplit = split;
        invalidSplit.insert(QStringLiteral("frame"), 50.5);
        REQUIRE_FALSE(bridge.applyBatch(batch(state, QStringLiteral("fraction-1"), {invalidSplit})).value(QStringLiteral("ok")).toBool());
        REQUIRE(timeline->getClipsCount() == 1);
    }
    SECTION("New project context invalidates old session requests")
    {
        bridge.bindContext(QStringLiteral("another-project"), timeline, undoStack);
        REQUIRE_FALSE(bridge.applyBatch(batch(state, QStringLiteral("old-project-1"), {split})).value(QStringLiteral("ok")).toBool());
        REQUIRE(timeline->getClipsCount() == 1);
    }
    SECTION("Binding the same context keeps requests valid")
    {
        bridge.bindContext(document.uuid().toString(), timeline, undoStack);
        REQUIRE(bridge.readState().value(QStringLiteral("sessionId")) == state.value(QStringLiteral("sessionId")));
        REQUIRE(bridge.applyBatch(batch(state, QStringLiteral("same-context"), {split})).value(QStringLiteral("ok")).toBool());
    }
    SECTION("A failed batch preserves the redo branch and selection")
    {
        REQUIRE(timeline->requestClipMove(clip, track, 150));
        undoStack->undo();
        timeline->requestAddToSelection(clip, true, true);
        const auto selectedState = bridge.readState();
        const int history = undoStack->count();
        const int index = undoStack->index();
        const QJsonObject overlappingMove{{QStringLiteral("type"), QStringLiteral("move")},
                                          {QStringLiteral("clipId"), clip}, {QStringLiteral("trackId"), track},
                                          {QStringLiteral("frame"), 60}};
        REQUIRE_FALSE(bridge.applyBatch(batch(selectedState, QStringLiteral("rollback-redo"), {split, overlappingMove}))
                          .value(QStringLiteral("ok")).toBool());
        REQUIRE(undoStack->count() == history);
        REQUIRE(undoStack->index() == index);
        REQUIRE(undoStack->canRedo());
        REQUIRE(timeline->getCurrentSelection() == std::unordered_set<int>{clip});
        REQUIRE(timeline->singleSelectionMode());
        undoStack->redo();
        REQUIRE(timeline->getClipPosition(clip) == 150);
    }
    SECTION("Temporary selection groups do not move unrelated selected clips")
    {
        int secondClip = -1;
        REQUIRE(timeline->requestClipInsertion(binId, track, 150, secondClip));
        REQUIRE(timeline->requestSetSelection({clip, secondClip}));
        const QJsonObject move{{QStringLiteral("type"), QStringLiteral("move")}, {QStringLiteral("clipId"), clip},
                               {QStringLiteral("trackId"), track}, {QStringLiteral("frame"), 25}};
        REQUIRE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("selection-move"), {move}))
                    .value(QStringLiteral("ok")).toBool());
        REQUIRE(timeline->getClipPosition(clip) == 25);
        REQUIRE(timeline->getClipPosition(secondClip) == 150);
    }
    SECTION("Single selection does not break an ordinary linked group")
    {
        int secondClip = -1;
        REQUIRE(timeline->requestClipInsertion(binId, track, 150, secondClip));
        REQUIRE(timeline->requestClipsGroup({clip, secondClip}) > -1);
        timeline->requestAddToSelection(clip, true, true);
        const QJsonObject move{{QStringLiteral("type"), QStringLiteral("move")}, {QStringLiteral("clipId"), clip},
                               {QStringLiteral("trackId"), track}, {QStringLiteral("frame"), 25}};
        REQUIRE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("single-group-move"), {move}))
                    .value(QStringLiteral("ok")).toBool());
        REQUIRE(timeline->getClipPosition(clip) == 25);
        REQUIRE(timeline->getClipPosition(secondClip) == 175);
        REQUIRE(timeline->isInGroup(secondClip));
    }
    SECTION("State uses stable native ids and ignores thumbnail noise")
    {
        const auto clips = state.value(QStringLiteral("clips")).toArray();
        REQUIRE(clips.first().toObject().value(QStringLiteral("id")).toInt() == clip);
        REQUIRE(clips.first().toObject().value(QStringLiteral("binId")).toString() == binId);
        REQUIRE(clips.first().toObject().value(QStringLiteral("trackId")).toInt() == track);
        REQUIRE(state.value(QStringLiteral("assets")).toArray().size() >= 1);
        const auto ix = timeline->makeClipIndexFromID(clip);
        timeline->notifyChange(ix, ix, TimelineModel::ClipThumbRole);
        REQUIRE(bridge.readState().value(QStringLiteral("revision")) == state.value(QStringLiteral("revision")));
        timeline->setTrackProperty(track, QStringLiteral("kdenlive:locked_track"), QStringLiteral("1"));
        REQUIRE_FALSE(bridge.applyBatch(batch(state, QStringLiteral("direct-track-change"), {split}))
                          .value(QStringLiteral("ok")).toBool());
    }
    SECTION("Inserting an existing bin asset returns native ids and is undoable")
    {
        const int history = undoStack->count();
        const QJsonObject insert{{QStringLiteral("type"), QStringLiteral("insert")}, {QStringLiteral("binId"), binId},
                                 {QStringLiteral("trackId"), track}, {QStringLiteral("frame"), 150}};
        const auto result = bridge.applyBatch(batch(state, QStringLiteral("insert-asset"), {insert}));
        REQUIRE(result.value(QStringLiteral("ok")).toBool());
        REQUIRE(result.value(QStringLiteral("createdClipIds")).toArray().size() == 1);
        const int inserted = result.value(QStringLiteral("createdClipIds")).toArray().first().toInt();
        REQUIRE(timeline->isClip(inserted));
        REQUIRE(timeline->getClipBinId(inserted) == binId);
        REQUIRE(timeline->getClipPosition(inserted) == 150);
        REQUIRE(undoStack->count() == history + 1);
        undoStack->undo();
        REQUIRE(timeline->getClipsCount() == 1);
        undoStack->redo();
        REQUIRE(timeline->isClip(inserted));
    }
    SECTION("Volume changes the audio partner with an explicit decibel gain")
    {
        const QString avBin = KdenliveTests::createAVProducer(pCore->getProjectProfile(), binModel);
        int avClip = -1;
        REQUIRE(timeline->requestClipInsertion(avBin, track, 150, avClip, true, false, false));
        const int audio = timeline->getClipSplitPartner(avClip);
        REQUIRE(audio >= 0);
        REQUIRE(timeline->isAudioTrack(timeline->getClipTrackId(audio)));
        const auto audioStack = timeline->getClipEffectStackModel(audio);
        const int previous = audioStack->rowCount();
        const QJsonObject volume{{QStringLiteral("type"), QStringLiteral("volume")}, {QStringLiteral("clipId"), avClip},
                                 {QStringLiteral("db"), -6.0}};
        REQUIRE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("volume-av"), {volume}))
                    .value(QStringLiteral("ok")).toBool());
        REQUIRE(audioStack->rowCount() == previous + 1);
        REQUIRE(audioStack->getAssetModelById(QStringLiteral("volume"))->getParamFromName(QStringLiteral("level")).toString()
                    .contains(QStringLiteral("-6")));
        undoStack->undo();
        REQUIRE(audioStack->rowCount() == previous);
        REQUIRE(timeline->isInGroup(avClip));
    }
    SECTION("Crop uses the native effect and rejects an empty remaining image")
    {
        const auto stack = timeline->getClipEffectStackModel(clip);
        const int previous = stack->rowCount();
        const QJsonObject crop{{QStringLiteral("type"), QStringLiteral("crop")}, {QStringLiteral("clipId"), clip},
                               {QStringLiteral("top"), 10}, {QStringLiteral("left"), 20},
                               {QStringLiteral("bottom"), 30}, {QStringLiteral("right"), 40}};
        REQUIRE(bridge.applyBatch(batch(state, QStringLiteral("crop-1"), {crop})).value(QStringLiteral("ok")).toBool());
        REQUIRE(stack->rowCount() == previous + 1);
        const auto effect = stack->getAssetModelById(QStringLiteral("crop"));
        REQUIRE(effect->getParamFromName(QStringLiteral("left")).toInt() == 20);
        REQUIRE(effect->getParamFromName(QStringLiteral("bottom")).toInt() == 30);
        auto invalid = crop;
        invalid.insert(QStringLiteral("left"), state.value(QStringLiteral("width")));
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("crop-empty"), {invalid}))
                          .value(QStringLiteral("ok")).toBool());
        undoStack->undo();
        REQUIRE(stack->rowCount() == previous);
    }
    SECTION("Watermark treatments use loaded native filters with bounded pixel regions")
    {
        const auto stack = timeline->getClipEffectStackModel(clip);
        for (const auto &method : {QStringLiteral("repair"), QStringLiteral("blur")}) {
            const QString effectId = method == QStringLiteral("repair") ? QStringLiteral("avfilter.delogo") : QStringLiteral("obscure");
            const bool available = EffectsRepository::get()->exists(effectId);
            const int previous = stack->rowCount();
            const QJsonObject watermark{{QStringLiteral("type"), QStringLiteral("watermark")}, {QStringLiteral("clipId"), clip},
                                        {QStringLiteral("method"), method},
                                        {QStringLiteral("rect"), QJsonObject{{QStringLiteral("x"), 10}, {QStringLiteral("y"), 20},
                                                                             {QStringLiteral("width"), 100}, {QStringLiteral("height"), 80}}}};
            const auto result = bridge.applyBatch(batch(bridge.readState(), QStringLiteral("watermark-") + method, {watermark}));
            REQUIRE(result.value(QStringLiteral("ok")).toBool() == available);
            if (available) {
                REQUIRE(stack->rowCount() == previous + 1);
                const auto effect = stack->getAssetModelById(effectId);
                if (method == QStringLiteral("repair")) {
                    const auto nativeEffect = std::static_pointer_cast<EffectItemModel>(stack->getEffectStackRow(previous));
                    REQUIRE(nativeEffect->filter().get_int("av.x") == 10);
                    REQUIRE(nativeEffect->filter().get_int("av.w") == 100);
                    REQUIRE(nativeEffect->filter().get_int("av.show") == 0);
                } else {
                    REQUIRE(effect->getParamFromName(QStringLiteral("start")).toString().contains(QStringLiteral("100")));
                }
                undoStack->undo();
                REQUIRE(stack->rowCount() == previous);
            }
        }
        const QJsonObject invalid{{QStringLiteral("type"), QStringLiteral("watermark")}, {QStringLiteral("clipId"), clip},
                                  {QStringLiteral("method"), QStringLiteral("blur")},
                                  {QStringLiteral("rect"), QJsonObject{{QStringLiteral("x"), -1}, {QStringLiteral("y"), 20},
                                                                       {QStringLiteral("width"), 100}, {QStringLiteral("height"), 80}}}};
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("watermark-invalid"), {invalid}))
                          .value(QStringLiteral("ok")).toBool());
    }
    SECTION("Subtitles are editable native model entries in the same undo transaction")
    {
        const QJsonObject subtitle{{QStringLiteral("type"), QStringLiteral("subtitle")}, {QStringLiteral("start"), 10},
                                   {QStringLiteral("end"), 60}, {QStringLiteral("text"), QStringLiteral("Native editable subtitle")}};
        const int history = undoStack->count();
        REQUIRE(bridge.applyBatch(batch(state, QStringLiteral("subtitle-1"), {subtitle})).value(QStringLiteral("ok")).toBool());
        const auto model = timeline->getSubtitleModel();
        REQUIRE(model);
        REQUIRE(model->rowCount() == 1);
        REQUIRE(model->data(model->index(0), SubtitleModel::StartFrameRole).toInt() == 10);
        REQUIRE(model->data(model->index(0), SubtitleModel::EndFrameRole).toInt() == 60);
        REQUIRE(model->data(model->index(0), SubtitleModel::SubtitleRole).toString() == QStringLiteral("Native editable subtitle"));
        REQUIRE(undoStack->count() == history + 1);
        undoStack->undo();
        REQUIRE(model->rowCount() == 0);
        undoStack->redo();
        REQUIRE(model->rowCount() == 1);
        const QJsonObject impossible{{QStringLiteral("type"), QStringLiteral("move")}, {QStringLiteral("clipId"), clip},
                                    {QStringLiteral("trackId"), track}, {QStringLiteral("frame"), -1}};
        auto next = subtitle;
        next.insert(QStringLiteral("start"), 100);
        next.insert(QStringLiteral("end"), 150);
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("subtitle-rollback"), {next, impossible}))
                          .value(QStringLiteral("ok")).toBool());
        REQUIRE(model->rowCount() == 1);
        const int id = model->data(model->index(0), SubtitleModel::IdRole).toInt();
        const int textHistory = undoStack->count();
        const QJsonObject correction{{"type", "subtitle_text"}, {"subtitleId", id}, {"text", QStringLiteral("剪映测试")}};
        const auto result = bridge.applyBatch(batch(bridge.readState(), QStringLiteral("correct-text"), {correction}));
        INFO(QJsonDocument(result).toJson().toStdString());
        REQUIRE(result["ok"].toBool());
        REQUIRE(model->rowCount() == 1);
        REQUIRE(model->data(model->index(0), SubtitleModel::StartFrameRole).toInt() == 10);
        REQUIRE(model->data(model->index(0), SubtitleModel::EndFrameRole).toInt() == 60);
        REQUIRE(model->data(model->index(0), SubtitleModel::SubtitleRole).toString() == QStringLiteral("剪映测试"));
        REQUIRE(undoStack->count() == textHistory + 1);
        undoStack->undo();
        REQUIRE(model->data(model->index(0), SubtitleModel::SubtitleRole).toString() == QStringLiteral("Native editable subtitle"));
        undoStack->redo();
        REQUIRE(model->data(model->index(0), SubtitleModel::SubtitleRole).toString() == QStringLiteral("剪映测试"));
        auto changed = correction;
        changed["text"] = QStringLiteral("不应保留的修改");
        auto missing = correction;
        missing["subtitleId"] = 9999999;
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("text-rollback"), {changed, missing}))["ok"].toBool());
        REQUIRE(model->rowCount() == 1);
        REQUIRE(model->data(model->index(0), SubtitleModel::SubtitleRole).toString() == QStringLiteral("剪映测试"));
        REQUIRE(undoStack->count() == textHistory + 1);
        model->switchLocked();
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("text-locked"), {changed}))["ok"].toBool());
        REQUIRE(model->data(model->index(0), SubtitleModel::SubtitleRole).toString() == QStringLiteral("剪映测试"));
        model->switchLocked();
    }
}

TEST_CASE("Assistant adds local creative effects transactionally", "[AssistantEffects]")
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
    EditBridge bridge;
    bridge.bindContext(document.uuid().toString(), timeline, undoStack);
    const auto state = bridge.readState();
    SECTION("State reports real effect capabilities and native IDs")
    {
        const auto clips = state.value(QStringLiteral("clips")).toArray();
        const auto capabilities = state.value(QStringLiteral("effectCapabilities")).toObject();
        for (const auto &entry : {std::pair{"color_adjust", "avfilter.eq"}, std::pair{"photo_motion", "pan_zoom"},
                                  std::pair{"audio_normalize", "dynamic_loudness"}, std::pair{"chroma_key", "chroma"}}) {
            REQUIRE(capabilities.contains(QLatin1String(entry.first)));
            REQUIRE(capabilities.value(QLatin1String(entry.first)).toBool() == EffectsRepository::get()->exists(QLatin1String(entry.second)));
            REQUIRE(state.value(QStringLiteral("supportedOperations")).toArray().contains(QString::fromLatin1(entry.first)));
        }
        REQUIRE(capabilities.value(QStringLiteral("subtitle_style")).toBool());
        REQUIRE(clips.first().toObject().value(QStringLiteral("effectIds")).isArray());
        REQUIRE(clips.first().toObject().value(QStringLiteral("effectDetails")).isArray());
    }
    SECTION("Color adjustment is one bounded native effect with undo and rollback")
    {
        REQUIRE(EffectsRepository::get()->exists(QStringLiteral("avfilter.eq")));
        const auto stack = timeline->getClipEffectStackModel(clip);
        const int previous = stack->rowCount(), history = undoStack->index();
        const QJsonObject color{{"type", "color_adjust"}, {"clipId", clip}, {"brightness", 0.12}, {"contrast", 1.1}, {"saturation", 0.8}};
        const auto applied = bridge.applyBatch(batch(bridge.readState(), QStringLiteral("color-add"), {color}));
        INFO(QJsonDocument(applied).toJson().toStdString());
        REQUIRE(applied["ok"].toBool());
        REQUIRE(stack->rowCount() == previous + 1);
        const auto effect = std::static_pointer_cast<EffectItemModel>(stack->getEffectStackRow(previous));
        REQUIRE(effect->filter().anim_get_double("av.brightness", 0) == Approx(0.12));
        REQUIRE(effect->filter().get_double("av.contrast") == Approx(1.1));
        REQUIRE(effect->filter().get_double("av.saturation") == Approx(0.8));
        REQUIRE(undoStack->index() == history + 1);
        const auto clips = bridge.readState()["clips"].toArray();
        REQUIRE(clips.first().toObject()["effectIds"].toArray().contains(QStringLiteral("avfilter.eq")));
        effect->setAssetEnabled(false);
        REQUIRE_FALSE(bridge.readState()["clips"].toArray().first().toObject()["effectDetails"].toArray().first().toObject()["enabled"].toBool());
        const auto disabled = bridge.applyBatch(batch(bridge.readState(), QStringLiteral("color-disabled"), {color}));
        REQUIRE_FALSE(disabled["ok"].toBool());
        REQUIRE(disabled["code"].toString() == QStringLiteral("effect_disabled"));
        REQUIRE(disabled["message"].toString().contains(QStringLiteral("禁用")));
        REQUIRE(effect->filter().anim_get_double("av.brightness", 0) == Approx(0.12));
        REQUIRE(undoStack->index() == history + 1);
        effect->setAssetEnabled(true);
        QJsonObject nativeParameters;
        for (const auto &parameter : effect->getAllParameters()) nativeParameters[parameter.first] = parameter.second.toString();
        INFO("color model parameters before update: " << QJsonDocument(nativeParameters).toJson(QJsonDocument::Compact).toStdString());
        INFO("brightness raw before update: " << effect->filter().get("av.brightness"));
        INFO("brightness model before update: " << effect->getParamFromName(QStringLiteral("av.brightness")).toString().toStdString());
        auto changed = color;
        changed["brightness"] = 0.2;
        REQUIRE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("color-update"), {changed}))["ok"].toBool());
        REQUIRE(stack->rowCount() == previous + 1);
        REQUIRE(effect->filter().anim_get_double("av.brightness", 0) == Approx(0.2));
        REQUIRE(effectParameters(bridge.readState(), clip, QStringLiteral("avfilter.eq"))["av.brightness"].toDouble() == Approx(0.2));
        REQUIRE(undoStack->index() == history + 2);
        undoStack->undo();
        REQUIRE(stack->rowCount() == previous + 1);
        INFO("brightness raw after undo: " << effect->filter().get("av.brightness"));
        INFO("brightness model after undo: " << effect->getParamFromName(QStringLiteral("av.brightness")).toString().toStdString());
        INFO("brightness animated value after undo: " << effect->filter().anim_get_double("av.brightness", 0));
        REQUIRE(effect->filter().anim_get_double("av.brightness", 0) == Approx(0.12));
        REQUIRE(effectParameters(bridge.readState(), clip, QStringLiteral("avfilter.eq"))["av.brightness"].toDouble() == Approx(0.12));
        undoStack->redo();
        REQUIRE(effect->filter().anim_get_double("av.brightness", 0) == Approx(0.2));
        undoStack->undo();
        undoStack->undo();
        REQUIRE(stack->rowCount() == previous);
        undoStack->redo();
        REQUIRE(stack->rowCount() == previous + 1);
        REQUIRE(std::static_pointer_cast<EffectItemModel>(stack->getEffectStackRow(previous))->filter().anim_get_double("av.brightness", 0) == Approx(0.12));
        undoStack->undo();
        const QJsonObject impossible{{"type", "move"}, {"clipId", clip}, {"trackId", track}, {"frame", -1}};
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("color-rollback"), {color, impossible}))["ok"].toBool());
        REQUIRE(stack->rowCount() == previous);
        REQUIRE(undoStack->index() == history);
        for (const auto &invalid : {QJsonObject{{"brightness", 1.01}}, QJsonObject{{"contrast", -0.1}},
                                    QJsonObject{{"saturation", 5.01}}, QJsonObject{{"brightness", QStringLiteral("0.1")}},
                                    QJsonObject{{"saturation", std::numeric_limits<double>::infinity()}}}) {
            auto operation = color;
            for (auto it = invalid.begin(); it != invalid.end(); ++it) operation[it.key()] = it.value();
            REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QUuid::createUuid().toString(), {operation}))["ok"].toBool());
            REQUIRE(stack->rowCount() == previous);
        }
        REQUIRE(stack->appendEffect(QStringLiteral("avfilter.eq")));
        REQUIRE(stack->appendEffect(QStringLiteral("avfilter.eq")));
        const int ambiguousHistory = undoStack->index();
        const auto ambiguous = bridge.applyBatch(batch(bridge.readState(), QStringLiteral("color-ambiguous"), {color}));
        REQUIRE_FALSE(ambiguous["ok"].toBool());
        REQUIRE(ambiguous["code"].toString() == QStringLiteral("ambiguous_effect"));
        REQUIRE(stack->rowCount() == previous + 2);
        REQUIRE(undoStack->index() == ambiguousHistory);
        undoStack->undo();
        undoStack->undo();
        REQUIRE(stack->rowCount() == previous);
        timeline->setTrackProperty(track, QStringLiteral("kdenlive:locked_track"), QStringLiteral("1"));
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("color-locked"), {color}))["ok"].toBool());
        REQUIRE(stack->rowCount() == previous);
    }
    SECTION("Updating a native preset preserves untouched fixed gamma and its undo")
    {
        const auto stack = timeline->getClipEffectStackModel(clip);
        const int previous = stack->rowCount();
        REQUIRE(stack->appendEffect(QStringLiteral("avfilter.eq"), false, {{"av.brightness", "0.12"}, {"av.gamma", "1.4"}}));
        const auto effect = std::static_pointer_cast<EffectItemModel>(stack->getEffectStackRow(previous));
        REQUIRE(effect->filter().anim_get_double("av.gamma", 0) == Approx(1.4));
        const int history = undoStack->index();
        QJsonObject color{{"type", "color_adjust"}, {"clipId", clip}, {"brightness", 0.2}, {"contrast", 1.1}, {"saturation", 0.8}};
        REQUIRE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("native-preset-update"), {color}))["ok"].toBool());
        REQUIRE(effect->filter().anim_get_double("av.brightness", 0) == Approx(0.2));
        REQUIRE(effect->filter().anim_get_double("av.gamma", 0) == Approx(1.4));
        REQUIRE(stack->rowCount() == previous + 1);
        REQUIRE(undoStack->index() == history + 1);
        undoStack->undo();
        REQUIRE(effect->filter().anim_get_double("av.brightness", 0) == Approx(0.12));
        REQUIRE(effect->filter().anim_get_double("av.gamma", 0) == Approx(1.4));
        undoStack->redo();
        REQUIRE(effect->filter().anim_get_double("av.brightness", 0) == Approx(0.2));
        REQUIRE(effect->filter().anim_get_double("av.gamma", 0) == Approx(1.4));
        color["brightness"] = 0.3;
        const QJsonObject impossible{{"type", "move"}, {"clipId", clip}, {"trackId", track}, {"frame", -1}};
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("native-preset-rollback"), {color, impossible}))["ok"].toBool());
        REQUIRE(effect->filter().anim_get_double("av.brightness", 0) == Approx(0.2));
        REQUIRE(effect->filter().anim_get_double("av.gamma", 0) == Approx(1.4));
    }
    SECTION("Photo motion follows source time after trimming and moving and stays undoable")
    {
        REQUIRE(EffectsRepository::get()->exists(QStringLiteral("pan_zoom")));
        const QString sourcePath = LocalCutTestPaths::fixture(QStringLiteral("media/中文 空格 样片.mp4"));
        REQUIRE(QFile::exists(sourcePath));
        auto producer = std::make_shared<Mlt::Producer>(pCore->getProjectProfile(), sourcePath.toUtf8().constData());
        REQUIRE(producer->is_valid());
        producer->set("length", 100);
        producer->set_in_and_out(0, 99);
        QString finiteBin;
        Fun addUndo = []() { return true; }, addRedo = []() { return true; };
        REQUIRE(binModel->requestAddBinClip(finiteBin, producer, binModel->getRootFolder()->clipId(), addUndo, addRedo));
        int movingClip = -1;
        REQUIRE(timeline->requestClipInsertion(finiteBin, track, 150, movingClip));
        REQUIRE(timeline->requestItemResize(movingClip, 80, false, true) == 80);
        REQUIRE(timeline->requestClipMove(movingClip, track, 175));
        const auto source = timeline->getClipInOut(movingClip);
        REQUIRE(source.first == 20);
        REQUIRE(source.second == 99);
        const auto stack = timeline->getClipEffectStackModel(movingClip);
        const int previous = stack->rowCount(), history = undoStack->index();
        const QJsonObject motion{{"type", "photo_motion"}, {"clipId", movingClip}, {"direction", "zoom_in"}, {"zoom", 1.2}};
        const auto applied = bridge.applyBatch(batch(bridge.readState(), QStringLiteral("motion-add"), {motion}));
        INFO(QJsonDocument(applied).toJson().toStdString());
        REQUIRE(applied["ok"].toBool());
        const auto effect = std::static_pointer_cast<EffectItemModel>(stack->getEffectStackRow(previous));
        const auto begin = effect->filter().anim_get_rect("transition.rect", source.first);
        const auto end = effect->filter().anim_get_rect("transition.rect", source.second);
        const double width = pCore->getProjectProfile().width(), height = pCore->getProjectProfile().height();
        REQUIRE(begin.x == Approx(0));
        REQUIRE(begin.y == Approx(0));
        REQUIRE(begin.w == Approx(width));
        REQUIRE(begin.h == Approx(height));
        REQUIRE(end.x == Approx(-width * 0.1));
        REQUIRE(end.y == Approx(-height * 0.1));
        REQUIRE(end.w == Approx(width * 1.2));
        REQUIRE(end.h == Approx(height * 1.2));
        REQUIRE(effect->filter().get_int("transition.distort") == 0);
        REQUIRE(undoStack->index() == history + 1);
        REQUIRE(timeline->getClipPosition(movingClip) == 175);
        auto changed = motion;
        changed["zoom"] = 1.3;
        REQUIRE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("motion-update"), {changed}))["ok"].toBool());
        REQUIRE(stack->rowCount() == previous + 1);
        REQUIRE(effect->filter().anim_get_rect("transition.rect", source.second).w == Approx(width * 1.3));
        REQUIRE(effectParameters(bridge.readState(), movingClip, QStringLiteral("pan_zoom"))["transition.rect"].toString()
                    == QString::fromUtf8(effect->filter().get("transition.rect")));
        undoStack->undo();
        REQUIRE(effect->filter().anim_get_rect("transition.rect", source.second).w == Approx(width * 1.2));
        undoStack->redo();
        REQUIRE(effect->filter().anim_get_rect("transition.rect", source.second).w == Approx(width * 1.3));
        undoStack->undo();
        undoStack->undo();
        REQUIRE(stack->rowCount() == previous);
        undoStack->redo();
        REQUIRE(stack->rowCount() == previous + 1);
        undoStack->undo();
        const QJsonObject impossible{{"type", "move"}, {"clipId", movingClip}, {"trackId", track}, {"frame", -1}};
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("motion-rollback"), {motion, impossible}))["ok"].toBool());
        REQUIRE(stack->rowCount() == previous);
        auto zoomOut = motion;
        zoomOut["direction"] = QStringLiteral("zoom_out");
        REQUIRE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("motion-out"), {zoomOut}))["ok"].toBool());
        const auto reverse = std::static_pointer_cast<EffectItemModel>(stack->getEffectStackRow(previous));
        REQUIRE(reverse->filter().anim_get_rect("transition.rect", source.first).w == Approx(width * 1.2));
        REQUIRE(reverse->filter().anim_get_rect("transition.rect", source.second).w == Approx(width));
        undoStack->undo();
        for (const auto &invalid : {QJsonObject{{"zoom", 0.9}}, QJsonObject{{"zoom", 1.51}}, QJsonObject{{"direction", "spin"}}}) {
            auto operation = motion;
            for (auto it = invalid.begin(); it != invalid.end(); ++it) operation[it.key()] = it.value();
            REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QUuid::createUuid().toString(), {operation}))["ok"].toBool());
            REQUIRE(stack->rowCount() == previous);
        }
        timeline->setTrackProperty(track, QStringLiteral("kdenlive:locked_track"), QStringLiteral("1"));
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("motion-locked"), {motion}))["ok"].toBool());
        REQUIRE(stack->rowCount() == previous);
        timeline->setTrackProperty(track, QStringLiteral("kdenlive:locked_track"), QStringLiteral("0"));
        REQUIRE(timeline->requestItemResize(movingClip, 1, true, true) == 1);
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("motion-one-frame"), {motion}))["ok"].toBool());
        REQUIRE(stack->rowCount() == previous);
    }
    SECTION("Native animated effects keep their curves and refuse scalar replacement")
    {
        QString effectId, parameter, animation;
        QJsonObject operation;
        int target = clip;
        SECTION("Animated color adjustment")
        {
            effectId = QStringLiteral("avfilter.eq");
            parameter = QStringLiteral("av.brightness");
            animation = QStringLiteral("0=0.1;50=0.4");
            operation = {{"type", "color_adjust"}, {"clipId", clip}, {"brightness", 0.2}, {"contrast", 1.1}, {"saturation", 0.8}};
        }
        SECTION("Animated dynamic loudness")
        {
            const QString avBin = KdenliveTests::createAVProducer(pCore->getProjectProfile(), binModel);
            int avClip = -1;
            REQUIRE(timeline->requestClipInsertion(avBin, track, 150, avClip, true, false, false));
            target = timeline->getClipSplitPartner(avClip);
            REQUIRE(target >= 0);
            effectId = QStringLiteral("dynamic_loudness");
            parameter = QStringLiteral("target_loudness");
            animation = QStringLiteral("0=-16;25=-20");
            operation = {{"type", "audio_normalize"}, {"clipId", avClip}, {"targetLufs", -18}};
        }
        SECTION("Animated chroma tolerance")
        {
            effectId = QStringLiteral("chroma");
            parameter = QStringLiteral("variance");
            animation = QStringLiteral("0=0.1;50=0.4");
            operation = {{"type", "chroma_key"}, {"clipId", clip}, {"color", "#00ff00"}, {"tolerance", 0.2}};
        }
        const auto stack = timeline->getClipEffectStackModel(target);
        INFO("animated native asset: " << effectId.toStdString());
        const int previous = stack->rowCount();
        REQUIRE(stack->appendEffect(effectId, false, {{parameter, animation}}));
        const auto effect = std::static_pointer_cast<EffectItemModel>(stack->getEffectStackRow(previous));
        const QString raw = QString::fromUtf8(effect->filter().get(parameter.toUtf8().constData()));
        const auto current = bridge.readState();
        QJsonObject details;
        for (const auto &entry : current["clips"].toArray()) {
            if (entry.toObject()["id"].toInt() == target) details = entry.toObject()["effectDetails"].toArray().first().toObject();
        }
        REQUIRE(details["animated"].toBool());
        REQUIRE(details["parameters"].toObject()[parameter].isString());
        REQUIRE(details["parameters"].toObject()[parameter].toString() == raw);
        const int history = undoStack->index();
        const auto result = bridge.applyBatch(batch(current, QStringLiteral("keep-native-curve"), {operation}));
        REQUIRE_FALSE(result["ok"].toBool());
        REQUIRE(result["code"].toString() == QStringLiteral("effect_animated"));
        REQUIRE(result["message"].toString().contains(QStringLiteral("关键帧")));
        REQUIRE(QString::fromUtf8(effect->filter().get(parameter.toUtf8().constData())) == raw);
        REQUIRE(stack->rowCount() == previous + 1);
        REQUIRE(undoStack->index() == history);
    }
    SECTION("Range-limited motion keeps its native zone and parameters untouched")
    {
        const auto stack = timeline->getClipEffectStackModel(clip);
        const int previous = stack->rowCount();
        const QJsonObject motion{{"type", "photo_motion"}, {"clipId", clip}, {"direction", "zoom_in"}, {"zoom", 1.2}};
        REQUIRE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("motion-before-zone"), {motion}))["ok"].toBool());
        const auto effect = std::static_pointer_cast<EffectItemModel>(stack->getEffectStackRow(previous));
        effect->setInOut(QStringLiteral("pan_zoom"), {10, 40}, true, false);
        REQUIRE(effect->filter().get_int("kdenlive:force_in_out") == 1);
        const QString geometry = QString::fromUtf8(effect->filter().get("transition.rect"));
        const int history = undoStack->index();
        const auto details = bridge.readState()["clips"].toArray().first().toObject()["effectDetails"].toArray().first().toObject();
        REQUIRE(details["rangeLimited"].toBool());
        REQUIRE(details["range"].toObject()["in"].toInt() == 10);
        REQUIRE(details["range"].toObject()["out"].toInt() == 40);
        auto changed = motion;
        changed["zoom"] = 1.3;
        const auto limited = bridge.applyBatch(batch(bridge.readState(), QStringLiteral("motion-zoned-update"), {changed}));
        REQUIRE_FALSE(limited["ok"].toBool());
        REQUIRE(limited["code"].toString() == QStringLiteral("effect_range_limited"));
        REQUIRE(limited["message"].toString().contains(QStringLiteral("区间")));
        REQUIRE(effect->getInOut().first == 10);
        REQUIRE(effect->getInOut().second == 40);
        REQUIRE(effect->filter().get_int("kdenlive:force_in_out") == 1);
        REQUIRE(QString::fromUtf8(effect->filter().get("transition.rect")) == geometry);
        REQUIRE(stack->rowCount() == previous + 1);
        REQUIRE(undoStack->index() == history);
    }
    SECTION("Dynamic loudness targets the audio partner and refuses locked or absent audio")
    {
        REQUIRE(EffectsRepository::get()->exists(QStringLiteral("dynamic_loudness")));
        const QString avBin = KdenliveTests::createAVProducer(pCore->getProjectProfile(), binModel);
        int avClip = -1;
        REQUIRE(timeline->requestClipInsertion(avBin, track, 150, avClip, true, false, false));
        const int audio = timeline->getClipSplitPartner(avClip);
        REQUIRE(audio >= 0);
        const auto audioStack = timeline->getClipEffectStackModel(audio), videoStack = timeline->getClipEffectStackModel(avClip);
        const int previous = audioStack->rowCount(), videoPrevious = videoStack->rowCount(), history = undoStack->index();
        const QJsonObject normalize{{"type", "audio_normalize"}, {"clipId", avClip}, {"targetLufs", -16.0}};
        const auto applied = bridge.applyBatch(batch(bridge.readState(), QStringLiteral("normalize-av"), {normalize}));
        INFO(QJsonDocument(applied).toJson().toStdString());
        REQUIRE(applied["ok"].toBool());
        REQUIRE(audioStack->rowCount() == previous + 1);
        REQUIRE(videoStack->rowCount() == videoPrevious);
        const auto effect = std::static_pointer_cast<EffectItemModel>(audioStack->getEffectStackRow(previous));
        REQUIRE(effect->filter().get_double("target_loudness") == Approx(-16));
        REQUIRE(effect->filter().get_double("window") == Approx(3));
        REQUIRE(effect->filter().get_double("max_gain") == Approx(15));
        REQUIRE(effect->filter().get_double("min_gain") == Approx(-15));
        REQUIRE(undoStack->index() == history + 1);
        auto changed = normalize;
        changed["clipId"] = audio;
        changed["targetLufs"] = -18;
        REQUIRE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("normalize-update"), {changed}))["ok"].toBool());
        REQUIRE(audioStack->rowCount() == previous + 1);
        REQUIRE(effect->filter().get_double("target_loudness") == Approx(-18));
        REQUIRE(effectParameters(bridge.readState(), audio, QStringLiteral("dynamic_loudness"))["target_loudness"].toDouble() == Approx(-18));
        undoStack->undo();
        REQUIRE(effect->filter().get_double("target_loudness") == Approx(-16));
        undoStack->redo();
        REQUIRE(effect->filter().get_double("target_loudness") == Approx(-18));
        undoStack->undo();
        undoStack->undo();
        REQUIRE(audioStack->rowCount() == previous);
        undoStack->redo();
        REQUIRE(audioStack->rowCount() == previous + 1);
        undoStack->undo();
        const QJsonObject impossible{{"type", "move"}, {"clipId", clip}, {"trackId", track}, {"frame", -1}};
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("normalize-rollback"), {normalize, impossible}))["ok"].toBool());
        REQUIRE(audioStack->rowCount() == previous);
        REQUIRE(undoStack->index() == history);
        auto invalid = normalize;
        invalid["targetLufs"] = -8;
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("normalize-range"), {invalid}))["ok"].toBool());
        invalid = normalize;
        invalid["clipId"] = clip;
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("normalize-no-audio"), {invalid}))["ok"].toBool());
        REQUIRE(timeline->getClipEffectStackModel(clip)->rowCount() == 0);
        timeline->setTrackProperty(timeline->getClipTrackId(audio), QStringLiteral("kdenlive:locked_track"), QStringLiteral("1"));
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("normalize-locked"), {normalize}))["ok"].toBool());
        REQUIRE(audioStack->rowCount() == previous);
        REQUIRE(videoStack->rowCount() == videoPrevious);
    }
    SECTION("Chroma key uses a bounded color and keeps rollback and undo intact")
    {
        REQUIRE(EffectsRepository::get()->exists(QStringLiteral("chroma")));
        const auto stack = timeline->getClipEffectStackModel(clip);
        const int previous = stack->rowCount(), history = undoStack->index();
        const QJsonObject chroma{{"type", "chroma_key"}, {"clipId", clip}, {"color", "#00ff00"}, {"tolerance", 0.12}};
        const auto applied = bridge.applyBatch(batch(bridge.readState(), QStringLiteral("chroma-add"), {chroma}));
        INFO(QJsonDocument(applied).toJson().toStdString());
        REQUIRE(applied["ok"].toBool());
        const auto effect = std::static_pointer_cast<EffectItemModel>(stack->getEffectStackRow(previous));
        const auto key = effect->filter().get_color("key");
        REQUIRE(key.r == 0);
        REQUIRE(key.g == 255);
        REQUIRE(key.b == 0);
        REQUIRE(key.a == 255);
        REQUIRE(effect->filter().anim_get_double("variance", 0) == Approx(0.12));
        REQUIRE(undoStack->index() == history + 1);
        auto changed = chroma;
        changed["tolerance"] = 0.2;
        REQUIRE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("chroma-update"), {changed}))["ok"].toBool());
        REQUIRE(stack->rowCount() == previous + 1);
        REQUIRE(effect->filter().anim_get_double("variance", 0) == Approx(0.2));
        REQUIRE(effectParameters(bridge.readState(), clip, QStringLiteral("chroma"))["variance"].toString()
                    == QString::fromUtf8(effect->filter().get("variance")));
        undoStack->undo();
        REQUIRE(effect->filter().anim_get_double("variance", 0) == Approx(0.12));
        undoStack->redo();
        REQUIRE(effect->filter().anim_get_double("variance", 0) == Approx(0.2));
        undoStack->undo();
        undoStack->undo();
        REQUIRE(stack->rowCount() == previous);
        undoStack->redo();
        REQUIRE(stack->rowCount() == previous + 1);
        undoStack->undo();
        const QJsonObject impossible{{"type", "move"}, {"clipId", clip}, {"trackId", track}, {"frame", -1}};
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("chroma-rollback"), {chroma, impossible}))["ok"].toBool());
        REQUIRE(stack->rowCount() == previous);
        for (const auto &invalid : {QJsonObject{{"color", "green"}}, QJsonObject{{"color", "#00ff00;evil"}}, QJsonObject{{"tolerance", -0.01}},
                                    QJsonObject{{"tolerance", 1.01}}}) {
            auto operation = chroma;
            for (auto it = invalid.begin(); it != invalid.end(); ++it) operation[it.key()] = it.value();
            REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QUuid::createUuid().toString(), {operation}))["ok"].toBool());
            REQUIRE(stack->rowCount() == previous);
        }
        timeline->setTrackProperty(track, QStringLiteral("kdenlive:locked_track"), QStringLiteral("1"));
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("chroma-locked"), {chroma}))["ok"].toBool());
        REQUIRE(stack->rowCount() == previous);
    }
    SECTION("Global subtitle styling preserves entries and native undo")
    {
        const QJsonObject style{{"type", "subtitle_style"}, {"fontSize", 48}, {"position", "bottom"}};
        REQUIRE_FALSE(bridge.applyBatch(batch(state, QStringLiteral("style-no-subtitle"), {style}))["ok"].toBool());
        const QJsonObject subtitle{{"type", "subtitle"}, {"start", 10}, {"end", 60}, {"text", QStringLiteral("字幕样式验收")}};
        REQUIRE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("style-subtitle"), {subtitle}))["ok"].toBool());
        const auto model = timeline->getSubtitleModel();
        REQUIRE(model);
        const QJsonObject missing{{"type", "subtitle_text"}, {"subtitleId", 9999999}, {"text", QStringLiteral("缺失字幕")}};
        const QString originalStyle = model->getForceStyle();
        const int styleHistory = undoStack->index();
        const auto styled = bridge.applyBatch(batch(bridge.readState(), QStringLiteral("subtitle-style"), {style}));
        INFO(QJsonDocument(styled).toJson().toStdString());
        REQUIRE(styled["ok"].toBool());
        REQUIRE(model->getForceStyle().contains(QStringLiteral("FontSize=48")));
        REQUIRE(model->getForceStyle().contains(QStringLiteral("Alignment=2")));
        REQUIRE(model->getForceStyle().contains(QStringLiteral("PrimaryColour=&H00FFFFFF")));
        REQUIRE(model->getForceStyle().contains(QStringLiteral("OutlineColour=&H00000000")));
        REQUIRE(model->rowCount() == 1);
        REQUIRE(model->data(model->index(0), SubtitleModel::StartFrameRole).toInt() == 10);
        REQUIRE(model->data(model->index(0), SubtitleModel::EndFrameRole).toInt() == 60);
        REQUIRE(undoStack->index() == styleHistory + 1);
        const QString changedStyle = model->getForceStyle();
        REQUIRE(bridge.readState()["subtitleForceStyle"].toString() == changedStyle);
        undoStack->undo();
        REQUIRE(model->getForceStyle() == originalStyle);
        REQUIRE(bridge.readState()["subtitleForceStyle"].toString() == originalStyle);
        undoStack->redo();
        REQUIRE(model->getForceStyle() == changedStyle);
        auto restyle = style;
        restyle["fontSize"] = 60;
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("style-rollback"), {restyle, missing}))["ok"].toBool());
        REQUIRE(model->getForceStyle() == changedStyle);
        auto invalidStyle = style;
        invalidStyle["fontSize"] = 121;
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("style-size"), {invalidStyle}))["ok"].toBool());
        invalidStyle = style;
        invalidStyle["fontSize"] = 48.5;
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("style-fraction"), {invalidStyle}))["ok"].toBool());
        invalidStyle = style;
        invalidStyle["position"] = QStringLiteral("outside");
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("style-position"), {invalidStyle}))["ok"].toBool());
        REQUIRE(model->getForceStyle() == changedStyle);
        model->switchLocked();
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QStringLiteral("style-locked"), {restyle}))["ok"].toBool());
        REQUIRE(model->getForceStyle() == changedStyle);
        model->switchLocked();
    }
}

TEST_CASE("Assistant retains explicit frame ranges and offers original presets", "[AssistantStory]")
{
    auto binModel = pCore->projectItemModel();
    binModel->clean();
    auto undoStack = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undoStack);
    pCore->projectManager()->testSetDocument(&document);
    struct CloseDocument { ~CloseDocument() { pCore->projectManager()->closeCurrentDocument(false, false); } } closeDocument;
    KdenliveTests::updateTimeline(false, QString(), QString(), QDateTime::currentDateTime(), 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    const int track = timeline->getTrackIndexFromPosition(2);
    const QString binId = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", binModel, 100, true);
    int clip = -1;
    REQUIRE(timeline->requestClipInsertion(binId, track, 20, clip));
    EditBridge bridge;
    bridge.bindContext(document.uuid().toString(), timeline, undoStack);
    const QJsonObject retain{{"type", "retain_ranges"}, {"clipId", clip},
        {"ranges", QJsonArray{QJsonObject{{"start", 10}, {"end", 30}}, QJsonObject{{"start", 50}, {"end", 80}}}}};
    SECTION("Retained source ranges compact to original start and undo as one edit") {
        const int history = undoStack->index();
        auto response = bridge.applyBatch(batch(bridge.readState(), "retain", {retain}));
        INFO(QJsonDocument(response).toJson().toStdString());
        REQUIRE(response["ok"].toBool());
        REQUIRE(timeline->getClipsCount() == 2);
        REQUIRE(timeline->getClipByPosition(track, 70) == -1);
        const int first = timeline->getClipByPosition(track, 20);
        const int second = timeline->getClipByPosition(track, 40);
        REQUIRE(first >= 0);
        REQUIRE(second >= 0);
        REQUIRE(timeline->getClipInOut(first).first == 10);
        REQUIRE(timeline->getClipPlaytime(first) == 20);
        REQUIRE(timeline->getClipInOut(second).first == 50);
        REQUIRE(timeline->getClipPlaytime(second) == 30);
        REQUIRE(undoStack->index() == history + 1);
        undoStack->undo();
        REQUIRE(timeline->getClipsCount() == 1);
        REQUIRE(timeline->getClipPosition(clip) == 20);
        REQUIRE(timeline->getClipPlaytime(clip) == 100);
        undoStack->redo();
        REQUIRE(timeline->getClipsCount() == 2);
    }
    SECTION("Invalid ranges leave timeline and history unchanged") {
        for (const QJsonArray &ranges : {QJsonArray{}, QJsonArray{QJsonObject{{"start", 5}, {"end", 5}}},
            QJsonArray{QJsonObject{{"start", -1}, {"end", 4}}}, QJsonArray{QJsonObject{{"start", 0}, {"end", 101}}},
            QJsonArray{QJsonObject{{"start", 10}, {"end", 40}}, QJsonObject{{"start", 30}, {"end", 50}}}}) {
            auto invalid = retain;
            invalid["ranges"] = ranges;
            const int history = undoStack->index();
            REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), QUuid::createUuid().toString(), {invalid}))["ok"].toBool());
            REQUIRE(timeline->getClipsCount() == 1);
            REQUIRE(undoStack->index() == history);
        }
    }
    SECTION("Other clips stay in place and locked tracks or subtitle overlap are refused") {
        int other = -1;
        REQUIRE(timeline->requestClipInsertion(binId, track, 150, other));
        timeline->setTrackProperty(track, QStringLiteral("kdenlive:locked_track"), QStringLiteral("1"));
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), "retain-locked", {retain}))["ok"].toBool());
        timeline->setTrackProperty(track, QStringLiteral("kdenlive:locked_track"), QStringLiteral("0"));
        REQUIRE(bridge.applyBatch(batch(bridge.readState(), "retain-other", {retain}))["ok"].toBool());
        REQUIRE(timeline->getClipPosition(other) == 150);
        undoStack->undo();
        REQUIRE(bridge.applyBatch(batch(bridge.readState(), "overlap-subtitle", {QJsonObject{{"type", "subtitle"}, {"start", 25}, {"end", 35}, {"text", "keep"}}}))["ok"].toBool());
        const int count = timeline->getClipsCount();
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), "retain-subtitle-conflict", {retain}))["ok"].toBool());
        REQUIRE(timeline->getClipsCount() == count);
    }
    SECTION("Complex groups are refused without touching other clips") {
        int other = -1;
        REQUIRE(timeline->requestClipInsertion(binId, track, 150, other));
        REQUIRE(timeline->requestClipsGroup({clip, other}) >= 0);
        const int history = undoStack->index();
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), "retain-group", {retain}))["ok"].toBool());
        REQUIRE(timeline->getClipPlaytime(clip) == 100);
        REQUIRE(timeline->getClipPosition(other) == 150);
        REQUIRE(undoStack->index() == history);
    }
    SECTION("Linked audio and video remain synchronized and locked partner is refused") {
        const QString avBin = KdenliveTests::createAVProducer(pCore->getProjectProfile(), binModel);
        int av = -1;
        REQUIRE(timeline->requestClipInsertion(avBin, track, 150, av, true, false, false));
        const int audio = timeline->getClipSplitPartner(av);
        REQUIRE(audio >= 0);
        auto linked = retain;
        linked["clipId"] = av;
        linked["ranges"] = QJsonArray{QJsonObject{{"start", 2}, {"end", 10}}, QJsonObject{{"start", 15}, {"end", 30}}};
        const int audioTrack = timeline->getClipTrackId(audio);
        timeline->setTrackProperty(audioTrack, QStringLiteral("kdenlive:locked_track"), QStringLiteral("1"));
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), "retain-av-locked", {linked}))["ok"].toBool());
        timeline->setTrackProperty(audioTrack, QStringLiteral("kdenlive:locked_track"), QStringLiteral("0"));
        REQUIRE(bridge.applyBatch(batch(bridge.readState(), "retain-av", {linked}))["ok"].toBool());
        for (int position : {150, 158}) {
            const int videoPart = timeline->getClipByPosition(track, position);
            const int audioPart = timeline->getClipByPosition(audioTrack, position);
            REQUIRE(videoPart >= 0);
            REQUIRE(audioPart >= 0);
            REQUIRE(timeline->getClipSplitPartner(videoPart) == audioPart);
            REQUIRE(timeline->getClipInOut(videoPart) == timeline->getClipInOut(audioPart));
        }
    }
    SECTION("Later batch failure restores all cuts and deletions") {
        const QJsonObject invalid{{"type", "delete"}, {"clipId", -1}};
        REQUIRE_FALSE(bridge.applyBatch(batch(bridge.readState(), "retain-rollback", {retain, invalid}))["ok"].toBool());
        REQUIRE(timeline->getClipsCount() == 1);
        REQUIRE(timeline->getClipPlaytime(clip) == 100);
    }
    SECTION("Presets are published and use native effect history") {
        REQUIRE(bridge.readState()["supportedOperations"].toArray().contains("color_preset"));
        REQUIRE(bridge.applyBatch(batch(bridge.readState(), "preset", {QJsonObject{{"type", "color_preset"}, {"clipId", clip}, {"preset", "monochrome"}}}))["ok"].toBool());
        REQUIRE(effectParameters(bridge.readState(), clip, "avfilter.eq")["av.saturation"].toString().toDouble() == Approx(0));
        undoStack->undo();
        REQUIRE(timeline->getClipEffectStackModel(clip)->rowCount() == 0);
    }
    SECTION("Subtitle presets preserve text and timing") {
        REQUIRE(bridge.applyBatch(batch(bridge.readState(), "subtitle", {QJsonObject{{"type", "subtitle"}, {"start", 0}, {"end", 10}, {"text", "original"}}}))["ok"].toBool());
        const auto model = timeline->getSubtitleModel();
        const QString before = model->getForceStyle();
        const auto beforeEntries = bridge.readState()["subtitles"].toArray();
        REQUIRE(bridge.applyBatch(batch(bridge.readState(), "subtitle-preset", {QJsonObject{{"type", "subtitle_preset"}, {"preset", "highlight"}}}))["ok"].toBool());
        REQUIRE(model->getForceStyle().contains(QStringLiteral("FontName=Microsoft YaHei")));
        REQUIRE(bridge.readState()["subtitles"].toArray() == beforeEntries);
        undoStack->undo();
        REQUIRE(model->getForceStyle() == before);
    }
}
