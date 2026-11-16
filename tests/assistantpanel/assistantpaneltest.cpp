/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../src/assistant/assistantpanel.h"

#include "../../src/assistant/workspacepaths.h"
#include <QAction>
#include <QComboBox>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSignalSpy>
#include <QSpinBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QtTest>

class AssistantPanelTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void sendingRequiresReadyIdleAndText();
    void sendEmitsTextAndClearsInput();
    void stateUpdatesKeepDraft();
    void newlineAndControlEnter();
    void inputSendOverridesConflictingWindowShortcut_data();
    void inputSendOverridesConflictingWindowShortcut();
    void stopWaitsForBackend();
    void transcriptPreservesPlainChineseStream();
    void interleavedStatusKeepsAssistantOwnership();
    void toolbarActionsEmitSignals();
    void videoRequiresPromptAndKey();
    void videoEmitsFormAndStopsWaiting();
    void videoProviderDefaultsToSeedanceAndKeepsKeyRequirement();
    void ltxProviderAllowsEmptyKeyAndShowsQuotaMessage();
    void ltxFirstFrameBlocksGenerationUntilCleared();
    void prepareVideoWithLtxFirstFrameDoesNotSubmitOrChangeProvider();
    void switchingBackToSeedanceRestoresKeyRequirement();
    void seedanceQualityOffers720pAnd1080p();
    void videoRequestAppendsProviderAndResolution();
    void ltxProviderDisablesResume();
    void providerSelectionIsLockedWhileBusy();
    void narrowSidebarFitsBothPages();
    void resumeNeedsKeyAndGuardsRepeatedRequests();
    void prepareVideoOnlyPopulatesForm();
    void quickTasksOnlyFillDraftAndRespectBusyState();
    void localHistoryWorksWithoutModelConnection();
    void outputFolderActionsWorkWithoutModelConnection();
    void chatAreasDistinguishHistoryFromEditableRequest();
    void chineseInputCommitsAndSends();
    void ltxDurationMatchesSupportedRange();
    void toolbarWaitsForAssistantToFinish();
    void creativeQuickTasksOnlyPopulateDraft();
    void compactFreeVideoPageKeepsStatusReachable();
    void connectionButtonNamesAllAiProviders();
    void externalControlBlocksInternalRequestsAndKeepsHistory();
};

void AssistantPanelTest::sendingRequiresReadyIdleAndText()
{
    AssistantPanel panel;
    auto *input = panel.findChild<QPlainTextEdit *>(QStringLiteral("messageInput"));
    auto *send = panel.findChild<QPushButton *>(QStringLiteral("sendButton"));
    QVERIFY(input);
    QVERIFY(send);
    input->setPlainText(QStringLiteral("剪到十秒"));
    QVERIFY(!send->isEnabled());
    panel.setAssistantState(true, true, QStringLiteral("正在处理"));
    QVERIFY(!send->isEnabled());
    panel.setAssistantState(true, false, QStringLiteral("可以开始"));
    QVERIFY(send->isEnabled());
    input->setPlainText(QStringLiteral(" \n\t"));
    QVERIFY(!send->isEnabled());
}

void AssistantPanelTest::sendEmitsTextAndClearsInput()
{
    AssistantPanel panel;
    panel.resize(360, 780);
    panel.show();
    QVERIFY(QTest::qWaitForWindowActive(&panel));
    auto *input = panel.findChild<QPlainTextEdit *>(QStringLiteral("messageInput"));
    auto *send = panel.findChild<QPushButton *>(QStringLiteral("sendButton"));
    QVERIFY(input);
    QVERIFY(send);
    QSignalSpy requests(&panel, &AssistantPanel::messageRequested);
    panel.setAssistantState(true, false, {});
    input->setPlainText(QStringLiteral("  保留开头\n加中文字幕  "));
    QTest::mouseClick(send, Qt::LeftButton);
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.first().first().toString(), QStringLiteral("保留开头\n加中文字幕"));
    QVERIFY(input->toPlainText().isEmpty());
    QVERIFY(!send->isEnabled());
}

void AssistantPanelTest::stateUpdatesKeepDraft()
{
    AssistantPanel panel;
    auto *input = panel.findChild<QPlainTextEdit *>(QStringLiteral("messageInput"));
    QVERIFY(input);
    input->setPlainText(QStringLiteral("明天继续修改的草稿"));
    panel.setAssistantState(false, true, QStringLiteral("连接中"));
    panel.setAssistantState(true, false, QStringLiteral("已连接"));
    panel.appendStatus(QStringLiteral("素材已选择"));
    panel.setVideoState(true, QStringLiteral("正在生成"));
    QCOMPARE(input->toPlainText(), QStringLiteral("明天继续修改的草稿"));
}

void AssistantPanelTest::newlineAndControlEnter()
{
    AssistantPanel panel;
    auto *input = panel.findChild<QPlainTextEdit *>(QStringLiteral("messageInput"));
    QVERIFY(input);
    panel.resize(360, 760);
    panel.show();
    QVERIFY(QTest::qWaitForWindowActive(&panel));
    panel.setAssistantState(true, false, {});
    QSignalSpy requests(&panel, &AssistantPanel::messageRequested);
    input->setFocus();
    input->setPlainText(QStringLiteral("第一段"));
    input->moveCursor(QTextCursor::End);
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(requests.size(), 0);
    QCOMPARE(input->toPlainText(), QStringLiteral("第一段\n"));
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
    QCOMPARE(requests.size(), 1);
    panel.setAssistantState(false, false, {});
    input->setPlainText(QStringLiteral("尚未连接"));
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
    QCOMPARE(requests.size(), 1);
}

void AssistantPanelTest::inputSendOverridesConflictingWindowShortcut_data()
{
    QTest::addColumn<bool>("useAction");
    QTest::addColumn<int>("key");
    QTest::addColumn<bool>("keypad");
    QTest::newRow("action-return") << true << int(Qt::Key_Return) << false;
    QTest::newRow("shortcut-return") << false << int(Qt::Key_Return) << false;
    QTest::newRow("action-keypad-enter") << true << int(Qt::Key_Enter) << true;
    QTest::newRow("shortcut-keypad-enter") << false << int(Qt::Key_Enter) << true;
}

