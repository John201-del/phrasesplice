/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../src/assistant/assistantconnectiondialog.h"
#include "../../src/assistant/workspacepaths.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QtTest>

class AssistantConnectionDialogTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void connectionPresets_data();
    void connectionPresets();
    void connectionCredentialsClearWhenDestinationChanges();
    void connectionSettingsNeverPersistCredentials();
    void externalConnectionModesHideHttpFields();
    void connectionNeedsCompleteEndpointAndModel();
    void invalidEndpointsNeverReachSettings_data();
    void invalidEndpointsNeverReachSettings();
    void connectionAcceptsLoopbackHttp_data();
    void connectionAcceptsLoopbackHttp();
};

void AssistantConnectionDialogTest::initTestCase()
{
    const auto root = WorkspacePaths::root() + QStringLiteral("/connection-settings");
    QVERIFY(QDir().mkpath(root));
    QCoreApplication::setOrganizationName(QStringLiteral("LocalCutConnectionTests"));
    QCoreApplication::setApplicationName(QStringLiteral("assistantpaneltest"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root);
    QSettings().clear();
}

void AssistantConnectionDialogTest::connectionPresets_data()
{
    QTest::addColumn<QString>("providerId");
    QTest::addColumn<QString>("endpoint");
    QTest::newRow("deepseek") << QStringLiteral("deepseek") << QStringLiteral("https://api.deepseek.com/chat/completions");
    QTest::newRow("qwen") << QStringLiteral("qwen") << QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions");
    QTest::newRow("doubao") << QStringLiteral("doubao") << QStringLiteral("https://ark.cn-beijing.volces.com/api/v3/chat/completions");
    QTest::newRow("ollama") << QStringLiteral("ollama") << QStringLiteral("http://127.0.0.1:11434/v1/chat/completions");
    QTest::newRow("custom") << QStringLiteral("custom") << QString();
}

void AssistantConnectionDialogTest::connectionPresets()
{
    QFETCH(QString, providerId);
    QFETCH(QString, endpoint);
    QSettings().clear();
    AssistantConnectionDialog dialog;
    auto *providers = dialog.findChild<QComboBox *>(QStringLiteral("connectionProvider"));
    auto *model = dialog.findChild<QLineEdit *>(QStringLiteral("connectionModel"));
    QVERIFY(providers && model);
    QCOMPARE(providers->count(), 7);
    providers->setCurrentIndex(providers->findData(providerId));
    QCOMPARE(dialog.options().provider, providerId);
    QCOMPARE(dialog.options().endpoint, endpoint);
    QVERIFY(dialog.options().model.isEmpty());
    model->setText(QStringLiteral("my-available-model-or-endpoint-id"));
    QCOMPARE(dialog.options().model, model->text());
}

void AssistantConnectionDialogTest::connectionCredentialsClearWhenDestinationChanges()
{
    QSettings().clear();
    AssistantConnectionDialog dialog;
    auto *providers = dialog.findChild<QComboBox *>(QStringLiteral("connectionProvider"));
    auto *endpoint = dialog.findChild<QLineEdit *>(QStringLiteral("connectionEndpoint"));
    auto *key = dialog.findChild<QLineEdit *>(QStringLiteral("connectionApiKey"));
    QVERIFY(providers && endpoint && key);
    providers->setCurrentIndex(providers->findData(QStringLiteral("deepseek")));
    QCOMPARE(key->echoMode(), QLineEdit::Password);
    key->setText(QStringLiteral("test-memory-only-credential"));
    endpoint->setText(QStringLiteral("https://api.deepseek.com/v1/chat/completions"));
    QVERIFY(!key->text().isEmpty());
    endpoint->setText(QStringLiteral("https://example.invalid/chat/completions"));
    QVERIFY(key->text().isEmpty());
    key->setText(QStringLiteral("test-memory-only-credential"));
    providers->setCurrentIndex(providers->findData(QStringLiteral("qwen")));
    QVERIFY(key->text().isEmpty());
    key->setText(QStringLiteral("test-memory-only-credential"));
    providers->setCurrentIndex(providers->findData(QStringLiteral("codex")));
    QVERIFY(key->text().isEmpty());
}

void AssistantConnectionDialogTest::connectionSettingsNeverPersistCredentials()
{
    QSettings().clear();
    AssistantConnectionDialog dialog;
    auto *providers = dialog.findChild<QComboBox *>(QStringLiteral("connectionProvider"));
    auto *endpoint = dialog.findChild<QLineEdit *>(QStringLiteral("connectionEndpoint"));
    auto *model = dialog.findChild<QLineEdit *>(QStringLiteral("connectionModel"));
    auto *key = dialog.findChild<QLineEdit *>(QStringLiteral("connectionApiKey"));
    QVERIFY(providers && endpoint && model && key);
    providers->setCurrentIndex(providers->findData(QStringLiteral("custom")));
    endpoint->setText(QStringLiteral("https://example.invalid/v1/chat/completions"));
    model->setText(QStringLiteral("my-model"));
    key->setText(QStringLiteral("test-memory-only-credential"));
    dialog.accept();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.options().apiKey, key->text());
    QSettings saved;
    saved.sync();
    QCOMPARE(saved.allKeys(), QStringList({QStringLiteral("AssistantConnection/endpoint"), QStringLiteral("AssistantConnection/model"),
                                          QStringLiteral("AssistantConnection/provider")}));
    QFile file(saved.fileName());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(!file.readAll().contains("test-memory-only-credential"));
    AssistantConnectionDialog restored;
    QCOMPARE(restored.options().provider, QStringLiteral("custom"));
    QCOMPARE(restored.options().endpoint, endpoint->text());
    QCOMPARE(restored.options().model, model->text());
    QVERIFY(restored.options().apiKey.isEmpty());
    restored.findChild<QLineEdit *>(QStringLiteral("connectionModel"))->setText(QStringLiteral("cancelled-change"));
    restored.reject();
    QCOMPARE(QSettings().value(QStringLiteral("AssistantConnection/model")).toString(), QStringLiteral("my-model"));
}

