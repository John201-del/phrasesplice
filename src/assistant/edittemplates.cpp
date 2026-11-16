/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "edittemplates.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QSet>

QJsonObject EditTemplates::catalog()
{
    QFile file(QStandardPaths::locate(QStandardPaths::AppDataLocation, QStringLiteral("assistant/edit_templates.json")));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto root = QJsonDocument::fromJson(file.readAll()).object();
    if (root["schemaVersion"].toInt() != 1 || root["templates"].toArray().size() != 8) return {};
    for (auto it = root.begin(); it != root.end(); ++it) if (it.key() != "schemaVersion" && it.key() != "templates") return {};
    const QSet<QString> ids{"photo_story_v1", "knowledge_steps_v1", "product_three_shot_v1", "travel_chapters_v1",
                            "event_recap_v1", "before_after_v1", "vertical_teaser_v1", "talking_clean_v1"};
    QSet<QString> seen;
    for (const auto &value : root["templates"].toArray()) {
        const auto item = value.toObject();
        const QString id = item["id"].toString();
        if (!ids.contains(id) || seen.contains(id) || item["version"].toInt() != 1) return {};
        if (!item["name"].isString() || item["name"].toString().isEmpty() || !item["description"].isString()) return {};
        const int minimum = item["minInputs"].toInt(-1), maximum = item["maxInputs"].toInt(-1);
        if (minimum < 0 || maximum > 12 || minimum > maximum || (id == "talking_clean_v1" ? maximum != 0 : minimum < 2)) return {};
        for (auto it = item.begin(); it != item.end(); ++it)
            if (!QSet<QString>{"id", "version", "name", "description", "minInputs", "maxInputs"}.contains(it.key())) return {};
        seen.insert(id);
    }
    return root;
}

QJsonObject EditTemplates::find(const QString &id, int version)
{
    for (const auto &value : catalog()["templates"].toArray()) {
        const auto item = value.toObject();
        if (item["id"].toString() == id && item["version"].toInt() == version) return item;
    }
    return {};
}

QVector<int> EditTemplates::allocate(int total, int count, const QString &pattern)
{
    if (count < 1 || total < count) return {};
    QVector<int> weights(count, 1), frames;
    if (pattern == "product_three_shot_v1" && count == 3) weights = {1, 1, 2};
    if (pattern == "event_recap_v1" && count >= 3) {
        weights.fill(3, count);
        weights[0] = weights[count - 1] = count - 2;
    }
    if (pattern == "knowledge_steps_v1") weights[count - 1] = 2;
    if (pattern == "vertical_teaser_v1") weights[0] = 2;
    int sum = 0, used = 0;
    for (int weight : weights) sum += weight;
    for (int i = 0; i < count; ++i) {
        const int value = i == count - 1 ? total - used : int(qint64(total) * weights[i] / sum);
        if (value < 1) return {};
        frames.append(value);
        used += value;
    }
    return frames;
}
