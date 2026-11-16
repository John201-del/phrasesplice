/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QWidget>

class QLabel;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;

class AssistantPanel : public QWidget
{
    Q_OBJECT
public:
    explicit AssistantPanel(QWidget *parent = nullptr);
    void setAssistantState(bool ready, bool busy, const QString &message);
    void setExternalControl(bool external);
    void appendUserMessage(const QString &text);
    void beginAssistantMessage();
    void appendAssistantText(const QString &text);
    void appendStatus(const QString &text);
    void setVideoState(bool busy, const QString &message);
    void prepareVideo(const QString &prompt, const QString &ratio, int duration, const QString &firstFramePath = {});

Q_SIGNALS:
    void messageRequested(QString text);
    void stopRequested();
    void undoRequested();
    void redoRequested();
    void openProjectsRequested();
    void openExportsRequested();
    void chooseMediaRequested();
    void exportRequested();
    void connectRequested();
    void videoRequested(QString prompt, QString firstFramePath, QString ratio, int duration, QString apiKey, QString providerId, QString resolution);
    void cancelVideoRequested();
    void resumeVideoRequested(QString apiKey, QString providerId);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateActions();
    void sendMessage();

    bool m_ready{false};
    bool m_busy{false};
    bool m_externalControl{false};
    bool m_videoBusy{false};
    bool m_assistantTextAtEnd{false};
    QPlainTextEdit *m_transcript;
    QPlainTextEdit *m_messageInput;
    QLabel *m_assistantStatus;
    QPushButton *m_chooseMediaButton;
    QPushButton *m_exportButton;
    QPushButton *m_connectButton;
    QPushButton *m_sendButton;
    QPushButton *m_stopButton;
    QPushButton *m_undoButton;
    QPushButton *m_redoButton;
    QComboBox *m_quickTask;
    QPushButton *m_fillTaskButton;
    QComboBox *m_videoProvider;
    QComboBox *m_videoQuality;
    QPlainTextEdit *m_videoPrompt;
    QTabWidget *m_tabs;
    QLineEdit *m_videoFirstFrame;
    QComboBox *m_videoRatio;
    QSpinBox *m_videoDuration;
    QLineEdit *m_videoKey;
    QPushButton *m_videoGenerateButton;
    QPushButton *m_videoStopButton;
    QPushButton *m_videoResumeButton;
    QLabel *m_videoStatus;
};
