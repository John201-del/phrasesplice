/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "assistant/workspacepaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

class AssistantStorageTest : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir m_fixture{WorkspacePaths::root() + QStringLiteral("/storage-XXXXXX")};

    QJsonObject probe(const QString &root, const QString &candidate = {}, bool environmentOnly = false)
    {
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("LOCALCUT_WORKSPACE_ROOT"), environmentOnly ? root : m_fixture.path());
        environment.insert(QStringLiteral("LOCALCUT_TEST_ROOT"), environmentOnly ? QString() : root);
        environment.insert(QStringLiteral("LOCALCUT_TEST_CANDIDATE"), candidate);
        process.setProcessEnvironment(environment);
        process.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--probe")});
        if (!process.waitForFinished(15000) || process.exitCode() != 0) {
            return {{QStringLiteral("probeError"), QString::fromUtf8(process.readAllStandardError())}};
        }
        return QJsonDocument::fromJson(process.readAllStandardOutput()).object();
    }

    bool makeLink(const QString &link, const QString &target)
    {
#ifdef Q_OS_WIN
        QProcess process;
        process.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
            QDir::toNativeSeparators(link), QDir::toNativeSeparators(target)});
        return process.waitForFinished(15000) && process.exitCode() == 0;
#else
        return QFile::link(target, link);
#endif
    }

    bool removeLink(const QString &path)
    {
#ifdef Q_OS_WIN
        return RemoveDirectoryW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
#else
        return QFile::remove(path);
#endif
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY2(m_fixture.isValid(), "Create LOCALCUT_WORKSPACE_ROOT before running storage tests.");
    }

    void environmentRootWorksWithStandardQt()
    {
        const QString target = m_fixture.path() + QStringLiteral("/portable-中文");
        const auto result = probe(target, target + QStringLiteral("/exports/new/output.mp4"), true);
        QVERIFY2(result.value("ok").toBool(), qUtf8Printable(result.value("error").toString()));
        QCOMPARE(result.value("root").toString(), target);
        QVERIFY(result.value("inside").toBool());
        QVERIFY(result.value("settingsWritten").toBool());
        QVERIFY(result.value("settings").toString().startsWith(target + QStringLiteral("/config/")));
        QVERIFY(result.value("homeUnchanged").toBool());
        QVERIFY(result.value("profileUnchanged").toBool());
        QCOMPARE(result.value("temp").toString(), target + QStringLiteral("/tmp"));
        for (const QString &name : {QStringLiteral("projects"), QStringLiteral("exports"), QStringLiteral("cache/whisper")}) {
            QVERIFY(QFileInfo(target + QLatin1Char('/') + name).isDir());
        }
    }

    void explicitRootOverridesEnvironment()
    {
        const QString target = m_fixture.path() + QStringLiteral("/explicit");
        const auto result = probe(target, target);
        QVERIFY(result.value("ok").toBool());
        QCOMPARE(result.value("root").toString(), target);
        QVERIFY(result.value("inside").toBool());
    }

    void relativeRootRejected()
    {
        for (const bool environmentOnly : {false, true}) {
            const auto result = probe(QStringLiteral("relative-output"), {}, environmentOnly);
            QVERIFY(!result.contains("probeError"));
            QVERIFY(!result.value("ok").toBool());
            QVERIFY(!result.value("error").toString().isEmpty());
        }
        QVERIFY(!QFileInfo::exists(QStringLiteral("relative-output")));
    }

    void environmentTraversalIsRejectedBeforeNormalization()
    {
        const auto result = probe(m_fixture.path() + QStringLiteral("/child/../elsewhere"), {}, true);
        QVERIFY(!result.contains("probeError"));
        QVERIFY(!result.value("ok").toBool());
        QVERIFY(!QFileInfo::exists(m_fixture.path() + QStringLiteral("/elsewhere")));
    }

    void defaultRootUsesStandardUserDataWithoutCreatingIt()
    {
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.remove(QStringLiteral("LOCALCUT_WORKSPACE_ROOT"));
        process.setProcessEnvironment(environment);
        process.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--default-root")});
        QVERIFY(process.waitForFinished(15000));
        QCOMPARE(process.exitCode(), 0);
        const auto result = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        QCOMPARE(result.value("root").toString(), result.value("standardData").toString() + QStringLiteral("/LocalCut"));
    }

#ifdef Q_OS_WIN
    void installedResourcesUseTheKdenliveDataDirectory()
    {
        const auto organization = QCoreApplication::organizationName();
        const auto application = QCoreApplication::applicationName();
        const auto restore = qScopeGuard([organization, application] {
            QCoreApplication::setOrganizationName(organization);
            QCoreApplication::setApplicationName(application);
        });
        QCoreApplication::setApplicationName(QStringLiteral("kdenlive"));
        const QString directory = QCoreApplication::applicationDirPath() + QStringLiteral("/data/kdenlive");
        QVERIFY(QDir().mkpath(directory));
        QTemporaryFile marker(directory + QStringLiteral("/localcut-resource-XXXXXX"));
        QVERIFY(marker.open());
        marker.close();
        const QString relative = QFileInfo(marker.fileName()).fileName();
        QCoreApplication::setOrganizationName(QStringLiteral("LocalCut"));
        QVERIFY(QStandardPaths::locate(QStandardPaths::AppDataLocation, relative).isEmpty());
        // Match main.cpp: an empty organization name preserves the upstream resource layout.
        QCoreApplication::setOrganizationName({});
        QCOMPARE(QStandardPaths::locate(QStandardPaths::AppDataLocation, relative), marker.fileName());
    }
