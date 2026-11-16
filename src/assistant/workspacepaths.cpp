/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "workspacepaths.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <cstdlib>
#include <utility>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
QString storageRoot;
#ifdef Q_OS_WIN
constexpr auto pathCase = Qt::CaseInsensitive;
#else
constexpr auto pathCase = Qt::CaseSensitive;
#endif

bool within(const QString &path, const QString &root)
{
    return !root.isEmpty() && (path.compare(root, pathCase) == 0 || path.startsWith(root + QLatin1Char('/'), pathCase));
}

// Resolve an existing ancestor without following links; a new output's tail may not exist yet.
QString ordinaryPath(const QString &path)
{
    const QString native = QDir::fromNativeSeparators(path);
    if (!QDir::isAbsolutePath(native) || native.split(QLatin1Char('/')).contains(QStringLiteral(".."))) return {};
#ifdef Q_OS_WIN
    if (native.size() < 3 || !native[0].isLetter() || native[1] != QLatin1Char(':') || native[2] != QLatin1Char('/')) return {};
#endif
    const QString clean = QDir::cleanPath(native);
    QFileInfo current(clean);
    QString resolved;
    QStringList tail;
    for (;;) {
        if (current.isSymLink() || current.isJunction()) return {};
#ifdef Q_OS_WIN
        const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(current.absoluteFilePath()).utf16()));
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
#endif
        if (resolved.isEmpty()) {
            if (current.exists()) {
                if (!tail.isEmpty() && !current.isDir()) return {};
                resolved = QDir::fromNativeSeparators(current.canonicalFilePath());
                if (resolved.isEmpty()) return {};
                if (!tail.isEmpty()) resolved = QDir(resolved).filePath(tail.join(QLatin1Char('/')));
            } else {
                tail.prepend(current.fileName());
            }
        }
        const QString parent = current.absolutePath();
        if (parent == current.filePath()) break;
        current.setFile(parent);
    }
    return resolved.isEmpty() ? QString() : QDir::cleanPath(resolved);
}

bool setEnvironment(const char *name, const QString &value)
{
#ifdef Q_OS_WIN
    return _wputenv_s(QString::fromLatin1(name).toStdWString().c_str(), value.toStdWString().c_str()) == 0;
#else
    return qputenv(name, value.toUtf8());
#endif
}
}

QString WorkspacePaths::root()
{
    if (!storageRoot.isEmpty()) return storageRoot;
    const QString configured = qEnvironmentVariable("LOCALCUT_WORKSPACE_ROOT");
    return configured.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/LocalCut")
                                : QDir::fromNativeSeparators(configured);
}

bool WorkspacePaths::isInside(const QString &path)
{
    const QString base = ordinaryPath(root());
    const QString candidate = ordinaryPath(path);
    return within(candidate, base);
}

bool WorkspacePaths::initialize(const QString &root, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    const QString selected = root.isEmpty() ? WorkspacePaths::root() : root;
    const QString selectedRoot = ordinaryPath(selected);
    if (selectedRoot.isEmpty() || (QFileInfo::exists(selectedRoot) && !QFileInfo(selectedRoot).isDir()))
        return fail(QStringLiteral("工作区必须是没有符号链接或目录联接的本地绝对目录，请检查路径和权限。"));
    const QStringList directories = {
        QStringLiteral("data"), QStringLiteral("config"), QStringLiteral("config/kdenlive"), QStringLiteral("cache"), QStringLiteral("state"),
        QStringLiteral("projects"), QStringLiteral("documents"), QStringLiteral("images"), QStringLiteral("audio"), QStringLiteral("downloads"),
        QStringLiteral("exports"), QStringLiteral("capture"), QStringLiteral("library"), QStringLiteral("tmp"), QStringLiteral("cache/whisper"),
        QStringLiteral("cache/torch"), QStringLiteral("cache/huggingface"), QStringLiteral("cache/numba"), QStringLiteral("cache/pip")};
    for (const QString &directory : directories) {
        const QString target = selectedRoot + QLatin1Char('/') + directory;
        if (!within(ordinaryPath(target), selectedRoot)) {
            return fail(QStringLiteral("工作区存储目录包含链接或无效路径，请改用普通目录：%1").arg(target));
        }
    }
    for (const QString &directory : directories) {
        if (!QDir().mkpath(selectedRoot + QLatin1Char('/') + directory)) {
            return fail(QStringLiteral("无法创建工作区存储目录：%1").arg(selectedRoot + QLatin1Char('/') + directory));
        }
    }
    if (!setEnvironment("LOCALCUT_WORKSPACE_ROOT", selectedRoot)) return fail(QStringLiteral("无法设置工作区环境变量。"));
    storageRoot = selectedRoot;
    const std::pair<const char *, const char *> environment[] = {{"TEMP", "tmp"},
                                                               {"TMP", "tmp"},
                                                               {"XDG_CACHE_HOME", "cache"},
                                                               {"TORCH_HOME", "cache/torch"},
                                                               {"HF_HOME", "cache/huggingface"},
                                                               {"NUMBA_CACHE_DIR", "cache/numba"},
                                                               {"PIP_CACHE_DIR", "cache/pip"}};
    for (const auto &entry : environment) {
        if (!setEnvironment(entry.first, storageRoot + QLatin1Char('/') + QString::fromLatin1(entry.second))) {
            return fail(QStringLiteral("无法设置工作区缓存环境变量：%1").arg(QString::fromLatin1(entry.first)));
        }
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, storageRoot + QStringLiteral("/config"));
    if (error) {
        error->clear();
    }
    return true;
}