void AssistantPanelTest::inputSendOverridesConflictingWindowShortcut()
{
    QFETCH(bool, useAction);
    QFETCH(int, key);
    QFETCH(bool, keypad);
    QWidget host;
    host.setFocusPolicy(Qt::StrongFocus);
    auto *layout = new QVBoxLayout(&host);
    auto *panel = new AssistantPanel(&host);
    layout->addWidget(panel);
    int hostRequests = 0;
    const QKeySequence sequence(Qt::CTRL | key);
    if (useAction) {
        auto *action = new QAction(&host);
        action->setShortcut(sequence);
        action->setShortcutContext(Qt::WindowShortcut);
        host.addAction(action);
        connect(action, &QAction::triggered, &host, [&hostRequests] { ++hostRequests; });
    } else {
        auto *shortcut = new QShortcut(sequence, &host);
        shortcut->setContext(Qt::WindowShortcut);
        connect(shortcut, &QShortcut::activated, &host, [&hostRequests] { ++hostRequests; });
    }
    host.resize(420, 850);
    host.show();
    QVERIFY(QTest::qWaitForWindowActive(&host));
    auto *input = panel->findChild<QPlainTextEdit *>(QStringLiteral("messageInput"));
    QVERIFY(input);
    QSignalSpy requests(panel, &AssistantPanel::messageRequested);
    panel->setAssistantState(true, false, {});
    input->setFocus();
    input->setPlainText(QStringLiteral("添加字幕"));
    input->moveCursor(QTextCursor::End);
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(input->toPlainText(), QStringLiteral("添加字幕\n"));
    QCOMPARE(requests.size(), 0);
    Qt::KeyboardModifiers modifiers = Qt::ControlModifier;
    if (keypad) modifiers |= Qt::KeypadModifier;
    QTest::keyClick(input, Qt::Key(key), modifiers);
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.first().first().toString(), QStringLiteral("添加字幕"));
    QCOMPARE(hostRequests, 0);
    for (const bool busy : {false, true}) {
        panel->setAssistantState(busy, busy, {});
        input->setPlainText(QStringLiteral("保留草稿"));
        QTest::keyClick(input, Qt::Key(key), modifiers);
        QCOMPARE(requests.size(), 1);
        QCOMPARE(input->toPlainText(), QStringLiteral("保留草稿"));
        QCOMPARE(hostRequests, 0);
    }
    host.setFocus();
    QTest::keyClick(&host, Qt::Key(key), modifiers);
    QCOMPARE(hostRequests, 1);
    QCOMPARE(requests.size(), 1);
}

void AssistantPanelTest::stopWaitsForBackend()
{
    AssistantPanel panel;
    panel.resize(360, 780);
    panel.show();
    QVERIFY(QTest::qWaitForWindowActive(&panel));
    auto *stop = panel.findChild<QPushButton *>(QStringLiteral("stopButton"));
    auto *transcript = panel.findChild<QPlainTextEdit *>(QStringLiteral("transcript"));
    QVERIFY(stop);
    QVERIFY(transcript);
    QVERIFY(!stop->isEnabled());
    panel.appendStatus(QStringLiteral("正在调整"));
    const QString before = transcript->toPlainText();
    QSignalSpy stopped(&panel, &AssistantPanel::stopRequested);
    panel.setAssistantState(true, true, QStringLiteral("正在调整"));
    QTest::mouseClick(stop, Qt::LeftButton);
    QCOMPARE(stopped.size(), 1);
    QCOMPARE(transcript->toPlainText(), before);
    QVERIFY(stop->isEnabled());
}

void AssistantPanelTest::transcriptPreservesPlainChineseStream()
{
    AssistantPanel panel;
    auto *transcript = panel.findChild<QPlainTextEdit *>(QStringLiteral("transcript"));
    QVERIFY(transcript);
    QVERIFY(transcript->isReadOnly());
    panel.appendUserMessage(QStringLiteral("缩短视频"));
    panel.beginAssistantMessage();
    panel.appendAssistantText(QStringLiteral("正在"));
    panel.appendAssistantText(QStringLiteral("处理中文"));
    panel.appendAssistantText(QStringLiteral("<b>原样</b>\n下一行"));
    panel.appendStatus(QStringLiteral("等待确认"));
    const auto text = transcript->toPlainText();
    QVERIFY(text.contains(QStringLiteral("正在处理中文<b>原样</b>\n下一行")));
    QVERIFY(text.indexOf(QStringLiteral("缩短视频")) < text.indexOf(QStringLiteral("正在处理")));
    QVERIFY(text.indexOf(QStringLiteral("下一行")) < text.indexOf(QStringLiteral("等待确认")));
}

void AssistantPanelTest::toolbarActionsEmitSignals()
{
    AssistantPanel panel;
    panel.resize(360, 780);
    panel.show();
    QVERIFY(QTest::qWaitForWindowActive(&panel));
    QSignalSpy choose(&panel, &AssistantPanel::chooseMediaRequested);
    QSignalSpy exportVideo(&panel, &AssistantPanel::exportRequested);
    QSignalSpy connect(&panel, &AssistantPanel::connectRequested);
    QSignalSpy undo(&panel, &AssistantPanel::undoRequested);
    panel.setAssistantState(true, false, {});
    for (const auto &name : {"chooseMediaButton", "exportButton", "connectButton", "undoButton"}) {
        auto *button = panel.findChild<QPushButton *>(QLatin1String(name));
        QVERIFY(button);
        QTest::mouseClick(button, Qt::LeftButton);
    }
    QCOMPARE(choose.size(), 1);
    QCOMPARE(exportVideo.size(), 1);
    QCOMPARE(connect.size(), 1);
    QCOMPARE(undo.size(), 1);
}

void AssistantPanelTest::interleavedStatusKeepsAssistantOwnership()
{
    AssistantPanel panel;
    auto *transcript = panel.findChild<QPlainTextEdit *>(QStringLiteral("transcript"));
    QVERIFY(transcript);
    panel.beginAssistantMessage();
    panel.appendAssistantText(QStringLiteral("先剪开"));
    panel.appendStatus(QStringLiteral("已分割"));
    panel.appendAssistantText(QStringLiteral("再加"));
    panel.appendAssistantText(QStringLiteral("字幕"));
    QVERIFY(transcript->toPlainText().contains(QStringLiteral("助手：先剪开\n已分割\n助手：再加字幕")));
    panel.appendUserMessage(QStringLiteral("继续"));
    panel.appendAssistantText(QStringLiteral("继续处理"));
    QVERIFY(transcript->toPlainText().endsWith(QStringLiteral("你：继续\n助手：继续处理")));
}