void AssistantConnectionDialogTest::externalConnectionModesHideHttpFields()
{
    QSettings().clear();
    AssistantConnectionDialog dialog;
    auto *providers = dialog.findChild<QComboBox *>(QStringLiteral("connectionProvider"));
    auto *fields = dialog.findChild<QWidget *>(QStringLiteral("connectionHttpFields"));
    auto *help = dialog.findChild<QLabel *>(QStringLiteral("connectionHelp"));
    QVERIFY(providers && fields && help);
    QCOMPARE(dialog.windowTitle(), QStringLiteral("连接 AI"));
    QCOMPARE(dialog.options().provider, QStringLiteral("codex"));
    QVERIFY(fields->isHidden());
    providers->setCurrentIndex(providers->findData(QStringLiteral("workbuddy")));
    QVERIFY(fields->isHidden());
    QVERIFY(help->text().contains(QStringLiteral("MCP")));
    QVERIFY(help->text().contains(QStringLiteral("保持打开")));
    QVERIFY(dialog.options().endpoint.isEmpty());
    QVERIFY(dialog.options().apiKey.isEmpty());
    providers->setCurrentIndex(providers->findData(QStringLiteral("ollama")));
    QVERIFY(!fields->isHidden());
}

void AssistantConnectionDialogTest::connectionNeedsCompleteEndpointAndModel()
{
    QSettings().clear();
    AssistantConnectionDialog dialog;
    auto *providers = dialog.findChild<QComboBox *>(QStringLiteral("connectionProvider"));
    auto *endpoint = dialog.findChild<QLineEdit *>(QStringLiteral("connectionEndpoint"));
    auto *model = dialog.findChild<QLineEdit *>(QStringLiteral("connectionModel"));
    auto *buttons = dialog.findChild<QDialogButtonBox *>(QStringLiteral("connectionButtons"));
    QVERIFY(providers && endpoint && model && buttons);
    providers->setCurrentIndex(providers->findData(QStringLiteral("custom")));
    auto *connect = buttons->button(QDialogButtonBox::Ok);
    QVERIFY(!connect->isEnabled());
    model->setText(QStringLiteral("my-model"));
    endpoint->setText(QStringLiteral("/v1/chat/completions"));
    QVERIFY(!connect->isEnabled());
    endpoint->setText(QStringLiteral("http://127.0.0.1:11434/v1/chat/completions"));
    QVERIFY(connect->isEnabled());
    connect->click();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.options().endpoint, endpoint->text());
}

void AssistantConnectionDialogTest::invalidEndpointsNeverReachSettings_data()
{
    QTest::addColumn<QString>("endpoint");
    QTest::newRow("remote-http") << QStringLiteral("http://example.invalid/v1/chat/completions");
    QTest::newRow("lan-http") << QStringLiteral("http://192.0.2.1:11434/v1/chat/completions");
    QTest::newRow("incomplete-path") << QStringLiteral("https://example.invalid/v1");
    QTest::newRow("userinfo") << QStringLiteral("https://test-user:test-password@example.invalid/v1/chat/completions");
    QTest::newRow("query") << QStringLiteral("https://example.invalid/v1/chat/completions?api_key=test-query-credential");
    QTest::newRow("fragment") << QStringLiteral("https://example.invalid/v1/chat/completions#test-fragment-credential");
}

void AssistantConnectionDialogTest::invalidEndpointsNeverReachSettings()
{
    QFETCH(QString, endpoint);
    QSettings().clear();
    AssistantConnectionDialog dialog;
    auto *providers = dialog.findChild<QComboBox *>(QStringLiteral("connectionProvider"));
    auto *address = dialog.findChild<QLineEdit *>(QStringLiteral("connectionEndpoint"));
    auto *model = dialog.findChild<QLineEdit *>(QStringLiteral("connectionModel"));
    auto *buttons = dialog.findChild<QDialogButtonBox *>(QStringLiteral("connectionButtons"));
    QVERIFY(providers && address && model && buttons);
    providers->setCurrentIndex(providers->findData(QStringLiteral("custom")));
    model->setText(QStringLiteral("my-model"));
    address->setText(endpoint);
    QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
    dialog.accept();
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
    QVERIFY(QSettings().allKeys().isEmpty());
}

void AssistantConnectionDialogTest::connectionAcceptsLoopbackHttp_data()
{
    QTest::addColumn<QString>("endpoint");
    QTest::newRow("localhost") << QStringLiteral("http://localhost:11434/v1/chat/completions");
    QTest::newRow("ipv6-loopback") << QStringLiteral("http://[::1]:11434/v1/chat/completions");
}

void AssistantConnectionDialogTest::connectionAcceptsLoopbackHttp()
{
    QFETCH(QString, endpoint);
    QSettings().clear();
    AssistantConnectionDialog dialog;
    auto *providers = dialog.findChild<QComboBox *>(QStringLiteral("connectionProvider"));
    auto *address = dialog.findChild<QLineEdit *>(QStringLiteral("connectionEndpoint"));
    auto *model = dialog.findChild<QLineEdit *>(QStringLiteral("connectionModel"));
    QVERIFY(providers && address && model);
    providers->setCurrentIndex(providers->findData(QStringLiteral("custom")));
    model->setText(QStringLiteral("my-local-model"));
    address->setText(endpoint);
    dialog.accept();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.options().endpoint, endpoint);
}

QTEST_MAIN(AssistantConnectionDialogTest)
#include "assistantconnectiondialogtest.moc"
