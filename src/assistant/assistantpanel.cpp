/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "assistantpanel.h"
#include "workspacepaths.h"

#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextCursor>
#include <QVBoxLayout>

AssistantPanel::AssistantPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    auto *tabs = new QTabWidget(this);
    m_tabs = tabs;
    tabs->setObjectName(QStringLiteral("assistantTabs"));
    layout->addWidget(tabs);

    auto *chatPage = new QWidget(tabs);
    auto *chatLayout = new QVBoxLayout(chatPage);
    auto *toolbar = new QHBoxLayout;
    auto *chooseMedia = new QPushButton(tr("选择素材"), chatPage);
    m_chooseMediaButton = chooseMedia;
    chooseMedia->setObjectName(QStringLiteral("chooseMediaButton"));
    auto *exportVideo = new QPushButton(tr("导出"), chatPage);
    m_exportButton = exportVideo;
    exportVideo->setObjectName(QStringLiteral("exportButton"));
    auto *connectCodex = new QPushButton(tr("连接 AI"), chatPage);
    m_connectButton = connectCodex;
    connectCodex->setObjectName(QStringLiteral("connectButton"));
    toolbar->addWidget(chooseMedia);
    toolbar->addWidget(exportVideo);
    toolbar->addWidget(connectCodex);
    chatLayout->addLayout(toolbar);
    connect(chooseMedia, &QPushButton::clicked, this, &AssistantPanel::chooseMediaRequested);
    connect(exportVideo, &QPushButton::clicked, this, &AssistantPanel::exportRequested);
    connect(connectCodex, &QPushButton::clicked, this, &AssistantPanel::connectRequested);

    auto *quickActions = new QHBoxLayout;
    quickActions->addWidget(new QLabel(tr("快捷任务"), chatPage));
    m_quickTask = new QComboBox(chatPage);
    m_quickTask->setObjectName(QStringLiteral("quickTask"));
    m_quickTask->setMinimumWidth(0);
    m_quickTask->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_quickTask->setMinimumContentsLength(5);
    m_quickTask->addItem(tr("顺序成片"), tr("先读取真实工程。时间线为空时，把素材库中的视频和图片依次放到时间线，从0秒开始，保留视频原声；独立音频先不添加。已有时间线内容时先说明现状，不重复插入。完成后让我预览，不自动导出。"));
    m_quickTask->addItem(tr("中文字幕"), tr("读取真实工程，为时间线中有中文语音的片段识别并添加可编辑中文字幕。根据素材入点、时间线起点和速度对齐，同一源文件只识别一次。已有字幕先检查，不重复添加；无法准确映射时先说明。"));
    m_quickTask->addItem(tr("检查静音"), tr("读取真实工程，分析时间线片段的静音候选并列出对应时间位置，本轮不删除任何内容。区分真正静音和没有音轨的画面，保留后者；等待我确认后再剪。"));
    m_quickTask->addItem(tr("成片检查"), tr("读取真实工程，列出画面尺寸、帧率、总时长、片段和字幕数量，检查素材状态及轨道锁定情况。只报告实际读取到的信息和需要确认的问题，不修改、不保存、不导出。"));
    m_quickTask->addItem(tr("照片运镜"), tr("先读取真实工程。时间线为空时将素材库第一张图片从0秒插入合适的视频轨，修剪成5秒；已有时间线时只处理其中的图片。给图片添加温和的中心缓慢推近，保留比例，不改变其它内容；已有运镜就更新，不重复叠加。完成后让我预览，不自动导出。"));
    m_quickTask->addItem(tr("口播音量"), tr("读取真实工程，只对有音轨的时间线片段做温和的动态音量平衡，保留原声内容和时长；已有同类效果先读取实际参数再更新，纯画面不处理，不声称精确广播级响度。完成后让我试听，不自动导出。"));
    m_quickTask->addItem(tr("字幕排版"), tr("读取真实工程，把已有字幕统一成易读的白字黑边并放在底部，按画面尺寸适当放大字号，保持原文字和时间。没有字幕时先说明，不自动识别或增加字幕。完成后让我预览，不自动导出。"));
    m_quickTask->addItem(tr("口播粗剪"), tr("读取真实工程，对当前口播获取转写文字和静音候选，列出建议保留或删除的原话及时间，等待我确认。确认后用retain_ranges紧凑剪接当前片段并同步原声，其他素材不波纹移动；已有字幕先说明冲突。不要猜测口误，不自动导出。"));
    m_quickTask->addItem(tr("本地降噪"), tr("读取真实工程，为口播素材生成本地降噪副本并试听，原件保留。先调用process_media的denoise_audio，待返回已加载的binId后再使用；时间线替换须保持源入点与同步，不能重复叠原声。不自动导出。"));
    m_quickTask->addItem(tr("人声分离"), tr("读取真实工程，用process_media的separate_vocals在本机分离所选素材的人声与伴奏，保留两份副本。先让我试听，不自动替换原声，不保证完全无伴奏泄露，不自动导出。"));
    m_quickTask->addItem(tr("人像去背景"), tr("读取真实工程，用process_media的remove_background生成带透明通道的人像视频副本，保留原声和原件。结果加载后再编排，背景要用我已有素材铺在下层；不要用绿幕抠除冒充AI人像分割。让我预览，不自动导出。"));
    m_quickTask->addItem(tr("画面增强"), tr("读取真实工程，用process_media的enhance_video生成温和降噪和锐化副本，保留尺寸、帧率、原声和原件。这是传统画面增强，不声称生成新的真实细节或AI超分辨率。让我预览，不自动导出。"));
    m_quickTask->addItem(tr("面部柔化"), tr("读取真实工程，用process_media的smooth_skin做轻度面部柔化，生成副本并保留原件。先检查可处理的人脸，不声称瘦脸、美体或剪映同等级美颜。让我对比预览，不自动导出。"));
    m_quickTask->addItem(tr("字幕包装"), tr("读取真实工程，为已有字幕选择易读的自有包装预设subtitle_preset，clean为白字黑边、headline为标题、highlight为醒目强调。保持字幕内容和时间，没有字幕先说明。让我预览，不自动导出。"));
    quickActions->addWidget(m_quickTask, 1);
    m_fillTaskButton = new QPushButton(tr("填入"), chatPage);
    m_fillTaskButton->setObjectName(QStringLiteral("fillTaskButton"));
    m_fillTaskButton->setToolTip(tr("填入可修改的要求，点击发送后才执行"));
    quickActions->addWidget(m_fillTaskButton);
    chatLayout->addLayout(quickActions);

    m_assistantStatus = new QLabel(tr("请连接 AI 后开始"), chatPage);
    m_assistantStatus->setObjectName(QStringLiteral("assistantStatus"));
    m_assistantStatus->setTextFormat(Qt::PlainText);
    m_assistantStatus->setWordWrap(true);
    chatLayout->addWidget(m_assistantStatus);
    chatLayout->addWidget(new QLabel(tr("对话记录（只读）"), chatPage));
    m_transcript = new QPlainTextEdit(chatPage);
    m_transcript->setObjectName(QStringLiteral("transcript"));
    m_transcript->setReadOnly(true);
    m_transcript->setAccessibleName(tr("对话记录（只读）"));
    m_transcript->setPlaceholderText(tr("助手回复显示在这里。请在下方“剪辑要求”中输入。"));
    m_transcript->setFocusPolicy(Qt::ClickFocus);
    m_transcript->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_transcript->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    chatLayout->addWidget(m_transcript, 1);
    auto *hint = new QLabel(tr("剪辑要求（在这里输入，Ctrl+Enter 发送）"), chatPage);
    hint->setObjectName(QStringLiteral("messageInputLabel"));
    hint->setWordWrap(true);
    chatLayout->addWidget(hint);
    m_messageInput = new QPlainTextEdit(chatPage);
    m_messageInput->setObjectName(QStringLiteral("messageInput"));
    m_messageInput->installEventFilter(this);
    m_messageInput->setAccessibleName(tr("剪辑要求"));
    hint->setBuddy(m_messageInput);
    m_messageInput->setPlaceholderText(tr("选择快捷任务，或描述具体修改，例如：把第一个片段在2秒处分开"));
    m_messageInput->setMinimumHeight(90);
    m_messageInput->setMaximumHeight(140);
    m_messageInput->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    chatLayout->addWidget(m_messageInput);
    auto *actions = new QHBoxLayout;
    m_sendButton = new QPushButton(tr("发送"), chatPage);
    m_sendButton->setObjectName(QStringLiteral("sendButton"));
    m_sendButton->setToolTip(tr("Ctrl+Enter 发送，Enter 换行"));
    m_stopButton = new QPushButton(tr("停止"), chatPage);
    m_stopButton->setObjectName(QStringLiteral("stopButton"));
    m_undoButton = new QPushButton(tr("撤销"), chatPage);
    m_undoButton->setObjectName(QStringLiteral("undoButton"));
    m_redoButton = new QPushButton(tr("重做"), chatPage);
    m_redoButton->setObjectName(QStringLiteral("redoButton"));
    actions->addWidget(m_sendButton);
    actions->addWidget(m_stopButton);
    chatLayout->addLayout(actions);
    auto *history = new QHBoxLayout;
    history->addWidget(m_undoButton);
    history->addWidget(m_redoButton);
    chatLayout->addLayout(history);
    connect(m_messageInput, &QPlainTextEdit::textChanged, this, &AssistantPanel::updateActions);
    connect(m_sendButton, &QPushButton::clicked, this, &AssistantPanel::sendMessage);
    connect(m_stopButton, &QPushButton::clicked, this, &AssistantPanel::stopRequested);
    connect(m_undoButton, &QPushButton::clicked, this, &AssistantPanel::undoRequested);
    connect(m_redoButton, &QPushButton::clicked, this, &AssistantPanel::redoRequested);
    connect(m_fillTaskButton, &QPushButton::clicked, this, [this]() {
        if (!m_busy && !m_externalControl) {
            m_messageInput->setPlainText(m_quickTask->currentData().toString());
            m_messageInput->setFocus();
        }
    });
    auto *folders = new QHBoxLayout;
    auto *projects = new QPushButton(tr("工程目录"), chatPage);
    projects->setObjectName(QStringLiteral("openProjectsButton"));
    auto *exports = new QPushButton(tr("成片目录"), chatPage);
    exports->setObjectName(QStringLiteral("openExportsButton"));
    folders->addWidget(projects);
    folders->addWidget(exports);
    chatLayout->addLayout(folders);
    connect(projects, &QPushButton::clicked, this, &AssistantPanel::openProjectsRequested);
    connect(exports, &QPushButton::clicked, this, &AssistantPanel::openExportsRequested);
    tabs->addTab(chatPage, tr("语言剪辑"));

    auto *videoPage = new QWidget(tabs);
    auto *videoLayout = new QVBoxLayout(videoPage);
    auto *providerOptions = new QHBoxLayout;
    m_videoProvider = new QComboBox(videoPage);
    m_videoProvider->setObjectName(QStringLiteral("videoProvider"));
    m_videoProvider->setMinimumWidth(0);
    m_videoProvider->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_videoProvider->setMinimumContentsLength(5);
    m_videoProvider->addItem(tr("Seedance（付费）"), QStringLiteral("seedance"));
    m_videoProvider->addItem(tr("LTX（免费限额）"), QStringLiteral("ltx-space"));
    providerOptions->addWidget(new QLabel(tr("服务"), videoPage));
    providerOptions->addWidget(m_videoProvider, 1);
    m_videoQuality = new QComboBox(videoPage);
    m_videoQuality->setObjectName(QStringLiteral("videoQuality"));
    providerOptions->addWidget(new QLabel(tr("质量"), videoPage));
    providerOptions->addWidget(m_videoQuality, 1);
    videoLayout->addLayout(providerOptions);
    auto *providerNotice = new QLabel(videoPage);
    providerNotice->setObjectName(QStringLiteral("videoProviderNotice"));
    providerNotice->setWordWrap(true);
    videoLayout->addWidget(providerNotice);
    videoLayout->addWidget(new QLabel(tr("镜头描述"), videoPage));
    m_videoPrompt = new QPlainTextEdit(videoPage);
    m_videoPrompt->setObjectName(QStringLiteral("videoPrompt"));
    m_videoPrompt->setPlaceholderText(tr("描述画面主体、动作、环境和风格"));
    m_videoPrompt->setMinimumHeight(90);
    m_videoPrompt->setMaximumHeight(160);
    m_videoPrompt->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    videoLayout->addWidget(m_videoPrompt);
    videoLayout->addWidget(new QLabel(tr("首帧图片（可选）"), videoPage));
    auto *firstFrame = new QLineEdit(videoPage);
    m_videoFirstFrame = firstFrame;
    firstFrame->setObjectName(QStringLiteral("videoFirstFrame"));
    firstFrame->setReadOnly(true);
    firstFrame->setPlaceholderText(tr("未选择图片"));
    firstFrame->setMinimumWidth(0);
    videoLayout->addWidget(firstFrame);
    auto *imageActions = new QHBoxLayout;
    auto *chooseFrame = new QPushButton(tr("选择图片"), videoPage);
    chooseFrame->setObjectName(QStringLiteral("videoChooseFrameButton"));
    auto *clearFrame = new QPushButton(tr("清除"), videoPage);
    clearFrame->setObjectName(QStringLiteral("videoClearFrameButton"));
    imageActions->addWidget(chooseFrame);
    imageActions->addWidget(clearFrame);
    imageActions->addStretch();
    videoLayout->addLayout(imageActions);
    connect(chooseFrame, &QPushButton::clicked, this, [this, firstFrame] {
        const auto path = QFileDialog::getOpenFileName(this, tr("选择首帧图片"), WorkspacePaths::root(),
                                                       tr("图片 (*.png *.jpg *.jpeg *.webp *.bmp);;所有文件 (*)"));
        if (!path.isEmpty()) {
            firstFrame->setText(path);
        }
    });
    connect(clearFrame, &QPushButton::clicked, firstFrame, &QLineEdit::clear);

    auto *videoOptions = new QHBoxLayout;
    videoOptions->addWidget(new QLabel(tr("画幅"), videoPage));
    auto *ratio = new QComboBox(videoPage);
    m_videoRatio = ratio;
    ratio->setObjectName(QStringLiteral("videoRatio"));
    ratio->addItems({QStringLiteral("16:9"), QStringLiteral("9:16"), QStringLiteral("1:1")});
    videoOptions->addWidget(ratio);
    videoOptions->addWidget(new QLabel(tr("时长"), videoPage));
    auto *duration = new QSpinBox(videoPage);
    m_videoDuration = duration;
    duration->setObjectName(QStringLiteral("videoDuration"));
    duration->setRange(4, 30);
    duration->setValue(5);
    duration->setSuffix(tr(" 秒"));
    videoOptions->addWidget(duration);
    videoLayout->addLayout(videoOptions);
    auto *videoKeyLabel = new QLabel(tr("火山方舟 API 密钥"), videoPage);
    videoLayout->addWidget(videoKeyLabel);
    m_videoKey = new QLineEdit(videoPage);
    m_videoKey->setObjectName(QStringLiteral("videoKey"));
    m_videoKey->setEchoMode(QLineEdit::Password);
    m_videoKey->setPlaceholderText(tr("仅在本次打开的面板中使用"));
    videoLayout->addWidget(m_videoKey);
    auto *videoActions = new QHBoxLayout;
    m_videoGenerateButton = new QPushButton(videoPage);
    m_videoGenerateButton->setObjectName(QStringLiteral("videoGenerateButton"));
    m_videoStopButton = new QPushButton(tr("停止等待"), videoPage);
    m_videoStopButton->setObjectName(QStringLiteral("videoStopButton"));
    videoActions->addWidget(m_videoGenerateButton);
    videoActions->addWidget(m_videoStopButton);
    videoLayout->addLayout(videoActions);
    m_videoResumeButton = new QPushButton(tr("检查上次任务"), videoPage);
    m_videoResumeButton->setObjectName(QStringLiteral("videoResumeButton"));
    m_videoResumeButton->setToolTip(tr("仅检查已有任务，不创建新任务"));
    videoLayout->addWidget(m_videoResumeButton);
    m_videoStatus = new QLabel(tr("未连接：填写描述和所选服务的必填项后可开始生成"), videoPage);
    m_videoStatus->setObjectName(QStringLiteral("videoStatus"));
    m_videoStatus->setTextFormat(Qt::PlainText);
    m_videoStatus->setWordWrap(true);
    videoLayout->addWidget(m_videoStatus);
    videoLayout->addStretch();
    connect(m_videoPrompt, &QPlainTextEdit::textChanged, this, &AssistantPanel::updateActions);
    connect(m_videoKey, &QLineEdit::textChanged, this, &AssistantPanel::updateActions);
    connect(firstFrame, &QLineEdit::textChanged, this, &AssistantPanel::updateActions);
    const auto updateProvider = [this, providerNotice, chooseFrame, videoKeyLabel] {
        const bool seedance = m_videoProvider->currentData().toString() == QStringLiteral("seedance");
        chooseFrame->setEnabled(seedance);
        videoKeyLabel->setVisible(seedance);
        m_videoKey->setVisible(seedance);
        m_videoDuration->setRange(4, seedance ? 30 : 8);
        m_videoQuality->clear();
        if (seedance) {
            m_videoQuality->addItem(QStringLiteral("720p"), QStringLiteral("720p"));
            m_videoQuality->addItem(QStringLiteral("1080p"), QStringLiteral("1080p"));
            providerNotice->setText(tr("Seedance 高质量生成：需要火山方舟 API 密钥，按服务付费。"));
            m_videoGenerateButton->setText(tr("生成（Seedance 付费）"));
        } else {
            m_videoQuality->addItem(tr("Space 默认"), QStringLiteral("space-default"));
            providerNotice->setText(tr("LTX 官方公共 Space：匿名免费共享配额，无额度保证；4–8秒，实际时长略有取整。可能排队或不可用，失败不转付费。仅文生视频，需清除首帧。"));
            m_videoGenerateButton->setText(tr("生成（LTX 免费共享额度）"));
        }
        updateActions();
    };
    connect(m_videoProvider, &QComboBox::currentIndexChanged, this, updateProvider);
    connect(m_videoGenerateButton, &QPushButton::clicked, this, [this, firstFrame, ratio, duration] {
        if (!m_videoGenerateButton->isEnabled()) {
            return;
        }
        m_videoBusy = true;
        updateActions();
        const auto providerId = m_videoProvider->currentData().toString();
        const auto apiKey = providerId == QStringLiteral("seedance") ? m_videoKey->text() : QString();
        Q_EMIT videoRequested(m_videoPrompt->toPlainText().trimmed(), firstFrame->text(), ratio->currentText(), duration->value(), apiKey, providerId,
                              m_videoQuality->currentData().toString());
    });
    connect(m_videoStopButton, &QPushButton::clicked, this, &AssistantPanel::cancelVideoRequested);
    connect(m_videoResumeButton, &QPushButton::clicked, this, [this] {
        if (!m_videoResumeButton->isEnabled()) {
            return;
        }
        m_videoBusy = true;
        updateActions();
        Q_EMIT resumeVideoRequested(m_videoKey->text(), m_videoProvider->currentData().toString());
    });
    auto *videoScroll = new QScrollArea(tabs);
    videoScroll->setObjectName(QStringLiteral("videoScroll"));
    videoScroll->setWidgetResizable(true);
    videoScroll->setFrameShape(QFrame::NoFrame);
    videoScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    videoScroll->setWidget(videoPage);
    tabs->addTab(videoScroll, tr("生成视频"));
    updateProvider();
}

