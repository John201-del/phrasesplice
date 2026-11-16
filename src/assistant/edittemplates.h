/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#pragma once
#include <QJsonObject>
#include <QVector>

namespace EditTemplates {
QJsonObject catalog();
QJsonObject find(const QString &id, int version);
QVector<int> allocate(int total, int count, const QString &pattern);
}