void AssistantPanelTest::videoRequiresPromptAndKey()
{
    AssistantPanel panel;
    auto *prompt = panel.findChild<QPlainTextEdit *>(QStringLiteral("videoPrompt"));
    auto *key = panel.findChild<QLineEdit *>(QStringLiteral("videoKey"));
    auto *generate = panel.findChild<QPushButton *>(QStringLiteral("videoGenerateButton"));
    auto *status = panel.findChild<QLabel *>(QStringLiteral("videoStatus"));
    QVERIFY(prompt);
    QVERIFY(key);
    QVERIFY(generate);
    QVERIFY(status);
    QVERIFY(status->text().startsWith(QStringLiteral("未连接：")));
    QCOMPARE(key->echoMode(), QLineEdit::Password);
    QSignalSpy requests(&panel, &AssistantPanel::videoRequested);
    prompt->setPlainText(QStringLiteral("海边日出"));
    QVERIFY(!generate->isEnabled());
    QTest::mouseClick(generate, Qt::LeftButton);
    QCOMPARE(requests.size(), 0);
    key->setText(QStringLiteral("  "));
    QVERIFY(!generate->isEnabled());
    key->setText(QStringLiteral("test-only-key"));
    QVERIFY(generate->isEnabled());
    panel.setVideoState(true, QStringLiteral("等待服务"));
    QVERIFY(!generate->isEnabled());
}

void AssistantPanelTest::videoEmitsFormAndStopsWaiting()
{
    AssistantPanel panel;
    panel.resize(360, 780);
    panel.show();
    QVERIFY(QTest::qWaitForWindowActive(&panel));
    auto *tabs = panel.findChild<QTabWidget *>(QStringLiteral("assistantTabs"));
    auto *prompt = panel.findChild<QPlainTextEdit *>(QStringLiteral("videoPrompt"));
    auto *key = panel.findChild<QLineEdit *>(QStringLiteral("videoKey"));
    auto *frame = panel.findChild<QLineEdit *>(QStringLiteral("videoFirstFrame"));
    auto *ratio = panel.findChild<QComboBox *>(QStringLiteral("videoRatio"));
    auto *duration = panel.findChild<QSpinBox *>(QStringLiteral("videoDuration"));
    auto *generate = panel.findChild<QPushButton *>(QStringLiteral("videoGenerateButton"));
    auto *stop = panel.findChild<QPushButton *>(QStringLiteral("videoStopButton"));
    auto *transcript = panel.findChild<QPlainTextEdit *>(QStringLiteral("transcript"));
    QVERIFY(tabs && prompt && key && frame && ratio && duration && generate && stop && transcript);
    tabs->setCurrentIndex(1);
    QCOMPARE(duration->minimum(), 4);
    QCOMPARE(duration->maximum(), 30);
    QCOMPARE(duration->value(), 5);
    QVERIFY(!stop->isEnabled());
    prompt->setPlainText(QStringLiteral("  海边日出  "));
    key->setText(QStringLiteral("test-only-key"));
    frame->setText((WorkspacePaths::root() + QStringLiteral("/test-first-frame.png")));
    ratio->setCurrentText(QStringLiteral("9:16"));
    duration->setValue(12);
    QSignalSpy requests(&panel, &AssistantPanel::videoRequested);
    QSignalSpy cancelled(&panel, &AssistantPanel::cancelVideoRequested);
    QTest::mouseClick(generate, Qt::LeftButton);
    QCOMPARE(requests.size(), 1);
    const auto fields = requests.first();
    QCOMPARE(fields.size(), 7);
    QCOMPARE(fields[0].toString(), QStringLiteral("海边日出"));
    QCOMPARE(fields[1].toString(), frame->text());
    QCOMPARE(fields[2].toString(), QStringLiteral("9:16"));
    QCOMPARE(fields[3].toInt(), 12);
    QCOMPARE(fields[4].toString(), QStringLiteral("test-only-key"));
    QCOMPARE(fields[5].toString(), QStringLiteral("seedance"));
    QCOMPARE(fields[6].toString(), QStringLiteral("720p"));
    QVERIFY(!generate->isEnabled());
    QVERIFY(stop->isEnabled());
    QTest::mouseClick(stop, Qt::LeftButton);
    QCOMPARE(cancelled.size(), 1);
    QVERIFY(stop->isEnabled());
    QVERIFY(!transcript->toPlainText().contains(QStringLiteral("test-only-key")));
    panel.setVideoState(false, QStringLiteral("等待已结束"));
    QVERIFY(generate->isEnabled());
    QVERIFY(!stop->isEnabled());
}


void AssistantPanelTest::videoProviderDefaultsToSeedanceAndKeepsKeyRequirement()
{
    AssistantPanel panel;
    auto *provider = panel.findChild<QComboBox *>(QStringLiteral("videoProvider"));
    auto *prompt = panel.findChild<QPlainTextEdit *>(QStringLiteral("videoPrompt"));
    auto *key = panel.findChild<QLineEdit *>(QStringLiteral("videoKey"));
    auto *notice = panel.findChild<QLabel *>(QStringLiteral("videoProviderNotice"));
    auto *generate = panel.findChild<QPushButton *>(QStringLiteral("videoGenerateButton"));
    QVERIFY(provider && prompt && key && notice && generate);
    QCOMPARE(provider->count(), 2);
    QCOMPARE(provider->currentData().toString(), QStringLiteral("seedance"));
    QCOMPARE(provider->itemData(1).toString(), QStringLiteral("ltx-space"));
    QVERIFY(key->isEnabled());
    QVERIFY(notice->text().contains(QStringLiteral("高质量")));
    QVERIFY(notice->text().contains(QStringLiteral("付费")));
    QVERIFY(generate->text().contains(QStringLiteral("Seedance 付费")));
    prompt->setPlainText(QStringLiteral("海边日出"));
    QVERIFY(!generate->isEnabled());
    key->setText(QStringLiteral("test-only-key"));
    QVERIFY(generate->isEnabled());
}

