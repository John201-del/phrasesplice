/* SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL */
#include "../../src/doc/docundostack.hpp"
#include "../../src/undohelper.hpp"
#include <QSignalSpy>
#include <QtTest>

namespace {
void assign(DocUndoStack &stack, int &value, int next)
{
    const int previous = value;
    value = next;
    stack.push(new FunctionalUndoCommand([&value, previous] { value = previous; return true; },
                                         [&value, next] { value = next; return true; }, QStringLiteral("change")));
}
}

class AssistantUndoTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void commandsExecuteOnlyOnceWhenCaptured()
    {
        class Increment : public QUndoCommand {
        public:
            explicit Increment(int &value) : m_value(value) {}
            void redo() override { ++m_value; }
            void undo() override { --m_value; }
        private:
            int &m_value;
        };
        DocUndoStack stack;
        int value = 0;
        QVERIFY(stack.beginTransaction());
        stack.push(new Increment(value));
        stack.push(new Increment(value));
        QCOMPARE(value, 2);
        QVERIFY(stack.commitTransaction(QStringLiteral("regular commands")));
        QCOMPARE(value, 2);
        stack.undo();
        QCOMPARE(value, 0);
        stack.redo();
        QCOMPARE(value, 2);
    }
    void oneUndoRestoresTheEntireBatch()
    {
        DocUndoStack stack;
        int value = 0;
        assign(stack, value, 1);
        QVERIFY(stack.beginTransaction());
        assign(stack, value, 10);
        assign(stack, value, 20);
        QCOMPARE(value, 20);
        QCOMPARE(stack.count(), 1);
        QVERIFY(stack.commitTransaction(QStringLiteral("Assistant edits")));
        QCOMPARE(stack.count(), 2);
        QCOMPARE(value, 20);
        stack.undo();
        QCOMPARE(value, 1);
        stack.redo();
        QCOMPARE(value, 20);
        stack.undo();
        stack.undo();
        QCOMPARE(value, 0);
    }
    void rollbackPreservesExistingRedoHistoryAndCleanState()
    {
        DocUndoStack stack;
        int value = 0;
        assign(stack, value, 1);
        stack.setClean();
        assign(stack, value, 2);
        stack.undo();
        QSignalSpy invalidated(&stack, &DocUndoStack::invalidate);
        QVERIFY(stack.beginTransaction());
        assign(stack, value, 10);
        assign(stack, value, 20);
        stack.rollbackTransaction();
        QCOMPARE(value, 1);
        QCOMPARE(stack.index(), 1);
        QCOMPARE(stack.count(), 2);
        QVERIFY(stack.isClean());
        QCOMPARE(invalidated.count(), 0);
        stack.redo();
        QCOMPARE(value, 2);
    }
    void committingInvalidatesTheRedoBranchOnce()
    {
        DocUndoStack stack;
        int value = 0;
        assign(stack, value, 1);
        assign(stack, value, 2);
        stack.undo();
        QSignalSpy invalidated(&stack, &DocUndoStack::invalidate);
        QVERIFY(stack.beginTransaction());
        assign(stack, value, 10);
        assign(stack, value, 20);
        QCOMPARE(invalidated.count(), 0);
        QVERIFY(stack.commitTransaction(QStringLiteral("Assistant edits")));
        QCOMPARE(invalidated.count(), 1);
        QCOMPARE(stack.count(), 2);
        stack.undo();
        QCOMPARE(value, 1);
        stack.redo();
        QCOMPARE(value, 20);
    }
    void emptyAndNestedTransactionsCannotLoseEdits()
    {
        DocUndoStack stack;
        int value = 0;
        QVERIFY(stack.beginTransaction());
        QVERIFY(!stack.beginTransaction());
        QVERIFY(!stack.commitTransaction(QStringLiteral("empty")));
        QCOMPARE(stack.count(), 0);
        QVERIFY(stack.beginTransaction());
        assign(stack, value, 5);
        stack.rollbackTransaction();
        QCOMPARE(value, 0);
        QVERIFY(!stack.commitTransaction(QStringLiteral("closed")));
        QCOMPARE(stack.count(), 0);
    }
};
QTEST_GUILESS_MAIN(AssistantUndoTest)
#include "assistantundotest.moc"