bool AssistantPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_messageInput && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        const auto *key = static_cast<QKeyEvent *>(event);
        auto modifiers = key->modifiers();
        modifiers.setFlag(Qt::KeypadModifier, false);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && modifiers == Qt::ControlModifier) {
            event->accept();
            if (event->type() == QEvent::KeyPress) sendMessage();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AssistantPanel::updateActions()
{
    m_chooseMediaButton->setEnabled(!m_busy);
    m_exportButton->setEnabled(!m_busy);
    m_connectButton->setEnabled(!m_busy);
    m_messageInput->setEnabled(!m_externalControl);
    m_sendButton->setEnabled(m_ready && !m_busy && !m_externalControl && !m_messageInput->toPlainText().trimmed().isEmpty());
    m_stopButton->setEnabled(m_busy);
    m_undoButton->setEnabled(!m_busy);
    m_redoButton->setEnabled(!m_busy);
    m_quickTask->setEnabled(!m_busy && !m_externalControl);
    m_fillTaskButton->setEnabled(!m_busy && !m_externalControl);
    const bool seedance = m_videoProvider->currentData().toString() == QStringLiteral("seedance");
    const bool hasKey = !m_videoKey->text().trimmed().isEmpty();
    m_videoProvider->setEnabled(!m_videoBusy);
    m_videoQuality->setEnabled(!m_videoBusy);
    m_videoKey->setEnabled(seedance);
    m_videoGenerateButton->setEnabled(!m_videoBusy && !m_videoPrompt->toPlainText().trimmed().isEmpty() && (!seedance || hasKey)
                                      && (seedance || m_videoFirstFrame->text().isEmpty()));
    m_videoStopButton->setEnabled(m_videoBusy);
    m_videoResumeButton->setEnabled(!m_videoBusy && seedance && hasKey);
}

void AssistantPanel::sendMessage()
{
    const auto text = m_messageInput->toPlainText().trimmed();
    if (!m_ready || m_busy || m_externalControl || text.isEmpty()) {
        return;
    }
    m_busy = true;
    m_messageInput->clear();
    updateActions();
    Q_EMIT messageRequested(text);
}

void AssistantPanel::setAssistantState(bool ready, bool busy, const QString &message)
{
    m_ready = ready;
    m_busy = busy;
    m_assistantStatus->setText(message);
    updateActions();
}

void AssistantPanel::setExternalControl(bool external)
{
    m_externalControl = external;
    updateActions();
}

void AssistantPanel::appendUserMessage(const QString &text)
{
    m_transcript->appendPlainText(tr("你：") + text);
    m_assistantTextAtEnd = false;
}

void AssistantPanel::beginAssistantMessage()
{
    m_transcript->appendPlainText(tr("助手："));
    m_assistantTextAtEnd = true;
}

void AssistantPanel::appendAssistantText(const QString &text)
{
    if (!m_assistantTextAtEnd) {
        beginAssistantMessage();
    }
    QTextCursor cursor(m_transcript->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    m_transcript->setTextCursor(cursor);
    m_transcript->ensureCursorVisible();
}

void AssistantPanel::appendStatus(const QString &text)
{
    m_transcript->appendPlainText(text);
    m_assistantTextAtEnd = false;
}

void AssistantPanel::setVideoState(bool busy, const QString &message)
{
    m_videoBusy = busy;
    m_videoStatus->setText(message);
    updateActions();
}

void AssistantPanel::prepareVideo(const QString &prompt, const QString &ratio, int duration, const QString &firstFramePath)
{
    m_videoPrompt->setPlainText(prompt);
    m_videoRatio->setCurrentText(ratio);
    m_videoDuration->setValue(duration);
    if (m_videoProvider->currentData().toString() == QStringLiteral("ltx-space") && (duration < 4 || duration > 8)) {
        m_videoStatus->setText(tr("LTX 仅支持4–8秒，已调整表单时长；请检查后再点击生成，尚未提交。"));
    }
    m_videoFirstFrame->setText(firstFramePath);
    m_tabs->setCurrentIndex(1);
}
