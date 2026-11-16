/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "editbridge.h"
#include "edittemplates.h"

#include "assets/model/assetcommand.hpp"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "bin/model/subtitlemodel.hpp"
#include "core.h"
#include "doc/docundostack.hpp"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "effects/effectstack/model/effectitemmodel.hpp"
#include "effects/effectsrepository.hpp"
#include "project/projectmanager.h"
#include "timeline2/model/timelinefunctions.hpp"
#include "timeline2/model/timelineitemmodel.hpp"
#include "undohelper.hpp"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>
#include <framework/mlt_animation.h>

namespace {
QJsonArray sortedIds(const std::unordered_set<int> &ids)
{
    std::vector<int> ordered(ids.begin(), ids.end());
    std::sort(ordered.begin(), ordered.end());
    QJsonArray result;
    for (int id : ordered) {
        result.append(id);
    }
    return result;
}

bool integer(const QJsonValue &value, int &output)
{
    const double number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number < 0 || number > std::numeric_limits<int>::max() || std::floor(number) != number) {
        return false;
    }
    output = int(number);
    return true;
}

bool boundedNumber(const QJsonValue &value, double minimum, double maximum, double &output)
{
    output = value.toDouble();
    return value.isDouble() && std::isfinite(output) && output >= minimum && output <= maximum;
}

QVector<std::shared_ptr<EffectItemModel>> nativeEffects(const std::shared_ptr<EffectStackModel> &stack)
{
    QVector<std::shared_ptr<EffectItemModel>> effects;
    if (!stack) return effects;
    const auto collect = [&](const auto &self, const std::shared_ptr<TreeItem> &item) -> void {
        const auto effect = std::static_pointer_cast<AbstractEffectItem>(item);
        if (effect->effectItemType() == EffectItemType::Effect) {
            effects.append(std::static_pointer_cast<EffectItemModel>(effect));
        } else {
            for (int row = 0; row < item->childCount(); ++row) self(self, item->child(row));
        }
    };
    for (int row = 0; row < stack->rowCount(); ++row) collect(collect, stack->getEffectStackRow(row));
    return effects;
}

struct NativeParameter {
    QString raw, constant;
    int keys = 0;
};

NativeParameter nativeParameter(const std::shared_ptr<EffectItemModel> &effect, const QString &name)
{
    NativeParameter value;
    value.raw = QString::fromUtf8(effect->filter().get(name.toUtf8().constData()));
    value.constant = value.raw;
    if (value.raw.contains(QLatin1Char('='))) {
        Mlt::Properties properties;
        properties.set_lcnumeric("C");
        properties.set("value", value.raw.toUtf8().constData());
        value.constant = QString::fromUtf8(properties.anim_get("value", 0, -1));
        const auto animation = properties.get_animation("value");
        if (animation) value.keys = mlt_animation_key_count(animation);
    }
    return value;
}

void synchronizeFixedParameters(const std::shared_ptr<EffectItemModel> &effect)
{
    const auto parameters = effect->getAllParameters();
    for (const auto &parameter : parameters) {
        if (effect->getParamIndexFromName(parameter.first).isValid()) continue;
        const char *raw = effect->filter().get(parameter.first.toUtf8().constData());
        if (raw) effect->setParameter(parameter.first, QString::fromUtf8(raw), false);
    }
}

QJsonObject effectDetails(const std::shared_ptr<EffectItemModel> &effect)
{
    const QString id = effect->getAssetId();
    QJsonObject parameters;
    bool animated = false;
    const auto scalar = [&](const char *name) {
        const auto value = nativeParameter(effect, QLatin1String(name));
        animated = animated || value.keys > 1;
        bool numeric = false;
        const double number = value.constant.toDouble(&numeric);
        parameters[QLatin1String(name)] = value.keys > 1 || !numeric || !std::isfinite(number) ? QJsonValue(value.raw) : QJsonValue(number);
    };
    const auto raw = [&](const char *name) {
        const auto value = nativeParameter(effect, QLatin1String(name));
        animated = animated || value.keys > 1;
        parameters[QLatin1String(name)] = value.raw;
    };
    if (id == QStringLiteral("avfilter.eq")) {
        for (const auto *name : {"av.brightness", "av.contrast", "av.saturation"}) scalar(name);
    } else if (id == QStringLiteral("pan_zoom")) {
        raw("transition.rect");
    } else if (id == QStringLiteral("dynamic_loudness")) {
        for (const auto *name : {"target_loudness", "window", "max_gain", "min_gain", "max_rate"}) scalar(name);
    } else if (id == QStringLiteral("chroma")) {
        for (const auto *name : {"key", "variance"}) raw(name);
    }
    if (parameters.isEmpty()) return {};
    const bool rangeLimited = effect->filter().get_int("kdenlive:force_in_out") == 1;
    QJsonObject value{{"id", id}, {"enabled", effect->isAssetEnabled()}, {"animated", animated}, {"rangeLimited", rangeLimited}, {"parameters", parameters}};
    if (rangeLimited) {
        const auto range = effect->getInOut();
        value.insert(QStringLiteral("range"), QJsonObject{{"in", range.first}, {"out", range.second}});
    }
    return value;
}
}

EditBridge::EditBridge(QObject *parent) : QObject(parent) {}

void EditBridge::bindContext(const QString &projectId, const std::shared_ptr<TimelineItemModel> &timeline,
                             const std::shared_ptr<DocUndoStack> &undoStack)
{
    const auto oldTimeline = m_timeline.lock();
    const auto oldUndo = m_undoStack.lock();
    if (projectId == m_projectId && oldTimeline == timeline && oldUndo == undoStack) {
        return;
    }
    const bool newSession = projectId != m_projectId || oldUndo != undoStack || m_sessionId.isEmpty();
    for (const auto &connection : m_modelConnections) {
        disconnect(connection);
    }
    m_modelConnections.clear();
    m_effectStacks.clear();
    if (newSession) {
        disconnect(m_undoConnection);
        m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_requests.clear();
        if (undoStack) {
            m_undoConnection = connect(undoStack.get(), &QUndoStack::indexChanged, this, [this]() { ++m_revision; });
        }
    }
    m_projectId = projectId;
    m_timeline = timeline;
    m_undoStack = undoStack;
    m_sequenceId = timeline ? timeline->uuid().toString() : QString();
    m_fingerprint.clear();
    ++m_revision;
    if (!timeline) {
        return;
    }
    m_modelConnections.append(connect(timeline.get(), &QAbstractItemModel::dataChanged, this,
        [this](const QModelIndex &, const QModelIndex &, const QList<int> &roles) {
            if (roles.isEmpty() || std::any_of(roles.begin(), roles.end(), [](int role) {
                return role != TimelineModel::ClipThumbRole && role != TimelineModel::AudioLevelsRole;
            })) {
                ++m_revision;
            }
        }));
    m_modelConnections.append(connect(timeline.get(), &QAbstractItemModel::rowsInserted, this, [this]() { ++m_revision; }));
    m_modelConnections.append(connect(timeline.get(), &QAbstractItemModel::rowsRemoved, this, [this]() { ++m_revision; }));
    m_modelConnections.append(connect(timeline.get(), &QAbstractItemModel::modelReset, this, [this]() { ++m_revision; }));
    m_modelConnections.append(connect(timeline.get(), &TimelineModel::selectionChanged, this, [this]() { ++m_revision; }));
}

