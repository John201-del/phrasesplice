/*
    SPDX-FileCopyrightText: 2018-2022 Jean-Baptiste Mardelle <jb@kdenlive.org>
    SPDX-FileCopyrightText: 2017-2019 Nicolas Carion <french.ebook.lover@gmail.com>
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#define CATCH_CONFIG_RUNNER
#include "catch.hpp"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QtEnvironmentVariables>

#include "bin/projectitemmodel.h"
#include "assistant/workspacepaths.h"
#include "core.h"
#include "mltconnection.h"
#include "src/effects/effectsrepository.hpp"
#include <mlt++/MltFactory.h>
#include <mlt++/MltRepository.h>

/* This file is intended to remain empty.
Write your tests in a file with a name corresponding to what you're testing */

QString getMltRepositoryPath()
{
    for (auto varname : {"CRAFT_ROOT", "KDEROOT"}) {
        if (!qEnvironmentVariableIsSet(varname)) {
            continue;
        }

        qDebug() << varname << "envvar is set, try to use it for MLT repository path";
        QString repositoryPath = QDir::cleanPath(qgetenv(varname) + QDir::separator() + QStringLiteral("lib/mlt"));
        if (!QFile::exists(repositoryPath)) {
            qDebug() << repositoryPath << "does not exist ($" << varname << "/lib/mlt)";
            QString repositoryPath = QDir::cleanPath(qgetenv(varname) + QDir::separator() + QStringLiteral("lib/mlt-7"));
        }
        if (!QFile::exists(repositoryPath)) {
            qDebug() << repositoryPath << "does not exist ($" << varname << "/lib/mlt-7)";
            return {};
        }

        qDebug() << "Using this path as MLT repository path:" << repositoryPath;

        return repositoryPath;
    }
    return {};
}

int main(int argc, char *argv[])
{
    QString storageError;
    const QString storageRoot = qEnvironmentVariable("LOCALCUT_WORKSPACE_ROOT");
    if (storageRoot.isEmpty() || !QDir::isAbsolutePath(storageRoot)) {
        qCritical("Set LOCALCUT_WORKSPACE_ROOT to an absolute writable test directory. See docs/BUILD.md.");
        return 2;
    }
    const QString fixtures = qEnvironmentVariable("LOCALCUT_NATIVE_FIXTURES");
    if (fixtures.isEmpty() || !QDir::isAbsolutePath(fixtures) || !QFileInfo(fixtures).isDir()) {
        qCritical("Set LOCALCUT_NATIVE_FIXTURES to the existing manual media fixture directory. See docs/BUILD.md.");
        return 2;
    }
    for (const auto &relative : {QStringLiteral("language-effects-20261107/运镜测试.png"), QStringLiteral("media/中文 空格 样片.mp4"),
                                QStringLiteral("media/中文测试-25fps-双声道.mp4"), QStringLiteral("avatar-templates-20261107/template-music-six-seconds.wav"),
                                QStringLiteral("avatar-20261107/speech2.wav"), QStringLiteral("local-extension-20261107/native-import-alpha-fixture.mov")}) {
        if (!QFileInfo(QDir(fixtures).filePath(relative)).isFile()) {
            qCritical().noquote() << "Missing file under LOCALCUT_NATIVE_FIXTURES:" << relative;
            return 2;
        }
    }
    if (!WorkspacePaths::initialize(storageRoot, &storageError)) {
        qCritical().noquote() << storageError;
        return 2;
    }
    QHashSeed::setDeterministicGlobalSeed();
    qputenv("MLT_REPOSITORY_DENY", "libmltqt:libmltglaxnimate");
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("kdenlive"));
    qSetMessagePattern(QStringLiteral("%{time hh:mm:ss.zzz } %{file}:%{line} -- %{message}"));

    QString repoPath = getMltRepositoryPath();
    if (!repoPath.isEmpty() && !qEnvironmentVariableIsSet("MLT_DATA")) {
        QString dataPath = QDir::cleanPath(repoPath + QStringLiteral("/../../share/mlt"));
        if (QFile::exists(dataPath)) {
            qDebug() << "Setting MLT_DATA to" << dataPath;
            qputenv("MLT_DATA", dataPath.toUtf8());
        }
    }

    std::unique_ptr<Mlt::Repository> repo(Mlt::Factory::init(repoPath.isEmpty() ? NULL : repoPath.toUtf8().constData()));
    qputenv("MLT_TESTS", QByteArray("1"));
    Core::build(LinuxPackageType::Unknown, true);
    MltConnection::construct(QString());
    pCore->projectItemModel()->buildPlaylist(QUuid());
    // if Kdenlive is not installed, ensure we have one keyframable effect
    EffectsRepository::get()->reloadCustom(QFileInfo("../data/effects/audiobalance.xml").absoluteFilePath());

    int result = Catch::Session().run(argc, argv);
    pCore->cleanup();
    pCore->mediaUnavailable.reset();

    // global clean-up...
    // delete repo;
    pCore->projectItemModel()->clean();
    pCore->cleanup();
    return (result < 0xff ? result : 0xff);
}
