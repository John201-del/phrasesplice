/*
    SPDX-FileCopyrightText: 2017 Nicolas Carion
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QUndoCommand>
#include <memory>
#include <vector>

class QUndoGroup;
class QUndoCommand;

class DocUndoStack : public QUndoStack
{
    Q_OBJECT
public:
    explicit DocUndoStack(QUndoGroup *parent = Q_NULLPTR);
    void push(QUndoCommand *cmd);
    // Capture synchronous edits without changing the existing undo/redo history until commit.
    bool beginTransaction();
    bool commitTransaction(const QString &text);
    void rollbackTransaction();
Q_SIGNALS:
    void invalidate(int ix);
private:
    bool m_capturing{false};
    std::vector<std::unique_ptr<QUndoCommand>> m_pending;
};