void EditBridge::observeEffects(QObject *stack)
{
    if (!stack || m_effectStacks.contains(stack)) {
        return;
    }
    m_effectStacks.insert(stack);
    auto *effects = qobject_cast<EffectStackModel *>(stack);
    m_modelConnections.append(connect(effects, &EffectStackModel::modelChanged, this, [this]() { ++m_revision; }));
    m_modelConnections.append(connect(stack, &QObject::destroyed, this, [this, stack]() { m_effectStacks.remove(stack); }));
}

QJsonObject EditBridge::snapshot()
{
    const auto timeline = m_timeline.lock();
    if (m_projectId.isEmpty() || !timeline || m_undoStack.expired() || timeline->isClosed || !pCore || !pCore->projectManager()
        || pCore->projectManager()->getTimeline() != timeline) {
        return {{"available", false}, {"editable", false}, {"code", "no_context"}};
    }
    const auto &profile = pCore->getProjectProfile();
    QJsonArray tracks, clips, assets;
    for (int row = 0; row < timeline->rowCount(); ++row) {
        const auto trackIndex = timeline->index(row);
        const int trackId = timeline->data(trackIndex, TimelineModel::ItemIdRole).toInt();
        tracks.append(QJsonObject{{"id", trackId}, {"name", timeline->data(trackIndex, TimelineModel::NameRole).toString()},
            {"audio", timeline->isAudioTrack(trackId)}, {"locked", timeline->trackIsLocked(trackId)}, {"position", timeline->getTrackPosition(trackId)}});
        observeEffects(timeline->getTrackEffectStackModel(trackId).get());
        for (int item = 0; item < timeline->rowCount(trackIndex); ++item) {
            const auto index = timeline->index(item, 0, trackIndex);
            const int id = timeline->data(index, TimelineModel::ItemIdRole).toInt();
            if (!timeline->isClip(id)) {
                continue;
            }
            const auto inOut = timeline->getClipInOut(id);
            QJsonArray effectIds, details;
            for (const auto &effect : nativeEffects(timeline->getClipEffectStackModel(id))) {
                effectIds.append(effect->getAssetId());
                const auto value = effectDetails(effect);
                if (!value.isEmpty()) details.append(value);
            }
            clips.append(QJsonObject{{"id", id}, {"binId", timeline->getClipBinId(id)}, {"trackId", trackId},
                {"start", timeline->getClipPosition(id)}, {"duration", timeline->getClipPlaytime(id)}, {"in", inOut.first}, {"out", inOut.second},
                {"resource", timeline->data(index, TimelineModel::ResourceRole).toString()}, {"audio", timeline->isAudioTrack(trackId)},
                {"hasAudio", timeline->data(index, TimelineModel::CanBeAudioRole).toBool()},
                {"group", sortedIds(timeline->getGroupElements(id))}, {"speed", timeline->getClipSpeed(id)},
                {"timeRemap", timeline->data(index, TimelineModel::TimeRemapRole).toBool()},
                {"effects", QJsonValue::fromVariant(timeline->data(index, TimelineModel::EffectNamesRole))},
                {"effectIds", effectIds}, {"effectDetails", details}});
            observeEffects(timeline->getClipEffectStackModel(id).get());
        }
    }
    if (pCore && pCore->projectItemModel()) {
        auto ids = pCore->projectItemModel()->getAllClipIds();
        std::sort(ids.begin(), ids.end());
        for (const auto &id : ids) {
            const auto asset = pCore->projectItemModel()->getClipByBinID(id);
            if (asset) {
                assets.append(QJsonObject{{"binId", id}, {"name", asset->clipName()}, {"resource", asset->clipUrl()},
                    {"hasAudio", asset->hasAudio()}, {"hasVideo", asset->hasVideo()}});
            }
        }
    }
    QJsonArray subtitles;
    if (const auto model = timeline->getSubtitleModel()) {
        if (!m_effectStacks.contains(model.get())) {
            m_effectStacks.insert(model.get());
            m_modelConnections.append(connect(model.get(), &QAbstractItemModel::dataChanged, this, [this]() { ++m_revision; }));
            m_modelConnections.append(connect(model.get(), &QAbstractItemModel::rowsInserted, this, [this]() { ++m_revision; }));
            m_modelConnections.append(connect(model.get(), &QAbstractItemModel::rowsRemoved, this, [this]() { ++m_revision; }));
            m_modelConnections.append(connect(model.get(), &QObject::destroyed, this, [this, pointer = model.get()]() { m_effectStacks.remove(pointer); }));
        }
        for (int row = 0; row < model->rowCount(); ++row) {
            const auto index = model->index(row);
            subtitles.append(QJsonObject{{"id", model->data(index, SubtitleModel::IdRole).toInt()},
                {"start", model->data(index, SubtitleModel::StartFrameRole).toInt()},
                {"end", model->data(index, SubtitleModel::EndFrameRole).toInt()}, {"text", model->data(index, SubtitleModel::SubtitleRole).toString()}});
        }
    }
    const auto &repository = EffectsRepository::get();
    const QJsonObject capabilities{{"color_adjust", repository->exists(QStringLiteral("avfilter.eq"))},
        {"photo_motion", repository->exists(QStringLiteral("pan_zoom"))}, {"audio_normalize", repository->exists(QStringLiteral("dynamic_loudness"))},
        {"chroma_key", repository->exists(QStringLiteral("chroma"))}, {"subtitle_style", true}, {"subtitle_preset", true}, {"retain_ranges", true},
        {"color_preset", repository->exists(QStringLiteral("avfilter.eq"))}};
    const auto subtitleModel = timeline->getSubtitleModel();
    return {{"available", true}, {"editable", true}, {"code", "ready"}, {"frameRate", QJsonObject{{"num", profile.frame_rate_num()}, {"den", profile.frame_rate_den()}}},
            {"width", profile.width()}, {"height", profile.height()}, {"tracks", tracks}, {"clips", clips},
            {"selection", sortedIds(timeline->getCurrentSelection())}, {"assets", assets}, {"subtitles", subtitles},
            {"effectCapabilities", capabilities},
            {"colorPresets", QJsonArray{"natural", "soft", "crisp", "monochrome"}},
            {"subtitlePresets", QJsonArray{"clean", "headline", "highlight"}},
            {"retainRangesSemantics", QJsonObject{{"mode", "compact_within_clip"}, {"rippleFollowing", false},
                {"rangeUnit", "relative_current_clip_frames"}, {"endExclusive", true}, {"requiresNormalSpeed", true},
                {"rejectsSubtitleOverlap", true}, {"linkedAudioVideo", true}}}, {"subtitleForceStyle", subtitleModel ? subtitleModel->getForceStyle() : QString()},
            {"supportedOperations", QJsonArray{"split", "move", "trim", "delete", "insert", "volume", "watermark", "crop", "subtitle", "subtitle_text",
                                              "color_adjust", "photo_motion", "audio_normalize", "chroma_key", "subtitle_style", "retain_ranges", "color_preset", "subtitle_preset"}}};
}