void AssistantPanelTest::ltxProviderAllowsEmptyKeyAndShowsQuotaMessage()
{
    AssistantPanel panel;
    auto *provider = panel.findChild<QComboBox *>(QStringLiteral("videoProvider"));
    auto *quality = panel.findChild<QComboBox *>(QStringLiteral("videoQuality"));
    auto *prompt = panel.findChild<QPlainTextEdit *>(QStringLiteral("videoPrompt"));
    auto *key = panel.findChild<QLineEdit *>(QStringLiteral("videoKey"));
    auto *notice = panel.findChild<QLabel *>(QStringLiteral("videoProviderNotice"));
    auto *generate = panel.findChild<QPushButton *>(QStringLiteral("videoGenerateButton"));
    QVERIFY(provider && quality && prompt && key && notice && generate);
    provider->setCurrentIndex(provider->findData(QStringLiteral("ltx-space")));
    QVERIFY(key->text().isEmpty());
    QVERIFY(!key->isEnabled());
    QCOMPARE(quality->count(), 1);
    QCOMPARE(quality->currentData().toString(), QStringLiteral("space-default"));
    QVERIFY(notice->text().contains(QStringLiteral("官方公共 Space")));
    QVERIFY(notice->text().contains(QStringLiteral("共享配额")));
    QVERIFY(notice->text().contains(QStringLiteral("排队")));
    QVERIFY(notice->text().contains(QStringLiteral("不可用")));
    QVERIFY(notice->text().contains(QStringLiteral("不转付费")));
    QVERIFY(generate->text().contains(QStringLiteral("LTX 免费共享额度")));
    QVERIFY(!generate->isEnabled());
    prompt->setPlainText(QStringLiteral("海边日出"));
    QVERIFY(generate->isEnabled());
    QSignalSpy requests(&panel, &AssistantPanel::videoRequested);
    generate->click();
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.first().size(), 7);
    QCOMPARE(requests.first()[4].toString(), QString());
    QCOMPARE(requests.first()[5].toString(), QStringLiteral("ltx-space"));
    QCOMPARE(requests.first()[6].toString(), QStringLiteral("space-default"));

    panel.setVideoState(false, QStringLiteral("测试结束"));
    provider->setCurrentIndex(provider->findData(QStringLiteral("seedance")));
    key->setText(QStringLiteral("unit-test-credential"));
    provider->setCurrentIndex(provider->findData(QStringLiteral("ltx-space")));
    QCOMPARE(key->text(), QStringLiteral("unit-test-credential"));
    QVERIFY(key->isHidden());
    generate->click();
    QCOMPARE(requests.size(), 2);
    QCOMPARE(requests.last()[4].toString(), QString());
    QCOMPARE(requests.last()[5].toString(), QStringLiteral("ltx-space"));
}

void AssistantPanelTest::ltxFirstFrameBlocksGenerationUntilCleared()
{
    AssistantPanel panel;
    auto *provider = panel.findChild<QComboBox *>(QStringLiteral("videoProvider"));
    auto *prompt = panel.findChild<QPlainTextEdit *>(QStringLiteral("videoPrompt"));
    auto *key = panel.findChild<QLineEdit *>(QStringLiteral("videoKey"));
    auto *frame = panel.findChild<QLineEdit *>(QStringLiteral("videoFirstFrame"));
    auto *choose = panel.findChild<QPushButton *>(QStringLiteral("videoChooseFrameButton"));
    auto *clear = panel.findChild<QPushButton *>(QStringLiteral("videoClearFrameButton"));
    auto *generate = panel.findChild<QPushButton *>(QStringLiteral("videoGenerateButton"));
    auto *notice = panel.findChild<QLabel *>(QStringLiteral("videoProviderNotice"));
    QVERIFY(provider && prompt && key && frame && choose && clear && generate && notice);
    prompt->setPlainText(QStringLiteral("海边日出"));
    key->setText(QStringLiteral("test-only-key"));
    const auto path = (WorkspacePaths::root() + QStringLiteral("/first-frame.png"));
    frame->setText(path);
    QVERIFY(generate->isEnabled());
    QVERIFY(choose->isEnabled());
    provider->setCurrentIndex(provider->findData(QStringLiteral("ltx-space")));
    QCOMPARE(frame->text(), path);
    QVERIFY(!generate->isEnabled());
    QVERIFY(!choose->isEnabled());
    QVERIFY(clear->isEnabled());
    QVERIFY(notice->text().contains(QStringLiteral("仅文生视频，需清除首帧")));
    QSignalSpy requests(&panel, &AssistantPanel::videoRequested);
    generate->click();
    QVERIFY(QMetaObject::invokeMethod(generate, "clicked", Qt::DirectConnection, Q_ARG(bool, false)));
    QCOMPARE(requests.size(), 0);
    provider->setCurrentIndex(provider->findData(QStringLiteral("seedance")));
    QCOMPARE(frame->text(), path);
    QVERIFY(choose->isEnabled());
    QVERIFY(generate->isEnabled());
    generate->click();
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.first()[1].toString(), path);
    QCOMPARE(requests.first()[5].toString(), QStringLiteral("seedance"));
    panel.setVideoState(false, QStringLiteral("等待已结束"));
    provider->setCurrentIndex(provider->findData(QStringLiteral("ltx-space")));
    clear->click();
    QVERIFY(frame->text().isEmpty());
    QVERIFY(generate->isEnabled());
    frame->setText(path);
    QVERIFY(!generate->isEnabled());
    clear->click();
    QVERIFY(generate->isEnabled());
    generate->click();
    QCOMPARE(requests.size(), 2);
    QVERIFY(requests.last()[1].toString().isEmpty());
    QCOMPARE(requests.last()[5].toString(), QStringLiteral("ltx-space"));
}

