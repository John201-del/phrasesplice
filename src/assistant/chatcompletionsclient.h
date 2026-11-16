/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUrl>

class QNetworkReply;

class ChatCompletionsClient : public QObject
{
    Q_OBJECT
public:
    explicit ChatCompletionsClient(QObject *parent = nullptr);
    ~ChatCompletionsClient() override;
    void start(const QUrl &endpoint, const QString &model, const QString &apiKey,
               const QJsonArray &tools, const QString &instructions);
    void submit(const QString &text);
    void cancel();
    void stop();
    void replyToTool(const QJsonValue &requestId, const QJsonObject &result, bool success);
    void replyToTool(const QJsonValue &requestId, const QJsonArray &contentItems, bool success);
    bool isReady() const;
    bool isBusy() const;
Q_SIGNALS:
    void ready();
    void statusChanged(const QString &status);
    void textDelta(const QString &text);
    void toolRequested(const QJsonValue &requestId, const QString &name, const QJsonObject &arguments);
    void turnFinished(bool success);
    void failed(const QString &message);
private:
    void requestCompletion();
    void receive(const QByteArray &body);
    void dispatchTool();
    void abortRequest();
    void finishTurn(bool success);
    void fail(const QString &message);

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_timeout;
    QUrl m_endpoint;
    QString m_model, m_apiKey, m_toolToken;
    QJsonArray m_tools, m_messages, m_calls, m_images;
    QSet<QString> m_toolNames, m_seenCallIds;
    QByteArray m_buffer;
    quint64 m_epoch = 0, m_nextToken = 0;
    qsizetype m_turnStart = 0, m_callIndex = 0;
    int m_rounds = 0;
    bool m_ready = false, m_busy = false;
};