QJsonObject EditBridge::readState()
{
    auto state = snapshot();
    const QByteArray fingerprint = QCryptographicHash::hash(QJsonDocument(state).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    if (!m_fingerprint.isEmpty() && fingerprint != m_fingerprint) {
        ++m_revision;
    }
    m_fingerprint = fingerprint;
    state.insert(QStringLiteral("sessionId"), m_sessionId);
    state.insert(QStringLiteral("projectId"), m_projectId);
    state.insert(QStringLiteral("sequenceId"), m_sequenceId);
    state.insert(QStringLiteral("revision"), m_revision);
    return state;
}

QJsonObject EditBridge::result(bool ok, const QString &code) const
{
    QJsonObject value{{"ok", ok}, {"code", code}, {"revision", m_revision}};
    if (code == QStringLiteral("effect_disabled")) {
        value.insert(QStringLiteral("message"), QStringLiteral("现有效果、效果组或整组效果已禁用，本次未修改。请先启用后再调整。"));
    } else if (code == QStringLiteral("effect_range_limited")) {
        value.insert(QStringLiteral("message"), QStringLiteral("现有效果只作用于片段的指定区间，无法直接按整段调整。本次未修改原区间或参数。"));
    } else if (code == QStringLiteral("effect_animated")) {
        value.insert(QStringLiteral("message"), QStringLiteral("现有效果包含多个关键帧，直接设置固定参数会覆盖原动画，本次未修改。"));
    }
    return value;
}

QJsonObject EditBridge::listEditTemplates()
{
    auto catalog = EditTemplates::catalog();
    if (QThread::currentThread() != thread()) return result(false, QStringLiteral("wrong_thread"));
    if (catalog.isEmpty()) return result(false, QStringLiteral("template_catalog_unavailable"));
    if (!snapshot()["editable"].toBool()) return result(false, QStringLiteral("no_context"));
    QJsonArray inputs;
    for (const auto &id : pCore->projectItemModel()->getAllClipIds()) {
        const auto asset = pCore->projectItemModel()->getClipByBinID(id);
        if (!asset || asset->clipStatus() != FileStatus::StatusReady) continue;
        const auto type = asset->clipType();
        if (type != ClipType::Image && type != ClipType::Video && type != ClipType::AV && type != ClipType::Audio) continue;
        inputs.append(QJsonObject{{"binId", id}, {"name", asset->clipName()}, {"ready", true}, {"isImage", type == ClipType::Image},
            {"hasAudio", asset->hasAudio()}, {"hasVideo", asset->hasVideo()},
            {"durationFrames", type == ClipType::Image ? 0 : asset->getFramePlaytime()}});
    }
    catalog["availableInputs"] = inputs;
    catalog["imageDurationPolicy"] = QStringLiteral("图片时长可延长；sourceInFrames=0，sourceDurationFrames填写分配所需帧数。");
    return catalog;
}

QJsonObject EditBridge::applyEditTemplate(const QJsonObject &request)
{
    if (QThread::currentThread() != thread()) return result(false, QStringLiteral("wrong_thread"));
    if (m_templateApplying || m_applying) return result(false, QStringLiteral("busy"));
    const auto recipe = EditTemplates::find(request["templateId"].toString(), request["templateVersion"].toInt());
    int version;
    if (!integer(request["templateVersion"], version) || recipe.isEmpty()) return result(false, QStringLiteral("unknown_template_version"));
    const auto parameters = request["parameters"].toObject();
    const auto only = [](const QJsonObject &object, const QSet<QString> &keys) {
        for (auto it = object.begin(); it != object.end(); ++it) if (!keys.contains(it.key())) return false;
        return true;
    };
    if (!only(request, {"sessionId", "projectId", "sequenceId", "expectedRevision", "requestId", "templateId", "templateVersion",
                        "inputs", "clipId", "parameters", "target"}) || !request["parameters"].isObject() || !request["target"].isObject()
        || !only(request["target"].toObject(), {"videoTrackId", "audioTrackId"}))
        return result(false, QStringLiteral("invalid_template_parameters"));
    for (const auto &value : request["inputs"].toArray())
        if (!value.isObject() || !only(value.toObject(), {"binId", "sourceInFrames", "sourceDurationFrames"}))
            return result(false, QStringLiteral("invalid_template_parameters"));
    const bool talking = request["templateId"].toString() == QStringLiteral("talking_clean_v1");
    if ((talking && (request.contains("inputs") || !request.contains("clipId") || !parameters["ranges"].isArray()))
        || (!talking && (request.contains("clipId") || parameters.contains("ranges") || !request["inputs"].isArray())))
        return result(false, QStringLiteral("invalid_template_parameters"));
    for (const auto &value : parameters["ranges"].toArray())
        if (!value.isObject() || !only(value.toObject(), {"start", "end"}))
            return result(false, QStringLiteral("invalid_template_parameters"));
    for (auto it = parameters.begin(); it != parameters.end(); ++it)
        if (!QSet<QString>{"totalFrames", "originalAudio", "bgmBinId", "bgmDb", "texts", "ranges"}.contains(it.key()))
            return result(false, QStringLiteral("invalid_template_parameters"));
    QJsonObject operation = request;
    operation["type"] = QStringLiteral("template_story");
    QJsonObject batch = request;
    batch["operations"] = QJsonArray{operation};
    QScopedValueRollback<bool> templating(m_templateApplying, true);
    auto response = applyBatch(batch);
    if (response["ok"].toBool()) {
        if (response.contains(QStringLiteral("templateId"))) return response;
        response["templateId"] = request["templateId"];
        response["templateVersion"] = request["templateVersion"];
        response["appliedFrames"] = request["parameters"].toObject()["totalFrames"];
        response["frameRate"] = snapshot()["frameRate"];
        response["actualOriginalAudio"] = request["parameters"].toObject()["originalAudio"];
        response["bgmApplied"] = !request["parameters"].toObject()["bgmBinId"].toString().isEmpty();
        const auto replay = m_requests.find(request["requestId"].toString());
        if (replay != m_requests.end()) replay->result = response;
    }
    return response;
}

QJsonObject EditBridge::applyBatch(const QJsonObject &request)
{
    if (QThread::currentThread() != thread()) {
        return result(false, QStringLiteral("wrong_thread"));
    }
    if (m_applying) {
        return result(false, QStringLiteral("busy"));
    }
    const auto before = readState();
    const auto timeline = m_timeline.lock();
    const auto undoStack = m_undoStack.lock();
    if (!before["editable"].toBool() || !timeline || !undoStack) {
        return result(false, QStringLiteral("no_context"));
    }
    QStringList invalidFields;
    for (const auto &field : {QStringLiteral("sessionId"), QStringLiteral("projectId"), QStringLiteral("sequenceId")}) {
        if (!request[field].isString() || request[field].toString().trimmed().isEmpty()) invalidFields.append(field);
    }
    const auto revision = request["expectedRevision"];
    if (!revision.isDouble() || revision.toInteger(-1) < 0) invalidFields.append(QStringLiteral("expectedRevision"));
    if (!invalidFields.isEmpty()) {
        auto failure = result(false, QStringLiteral("invalid_request"));
        failure.insert(QStringLiteral("message"), QStringLiteral(
            "参数缺失或格式错误：%1。请将 read_project_state 返回的 sessionId、projectId、sequenceId 原样放在请求顶层，"
            "并将 revision 以非负整数作为顶层 expectedRevision；不要省略、改名或嵌套这些字段。")
            .arg(invalidFields.join(QStringLiteral("、"))));
        return failure;
    }
    if (request["sessionId"] != m_sessionId || request["projectId"] != m_projectId || request["sequenceId"] != m_sequenceId) {
        return result(false, QStringLiteral("stale_state"));
    }
    const QString requestId = request["requestId"].toString();
    if (requestId.isEmpty() || !request["operations"].isArray() || request["operations"].toArray().isEmpty()) {
        return result(false, QStringLiteral("invalid_request"));
    }
    const QByteArray digest = QCryptographicHash::hash(QJsonDocument(request).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    const auto replay = m_requests.constFind(requestId);
    if (replay != m_requests.cend()) {
        return replay->digest == digest ? replay->result : result(false, QStringLiteral("request_id_conflict"));
    }
    if (revision.toInteger() != m_revision) {
        return result(false, QStringLiteral("stale_state"));
    }
    const auto operations = request["operations"].toArray();
    if (std::any_of(operations.begin(), operations.end(), [](const QJsonValue &value) { return !value.isObject(); })) {
        return result(false, QStringLiteral("invalid_operation"));
    }
    if (!undoStack->beginTransaction()) {
        return result(false, QStringLiteral("busy"));
    }
    QScopedValueRollback<bool> applying(m_applying, true);
    const auto selection = timeline->getCurrentSelection();
    const bool singleSelection = timeline->singleSelectionMode();
    auto restoreSelection = [&]() {
        timeline->requestClearSelection();
        std::unordered_set<int> surviving;
        for (int id : selection) {
            if (timeline->isItem(id)) {
                surviving.insert(id);
            }
        }
        if (singleSelection) {
            for (int id : surviving) {
                timeline->requestAddToSelection(id, false, true);
            }
        } else {
            timeline->requestSetSelection(surviving);
        }
    };
    // Selection groups and single-selection mode must not override native AV or
    // ordinary group behavior for a specifically addressed clip.
    timeline->requestClearSelection();
    QString error;
    for (const auto &operation : operations) {
        if (!applyOperation(operation.toObject(), timeline, undoStack, error)) {
            undoStack->rollbackTransaction();
            restoreSelection();
            readState();
            auto failure = result(false, error);
            failure.insert(QStringLiteral("changed"), false);
            m_requests.insert(requestId, {digest, failure});
            return failure;
        }
    }
    restoreSelection();
    const bool changed = undoStack->commitTransaction(QStringLiteral("AI 剪辑"));
    const auto after = readState();
    std::unordered_set<int> previousIds, createdIds;
    for (const auto &clip : before["clips"].toArray()) {
        previousIds.insert(clip.toObject()["id"].toInt());
    }
    for (const auto &clip : after["clips"].toArray()) {
        const int id = clip.toObject()["id"].toInt();
        if (previousIds.count(id) == 0) {
            createdIds.insert(id);
        }
    }
    auto success = result(true, QStringLiteral("applied"));
    success.insert(QStringLiteral("changed"), changed);
    success.insert(QStringLiteral("createdClipIds"), sortedIds(createdIds));
    m_requests.insert(requestId, {digest, success});
    return success;
}

bool EditBridge::applyOperation(const QJsonObject &operation, const std::shared_ptr<TimelineItemModel> &timeline,
                                const std::shared_ptr<DocUndoStack> &undoStack, QString &error)
{
    auto reject = [&error](const QString &code) {
        error = code;
        return false;
    };
    const QString type = operation["type"].toString();
    if (type == QStringLiteral("template_story")) {
        if (!m_templateApplying) return reject(QStringLiteral("unsupported_operation"));
        const QString id = operation["templateId"].toString();
        const auto recipe = EditTemplates::find(id, operation["templateVersion"].toInt());
        const auto parameters = operation["parameters"].toObject();
        const auto texts = parameters["texts"].toArray();
        const auto subtitles = timeline->getSubtitleModel();
        if (subtitles && (subtitles->isLocked() || !subtitles->getAllSubtitles().empty())) return reject(QStringLiteral("template_subtitle_conflict"));
        const auto run = [&](const QJsonObject &op) { return applyOperation(op, timeline, undoStack, error); };
        if (id == QStringLiteral("talking_clean_v1")) {
            int clip;
            if (!integer(operation["clipId"], clip) || !timeline->isClip(clip) || texts.size() != 1 || texts.first().toString().trimmed().isEmpty()
                || parameters["originalAudio"].toString() != "keep" || parameters.contains("bgmBinId"))
                return reject(QStringLiteral("invalid_talking_template"));
            int targetTrack;
            if (!integer(operation["target"].toObject()["videoTrackId"], targetTrack) || targetTrack != timeline->getClipTrackId(clip)
                || timeline->isAudioTrack(targetTrack)) return reject(QStringLiteral("invalid_template_target"));
            if (operation["target"].toObject().contains("audioTrackId")) {
                int audioTrack;
                const int partner = timeline->getClipSplitPartner(clip);
                if (!integer(operation["target"].toObject()["audioTrackId"], audioTrack) || partner < 0
                    || audioTrack != timeline->getClipTrackId(partner)) return reject(QStringLiteral("invalid_template_target"));
            }
            const int start = timeline->getClipPosition(clip);
            if (!run({{"type", "retain_ranges"}, {"clipId", clip}, {"ranges", parameters["ranges"]}})) return false;
            int total = 0;
            for (const auto &value : parameters["ranges"].toArray()) {
                const auto range = value.toObject();
                total += range["end"].toInt() - range["start"].toInt();
            }
            if (parameters["totalFrames"].toInt() != total) return reject(QStringLiteral("invalid_total_frames"));
            return run({{"type", "subtitle"}, {"start", start}, {"end", start + total}, {"text", texts.first()}})
                && run({{"type", "subtitle_preset"}, {"preset", "clean"}});
        }
        if (!snapshot()["clips"].toArray().isEmpty() || timeline->getCompositionsCount() != 0)
            return reject(QStringLiteral("template_requires_empty_timeline"));
        const auto inputs = operation["inputs"].toArray();
        int total, video;
        const auto target = operation["target"].toObject();
        const QString policy = parameters["originalAudio"].toString();
        if (!integer(parameters["totalFrames"], total) || !integer(target["videoTrackId"], video)
            || !timeline->isTrack(video) || timeline->isAudioTrack(video) || timeline->trackIsLocked(video)
            || inputs.size() < recipe["minInputs"].toInt() || inputs.size() > recipe["maxInputs"].toInt()
            || texts.size() != inputs.size() || (policy != "keep" && policy != "mute")) return reject(QStringLiteral("invalid_template_parameters"));
        if (id == "vertical_teaser_v1" && pCore->getProjectProfile().height() <= pCore->getProjectProfile().width())
            return reject(QStringLiteral("template_requires_vertical_profile"));
        const auto frames = EditTemplates::allocate(total, inputs.size(), id);
        if (frames.size() != inputs.size()) return reject(QStringLiteral("invalid_total_frames"));
        bool needsAudio = false;
        for (int i = 0; i < inputs.size(); ++i) {
            const auto input = inputs[i].toObject();
            const auto asset = pCore->projectItemModel()->getClipByBinID(input["binId"].toString());
            int in, available;
            if (!asset || asset->clipStatus() != FileStatus::StatusReady || !asset->hasVideo()
                || !integer(input["sourceInFrames"], in) || !integer(input["sourceDurationFrames"], available)
                || available < frames[i] || qint64(in) + available > std::numeric_limits<int>::max()
                || texts[i].toString().trimmed().isEmpty()) return reject(QStringLiteral("invalid_template_source"));
            const bool image = asset->clipType() == ClipType::Image;
            if (!image && asset->clipType() != ClipType::Video && asset->clipType() != ClipType::AV)
                return reject(QStringLiteral("unsupported_template_source_type"));
            if (policy == "keep" && asset->hasAudio() && asset->activeStreams().size() != 1)
                return reject(QStringLiteral("template_requires_single_audio_stream"));
            if ((id == "photo_story_v1" && !image) || (image && in != 0)
                || (!image && qint64(in) + available > asset->getFramePlaytime())) return reject(QStringLiteral("template_source_too_short"));
            needsAudio = needsAudio || (policy == "keep" && asset->hasAudio());
        }
        const int mirror = timeline->getMirrorTrackId(video);
        if (needsAudio && (!timeline->isTrack(mirror) || timeline->trackIsLocked(mirror)
            || (!parameters.contains("bgmBinId") && target.contains("audioTrackId") && target["audioTrackId"].toInt() != mirror)))
            return reject(QStringLiteral("unavailable_linked_track"));
        const QString bgm = parameters["bgmBinId"].toString();
        int musicTrack = -1;
        double musicDb = 0;
        if (!bgm.isEmpty()) {
            const auto asset = pCore->projectItemModel()->getClipByBinID(bgm);
            if (!integer(target["audioTrackId"], musicTrack) || !timeline->isTrack(musicTrack) || !timeline->isAudioTrack(musicTrack)
                || timeline->trackIsLocked(musicTrack) || (needsAudio && musicTrack == mirror)
                || !asset || asset->clipStatus() != FileStatus::StatusReady || !asset->hasAudio() || asset->hasVideo() || asset->activeStreams().size() != 1
                || asset->getFramePlaytime() < total || !boundedNumber(parameters["bgmDb"], -60, 6, musicDb))
                return reject(QStringLiteral("invalid_template_bgm"));
        }
        const auto insert = [&](const QString &source, int track, int position, int in, int length, int &created) {
            Fun undo = []() { return true; }, redo = []() { return true; };
            const QString zone = QStringLiteral("%1/%2/%3").arg(source).arg(in).arg(in + length - 1);
            if (!timeline->requestClipInsertion(zone, track, position, created, true, true, false, undo, redo, {track, mirror}, 1)) {
                undo();
                return reject(QStringLiteral("operation_failed"));
            }
            undoStack->push(new FunctionalUndoCommand(undo, redo, QStringLiteral("模板插入素材")));
            return timeline->getClipPosition(created) == position && timeline->getClipPlaytime(created) == length
                && timeline->getClipInOut(created).first == in;
        };
        int position = 0;
        for (int i = 0; i < inputs.size(); ++i) {
            const auto input = inputs[i].toObject();
            const QString bin = input["binId"].toString();
            int created = -1;
            if (!insert((policy == "mute" ? QStringLiteral("V") : QString()) + bin, video, position,
                        input["sourceInFrames"].toInt(), frames[i], created)) return reject(error.isEmpty() ? QStringLiteral("template_native_mapping") : error);
            const auto asset = pCore->projectItemModel()->getClipByBinID(bin);
            if (policy == "keep" && asset->hasAudio()) {
                const int partner = timeline->getClipSplitPartner(created);
                if (partner < 0 || timeline->getClipInOut(partner) != timeline->getClipInOut(created)
                    || timeline->getClipPosition(partner) != position || timeline->getClipPlaytime(partner) != frames[i])
                    return reject(QStringLiteral("template_native_mapping"));
            }
            if (id == "photo_story_v1" && !run({{"type", "photo_motion"}, {"clipId", created}, {"direction", i % 2 ? "zoom_out" : "zoom_in"}, {"zoom", 1.08}})) return false;
            if (id == "product_three_shot_v1" && !run({{"type", "color_preset"}, {"clipId", created}, {"preset", "natural"}})) return false;
            const QString text = id == "before_after_v1" ? (i == 0 ? QStringLiteral("处理前：") : QStringLiteral("处理后：")) + texts[i].toString()
                : id == "travel_chapters_v1" ? QStringLiteral("第%1章：%2").arg(i + 1).arg(texts[i].toString()) : texts[i].toString();
            if (!run({{"type", "subtitle"}, {"start", position}, {"end", position + frames[i]}, {"text", text}})) return false;
            position += frames[i];
        }
        if (!bgm.isEmpty()) {
            int created = -1;
            if (!insert(QStringLiteral("A") + bgm, musicTrack, 0, 0, total, created)
                || !run({{"type", "volume"}, {"clipId", created}, {"db", musicDb}})) return false;
        }
        return run({{"type", "subtitle_preset"}, {"preset", id == "vertical_teaser_v1" ? "headline" : "clean"}});
    }
    if (type == QStringLiteral("color_preset")) {
        const QString preset = operation["preset"].toString();
        double brightness = 0, contrast = 1, saturation = 1;
        if (preset == QStringLiteral("soft")) { brightness = 0.02; contrast = 1.08; saturation = 1.12; }
        else if (preset == QStringLiteral("crisp")) { brightness = -0.02; contrast = 1.06; saturation = 0.88; }
        else if (preset == QStringLiteral("monochrome")) saturation = 0;
        else if (preset != QStringLiteral("natural")) return reject(QStringLiteral("invalid_color_preset"));
        return applyOperation(QJsonObject{{"type", "color_adjust"}, {"clipId", operation["clipId"]},
            {"brightness", brightness}, {"contrast", contrast}, {"saturation", saturation}}, timeline, undoStack, error);
    }
    if (type == QStringLiteral("subtitle_preset")) {
        const QString preset = operation["preset"].toString();
        if (preset != QStringLiteral("clean") && preset != QStringLiteral("headline") && preset != QStringLiteral("highlight"))
            return reject(QStringLiteral("invalid_subtitle_preset"));
        const auto model = timeline->getSubtitleModel();
        if (!model || model->rowCount() == 0 || model->isLocked()) return reject(QStringLiteral("locked_or_missing_subtitle"));
        const QString style = QStringLiteral("FontName=Microsoft YaHei,FontSize=%1,PrimaryColour=%2,OutlineColour=&H00000000,"
            "BorderStyle=1,Outline=%3,Shadow=0,Alignment=2,MarginV=30,Bold=%4")
            .arg(preset == QStringLiteral("headline") ? 60 : 42)
            .arg(preset == QStringLiteral("highlight") ? QStringLiteral("&H0000FFFF") : QStringLiteral("&H00FFFFFF"))
            .arg(preset == QStringLiteral("clean") ? 2 : 3).arg(preset == QStringLiteral("clean") ? 0 : 1);
        if (model->getForceStyle() != style) model->setForceStyle(style);
        return true;
    }
    if (type == QStringLiteral("retain_ranges")) {
        int clip;
        if (!integer(operation["clipId"], clip) || !timeline->isClip(clip)) return reject(QStringLiteral("invalid_clip"));
        const int position = timeline->getClipPosition(clip), duration = timeline->getClipPlaytime(clip);
        const int track = timeline->getClipTrackId(clip), partner = timeline->getClipSplitPartner(clip);
        const auto members = timeline->getGroupElements(clip);
        if (members.size() != (partner < 0 ? 1u : 2u) || (partner >= 0 && !members.count(partner)))
            return reject(QStringLiteral("retain_complex_group"));
        for (int member : members) {
            if (!timeline->isClip(member)) return reject(QStringLiteral("retain_complex_group"));
            if (timeline->trackIsLocked(timeline->getClipTrackId(member))) return reject(QStringLiteral("locked_track"));
            if (std::abs(timeline->getClipSpeed(member) - 1.0) > 1e-9
                || timeline->data(timeline->makeClipIndexFromID(member), TimelineModel::TimeRemapRole).toBool())
                return reject(QStringLiteral("retain_requires_normal_speed"));
            if (timeline->getMixDuration(member) > 0 || timeline->hasClipEndMix(member)) return reject(QStringLiteral("retain_mix_conflict"));
            if (timeline->getClipPosition(member) != position || timeline->getClipPlaytime(member) != duration
                || timeline->getClipInOut(member) != timeline->getClipInOut(clip)) return reject(QStringLiteral("retain_av_mapping"));
        }
        if (!operation["ranges"].isArray() || operation["ranges"].toArray().isEmpty()) return reject(QStringLiteral("invalid_retain_ranges"));
        QVector<std::pair<int, int>> ranges;
        int previousEnd = 0;
        for (const auto &value : operation["ranges"].toArray()) {
            const auto range = value.toObject();
            int start, end;
            if (!integer(range["start"], start) || !integer(range["end"], end) || start < previousEnd || end <= start || end > duration)
                return reject(QStringLiteral("invalid_retain_ranges"));
            ranges.push_back({start, end});
            previousEnd = end;
        }
        const auto subtitles = timeline->getSubtitleModel();
        if (subtitles) {
            for (int row = 0; row < subtitles->rowCount(); ++row) {
                const auto index = subtitles->index(row);
                if (subtitles->data(index, SubtitleModel::StartFrameRole).toInt() < qint64(position) + duration
                    && subtitles->data(index, SubtitleModel::EndFrameRole).toInt() > position)
                    return reject(QStringLiteral("retain_subtitle_conflict"));
            }
        }
        // Split only this clip's pure AV group. Descending boundaries preserve
        // the original ID for every subsequent cut and never affect other tracks.
        QVector<int> boundaries;
        for (const auto &range : ranges) { boundaries.push_back(range.first); boundaries.push_back(range.second); }
        std::sort(boundaries.begin(), boundaries.end(), std::greater<int>());
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
        for (int boundary : boundaries) {
            if (boundary > 0 && boundary < duration && !TimelineFunctions::requestClipCut(timeline, clip, position + boundary))
                return reject(QStringLiteral("operation_failed"));
        }
        QVector<int> retained;
        int cursor = 0;
        for (const auto &range : ranges) {
            if (cursor < range.first) {
                const int discarded = timeline->getClipByPosition(track, position + cursor);
                if (discarded < 0 || !timeline->requestItemDeletion(discarded, true)) return reject(QStringLiteral("operation_failed"));
            }
            retained.push_back(timeline->getClipByPosition(track, position + range.first));
            cursor = range.second;
        }
        if (cursor < duration) {
            const int discarded = timeline->getClipByPosition(track, position + cursor);
            if (discarded < 0 || !timeline->requestItemDeletion(discarded, true)) return reject(QStringLiteral("operation_failed"));
        }
        cursor = position;
        for (int retainedClip : retained) {
            if (retainedClip < 0 || (timeline->getClipPosition(retainedClip) != cursor
                && (!timeline->requestClipMove(retainedClip, track, cursor, true, true, true)
                    || timeline->getClipPosition(retainedClip) != cursor))) return reject(QStringLiteral("operation_failed"));
            cursor += timeline->getClipPlaytime(retainedClip);
        }
        return true;
    }
    if (type == QStringLiteral("subtitle_style")) {
        int fontSize;
        const QString position = operation["position"].toString();
        if (!integer(operation["fontSize"], fontSize) || fontSize < 12 || fontSize > 120
            || (position != QStringLiteral("bottom") && position != QStringLiteral("center") && position != QStringLiteral("top"))) {
            return reject(QStringLiteral("invalid_subtitle_style"));
        }
        const auto model = timeline->getSubtitleModel();
        if (!model || model->rowCount() == 0 || model->isLocked()) return reject(QStringLiteral("locked_or_missing_subtitle"));
        const int alignment = position == QStringLiteral("bottom") ? 2 : position == QStringLiteral("center") ? 5 : 8;
        const QString style = QStringLiteral("FontName=Microsoft YaHei,FontSize=%1,PrimaryColour=&H00FFFFFF,OutlineColour=&H00000000,"
                                             "BorderStyle=1,Outline=2,Shadow=0,Alignment=%2,MarginV=30").arg(fontSize).arg(alignment);
        if (model->getForceStyle() != style) model->setForceStyle(style);
        return true;
    }
    if (type == QStringLiteral("subtitle_text")) {
        int id;
        const QString text = operation["text"].toString();
        if (!integer(operation["subtitleId"], id) || text.trimmed().isEmpty()) {
            return reject(QStringLiteral("invalid_subtitle"));
        }
        const auto model = timeline->getSubtitleModel();
        if (!model || model->isLocked()) {
            return reject(QStringLiteral("locked_or_missing_subtitle"));
        }
        return model->setText(id, text) || reject(QStringLiteral("invalid_subtitle_id"));
    }
    if (type == QStringLiteral("subtitle")) {
        int start, end;
        const QString text = operation["text"].toString();
        if (!integer(operation["start"], start) || !integer(operation["end"], end) || end <= start || text.trimmed().isEmpty()) {
            return reject(QStringLiteral("invalid_subtitle"));
        }
        const auto model = timeline->createSubtitleModel();
        if (!model || model->isLocked()) {
            return reject(QStringLiteral("locked_subtitle"));
        }
        const double fps = pCore->getProjectProfile().fps();
        for (const auto &existing : model->getAllSubtitles()) {
            if (existing.first.first == 0 && existing.first.second.frames(fps) == start) {
                return reject(QStringLiteral("subtitle_start_conflict"));
            }
        }
        Fun undo = []() { return true; };
        Fun redo = []() { return true; };
        if (!model->addSubtitle({0, GenTime(start, fps)},
                SubtitleEvent(true, GenTime(end, fps), model->getLayerDefaultStyle(0), QString(), 0, 0, 0, QString(), text), undo, redo)) {
            return reject(QStringLiteral("operation_failed"));
        }
        undoStack->push(new FunctionalUndoCommand(undo, redo, QStringLiteral("添加字幕")));
        return true;
    }
    if (type == QStringLiteral("insert")) {
        int track, frame;
        const QString binId = operation["binId"].toString();
        if (!integer(operation["trackId"], track) || !timeline->isTrack(track) || !integer(operation["frame"], frame)
            || binId.isEmpty() || !pCore->projectItemModel()->getClipByBinID(binId)) {
            return reject(QStringLiteral("invalid_asset_or_target"));
        }
        if (timeline->trackIsLocked(track)) {
            return reject(QStringLiteral("locked_track"));
        }
        const auto asset = pCore->projectItemModel()->getClipByBinID(binId);
        if (asset->hasAudio() && asset->hasVideo()) {
            const int mirror = timeline->getMirrorTrackId(track);
            if (!timeline->isTrack(mirror) || timeline->trackIsLocked(mirror)) {
                return reject(QStringLiteral("unavailable_linked_track"));
            }
        }
        int inserted = -1;
        Fun undo = []() { return true; }, redo = []() { return true; };
        // The accumulating overload skips the public wrapper's track-creation
        // dialog while finalizing duration and view state as a committed edit.
        if (!timeline->requestClipInsertion(binId, track, frame, inserted, true, true, false, undo, redo, {}, 1)) {
            undo();
            return reject(QStringLiteral("operation_failed"));
        }
        undoStack->push(new FunctionalUndoCommand(undo, redo, QStringLiteral("插入素材")));
        return true;
    }
    if (type != QStringLiteral("split") && type != QStringLiteral("move") && type != QStringLiteral("trim") && type != QStringLiteral("delete")
        && type != QStringLiteral("volume") && type != QStringLiteral("crop") && type != QStringLiteral("watermark")
        && type != QStringLiteral("color_adjust") && type != QStringLiteral("photo_motion")
        && type != QStringLiteral("audio_normalize") && type != QStringLiteral("chroma_key")) {
        return reject(QStringLiteral("unsupported_operation"));
    }
    int clip;
    if (!integer(operation["clipId"], clip) || !timeline->isClip(clip)) {
        return reject(QStringLiteral("invalid_clip"));
    }
    if (type == QStringLiteral("split") || type == QStringLiteral("move") || type == QStringLiteral("trim") || type == QStringLiteral("delete")) {
        for (int member : timeline->getGroupElements(clip)) {
            if (timeline->isSubTitle(member)) {
                const auto model = timeline->getSubtitleModel();
                if (model && model->isLocked()) {
                    return reject(QStringLiteral("locked_subtitle"));
                }
            } else if (timeline->isItem(member)) {
                const int track = timeline->getItemTrackId(member);
                if (!timeline->isTrack(track) || timeline->trackIsLocked(track)) {
                    return reject(QStringLiteral("locked_track"));
                }
            }
        }
    }
    if (type == QStringLiteral("split")) {
        int frame;
        if (!integer(operation["frame"], frame) || frame <= timeline->getClipPosition(clip)
            || qint64(frame) >= qint64(timeline->getClipPosition(clip)) + timeline->getClipPlaytime(clip)) {
            return reject(QStringLiteral("invalid_frame"));
        }
        return TimelineFunctions::requestClipCut(timeline, clip, frame) || reject(QStringLiteral("operation_failed"));
    }
    if (type == QStringLiteral("move")) {
        int track, frame;
        if (!integer(operation["trackId"], track) || !timeline->isTrack(track) || !integer(operation["frame"], frame)) {
            return reject(QStringLiteral("invalid_target"));
        }
        if (timeline->trackIsLocked(track)) {
            return reject(QStringLiteral("locked_track"));
        }
        const int partner = timeline->getClipSplitPartner(clip);
        if (partner >= 0) {
            const int mirror = timeline->getMirrorTrackId(track);
            if (!timeline->isTrack(mirror) || timeline->trackIsLocked(mirror)) {
                return reject(QStringLiteral("unavailable_linked_track"));
            }
        }
        return (timeline->requestClipMove(clip, track, frame, true, true, true)
            && timeline->getClipTrackId(clip) == track && timeline->getClipPosition(clip) == frame) || reject(QStringLiteral("operation_failed"));
    }
    if (type == QStringLiteral("trim")) {
        int length;
        const QString side = operation["side"].toString();
        if (!integer(operation["duration"], length) || length <= 0 || (side != QStringLiteral("left") && side != QStringLiteral("right"))) {
            return reject(QStringLiteral("invalid_trim"));
        }
        return timeline->requestItemResize(clip, length, side == QStringLiteral("right"), true, -1, false) == length
            || reject(QStringLiteral("operation_failed"));
    }
    if (type == QStringLiteral("delete")) {
        return timeline->requestItemDeletion(clip, true) || reject(QStringLiteral("operation_failed"));
    }
    const bool audioEffect = type == QStringLiteral("volume") || type == QStringLiteral("audio_normalize");
    int target = clip;
    if (timeline->isAudioTrack(timeline->getClipTrackId(clip)) != audioEffect) {
        const int partner = timeline->getClipSplitPartner(clip);
        if (partner >= 0) {
            target = partner;
        }
    }
    const int targetTrack = timeline->getClipTrackId(target);
    if (timeline->trackIsLocked(targetTrack)) {
        return reject(QStringLiteral("locked_track"));
    }
    if (!audioEffect && timeline->isAudioTrack(targetTrack)) {
        return reject(QStringLiteral("video_required"));
    }
    if (type == QStringLiteral("audio_normalize") && (!timeline->isAudioTrack(targetTrack)
        || !timeline->data(timeline->makeClipIndexFromID(target), TimelineModel::CanBeAudioRole).toBool())) {
        return reject(QStringLiteral("audio_required"));
    }
    const auto stack = timeline->getClipEffectStackModel(target);
    QString effectId;
    stringMap parameters;
    const bool typedEffect = type == QStringLiteral("color_adjust") || type == QStringLiteral("photo_motion")
        || type == QStringLiteral("audio_normalize") || type == QStringLiteral("chroma_key");
    if (type == QStringLiteral("color_adjust")) {
        double brightness, contrast, saturation;
        if (!boundedNumber(operation["brightness"], -1, 1, brightness) || !boundedNumber(operation["contrast"], 0, 3, contrast)
            || !boundedNumber(operation["saturation"], 0, 5, saturation)) return reject(QStringLiteral("invalid_color_adjustment"));
        effectId = QStringLiteral("avfilter.eq");
        parameters = {{"av.brightness", QString::number(brightness, 'g', 12)}, {"av.contrast", QString::number(contrast, 'g', 12)},
                      {"av.saturation", QString::number(saturation, 'g', 12)}};
    } else if (type == QStringLiteral("photo_motion")) {
        double zoom;
        const QString direction = operation["direction"].toString();
        const int duration = timeline->getClipPlaytime(target);
        if (!boundedNumber(operation["zoom"], 1, 1.5, zoom) || duration < 2
            || (direction != QStringLiteral("zoom_in") && direction != QStringLiteral("zoom_out"))) return reject(QStringLiteral("invalid_motion"));
        if (std::abs(timeline->getClipSpeed(target) - 1.0) > 1e-9
            || timeline->data(timeline->makeClipIndexFromID(target), TimelineModel::TimeRemapRole).toBool()) return reject(QStringLiteral("motion_requires_normal_speed"));
        const int width = pCore->getProjectProfile().width(), height = pCore->getProjectProfile().height();
        const int sourceIn = timeline->getClipInOut(target).first;
        const auto rect = [width, height](double scale) {
            return QStringLiteral("%1 %2 %3 %4 1").arg(QString::number(width * (1 - scale) / 2, 'g', 12),
                QString::number(height * (1 - scale) / 2, 'g', 12), QString::number(width * scale, 'g', 12), QString::number(height * scale, 'g', 12));
        };
        const QString start = rect(direction == QStringLiteral("zoom_in") ? 1 : zoom);
        const QString end = rect(direction == QStringLiteral("zoom_in") ? zoom : 1);
        effectId = QStringLiteral("pan_zoom");
        parameters = {{"transition.rect", QStringLiteral("%1=%2;%3=%4").arg(sourceIn).arg(start).arg(sourceIn + duration - 1).arg(end)},
                      {"transition.distort", "0"}, {"use_normalised", "0"}, {"transition.repeat_off", "1"}, {"transition.mirror_off", "1"}};
    } else if (type == QStringLiteral("audio_normalize")) {
        double targetLufs;
        if (!boundedNumber(operation["targetLufs"], -50, -10, targetLufs)) return reject(QStringLiteral("invalid_loudness"));
        effectId = QStringLiteral("dynamic_loudness");
        parameters = {{"target_loudness", QString::number(targetLufs, 'g', 12)}, {"window", "3"}, {"max_gain", "15"}, {"min_gain", "-15"}, {"max_rate", "3"}};
    } else if (type == QStringLiteral("chroma_key")) {
        double tolerance;
        const QString color = operation["color"].toString();
        static const QRegularExpression rgb(QStringLiteral("^#[0-9a-fA-F]{6}$"));
        if (!rgb.match(color).hasMatch() || !boundedNumber(operation["tolerance"], 0, 1, tolerance)) return reject(QStringLiteral("invalid_chroma_key"));
        effectId = QStringLiteral("chroma");
        parameters = {{"key", QStringLiteral("0x%1ff").arg(color.mid(1))}, {"variance", QString::number(tolerance, 'g', 12)}};
    } else if (audioEffect) {
        const double db = operation["db"].toDouble();
        if (!operation["db"].isDouble() || !std::isfinite(db) || db < -100 || db > 50) {
            return reject(QStringLiteral("invalid_gain"));
        }
        effectId = QStringLiteral("volume");
        parameters.insert(QStringLiteral("level"), QString::number(db, 'g', 12) + QStringLiteral("dB"));
    } else {
        const int width = pCore->getProjectProfile().width(), height = pCore->getProjectProfile().height();
        if (type == QStringLiteral("crop")) {
            int top, left, bottom, right;
            if (!integer(operation["top"], top) || !integer(operation["left"], left) || !integer(operation["bottom"], bottom)
                || !integer(operation["right"], right) || qint64(top) + bottom >= height || qint64(left) + right >= width) {
                return reject(QStringLiteral("invalid_crop"));
            }
            effectId = QStringLiteral("crop");
            parameters = {{"top", QString::number(top)}, {"left", QString::number(left)}, {"bottom", QString::number(bottom)},
                          {"right", QString::number(right)}, {"center", "0"}, {"use_profile", "1"}};
        } else {
            const auto rect = operation["rect"].toObject();
            int x, y, w, h;
            if (!integer(rect["x"], x) || !integer(rect["y"], y) || !integer(rect["width"], w) || !integer(rect["height"], h)
                || w <= 0 || h <= 0 || qint64(x) + w > width || qint64(y) + h > height) {
                return reject(QStringLiteral("invalid_rect"));
            }
            const QString geometry = QStringLiteral("%1 %2 %3 %4").arg(x).arg(y).arg(w).arg(h);
            const QString method = operation["method"].toString();
            if (method == QStringLiteral("repair")) {
                if (x < 1 || y < 1 || qint64(x) + w >= width || qint64(y) + h >= height) {
                    return reject(QStringLiteral("repair_requires_border_use_blur_or_crop"));
                }
                effectId = QStringLiteral("avfilter.delogo");
                parameters = {{"av.x", QString::number(x)}, {"av.y", QString::number(y)}, {"av.w", QString::number(w)},
                              {"av.h", QString::number(h)}, {"av.show", "0"}, {"kdenlive:fakerect", geometry}};
            } else if (method == QStringLiteral("blur")) {
                effectId = QStringLiteral("obscure");
                parameters.insert(QStringLiteral("start"), geometry);
            } else {
                return reject(QStringLiteral("invalid_watermark_method"));
            }
        }
    }
    if (!stack || !EffectsRepository::get()->exists(effectId)) {
        return reject(QStringLiteral("effect_unavailable"));
    }
    if (typedEffect) {
        if (!stack->isStackEnabled()) return reject(QStringLiteral("effect_disabled"));
        std::shared_ptr<EffectItemModel> existing;
        for (const auto &effect : nativeEffects(stack)) {
            if (effect->getAssetId() != effectId) continue;
            if (existing) return reject(QStringLiteral("ambiguous_effect"));
            existing = effect;
        }
        if (existing) {
            if (!existing->isAssetEnabled()) return reject(QStringLiteral("effect_disabled"));
            if (existing->filter().get_int("kdenlive:force_in_out") == 1) return reject(QStringLiteral("effect_range_limited"));
            if (type != QStringLiteral("photo_motion")) {
                for (auto parameter = parameters.cbegin(); parameter != parameters.cend(); ++parameter) {
                    if (nativeParameter(existing, parameter.key()).keys > 1) return reject(QStringLiteral("effect_animated"));
                }
            }
            synchronizeFixedParameters(existing);
            auto values = existing->getAllParameters();
            for (auto parameter = parameters.cbegin(); parameter != parameters.cend(); ++parameter) {
                const auto entry = std::find_if(values.begin(), values.end(), [&](const auto &value) { return value.first == parameter.key(); });
                if (entry == values.end()) values.append({parameter.key(), parameter.value()});
                else entry->second = parameter.value();
            }
            undoStack->push(new AssetUpdateCommand(existing, values));
            return true;
        }
    }
    if (!stack->appendEffect(effectId, false, parameters)) return reject(QStringLiteral("operation_failed"));
    if (typedEffect) {
        for (const auto &effect : nativeEffects(stack)) {
            if (effect->getAssetId() == effectId) synchronizeFixedParameters(effect);
        }
    }
    return true;
}

void EditBridge::cancelPending() { ++m_revision; }