void AssistantPanelTest::prepareVideoWithLtxFirstFrameDoesNotSubmitOrChangeProvider()
{
    AssistantPanel panel;
    auto *provider = panel.findChild<QComboBox *>(QStringLiteral("videoProvider"));
    auto *frame = panel.findChild<QLineEdit *>(QStringLiteral("videoFirstFrame"));
    auto *generate = panel.findChild<QPushButton *>(QStringLiteral("videoGenerateButton"));
    auto *clear = panel.findChild<QPushButton *>(QStringLiteral("videoClearFrameButton"));
    auto *status = panel.findChild<QLabel *>(QStringLiteral("videoStatus"));
    QVERIFY(provider && frame && generate && clear && status);
    provider->setCurrentIndex(provider->findData(QStringLiteral("ltx-space")));
    panel.prepareVideo(QStringLiteral("海边日出"), QStringLiteral("16:9"), 5);
    QVERIFY(generate->isEnabled());
    const auto statusBefore = status->text();
    QSignalSpy generated(&panel, &AssistantPanel::videoRequested);
    QSignalSpy resumed(&panel, &AssistantPanel::resumeVideoRequested);
    const auto path = (WorkspacePaths::root() + QStringLiteral("/first-frame.png"));
    panel.prepareVideo(QStringLiteral("海边日出"), QStringLiteral("9:16"), 8, path);
    QCOMPARE(provider->currentData().toString(), QStringLiteral("ltx-space"));
    QCOMPARE(frame->text(), path);
    QCOMPARE(status->text(), statusBefore);
    QVERIFY(!generate->isEnabled());
    generate->click();
    QVERIFY(QMetaObject::invokeMethod(generate, "clicked", Qt::DirectConnection, Q_ARG(bool, false)));
    QCOMPARE(generated.size(), 0);
    QCOMPARE(resumed.size(), 0);
    clear->click();
    QVERIFY(generate->isEnabled());
}

void AssistantPanelTest::switchingBackToSeedanceRestoresKeyRequirement()
{
    AssistantPanel panel;
    auto *provider = panel.findChild<QComboBox *>(QStringLiteral("videoProvider"));
    auto *quality = panel.findChild<QComboBox *>(QStringLiteral("videoQuality"));
    auto *prompt = panel.findChild<QPlainTextEdit *>(QStringLiteral("videoPrompt"));
    auto *frame = panel.findChild<QLineEdit *>(QStringLiteral("videoFirstFrame"));
    auto *ratio = panel.findChild<QComboBox *>(QStringLiteral("videoRatio"));
    auto *duration = panel.findChild<QSpinBox *>(QStringLiteral("videoDuration"));
    auto *key = panel.findChild<QLineEdit *>(QStringLiteral("videoKey"));
    auto *generate = panel.findChild<QPushButton *>(QStringLiteral("videoGenerateButton"));
    auto *resume = panel.findChild<QPushButton *>(QStringLiteral("videoResumeButton"));
    QVERIFY(provider && quality && prompt && frame && ratio && duration && key && generate && resume);
    provider->setCurrentIndex(provider->findData(QStringLiteral("ltx-space")));
    panel.prepareVideo(QStringLiteral("山间日出"), QStringLiteral("9:16"), 8, (WorkspacePaths::root() + QStringLiteral("/first-frame.png")));
    QCOMPARE(provider->currentData().toString(), QStringLiteral("ltx-space"));
    QVERIFY(!generate->isEnabled());
    provider->setCurrentIndex(provider->findData(QStringLiteral("seedance")));
    QVERIFY(key->isEnabled());
    QVERIFY(!generate->isEnabled());
    QVERIFY(!resume->isEnabled());
    QCOMPARE(quality->count(), 2);
    key->setText(QStringLiteral("test-only-key"));
    for (const auto &id : {QStringLiteral("ltx-space"), QStringLiteral("seedance")}) {
        provider->setCurrentIndex(provider->findData(id));
        QCOMPARE(prompt->toPlainText(), QStringLiteral("山间日出"));
        QCOMPARE(frame->text(), (WorkspacePaths::root() + QStringLiteral("/first-frame.png")));
        QCOMPARE(ratio->currentText(), QStringLiteral("9:16"));
        QCOMPARE(duration->value(), 8);
        QCOMPARE(key->text(), QStringLiteral("test-only-key"));
    }
    QVERIFY(key->isEnabled());
    QVERIFY(generate->isEnabled());
    QVERIFY(resume->isEnabled());
}

void AssistantPanelTest::seedanceQualityOffers720pAnd1080p()
{
    AssistantPanel panel;
    auto *quality = panel.findChild<QComboBox *>(QStringLiteral("videoQuality"));
    QVERIFY(quality);
    QCOMPARE(quality->count(), 2);
    QCOMPARE(quality->currentData().toString(), QStringLiteral("720p"));
    QCOMPARE(quality->itemData(1).toString(), QStringLiteral("1080p"));
}

void AssistantPanelTest::videoRequestAppendsProviderAndResolution()
{
    AssistantPanel panel;
    auto *quality = panel.findChild<QComboBox *>(QStringLiteral("videoQuality"));
    auto *key = panel.findChild<QLineEdit *>(QStringLiteral("videoKey"));
    auto *generate = panel.findChild<QPushButton *>(QStringLiteral("videoGenerateButton"));
    QVERIFY(quality && key && generate);
    quality->setCurrentIndex(quality->findData(QStringLiteral("1080p")));
    panel.prepareVideo(QStringLiteral("海边日出"), QStringLiteral("9:16"), 12, (WorkspacePaths::root() + QStringLiteral("/first-frame.png")));
    key->setText(QStringLiteral("test-only-key"));
    QSignalSpy requests(&panel, &AssistantPanel::videoRequested);
    generate->click();
    QCOMPARE(requests.size(), 1);
    const auto fields = requests.first();
    QCOMPARE(fields.size(), 7);
    QCOMPARE(fields[0].toString(), QStringLiteral("海边日出"));
    QCOMPARE(fields[1].toString(), (WorkspacePaths::root() + QStringLiteral("/first-frame.png")));
    QCOMPARE(fields[2].toString(), QStringLiteral("9:16"));
    QCOMPARE(fields[3].toInt(), 12);
    QCOMPARE(fields[4].toString(), QStringLiteral("test-only-key"));
    QCOMPARE(fields[5].toString(), QStringLiteral("seedance"));
    QCOMPARE(fields[6].toString(), QStringLiteral("1080p"));
}

