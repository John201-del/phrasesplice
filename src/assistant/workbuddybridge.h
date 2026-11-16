/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QPointer>
#include <QSet>
#include <memory>

class QLocalSocket;
class QLockFile;

class WorkBuddyBridge : public QObject
{
    Q_OBJECT
public:
    explicit WorkBuddyBridge(QObject *parent = nullptr);
    ~WorkBuddyBridge() override;
    void start(const QJsonArray &tools, const QString &instructions);
    void stop();
    void cancel();
    bool isReady() const;
    bool isBusy() const;
    QJsonObject configuration() const;
    void replyToTool(const QJsonValue &token, const QJsonObject &result, bool success);
    void replyToTool(const QJsonValue &token, const QJsonArray &contentItems, bool success);
Q_SIGNALS:
    void ready();
    void statusChanged(const QString &status);
    void textDelta(const QString &text);
    void toolRequested(const QJsonValue &token, const QString &name, const QJsonObject &arguments);
    void turnFinished(bool success);
    void failed(const QString &message);
    void toolStarted();
    void cancelRequested();
private:
    void acceptConnection();
    void readMessages();
    void receive(const QJsonObject &message);
    bool send(const QJsonObject &message);
    void error(const QJsonValue &id, int code, const QString &message);
    void finish(const QJsonArray &content, bool success);
    QLocalServer m_server;
    std::unique_ptr<QLockFile> m_lock;
    QPointer<QLocalSocket> m_socket;
    QByteArray m_buffer;
    QJsonArray m_tools;
    QSet<QString> m_toolNames;
    QSet<QByteArray> m_requestIds;
    QString m_instructions;
    QString m_socketName;
    QString m_toolToken;
    QJsonValue m_toolId;
    bool m_initializeReceived = false;
    bool m_initialized = false;
};
