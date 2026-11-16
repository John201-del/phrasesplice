/* SPDX-License-Identifier: GPL-3.0-only */
#include "assistantsession.h"
#include "codexclient.h"
#include "chatcompletionsclient.h"
#include "workbuddybridge.h"
#include <QScopedValueRollback>

template<typename Client> void AssistantSession::forward(Client *client, Backend backend)
{
    connect(client, &Client::ready, this, [this, backend] { if (m_backend == backend) Q_EMIT ready(); });
    connect(client, &Client::statusChanged, this, [this, backend](const QString &value) { if (m_backend == backend) Q_EMIT statusChanged(value); });
    connect(client, &Client::textDelta, this, [this, backend](const QString &value) { if (m_backend == backend) Q_EMIT textDelta(value); });
    connect(client, &Client::toolRequested, this, [this, backend](const QJsonValue &token, const QString &name, const QJsonObject &args) {
        if (m_backend != backend || !token.isString() || token.toString().isEmpty()) return;
        m_toolTokens.insert(token.toString());
        Q_EMIT toolRequested(token, name, args);
    });
    connect(client, &Client::turnFinished, this, [this, backend](bool success) {
        if (m_backend == backend) { m_toolTokens.clear(); Q_EMIT turnFinished(success); }
    });
    connect(client, &Client::failed, this, [this, backend](const QString &value) {
        if (m_backend == backend) { m_toolTokens.clear(); Q_EMIT failed(value); }
    });
}

AssistantSession::AssistantSession(QObject *parent) : QObject(parent), m_codex(new CodexClient(this)),
    m_compatible(new ChatCompletionsClient(this)), m_external(new WorkBuddyBridge(this))
{
    forward(m_codex, Backend::Codex);
    forward(m_compatible, Backend::Compatible);
    forward(m_external, Backend::External);
    connect(m_external, &WorkBuddyBridge::toolStarted, this, [this] { if (isExternal()) Q_EMIT externalToolStarted(); });
    connect(m_external, &WorkBuddyBridge::cancelRequested, this, [this] {
        if (isExternal() && !m_suppressExternalCancellation) Q_EMIT externalCancelRequested();
    });
}

void AssistantSession::start(const QString &program, const QString &directory, const QJsonArray &tools, const QString &instructions)
{
    stop(); m_backend = Backend::Codex; m_codex->start(program, directory, tools, instructions);
}

void AssistantSession::startCompatible(const QUrl &endpoint, const QString &model, const QString &key, const QJsonArray &tools, const QString &instructions)
{
    stop(); m_backend = Backend::Compatible; m_compatible->start(endpoint, model, key, tools, instructions);
}

void AssistantSession::startExternal(const QJsonArray &tools, const QString &instructions)
{
    stop(); m_backend = Backend::External; m_external->start(tools, instructions);
}

void AssistantSession::stop()
{
    const QScopedValueRollback<bool> suppressCancellation(m_suppressExternalCancellation, true);
    // Disconnect the route before stopping providers, so late completion cannot affect a new turn.
    m_backend = Backend::None; m_toolTokens.clear(); m_codex->stop(); m_compatible->stop(); m_external->stop();
}

void AssistantSession::submit(const QString &text)
{
    if (m_backend == Backend::Codex) m_codex->submit(text);
    else if (m_backend == Backend::Compatible) m_compatible->submit(text);
}

void AssistantSession::cancel()
{
    const QScopedValueRollback<bool> suppressCancellation(m_suppressExternalCancellation, true);
    m_toolTokens.clear();
    if (m_backend == Backend::Codex) m_codex->cancel();
    else if (m_backend == Backend::Compatible) m_compatible->cancel();
    else if (m_backend == Backend::External) m_external->cancel();
}

void AssistantSession::replyToTool(const QJsonValue &token, const QJsonObject &result, bool success)
{
    if (!token.isString() || !m_toolTokens.remove(token.toString())) return;
    if (m_backend == Backend::Codex) m_codex->replyToTool(token, result, success);
    else if (m_backend == Backend::Compatible) m_compatible->replyToTool(token, result, success);
    else if (m_backend == Backend::External) m_external->replyToTool(token, result, success);
}

void AssistantSession::replyToTool(const QJsonValue &token, const QJsonArray &content, bool success)
{
    if (!token.isString() || !m_toolTokens.remove(token.toString())) return;
    if (m_backend == Backend::Codex) m_codex->replyToTool(token, content, success);
    else if (m_backend == Backend::Compatible) m_compatible->replyToTool(token, content, success);
    else if (m_backend == Backend::External) m_external->replyToTool(token, content, success);
}

bool AssistantSession::isReady() const
{
    return m_backend == Backend::Codex ? m_codex->isReady() : m_backend == Backend::Compatible ? m_compatible->isReady()
        : m_backend == Backend::External && m_external->isReady();
}

bool AssistantSession::isBusy() const
{
    return m_backend == Backend::Codex ? m_codex->isBusy() : m_backend == Backend::Compatible ? m_compatible->isBusy()
        : m_backend == Backend::External && m_external->isBusy();
}

bool AssistantSession::isExternal() const { return m_backend == Backend::External; }
bool AssistantSession::isCompatible() const { return m_backend == Backend::Compatible; }
QJsonObject AssistantSession::externalConfiguration() const { return m_external->configuration(); }