void AssistantPanelTest::ltxProviderDisablesResume()
{
    AssistantPanel panel;
    auto *provider = panel.findChild<QComboBox *>(QStringLiteral("videoProvider"));
    auto *key = panel.findChild<QLineEdit *>(QStringLiteral("videoKey"));
    auto *resume = panel.findChild<QPushButton *>(QStringLiteral("videoResumeButton"));
    QVERIFY(provider && key && resume);
    key->setText(QStringLiteral("test-only-key"));
    QVERIFY(resume->isEnabled());
    provider->setCurrentIndex(provider->findData(QStringLiteral("ltx-space")));
    QVERIFY(!resume->isEnabled());
    QSignalSpy requests(&panel, &AssistantPanel::resumeVideoRequested);
    resume->click();
    QVERIFY(QMetaObject::invokeMethod(resume, "clicked", Qt::DirectConnection, Q_ARG(bool, false)));
    QCOMPARE(requests.size(), 0);
    panel.setVideoState(true, QStringLiteral("正在生成"));
    panel.setVideoState(false, QStringLiteral("等待已结束"));
    QVERIFY(!resume->isEnabled());
    QCOMPARE(requests.size(), 0);
}

void AssistantPanelTest::providerSelectionIsLockedWhileBusy()
{
    AssistantPanel panel;
    auto *provider = panel.findChild<QComboBox *>(QStringLiteral("videoProvider"));
    auto *quality = panel.findChild<QComboBox *>(QStringLiteral("videoQuality"));
    QVERIFY(provider && quality);
    for (const auto &id : {QStringLiteral("seedance"), QStringLiteral("ltx-space")}) {
        provider->setCurrentIndex(provider->findData(id));
        QVERIFY(provider->isEnabled());
        QVERIFY(quality->isEnabled());
        panel.setVideoState(true, QStringLiteral("正在生成"));
        QVERIFY(!provider->isEnabled());
        QVERIFY(!quality->isEnabled());
        panel.setVideoState(false, QStringLiteral("等待已结束"));
        QVERIFY(provider->isEnabled());
        QVERIFY(quality->isEnabled());
    }
}

void AssistantPanelTest::narrowSidebarFitsBothPages()
{
    AssistantPanel panel;
    auto *tabs = panel.findChild<QTabWidget *>(QStringLiteral("assistantTabs"));
    QVERIFY(tabs);
    panel.resize(360, 780);
    panel.show();
    QTest::qWait(1);
    QVERIFY2(panel.minimumSizeHint().width() <= 360, qPrintable(QString::number(panel.minimumSizeHint().width())));
    QCOMPARE(panel.width(), 360);
    for (int page = 0; page < tabs->count(); ++page) {
        tabs->setCurrentIndex(page);
        QCoreApplication::processEvents();
        for (auto *widget : tabs->currentWidget()->findChildren<QWidget *>()) {
            if (!widget->isVisibleTo(&panel) || widget->width() == 0) {
                continue;
            }
            const int left = widget->mapTo(&panel, QPoint()).x();
            QVERIFY2(left >= 0 && left + widget->width() <= panel.width(), qPrintable(widget->objectName()));
        }
    }
}

void AssistantPanelTest::resumeNeedsKeyAndGuardsRepeatedRequests()
{
    AssistantPanel panel;
    auto *tabs = panel.findChild<QTabWidget *>(QStringLiteral("assistantTabs"));
    auto *key = panel.findChild<QLineEdit *>(QStringLiteral("videoKey"));
    auto *resume = panel.findChild<QPushButton *>(QStringLiteral("videoResumeButton"));
    auto *stop = panel.findChild<QPushButton *>(QStringLiteral("videoStopButton"));
    QVERIFY(tabs && key && resume && stop);
    tabs->setCurrentIndex(1);
    panel.resize(360, 780);
    panel.show();
    QVERIFY(QTest::qWaitForWindowActive(&panel));
    QSignalSpy requests(&panel, &AssistantPanel::resumeVideoRequested);
    QSignalSpy generated(&panel, &AssistantPanel::videoRequested);
    QVERIFY(!resume->isEnabled());
    QTest::mouseClick(resume, Qt::LeftButton);
    QCOMPARE(requests.size(), 0);
    key->setText(QStringLiteral("  "));
    QVERIFY(!resume->isEnabled());
    key->setText(QStringLiteral("test-only-key"));
    QVERIFY(resume->isEnabled());
    QTest::mouseClick(resume, Qt::LeftButton);
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.first().size(), 2);
    QCOMPARE(requests.first().first().toString(), key->text());
    QCOMPARE(requests.first()[1].toString(), QStringLiteral("seedance"));
    QVERIFY(!resume->isEnabled());
    QVERIFY(stop->isEnabled());
    QTest::mouseClick(resume, Qt::LeftButton);
    QCOMPARE(requests.size(), 1);
    QCOMPARE(generated.size(), 0);
    panel.setVideoState(false, QStringLiteral("检查结束"));
    QVERIFY(resume->isEnabled());
    panel.setVideoState(true, QStringLiteral("正在生成"));
    QVERIFY(!resume->isEnabled());
}

void AssistantPanelTest::prepareVideoOnlyPopulatesForm()
{
    AssistantPanel panel;
    auto *tabs = panel.findChild<QTabWidget *>(QStringLiteral("assistantTabs"));
    auto *prompt = panel.findChild<QPlainTextEdit *>(QStringLiteral("videoPrompt"));
    auto *key = panel.findChild<QLineEdit *>(QStringLiteral("videoKey"));
    auto *frame = panel.findChild<QLineEdit *>(QStringLiteral("videoFirstFrame"));
    auto *ratio = panel.findChild<QComboBox *>(QStringLiteral("videoRatio"));
    auto *duration = panel.findChild<QSpinBox *>(QStringLiteral("videoDuration"));
    auto *generate = panel.findChild<QPushButton *>(QStringLiteral("videoGenerateButton"));
    auto *status = panel.findChild<QLabel *>(QStringLiteral("videoStatus"));
    QVERIFY(tabs && prompt && key && frame && ratio && duration && generate && status);
    key->setText(QStringLiteral("test-only-key"));
    const auto statusBefore = status->text();
    QSignalSpy generated(&panel, &AssistantPanel::videoRequested);
    QSignalSpy resumed(&panel, &AssistantPanel::resumeVideoRequested);
    panel.prepareVideo(QStringLiteral("山间日出"), QStringLiteral("9:16"), 12, (WorkspacePaths::root() + QStringLiteral("/first-frame.png")));
    QCOMPARE(tabs->currentIndex(), 1);
    QCOMPARE(prompt->toPlainText(), QStringLiteral("山间日出"));
    QCOMPARE(ratio->currentText(), QStringLiteral("9:16"));
    QCOMPARE(duration->value(), 12);
    QCOMPARE(frame->text(), (WorkspacePaths::root() + QStringLiteral("/first-frame.png")));
    QCOMPARE(key->text(), QStringLiteral("test-only-key"));
    QCOMPARE(status->text(), statusBefore);
    QVERIFY(generate->isEnabled());
    panel.prepareVideo(QStringLiteral("海上月亮"), QStringLiteral("1:1"), 5);
    QVERIFY(frame->text().isEmpty());
    QCOMPARE(generated.size(), 0);
    QCOMPARE(resumed.size(), 0);
}

