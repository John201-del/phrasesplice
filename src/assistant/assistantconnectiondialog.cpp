/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "assistantconnectiondialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>

AssistantConnectionDialog::AssistantConnectionDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("连接 AI"));
    setMinimumWidth(540);
    auto *layout = new QVBoxLayout(this);
    auto *providerForm = new QFormLayout;
    m_provider = new QComboBox(this);
    m_provider->setObjectName(QStringLiteral("connectionProvider"));
    m_provider->addItem(tr("Codex（本机 CLI）"), QStringLiteral("codex"));
    m_provider->addItem(tr("WorkBuddy（MCP 外部控制）"), QStringLiteral("workbuddy"));
    m_provider->addItem(tr("DeepSeek"), QStringLiteral("deepseek"));
    m_provider->addItem(tr("通义千问"), QStringLiteral("qwen"));
    m_provider->addItem(tr("豆包 / 火山方舟"), QStringLiteral("doubao"));
    m_provider->addItem(tr("Ollama（本机）"), QStringLiteral("ollama"));
    m_provider->addItem(tr("自定义兼容 API"), QStringLiteral("custom"));
    providerForm->addRow(tr("服务"), m_provider);
    layout->addLayout(providerForm);

    m_help = new QLabel(this);
    m_help->setObjectName(QStringLiteral("connectionHelp"));
    m_help->setWordWrap(true);
    layout->addWidget(m_help);
    m_httpFields = new QWidget(this);
    m_httpFields->setObjectName(QStringLiteral("connectionHttpFields"));
    auto *form = new QFormLayout(m_httpFields);
    form->setContentsMargins(0, 0, 0, 0);
    m_endpoint = new QLineEdit(m_httpFields);
    m_endpoint->setObjectName(QStringLiteral("connectionEndpoint"));
    m_endpoint->setPlaceholderText(QStringLiteral("https://example.com/v1/chat/completions"));
    form->addRow(tr("完整请求地址"), m_endpoint);
    m_model = new QLineEdit(m_httpFields);
    m_model->setObjectName(QStringLiteral("connectionModel"));
    m_model->setPlaceholderText(tr("填写服务中实际可用的模型名或接入点 ID"));
    form->addRow(tr("模型 / 接入点 ID"), m_model);
    m_apiKey = new QLineEdit(m_httpFields);
    m_apiKey->setObjectName(QStringLiteral("connectionApiKey"));
    m_apiKey->setEchoMode(QLineEdit::Password);
    m_apiKey->setPlaceholderText(tr("按服务要求填写；本机 Ollama 通常可留空"));
    form->addRow(tr("API Key"), m_apiKey);
    auto *privacy = new QLabel(tr("密钥仅保留在本次运行的内存中，不写入设置。切换服务或请求地址的主机会清空密钥。"), m_httpFields);
    privacy->setWordWrap(true);
    form->addRow(privacy);
    layout->addWidget(m_httpFields);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->setObjectName(QStringLiteral("connectionButtons"));
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("连接"));
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    layout->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &AssistantConnectionDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_provider, &QComboBox::currentIndexChanged, this, &AssistantConnectionDialog::updateProvider);
    connect(m_model, &QLineEdit::textChanged, this, &AssistantConnectionDialog::updateActions);
    connect(m_endpoint, &QLineEdit::textChanged, this, [this](const QString &value) {
        const QUrl url(value.trimmed());
        const QString origin = url.scheme().toLower() + QStringLiteral("://") + url.authority().toLower();
        if (origin != m_endpointOrigin) m_apiKey->clear();
        m_endpointOrigin = origin;
        updateActions();
    });

    QSettings settings;
    settings.beginGroup(QStringLiteral("AssistantConnection"));
    const int savedProvider = m_provider->findData(settings.value(QStringLiteral("provider"), QStringLiteral("codex")));
    m_provider->setCurrentIndex(savedProvider < 0 ? 0 : savedProvider);
    updateProvider();
    if (usesHttp()) {
        m_endpoint->setText(settings.value(QStringLiteral("endpoint"), m_endpoint->text()).toString());
        m_model->setText(settings.value(QStringLiteral("model")).toString());
    }
    updateActions();
}

bool AssistantConnectionDialog::usesHttp() const
{
    const auto provider = m_provider->currentData().toString();
    return provider != QStringLiteral("codex") && provider != QStringLiteral("workbuddy");
}

bool AssistantConnectionDialog::hasValidHttpOptions() const
{
    const QUrl endpoint(m_endpoint->text().trimmed());
    const bool local = endpoint.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0 || QHostAddress(endpoint.host()).isLoopback();
    return endpoint.isValid() && !endpoint.host().isEmpty() && endpoint.userInfo().isEmpty() && !endpoint.hasQuery() && !endpoint.hasFragment()
        && (endpoint.scheme() == QStringLiteral("https") || (local && endpoint.scheme() == QStringLiteral("http")))
        && endpoint.path().endsWith(QStringLiteral("/chat/completions")) && !m_model->text().trimmed().isEmpty();
}

void AssistantConnectionDialog::updateProvider()
{
    m_apiKey->clear();
    m_model->clear();
    const auto provider = m_provider->currentData().toString();
    QString endpoint;
    if (provider == QStringLiteral("deepseek")) endpoint = QStringLiteral("https://api.deepseek.com/chat/completions");
    else if (provider == QStringLiteral("qwen")) endpoint = QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions");
    else if (provider == QStringLiteral("doubao")) endpoint = QStringLiteral("https://ark.cn-beijing.volces.com/api/v3/chat/completions");
    else if (provider == QStringLiteral("ollama")) endpoint = QStringLiteral("http://127.0.0.1:11434/v1/chat/completions");
    m_endpoint->setText(endpoint);
    m_httpFields->setVisible(usesHttp());
    if (provider == QStringLiteral("codex")) {
        m_help->setText(tr("使用本机已安装并登录的 Codex CLI。"));
    } else if (provider == QStringLiteral("workbuddy")) {
        m_help->setText(tr("通过 MCP 由 WorkBuddy 外部控制。使用时请让 PhraseSplice 保持打开，并在 WorkBuddy 中发送要求。"));
    } else {
        m_help->setText(tr("填写完整请求地址（含 /chat/completions），软件不会自动补路径。云端使用 HTTPS，本机回环地址可用 HTTP；地址不能含账号密码或 ?、# 参数。模型名或接入点 ID 请以所选服务中实际可用的值为准。"));
    }
    updateActions();
}

void AssistantConnectionDialog::updateActions()
{
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(!usesHttp() || hasValidHttpOptions());
}

AssistantConnectionOptions AssistantConnectionDialog::options() const
{
    AssistantConnectionOptions result;
    result.provider = m_provider->currentData().toString();
    if (usesHttp()) {
        result.endpoint = m_endpoint->text().trimmed();
        result.model = m_model->text().trimmed();
        result.apiKey = m_apiKey->text();
    }
    return result;
}

void AssistantConnectionDialog::accept()
{
    if (usesHttp() && !hasValidHttpOptions()) return;
    const auto selected = options();
    QSettings settings;
    settings.beginGroup(QStringLiteral("AssistantConnection"));
    settings.setValue(QStringLiteral("provider"), selected.provider);
    settings.setValue(QStringLiteral("endpoint"), selected.endpoint);
    settings.setValue(QStringLiteral("model"), selected.model);
    QDialog::accept();
}
