/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#pragma once

#include <QJsonObject>
#include <QHash>
#include <QSet>
#include <QVector>
#include <QObject>
#include <memory>

class DocUndoStack;
class TimelineItemModel;

class EditBridge : public QObject
{
    Q_OBJECT
public:
    explicit EditBridge(QObject *parent = nullptr);
    void bindContext(const QString &projectId, const std::shared_ptr<TimelineItemModel> &timeline,
                     const std::shared_ptr<DocUndoStack> &undoStack);
    QJsonObject readState();
    QJsonObject applyBatch(const QJsonObject &request);
    QJsonObject listEditTemplates();
    QJsonObject applyEditTemplate(const QJsonObject &request);
    void cancelPending();

private:
    struct Replay {
        QByteArray digest;
        QJsonObject result;
    };
    QJsonObject snapshot();
    QJsonObject result(bool ok, const QString &code) const;
    bool applyOperation(const QJsonObject &operation, const std::shared_ptr<TimelineItemModel> &timeline,
                        const std::shared_ptr<DocUndoStack> &undoStack, QString &error);
    void observeEffects(QObject *stack);
    std::weak_ptr<TimelineItemModel> m_timeline;
    std::weak_ptr<DocUndoStack> m_undoStack;
    QString m_projectId, m_sessionId, m_sequenceId;
    qint64 m_revision = 0;
    QByteArray m_fingerprint;
    bool m_applying = false;
    bool m_templateApplying = false;
    QMetaObject::Connection m_undoConnection;
    QVector<QMetaObject::Connection> m_modelConnections;
    QSet<QObject *> m_effectStacks;
    QHash<QString, Replay> m_requests;
};
