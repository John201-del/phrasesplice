/*
    SPDX-FileCopyrightText: 2017 Nicolas Carion
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "docundostack.hpp"
#include "undohelper.hpp"
#include <QUndoCommand>
#include <QUndoGroup>

DocUndoStack::DocUndoStack(QUndoGroup *parent)
    : QUndoStack(parent)
{
}

// TODO: custom undostack everywhere do that
void DocUndoStack::push(QUndoCommand *cmd)
{
    if (m_capturing) {
        cmd->redo();
        m_pending.emplace_back(cmd);
        return;
    }
    if (index() < count()) {
        Q_EMIT invalidate(index());
    }
    QUndoStack::push(cmd);
}

bool DocUndoStack::beginTransaction()
{
    if (m_capturing) {
        return false;
    }
    m_capturing = true;
    return true;
}

bool DocUndoStack::commitTransaction(const QString &text)
{
    if (!m_capturing) {
        return false;
    }
    m_capturing = false;
    if (m_pending.empty()) {
        return false;
    }
    auto commands = std::make_shared<std::vector<std::unique_ptr<QUndoCommand>>>(std::move(m_pending));
    push(new FunctionalUndoCommand(
        [commands] {
            for (auto it = commands->rbegin(); it != commands->rend(); ++it) {
                (*it)->undo();
            }
            return true;
        },
        [commands] {
            for (const auto &command : *commands) {
                command->redo();
            }
            return true;
        }, text));
    return true;
}

void DocUndoStack::rollbackTransaction()
{
    m_capturing = false;
    for (auto it = m_pending.rbegin(); it != m_pending.rend(); ++it) {
        (*it)->undo();
    }
    m_pending.clear();
}