void AssistantPanelTest::quickTasksOnlyFillDraftAndRespectBusyState()
{
    AssistantPanel panel;
    auto *tasks = panel.findChild<QComboBox *>(QStringLiteral("quickTask"));
    auto *fill = panel.findChild<QPushButton *>(QStringLiteral("fillTaskButton"));
    auto *input = panel.findChild<QPlainTextEdit *>(QStringLiteral("messageInput"));
    QVERIFY(tasks && fill && input);
    QCOMPARE(tasks->count(), 14);
    QSignalSpy requests(&panel, &AssistantPanel::messageRequested);
    for (int i = 0; i < tasks->count(); ++i) {
        tasks->setCurrentIndex(i);
        input->clear();
        fill->click();
        QVERIFY(!input->toPlainText().isEmpty());
        QCOMPARE(requests.size(), 0);
    }
    tasks->setCurrentIndex(2);
    fill->click();
    QVERIFY(input->toPlainText().contains(QStringLiteral("不删除")));
    const auto draft = input->toPlainText();
    panel.setAssistantState(true, true, QStringLiteral("正在处理"));
    QVERIFY(!fill->isEnabled());
    QVERIFY(!tasks->isEnabled());
    fill->click();
    QCOMPARE(input->toPlainText(), draft);
    panel.setAssistantState(false, false, {});
    QVERIFY(fill->isEnabled());
}

void AssistantPanelTest::localHistoryWorksWithoutModelConnection()
{
    AssistantPanel panel;
    auto *undo = panel.findChild<QPushButton *>(QStringLiteral("undoButton"));
    auto *redo = panel.findChild<QPushButton *>(QStringLiteral("redoButton"));
    QVERIFY(undo && redo);
    QSignalSpy undone(&panel, &AssistantPanel::undoRequested);
    QSignalSpy redone(&panel, SIGNAL(redoRequested()));
    QVERIFY(redone.isValid());
    QVERIFY(undo->isEnabled());
    QVERIFY(redo->isEnabled());
    undo->click();
    redo->click();
    QCOMPARE(undone.size(), 1);
    QCOMPARE(redone.size(), 1);
    panel.setAssistantState(false, true, QStringLiteral("正在连接"));
    QVERIFY(!undo->isEnabled());
    QVERIFY(!redo->isEnabled());
}

void AssistantPanelTest::outputFolderActionsWorkWithoutModelConnection()
{
    AssistantPanel panel;
    auto *projects = panel.findChild<QPushButton *>(QStringLiteral("openProjectsButton"));
    auto *exports = panel.findChild<QPushButton *>(QStringLiteral("openExportsButton"));
    QVERIFY(projects && exports);
    QSignalSpy projectOpened(&panel, SIGNAL(openProjectsRequested()));
    QSignalSpy exportOpened(&panel, SIGNAL(openExportsRequested()));
    QVERIFY(projectOpened.isValid() && exportOpened.isValid());
    projects->click();
    exports->click();
    QCOMPARE(projectOpened.size(), 1);
    QCOMPARE(exportOpened.size(), 1);
}

void AssistantPanelTest::chatAreasDistinguishHistoryFromEditableRequest()
{
    AssistantPanel panel;
    auto *transcript = panel.findChild<QPlainTextEdit *>(QStringLiteral("transcript"));
    auto *input = panel.findChild<QPlainTextEdit *>(QStringLiteral("messageInput"));
    QVERIFY(transcript && input);
    QVERIFY(transcript->isReadOnly());
    QVERIFY(transcript->accessibleName().contains(QStringLiteral("只读")));
    QVERIFY(transcript->placeholderText().contains(QStringLiteral("下方")));
    QVERIFY(!input->isReadOnly());
    QCOMPARE(input->accessibleName(), QStringLiteral("剪辑要求"));
    auto *label = panel.findChild<QLabel *>(QStringLiteral("messageInputLabel"));
    QVERIFY(label);
    QCOMPARE(label->buddy(), input);
}

void AssistantPanelTest::chineseInputCommitsAndSends()
{
    AssistantPanel panel;
    panel.resize(360, 780);
    panel.show();
    QVERIFY(QTest::qWaitForWindowActive(&panel));
    panel.setAssistantState(true, false, {});
    auto *input = panel.findChild<QPlainTextEdit *>(QStringLiteral("messageInput"));
    QVERIFY(input);
    input->setFocus();
    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("保留原声，加中文字幕"));
    QApplication::sendEvent(input, &commit);
    QCOMPARE(input->toPlainText(), QStringLiteral("保留原声，加中文字幕"));
    QSignalSpy requests(&panel, &AssistantPanel::messageRequested);
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.first().first().toString(), QStringLiteral("保留原声，加中文字幕"));
}

void AssistantPanelTest::ltxDurationMatchesSupportedRange()
{
    AssistantPanel panel;
    auto *provider = panel.findChild<QComboBox *>(QStringLiteral("videoProvider"));
    auto *duration = panel.findChild<QSpinBox *>(QStringLiteral("videoDuration"));
    auto *status = panel.findChild<QLabel *>(QStringLiteral("videoStatus"));
    QVERIFY(provider && duration && status);
    QCOMPARE(duration->maximum(), 30);
    provider->setCurrentIndex(provider->findData(QStringLiteral("ltx-space")));
    QCOMPARE(duration->minimum(), 4);
    QCOMPARE(duration->maximum(), 8);
    panel.prepareVideo(QStringLiteral("海上日出"), QStringLiteral("16:9"), 20);
    QCOMPARE(duration->value(), 8);
    QVERIFY(status->text().contains(QStringLiteral("4–8")));
    provider->setCurrentIndex(provider->findData(QStringLiteral("seedance")));
    QCOMPARE(duration->maximum(), 30);
}