#endif

    void rejectsAdjacentPrefixRelativeAndTraversalPaths()
    {
        const QString target = m_fixture.path() + QStringLiteral("/boundary");
        for (const auto &candidate : {target + QStringLiteral("-other/result.mp4"), QStringLiteral("exports/output.mp4"),
                                     target + QStringLiteral("/../outside.mp4"), target + QStringLiteral("/exports/../../outside.mp4")}) {
            const auto result = probe(target, candidate);
            QVERIFY2(result.value("ok").toBool(), qUtf8Printable(result.value("error").toString()));
            QVERIFY2(!result.value("inside").toBool(), qUtf8Printable(candidate));
        }
    }

    void linkedCacheRejectedBeforeCreatingOutputs()
    {
        const QString target = m_fixture.path() + QStringLiteral("/linked-cache");
        const QString outside = m_fixture.path() + QStringLiteral("/outside");
        QVERIFY(QDir().mkpath(target));
        QVERIFY(QDir().mkpath(outside));
        const QString link = target + QStringLiteral("/cache");
        QVERIFY(makeLink(link, outside));
        const auto result = probe(target);
        const bool removed = removeLink(link);
        QVERIFY(removed);
        QVERIFY(!result.contains("probeError"));
        QVERIFY(!result.value("ok").toBool());
        QVERIFY(!QFileInfo::exists(target + QStringLiteral("/projects")));
        QVERIFY(!QFileInfo::exists(outside + QStringLiteral("/torch")));
        QVERIFY(QFileInfo(outside).isDir());
    }

    void linkedRootAndOutputBypassRejected()
    {
        const QString target = m_fixture.path() + QStringLiteral("/valid-root");
        const QString outside = m_fixture.path() + QStringLiteral("/link-target");
        QVERIFY(QDir().mkpath(target));
        QVERIFY(QDir().mkpath(outside));
        const QString link = target + QStringLiteral("/alias");
        QVERIFY(makeLink(link, outside));
        const auto rootResult = probe(link + QStringLiteral("/new-root"));
        const auto outputResult = probe(target, link + QStringLiteral("/new/output.mp4"));
        const bool removed = removeLink(link);
        QVERIFY(removed);
        QVERIFY(!rootResult.contains("probeError"));
        QVERIFY(!rootResult.value("ok").toBool());
        QVERIFY(outputResult.value("ok").toBool());
        QVERIFY(!outputResult.value("inside").toBool());
        QVERIFY(!QFileInfo::exists(outside + QStringLiteral("/new-root")));
        QVERIFY(QFileInfo(outside).isDir());
    }
};

int main(int argc, char **argv)
{
    if (argc == 2 && QByteArray(argv[1]) == "--default-root") {
        const QJsonObject result{{"root", WorkspacePaths::root()},
            {"standardData", QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)}};
        const auto json = QJsonDocument(result).toJson(QJsonDocument::Compact);
        std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
        return 0;
    }
    if (argc == 2 && QByteArray(argv[1]) == "--probe") {
        const QString home = qEnvironmentVariable("HOME");
        const QString profile = qEnvironmentVariable("USERPROFILE");
        QCoreApplication::setOrganizationName(QStringLiteral("LocalCutStorageTest"));
        QCoreApplication::setApplicationName(QStringLiteral("workspace-probe"));
        QString error;
        const bool ok = WorkspacePaths::initialize(qEnvironmentVariable("LOCALCUT_TEST_ROOT"), &error);
        QJsonObject result{{"ok", ok}, {"error", error}};
        if (ok) {
            QCoreApplication application(argc, argv);
            QSettings settings;
            settings.setValue(QStringLiteral("workspace"), true);
            settings.sync();
            result.insert("root", WorkspacePaths::root());
            result.insert("inside", WorkspacePaths::isInside(qEnvironmentVariable("LOCALCUT_TEST_CANDIDATE")));
            result.insert("settings", settings.fileName());
            result.insert("settingsWritten", settings.status() == QSettings::NoError && QFileInfo::exists(settings.fileName()));
            result.insert("temp", QDir::fromNativeSeparators(QDir::tempPath()));
            result.insert("homeUnchanged", qEnvironmentVariable("HOME") == home);
            result.insert("profileUnchanged", qEnvironmentVariable("USERPROFILE") == profile);
        }
        const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Compact);
        std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
        return 0;
    }
    QCoreApplication application(argc, argv);
    AssistantStorageTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "assistantstoragetest.moc"
