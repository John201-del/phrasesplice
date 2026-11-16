/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#pragma once

#include <QString>

namespace WorkspacePaths {
bool initialize(const QString &root, QString *error);
QString root();
bool isInside(const QString &path);
}
