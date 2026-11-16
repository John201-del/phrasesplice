/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtEnvironmentVariables>

namespace LocalCutTestPaths {
inline QString directory(const char *variable, const QString &relative = {})
{
    const QString root = QDir::fromNativeSeparators(qEnvironmentVariable(variable));
    if (root.isEmpty() || !QDir::isAbsolutePath(root) || !QFileInfo(root).isDir())
        qFatal("Set %s to an existing absolute directory before running this test.", variable);
    return relative.isEmpty() ? QDir::cleanPath(root) : QDir(root).filePath(relative);
}

inline QString workspace(const QString &relative = {}) { return directory("LOCALCUT_WORKSPACE_ROOT", relative); }
inline QString fixture(const QString &relative = {}) { return directory("LOCALCUT_NATIVE_FIXTURES", relative); }

inline QString executable(const char *variable, const QString &name)
{
    const QString configured = qEnvironmentVariable(variable);
    const QString path = configured.isEmpty() ? QStandardPaths::findExecutable(name) : configured;
    if (path.isEmpty() || !QFileInfo(path).isAbsolute() || !QFileInfo(path).isExecutable())
        qFatal("Set %s to an executable or make %s available on PATH.", variable, qPrintable(name));
    return QDir::fromNativeSeparators(path);
}

inline QString source(const QString &relative)
{
    const QDir repository(QFileInfo(QString::fromUtf8(__FILE__)).absoluteDir().filePath(QStringLiteral("..")));
    return repository.filePath(relative);
}
}
