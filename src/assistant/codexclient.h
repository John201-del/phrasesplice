#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

class QProcess;

class CodexClient : public QObject
{
    Q_OBJECT
public:
    explicit CodexClient(QObject *parent = nullptr);
    ~CodexClient() override;
    void start(const QString &executable, const QString &workingDirectory,
               const QJsonArray &tools, const QString &instructions,
               const QString &model = QString(), const QStringList &argumentPrefix = {});
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
    void launch();
    void releaseProcess();
    void send(const QJsonObject &message);
    void request(const QString &method, const QJsonObject &params);
    void receive(const QJsonObject &message);
    void response(const QString &method, const QJsonObject &result);
    void checkConfiguration(const QJsonObject &config);
    void interrupt();
    void finishTurn(bool success);
    void fail(const QString &message);

    QProcess *m_process = nullptr;
    QTimer m_requestTimeout;
    QTimer m_cancelTimeout;
    QByteArray m_buffer;
    QHash<qint64, QString> m_pending;
    QHash<QString, QJsonValue> m_toolRequests;
    QSet<QString> m_inventoryCursors;
    QSet<QString> m_modelCursors;
    QJsonObject m_threadParams;
    QString m_executable;
    QString m_directory;
    QString m_runtimeDirectory;
    QStringList m_arguments;
    QStringList m_disabledServers;
    QString m_threadId;
    QString m_catalogDefaultModel;
    QString m_turnId;
    qint64 m_nextRequestId = 0;
    quint64 m_nextToolToken = 0;
    bool m_ready = false;
    bool m_busy = false;
    bool m_restarted = false;
    bool m_cancelRequested = false;
    bool m_interruptSent = false;
};
