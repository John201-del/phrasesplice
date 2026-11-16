/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>
#include <QSet>

class CodexClient;
class ChatCompletionsClient;
class WorkBuddyBridge;

// Route one active assistant through the editor's existing tool lifecycle.
class AssistantSession : public QObject
{
    Q_OBJECT
public:
    explicit AssistantSession(QObject *parent = nullptr);
    void start(const QString &program, const QString &directory, const QJsonArray &tools, const QString &instructions);
    void startCompatible(const QUrl &endpoint, const QString &model, const QString &key, const QJsonArray &tools, const QString &instructions);
    void startExternal(const QJsonArray &tools, const QString &instructions);
    void submit(const QString &text);
    void cancel();
    void stop();
    void replyToTool(const QJsonValue &token, const QJsonObject &result, bool success);
    void replyToTool(const QJsonValue &token, const QJsonArray &content, bool success);
    bool isReady() const;
    bool isBusy() const;
    bool isExternal() const;
    bool isCompatible() const;
    QJsonObject externalConfiguration() const;
Q_SIGNALS:
    void ready();
    void statusChanged(const QString &status);
    void textDelta(const QString &text);
    void toolRequested(const QJsonValue &token, const QString &name, const QJsonObject &arguments);
    void turnFinished(bool success);
    void failed(const QString &message);
    void externalToolStarted();
    void externalCancelRequested();
private:
    enum class Backend { None, Codex, Compatible, External };
    template<typename Client> void forward(Client *client, Backend backend);
    Backend m_backend = Backend::None;
    bool m_suppressExternalCancellation = false;
    QSet<QString> m_toolTokens;
    CodexClient *m_codex;
    ChatCompletionsClient *m_compatible;
    WorkBuddyBridge *m_external;
};