void AssistantPanelTest::toolbarWaitsForAssistantToFinish()
{
    AssistantPanel panel;
    for (const auto *name : {"chooseMediaButton", "exportButton", "connectButton"}) {
        auto *button = panel.findChild<QPushButton *>(QLatin1String(name));
        QVERIFY(button);
        QVERIFY(button->isEnabled());
        panel.setAssistantState(false, true, QStringLiteral("正在连接"));
        QVERIFY(!button->isEnabled());
        panel.setAssistantState(false, false, QStringLiteral("已停止"));
        QVERIFY(button->isEnabled());
    }
}

void AssistantPanelTest::creativeQuickTasksOnlyPopulateDraft()
{
    AssistantPanel panel;
    auto *tasks = panel.findChild<QComboBox *>(QStringLiteral("quickTask"));
    auto *fill = panel.findChild<QPushButton *>(QStringLiteral("fillTaskButton"));
    auto *input = panel.findChild<QPlainTextEdit *>(QStringLiteral("messageInput"));
    QVERIFY(tasks && fill && input);
    QSignalSpy requests(&panel, &AssistantPanel::messageRequested);
    for (const auto &name : {QStringLiteral("照片运镜"), QStringLiteral("口播音量"), QStringLiteral("字幕排版"),
            QStringLiteral("口播粗剪"), QStringLiteral("本地降噪"), QStringLiteral("人声分离"), QStringLiteral("人像去背景"),
            QStringLiteral("画面增强"), QStringLiteral("面部柔化"), QStringLiteral("字幕包装")}) {
        const int index = tasks->findText(name);
        QVERIFY(index >= 0);
        tasks->setCurrentIndex(index);
        fill->click();
        QVERIFY(!input->toPlainText().isEmpty());
        QVERIFY(input->toPlainText().contains(QStringLiteral("不自动导出")));
        QCOMPARE(requests.size(), 0);
    }
}

void AssistantPanelTest::compactFreeVideoPageKeepsStatusReachable()
{
    AssistantPanel panel;
    panel.resize(360, 480);
    auto *tabs = panel.findChild<QTabWidget *>(QStringLiteral("assistantTabs"));
    auto *provider = panel.findChild<QComboBox *>(QStringLiteral("videoProvider"));
    auto *key = panel.findChild<QLineEdit *>(QStringLiteral("videoKey"));
    auto *scroll = panel.findChild<QScrollArea *>(QStringLiteral("videoScroll"));
    auto *status = panel.findChild<QLabel *>(QStringLiteral("videoStatus"));
    QVERIFY(tabs && provider && key && status);
    QVERIFY(scroll);
    tabs->setCurrentIndex(1);
    provider->setCurrentIndex(provider->findData(QStringLiteral("ltx-space")));
    QVERIFY(key->isHidden());
    panel.setVideoState(false, QStringLiteral("免费共享额度暂不可用。请等待平台恢复后再试，不会切换到付费服务。").repeated(5));
    panel.show();
    QVERIFY(QTest::qWaitForWindowActive(&panel));
    QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
    scroll->ensureWidgetVisible(status, 0, 0);
    QApplication::processEvents();
    QVERIFY(scroll->viewport()->rect().contains(status->mapTo(scroll->viewport(), QPoint(0, status->height() - 1))));
    provider->setCurrentIndex(provider->findData(QStringLiteral("seedance")));
    QVERIFY(!key->isHidden());
}

void AssistantPanelTest::connectionButtonNamesAllAiProviders()
{
    AssistantPanel panel;
    auto *button = panel.findChild<QPushButton *>(QStringLiteral("connectButton"));
    QVERIFY(button);
    QCOMPARE(button->text(), QStringLiteral("连接 AI"));
}

void AssistantPanelTest::externalControlBlocksInternalRequestsAndKeepsHistory()
{
    AssistantPanel panel;
    auto *input = panel.findChild<QPlainTextEdit *>(QStringLiteral("messageInput"));
    auto *send = panel.findChild<QPushButton *>(QStringLiteral("sendButton"));
    auto *fill = panel.findChild<QPushButton *>(QStringLiteral("fillTaskButton"));
    auto *quick = panel.findChild<QComboBox *>(QStringLiteral("quickTask"));
    auto *stop = panel.findChild<QPushButton *>(QStringLiteral("stopButton"));
    auto *undo = panel.findChild<QPushButton *>(QStringLiteral("undoButton"));
    QVERIFY(input && send && fill && quick && stop && undo);
    QSignalSpy messages(&panel, &AssistantPanel::messageRequested);
    QSignalSpy stopped(&panel, &AssistantPanel::stopRequested);
    QSignalSpy undone(&panel, &AssistantPanel::undoRequested);
    input->setPlainText(QStringLiteral("保留这份草稿"));
    panel.setAssistantState(true, false, {});
    panel.setExternalControl(true);
    QVERIFY(!input->isEnabled());
    QVERIFY(!send->isEnabled());
    QVERIFY(!fill->isEnabled());
    QVERIFY(!quick->isEnabled());
    send->click();
    fill->click();
    QTest::keyClick(input, Qt::Key_Return, Qt::ControlModifier);
    QCOMPARE(messages.size(), 0);
    QCOMPARE(input->toPlainText(), QStringLiteral("保留这份草稿"));
    QVERIFY(undo->isEnabled());
    undo->click();
    QCOMPARE(undone.size(), 1);
    panel.setAssistantState(true, true, {});
    QVERIFY(stop->isEnabled());
    stop->click();
    QCOMPARE(stopped.size(), 1);
    panel.setAssistantState(true, false, {});
    panel.setExternalControl(false);
    QVERIFY(input->isEnabled());
    QVERIFY(send->isEnabled());
    QVERIFY(fill->isEnabled());
    send->click();
    QCOMPARE(messages.size(), 1);
}

QTEST_MAIN(AssistantPanelTest)
#include "assistantpaneltest.moc"
