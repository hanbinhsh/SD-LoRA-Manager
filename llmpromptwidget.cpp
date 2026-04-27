#include "llmpromptwidget.h"
#include "ui_llmpromptwidget.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkReply>
#include <QAbstractScrollArea>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPixmap>
#include <QIcon>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSet>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
const QString kTaskCharacterReplace = "character_replace";
const QString kTaskOutfitReplace = "outfit_replace";
const QString kTaskPreferenceGenerate = "preference_generate";
const QString kTaskImageAdjust = "image_adjust";

struct ParsedThinkingBlocks {
    QString visibleText;
    QString thinkingText;
};

ParsedThinkingBlocks splitThinkingBlocks(const QString &text)
{
    ParsedThinkingBlocks parsed;
    QString remaining = text;
    const QString openTag = "<think>";
    const QString closeTag = "</think>";

    while (!remaining.isEmpty()) {
        int openIndex = remaining.indexOf(openTag, 0, Qt::CaseInsensitive);
        if (openIndex < 0) {
            parsed.visibleText += remaining;
            break;
        }

        parsed.visibleText += remaining.left(openIndex);
        remaining.remove(0, openIndex + openTag.size());

        int closeIndex = remaining.indexOf(closeTag, 0, Qt::CaseInsensitive);
        if (closeIndex < 0) {
            parsed.thinkingText += remaining;
            break;
        }

        parsed.thinkingText += remaining.left(closeIndex);
        remaining.remove(0, closeIndex + closeTag.size());
    }

    return parsed;
}

QString escapeAnglePromptSyntax(QString text)
{
    static const QRegularExpression re("<\\s*(lora|lyco|lokr|locon|hypernet|embedding)\\s*:[^>\\n]+>",
                                       QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator it = re.globalMatch(text);
    QList<QPair<int, int>> ranges;
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        ranges.append(qMakePair(match.capturedStart(), match.capturedLength()));
    }

    for (int i = ranges.size() - 1; i >= 0; --i) {
        const int start = ranges[i].first;
        const int length = ranges[i].second;
        QString captured = text.mid(start, length);
        captured.replace('&', "&amp;");
        captured.replace('<', "&lt;");
        captured.replace('>', "&gt;");
        text.replace(start, length, captured);
    }

    // Fallback for streaming/incomplete tags like "<lora:name:1" (without closing '>').
    static const QRegularExpression incompleteRe("<\\s*(lora|lyco|lokr|locon|hypernet|embedding)\\s*:",
                                                 QRegularExpression::CaseInsensitiveOption);
    text.replace(incompleteRe, "&lt;\\1:");
    return text;
}

QString normalizeStreamPiece(QString chunk)
{
    chunk.replace("\r\n", "\n");
    chunk.replace('\r', '\n');
    return chunk;
}

void appendStreamPiece(QString &target, const QString &chunk)
{
    if (chunk.isEmpty()) return;
    target += normalizeStreamPiece(chunk);
}

QString combineThinkingText(const QString &dedicatedThinking, const QString &embeddedThinking)
{
    QString result = dedicatedThinking.trimmed();
    QString extra = embeddedThinking.trimmed();
    if (!extra.isEmpty() && !result.contains(extra)) {
        if (!result.isEmpty()) result += "\n\n";
        result += extra;
    }
    return result.trimmed();
}

QString makeId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void scrollTextViewportToBottom(QWidget *widget)
{
    if (!widget) return;
    QAbstractScrollArea *scrollArea = qobject_cast<QAbstractScrollArea*>(widget);
    if (!scrollArea) return;
    QScrollBar *scrollBar = scrollArea->verticalScrollBar();
    if (!scrollBar) return;
    scrollBar->setValue(scrollBar->maximum());
}
}

LlmPromptWidget::LlmPromptWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::LlmPromptWidget)
    , m_netManager(new QNetworkAccessManager(this))
{
    ui->setupUi(this);

    connect(ui->btnFetchModels, &QPushButton::clicked, this, &LlmPromptWidget::onFetchModelsClicked);
    connect(ui->btnRefreshCandidates, &QPushButton::clicked, this, &LlmPromptWidget::onRefreshCandidatesClicked);
    connect(ui->btnClearCandidateSelections, &QPushButton::clicked, this, &LlmPromptWidget::onClearCandidateSelectionsClicked);
    connect(ui->btnAnalyzePreference, &QPushButton::clicked, this, &LlmPromptWidget::onAnalyzePreferenceClicked);
    connect(ui->btnGenerate, &QPushButton::clicked, this, &LlmPromptWidget::onGenerateClicked);
    connect(ui->btnStopGenerate, &QPushButton::clicked, this, &LlmPromptWidget::onStopGenerateClicked);
    connect(ui->btnUnloadModel, &QPushButton::clicked, this, &LlmPromptWidget::onUnloadModelClicked);
    connect(ui->btnContinueConversation, &QPushButton::clicked, this, &LlmPromptWidget::onContinueConversationClicked);
    connect(ui->btnNewChat, &QPushButton::clicked, this, &LlmPromptWidget::onNewChatClicked);
    connect(ui->btnRenameChat, &QPushButton::clicked, this, &LlmPromptWidget::onRenameChatClicked);
    connect(ui->btnDeleteChat, &QPushButton::clicked, this, &LlmPromptWidget::onDeleteChatClicked);
    connect(ui->btnClearChats, &QPushButton::clicked, this, &LlmPromptWidget::onClearChatsClicked);
    connect(ui->editChatSearch, &QLineEdit::textChanged, this, &LlmPromptWidget::onChatSearchChanged);
    connect(ui->listChatHistory, &QListWidget::currentItemChanged, this, &LlmPromptWidget::onChatSelectionChanged);
    connect(ui->btnChatSend, &QPushButton::clicked, this, &LlmPromptWidget::onChatSendClicked);
    connect(ui->btnChatStop, &QPushButton::clicked, this, &LlmPromptWidget::onChatStopClicked);
    connect(ui->btnChatAddImages, &QPushButton::clicked, this, &LlmPromptWidget::onChatAddImagesClicked);
    connect(ui->btnChatClearImages, &QPushButton::clicked, this, &LlmPromptWidget::onChatClearImagesClicked);
    connect(ui->btnCopyPrompt, &QPushButton::clicked, this, &LlmPromptWidget::onCopyPromptClicked);
    connect(ui->btnCopyResult, &QPushButton::clicked, this, &LlmPromptWidget::onCopyResultClicked);
    connect(ui->btnAddManualImages, &QPushButton::clicked, this, &LlmPromptWidget::onAddManualImagesClicked);
    connect(ui->btnResetPromptTemplate, &QPushButton::clicked, this, &LlmPromptWidget::onResetPromptTemplateClicked);
    connect(ui->comboTaskType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LlmPromptWidget::onTaskTypeChanged);
    connect(ui->comboTemplateTaskType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LlmPromptWidget::onTemplateTaskTypeChanged);
    connect(ui->tabMain, &QTabWidget::currentChanged, this, [this]() {
        if (ui->tabMain->currentWidget() == ui->pageChat && m_chatViewDirty) {
            m_chatViewDirty = false;
            updateChatView(false);
        }
    });
    connect(ui->comboBackend, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        const QString endpoint = ui->editEndpoint->text().trimmed();
        if (index == 1 && (endpoint.isEmpty() || endpoint == "http://127.0.0.1:11434")) {
            ui->editEndpoint->setText("http://127.0.0.1:1234");
        } else if (index == 0 && (endpoint.isEmpty() || endpoint == "http://127.0.0.1:1234")) {
            ui->editEndpoint->setText("http://127.0.0.1:11434");
        }
        saveSettings();
    });
    connect(ui->textPromptTemplate, &QPlainTextEdit::textChanged, this, &LlmPromptWidget::onPromptTemplateEdited);
    connect(ui->textTaskGuidance, &QPlainTextEdit::textChanged, this, &LlmPromptWidget::onTaskGuidanceEdited);
    connect(ui->textImageAttachmentNote, &QPlainTextEdit::textChanged, this, &LlmPromptWidget::onImageAttachmentNoteEdited);
    connect(ui->btnToggleThinking, &QToolButton::toggled, this, &LlmPromptWidget::onThinkingToggled);
    connect(ui->listLoraCandidates, &QListWidget::itemChanged, this, &LlmPromptWidget::onCandidateItemChanged);
    connect(ui->listImageCandidates, &QListWidget::itemChanged, this, &LlmPromptWidget::onCandidateItemChanged);

    ui->listLoraCandidates->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->listImageCandidates->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->btnToggleThinking->setEnabled(false);
    ui->btnContinueConversation->setEnabled(false);
    setupChatUi();
    ui->textPlaceholderHelp->setHtml(
        "<b>占位符说明</b><br>"
        "当前编辑的是所选任务类型的专属模板。切换任务后会自动切换到对应模板。<br>"
        "<code>{task_type}</code> 当前任务类型，例如人物替换。<br>"
        "<code>{instruction}</code> 用户输入的自然语言指令。<br>"
        "<code>{source_prompt}</code> 原始提示词。<br>"
        "<code>{parsed_old_target}</code> 从指令中解析出的旧目标。<br>"
        "<code>{parsed_new_target}</code> 从指令中解析出的新目标。<br>"
        "<code>{conservative_prompt}</code> 基于原 prompt 计算出的保守替换版本。<br>"
        "<code>{preference_summary}</code> 历史图库统计出的用户偏好摘要。<br>"
        "<code>{lora_context}</code> 选中的 LoRA 上下文，含触发词/预览提示词。<br>"
        "<code>{image_context}</code> 选中的参考图片和对应 prompt。<br>"
        "<code>{manual_trigger_words}</code> 手动输入的触发词。<br>"
        "<code>{manual_lora_prompts}</code> 手动输入的 LoRA 参考提示词。<br>"
        "<code>{task_guidance}</code> 针对任务类型的额外规则。<br>"
        "<code>{image_attachment_note}</code> 是否附带图片给模型的说明。"
    );

    loadSettings();
    loadConversations();
    onRefreshCandidatesClicked();
    updateContextSelectionSummary();
    updateChatButtons();
}

LlmPromptWidget::~LlmPromptWidget()
{
    m_destroying = true;
    if (m_chatRefreshTimer) {
        m_chatRefreshTimer->stop();
    }
    if (ui && ui->listChatMessages && ui->listChatMessages->viewport()) {
        ui->listChatMessages->viewport()->removeEventFilter(this);
    }
    if (m_activeGenerateReply) {
        m_activeGenerateReply->disconnect(this);
        m_activeGenerateReply->abort();
        m_activeGenerateReply->deleteLater();
        m_activeGenerateReply = nullptr;
    }
    if (m_activeChatReply) {
        m_activeChatReply->disconnect(this);
        m_activeChatReply->abort();
        m_activeChatReply->deleteLater();
        m_activeChatReply = nullptr;
    }
    saveSettings();
    saveConversations();
    Ui::LlmPromptWidget *uiToDelete = ui;
    ui = nullptr;
    delete uiToDelete;
}

bool LlmPromptWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (m_destroying || !ui) return QWidget::eventFilter(watched, event);
    if (event->type() == QEvent::Wheel) {
        QAbstractScrollArea *scrollArea = nullptr;
        if (ui->listChatMessages && watched == ui->listChatMessages->viewport()) {
            scrollArea = ui->listChatMessages;
        } else if (QWidget *widget = qobject_cast<QWidget*>(watched)) {
            scrollArea = qobject_cast<QAbstractScrollArea*>(widget->parentWidget());
        }

        if (scrollArea) {
            QWheelEvent *wheelEvent = static_cast<QWheelEvent*>(event);
            QScrollBar *bar = scrollArea->verticalScrollBar();
            if (bar) {
                const int delta = wheelEvent->angleDelta().y();
                const int direction = (delta > 0) ? -1 : (delta < 0 ? 1 : 0);
                if (direction != 0) {
                    const int oldValue = bar->value();
                    const int step = qMax(12, bar->singleStep());
                    bar->setValue(oldValue + direction * step * 3);
                }

                const bool atBottom = (bar->maximum() - bar->value()) <= 4;
                if (ui->listChatMessages && watched == ui->listChatMessages->viewport()) {
                    m_chatListAutoScrollEnabled = atBottom;
                    m_chatListScrollValue = bar->value();
                } else {
                    const QString areaType = watched->property("chatScrollAreaType").toString();
                    const bool pending = watched->property("chatScrollPending").toBool();
                    const QString scrollKey = watched->property("chatScrollKey").toString();
                    if (!scrollKey.isEmpty()) {
                        m_chatInnerScrollValues.insert(scrollKey, bar->value());
                    }
                    if (pending && areaType == "body") {
                        m_pendingChatBodyAutoScrollEnabled = atBottom;
                    } else if (pending && areaType == "thinking") {
                        m_pendingChatThinkingAutoScrollEnabled = atBottom;
                    }
                }
            }
            wheelEvent->accept();
            return true;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void LlmPromptWidget::setLibraryPaths(const QStringList &loraPaths, const QStringList &galleryPaths)
{
    m_loraPaths = loraPaths;
    m_galleryPaths = galleryPaths;
    onRefreshCandidatesClicked();
}

QString LlmPromptWidget::settingsPath() const
{
    QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);
    return configDir + "/settings.json";
}

QString LlmPromptWidget::conversationsPath() const
{
    QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);
    return configDir + "/llm_conversations.json";
}

LlmPromptWidget::LlmBackend LlmPromptWidget::currentBackend() const
{
    return ui->comboBackend->currentIndex() == 1 ? LlmBackend::LmStudio : LlmBackend::Ollama;
}

QString LlmPromptWidget::currentBackendName() const
{
    return currentBackend() == LlmBackend::LmStudio ? "LM Studio" : "Ollama";
}

QString LlmPromptWidget::backendKey(LlmBackend backend) const
{
    return backend == LlmBackend::LmStudio ? "lmstudio" : "ollama";
}

QString LlmPromptWidget::formatChatTimestamp(const QDateTime &time) const
{
    if (!time.isValid()) return "--:--";
    return time.toString("MM-dd HH:mm");
}

QString LlmPromptWidget::endpointBaseUrl() const
{
    QString endpoint = ui->editEndpoint->text().trimmed();
    if (endpoint.endsWith('/')) endpoint.chop(1);
    return endpoint;
}

QString LlmPromptWidget::taskKeyForIndex(int index) const
{
    switch (index) {
    case 0: return kTaskCharacterReplace;
    case 1: return kTaskOutfitReplace;
    case 2: return kTaskPreferenceGenerate;
    case 3: return kTaskImageAdjust;
    default: return kTaskCharacterReplace;
    }
}

QString LlmPromptWidget::currentTaskKey() const
{
    return taskKeyForIndex(ui->comboTaskType->currentIndex());
}

QString LlmPromptWidget::taskLabelForKey(const QString &taskKey) const
{
    if (taskKey == kTaskCharacterReplace) return "人物替换";
    if (taskKey == kTaskOutfitReplace) return "服装替换";
    if (taskKey == kTaskPreferenceGenerate) return "偏好提示词生成";
    if (taskKey == kTaskImageAdjust) return "图片调整";
    return "人物替换";
}

bool LlmPromptWidget::isReplacementTask(const QString &taskKey) const
{
    return taskKey == kTaskCharacterReplace || taskKey == kTaskOutfitReplace;
}

void LlmPromptWidget::loadPromptTemplateForTask(const QString &taskKey)
{
    m_syncingPromptTemplateEditor = true;
    ui->textPromptTemplate->setPlainText(m_taskPromptTemplates.value(taskKey, defaultPromptTemplate(taskKey)));
    ui->lblPromptTemplateTask->setText("当前模板任务");
    m_syncingPromptTemplateEditor = false;
}

void LlmPromptWidget::persistCurrentPromptTemplate()
{
    if (m_syncingPromptTemplateEditor) return;
    m_taskPromptTemplates.insert(currentTaskKey(), ui->textPromptTemplate->toPlainText());
}

void LlmPromptWidget::loadTaskPromptFieldsForTask(const QString &taskKey)
{
    m_syncingTaskPromptFields = true;
    ui->textTaskGuidance->setPlainText(m_taskGuidances.value(taskKey, defaultTaskGuidance(taskKey)));
    ui->textImageAttachmentNote->setPlainText(m_taskImageAttachmentNotes.value(taskKey, defaultImageAttachmentNote(taskKey)));
    m_syncingTaskPromptFields = false;
}

void LlmPromptWidget::persistCurrentTaskPromptFields()
{
    if (m_syncingTaskPromptFields) return;
    const QString taskKey = currentTaskKey();
    m_taskGuidances.insert(taskKey, ui->textTaskGuidance->toPlainText());
    m_taskImageAttachmentNotes.insert(taskKey, ui->textImageAttachmentNote->toPlainText());
}

void LlmPromptWidget::updateContextSelectionSummary()
{
    int loraChecked = 0;
    int imageChecked = 0;

    for (int i = 0; i < ui->listLoraCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listLoraCandidates->item(i);
        if (item && item->checkState() == Qt::Checked) ++loraChecked;
    }

    for (int i = 0; i < ui->listImageCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listImageCandidates->item(i);
        if (item && item->checkState() == Qt::Checked) ++imageChecked;
    }

    ui->lblLoraSelectionSummary->setText(QString("已勾选 %1 / %2 个 LoRA").arg(loraChecked).arg(ui->listLoraCandidates->count()));
    ui->lblImageSelectionSummary->setText(QString("已勾选 %1 / %2 张图片").arg(imageChecked).arg(ui->listImageCandidates->count()));
}

void LlmPromptWidget::setupChatUi()
{
    ui->horizontalLayoutChatRoot->setStretch(0, 0);
    ui->horizontalLayoutChatRoot->setStretch(1, 1);
    ui->verticalLayoutChatHistory->setSizeConstraint(QLayout::SetMinimumSize);
    ui->pageChat->setStyleSheet("QWidget#pageChat{background:#1e252f;}");
    ui->listChatHistory->setMinimumWidth(260);
    ui->listChatHistory->setMaximumWidth(320);
    ui->listChatHistory->setSelectionMode(QAbstractItemView::SingleSelection);
    ui->listChatMessages->setSelectionMode(QAbstractItemView::NoSelection);
    ui->listChatMessages->setFocusPolicy(Qt::NoFocus);
    ui->listChatMessages->setAlternatingRowColors(false);
    ui->listChatMessages->setUniformItemSizes(false);
    ui->listChatMessages->setSpacing(4);
    ui->listChatMessages->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    ui->listChatMessages->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->listChatMessages->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->listChatMessages->viewport()->installEventFilter(this);
    connect(ui->listChatMessages, &QListWidget::customContextMenuRequested, this, &LlmPromptWidget::showChatMessageMenu);
    ui->listChatMessages->setStyleSheet(
        "QListWidget{background:#16191e;border:1px solid #31363d;padding:2px;outline:none;}"
        "QListWidget::item{background:transparent;border:none;margin:0px;padding:0px;}"
        "QListWidget::item:hover{background:transparent;}"
        "QListWidget::item:selected{background:transparent;color:inherit;}"
        "QScrollBar:vertical{background:#12161c;width:4px;margin:2px 0 2px 0;border-radius:2px;}"
        "QScrollBar::handle:vertical{background:#3a4654;min-height:24px;border-radius:2px;}"
        "QScrollBar::handle:vertical:hover{background:#4b5a6b;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0px;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}");
    if (ui->listChatMessages->verticalScrollBar()) {
        ui->listChatMessages->verticalScrollBar()->setSingleStep(18);
        connect(ui->listChatMessages->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
            QScrollBar *bar = ui->listChatMessages->verticalScrollBar();
            if (!bar) return;
            m_chatListScrollValue = value;
            m_chatListAutoScrollEnabled = (bar->maximum() - value) <= 4;
        });
    }
    ui->textChatInput->setFixedHeight(88);
    ui->btnChatStop->setEnabled(false);

    m_chatRefreshTimer = new QTimer(this);
    m_chatRefreshTimer->setSingleShot(true);
    m_chatRefreshTimer->setInterval(140);
    connect(m_chatRefreshTimer, &QTimer::timeout, this, [this]() {
        const bool scrollToBottom = m_chatRefreshScrollToBottomPending;
        m_chatRefreshScrollToBottomPending = false;
        m_chatRefreshScheduled = false;
        updateChatView(scrollToBottom);
    });

    updateChatStatus("待命");
    updateChatImageInfoLabel();
}

void LlmPromptWidget::loadConversations()
{
    m_chatSessions.clear();
    QFile file(conversationsPath());
    if (!file.open(QIODevice::ReadOnly)) {
        refreshConversationList();
        updateChatView(false);
        return;
    }

    QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    m_activeChatSessionId = root["active_id"].toString();
    const QJsonArray conversations = root["conversations"].toArray();
    for (const QJsonValue &sessionValue : conversations) {
        QJsonObject obj = sessionValue.toObject();
        ChatSession session;
        session.id = obj["id"].toString();
        if (session.id.isEmpty()) session.id = makeId();
        session.title = obj["title"].toString("未命名对话");
        session.taskLabel = obj["taskLabel"].toString();
        session.createdAt = QDateTime::fromString(obj["createdAt"].toString(), Qt::ISODateWithMs);
        session.updatedAt = QDateTime::fromString(obj["updatedAt"].toString(), Qt::ISODateWithMs);
        if (!session.createdAt.isValid()) session.createdAt = QDateTime::currentDateTime();
        if (!session.updatedAt.isValid()) session.updatedAt = session.createdAt;
        session.backend = obj["backend"].toString("ollama");
        session.endpoint = obj["endpoint"].toString();
        session.modelName = obj["modelName"].toString();
        session.systemPrompt = obj["systemPrompt"].toString();
        session.options = obj["options"].toObject();
        const QJsonArray messages = obj["messages"].toArray();
        for (const QJsonValue &messageValue : messages) {
            QJsonObject msgObj = messageValue.toObject();
            ChatMessage msg;
            msg.id = msgObj["id"].toString();
            if (msg.id.isEmpty()) msg.id = makeId();
            msg.role = msgObj["role"].toString();
            msg.content = msgObj["content"].toString();
            msg.thinking = msgObj["thinking"].toString();
            msg.createdAt = QDateTime::fromString(msgObj["createdAt"].toString(), Qt::ISODateWithMs);
            if (!msg.createdAt.isValid()) msg.createdAt = session.createdAt;
            const QJsonArray images = msgObj["imagePaths"].toArray();
            for (const QJsonValue &imageValue : images) {
                QString path = imageValue.toString();
                if (!path.isEmpty()) msg.imagePaths.append(path);
            }
            if (!msg.role.isEmpty()) session.messages.append(msg);
        }
        m_chatSessions.append(session);
    }

    std::sort(m_chatSessions.begin(), m_chatSessions.end(), [](const ChatSession &a, const ChatSession &b) {
        return a.updatedAt > b.updatedAt;
    });
    if (chatSessionIndex(m_activeChatSessionId) < 0 && !m_chatSessions.isEmpty()) {
        m_activeChatSessionId = m_chatSessions.first().id;
    }
    refreshConversationList();
    selectChatSession(m_activeChatSessionId, false);
}

void LlmPromptWidget::saveConversations() const
{
    QJsonObject root;
    root["version"] = 1;
    root["active_id"] = m_activeChatSessionId;
    QJsonArray sessions;
    for (const ChatSession &session : m_chatSessions) {
        QJsonObject obj;
        obj["id"] = session.id;
        obj["title"] = session.title;
        obj["taskLabel"] = session.taskLabel;
        obj["createdAt"] = session.createdAt.toString(Qt::ISODateWithMs);
        obj["updatedAt"] = session.updatedAt.toString(Qt::ISODateWithMs);
        obj["backend"] = session.backend;
        obj["endpoint"] = session.endpoint;
        obj["modelName"] = session.modelName;
        obj["systemPrompt"] = session.systemPrompt;
        obj["options"] = session.options;
        QJsonArray messages;
        for (const ChatMessage &message : session.messages) {
            QJsonObject msg;
            msg["id"] = message.id;
            msg["role"] = message.role;
            msg["content"] = message.content;
            msg["thinking"] = message.thinking;
            msg["createdAt"] = message.createdAt.toString(Qt::ISODateWithMs);
            QJsonArray images;
            for (const QString &path : message.imagePaths) images.append(path);
            msg["imagePaths"] = images;
            messages.append(msg);
        }
        obj["messages"] = messages;
        sessions.append(obj);
    }
    root["conversations"] = sessions;

    QFile file(conversationsPath());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }
}

void LlmPromptWidget::refreshConversationList()
{
    const QSignalBlocker blocker(ui->listChatHistory);
    m_syncingChatList = true;
    ui->listChatHistory->clear();
    const QString filter = ui->editChatSearch->text().trimmed();
    for (const ChatSession &session : m_chatSessions) {
        if (!filter.isEmpty()
            && !session.title.contains(filter, Qt::CaseInsensitive)
            && !session.taskLabel.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        QListWidgetItem *item = new QListWidgetItem(QString("%1\n%2")
                                                        .arg(session.title, formatChatTimestamp(session.updatedAt)));
        item->setData(Qt::UserRole, session.id);
        item->setToolTip(QString("%1\n%2").arg(session.taskLabel, session.updatedAt.toString("yyyy-MM-dd HH:mm:ss")));
        item->setSizeHint(QSize(0, 42));
        ui->listChatHistory->addItem(item);
        if (session.id == m_activeChatSessionId) {
            ui->listChatHistory->setCurrentItem(item);
        }
    }
    m_syncingChatList = false;
    updateChatButtons();
}

int LlmPromptWidget::chatSessionIndex(const QString &sessionId) const
{
    for (int i = 0; i < m_chatSessions.size(); ++i) {
        if (m_chatSessions[i].id == sessionId) return i;
    }
    return -1;
}

LlmPromptWidget::ChatSession *LlmPromptWidget::activeChatSession()
{
    int index = chatSessionIndex(m_activeChatSessionId);
    return index >= 0 ? &m_chatSessions[index] : nullptr;
}

const LlmPromptWidget::ChatSession *LlmPromptWidget::activeChatSession() const
{
    int index = chatSessionIndex(m_activeChatSessionId);
    return index >= 0 ? &m_chatSessions[index] : nullptr;
}

void LlmPromptWidget::selectChatSession(const QString &sessionId, bool switchToChatTab)
{
    if (chatSessionIndex(sessionId) < 0) {
        m_activeChatSessionId.clear();
    } else {
        m_activeChatSessionId = sessionId;
    }
    if (switchToChatTab) {
        ui->tabMain->setCurrentWidget(ui->pageChat);
    }
    refreshConversationList();
    updateChatView(false);
    updateChatButtons();
    updateChatImageInfoLabel();
    saveConversations();
}

QString LlmPromptWidget::makeChatTitle(const QString &taskLabel, const QString &instruction) const
{
    QString title = instruction.simplified();
    if (title.isEmpty()) title = taskLabel;
    if (title.size() > 36) title = title.left(36) + "...";
    return QString("%1 - %2").arg(taskLabel.isEmpty() ? "对话" : taskLabel, title);
}

LlmPromptWidget::ChatSession LlmPromptWidget::createChatSession(const QString &title, const QString &taskLabel) const
{
    ChatSession session;
    session.id = makeId();
    session.title = title.isEmpty() ? "新对话" : title;
    session.taskLabel = taskLabel;
    session.createdAt = QDateTime::currentDateTime();
    session.updatedAt = session.createdAt;
    session.backend = backendKey(currentBackend());
    session.endpoint = endpointBaseUrl();
    session.modelName = ui->comboModel->currentText().trimmed();
    session.systemPrompt = ui->textSystemPrompt->toPlainText();
    session.options = buildGenerationOptions();
    return session;
}

LlmPromptWidget::ChatMessage LlmPromptWidget::makeChatMessage(const QString &role, const QString &content, const QString &thinking, const QStringList &imagePaths) const
{
    ChatMessage message;
    message.id = makeId();
    message.role = role;
    message.content = content;
    message.thinking = thinking;
    message.imagePaths = imagePaths;
    message.createdAt = QDateTime::currentDateTime();
    return message;
}

QString LlmPromptWidget::markdownToHtml(const QString &markdown) const
{
    QString normalized = markdown;
    normalized.replace("\r\n", "\n");
    normalized.replace('\r', '\n');
    const QString html = QTextDocumentFragment::fromMarkdown(normalized).toHtml();
    static const QRegularExpression bodyRe("<body[^>]*>(.*)</body>",
                                           QRegularExpression::CaseInsensitiveOption
                                           | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = bodyRe.match(html);
    return match.hasMatch() ? match.captured(1) : html;
}

QString LlmPromptWidget::imageMimeType(const QString &path) const
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "jpg" || suffix == "jpeg") return "image/jpeg";
    if (suffix == "webp") return "image/webp";
    if (suffix == "bmp") return "image/bmp";
    return "image/png";
}

void LlmPromptWidget::openImagePath(const QString &path) const
{
    if (!QFileInfo::exists(path)) return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void LlmPromptWidget::requestChatRefresh(bool scrollToBottom)
{
    m_chatRefreshScrollToBottomPending = m_chatRefreshScrollToBottomPending || scrollToBottom;
    if (!m_pendingChatSessionId.isEmpty() && m_activeChatSessionId != m_pendingChatSessionId) {
        m_chatViewDirty = true;
        updateChatButtons();
        return;
    }
    if (m_chatRefreshScheduled) return;
    if (!m_chatRefreshTimer) {
        updateChatView(m_chatRefreshScrollToBottomPending);
        m_chatRefreshScrollToBottomPending = false;
        return;
    }
    m_chatRefreshScheduled = true;
    m_chatRefreshTimer->start();
}

void LlmPromptWidget::updateChatView(bool scrollToBottom)
{
    if (ui->tabMain->currentWidget() != ui->pageChat) {
        m_chatViewDirty = true;
        const ChatSession *session = activeChatSession();
        ui->lblChatTitle->setText(session ? session->title : "未选择对话");
        ui->lblChatModelInfo->setText(session ? QString("模型：%1").arg(session->modelName.isEmpty() ? "未设置" : session->modelName)
                                             : "模型：未设置");
        updateChatButtons();
        return;
    }

    m_chatViewDirty = false;
    QScrollBar *listBarBefore = ui->listChatMessages->verticalScrollBar();
    if (listBarBefore) {
        m_chatListScrollValue = listBarBefore->value();
        m_chatListAutoScrollEnabled = (listBarBefore->maximum() - listBarBefore->value()) <= 4;
    }
    ui->listChatMessages->setUpdatesEnabled(false);
    ui->listChatMessages->clear();

    const ChatSession *session = activeChatSession();
    if (!session) {
        ui->lblChatTitle->setText("未选择对话");
        ui->lblChatModelInfo->setText("模型：未设置");
        ui->listChatMessages->setUpdatesEnabled(true);
        updateChatButtons();
        return;
    }

    ui->lblChatTitle->setText(session->title);
    ui->lblChatModelInfo->setText(QString("模型：%1").arg(session->modelName.isEmpty() ? "未设置" : session->modelName));

    const int viewportWidth = qMax(320, ui->listChatMessages->viewport()->width());
    const int rowWidth = qMax(260, viewportWidth - 8);
    const int maxBubbleWidth = qBound(280, int(viewportWidth * 0.66), qMax(280, viewportWidth - 56));
    const int minBubbleWidth = 150;
    const int bubbleBodyMaxHeight = 300;
    const int autoScrollThreshold = 4;
    auto scrollKeyFor = [](const QString &messageId, const QString &areaType, bool pending) {
        return QString("%1|%2").arg(pending ? "__pending__" : messageId, areaType);
    };
    auto bindInnerScrollState = [&](QTextBrowser *view, const QString &key, const QString &areaType, bool pending, bool autoScroll) {
        if (!view || !view->verticalScrollBar()) return;
        view->viewport()->setProperty("chatScrollAreaType", areaType);
        view->viewport()->setProperty("chatScrollPending", pending);
        view->viewport()->setProperty("chatScrollKey", key);
        connect(view->verticalScrollBar(), &QScrollBar::valueChanged, this, [this, key, areaType, pending](int value) {
            m_chatInnerScrollValues.insert(key, value);
            QScrollBar *bar = qobject_cast<QScrollBar*>(sender());
            if (!bar) return;
            const bool atBottom = (bar->maximum() - bar->value()) <= 4;
            if (pending && areaType == "body") {
                m_pendingChatBodyAutoScrollEnabled = atBottom;
            } else if (pending && areaType == "thinking") {
                m_pendingChatThinkingAutoScrollEnabled = atBottom;
            }
        });
        QPointer<QTextBrowser> safeView(view);
        QTimer::singleShot(0, view, [this, safeView, key, autoScroll]() {
            if (!safeView || !safeView->verticalScrollBar()) return;
            QScrollBar *bar = safeView->verticalScrollBar();
            if (autoScroll) {
                bar->setValue(bar->maximum());
                m_chatInnerScrollValues.insert(key, bar->value());
            } else if (m_chatInnerScrollValues.contains(key)) {
                bar->setValue(qBound(bar->minimum(), m_chatInnerScrollValues.value(key), bar->maximum()));
            }
        });
    };

    auto appendBubbleRow = [&](const ChatMessage &message, bool pending) {
        const bool isUser = (message.role == "user");
        const QString bubbleBg = isUser ? "#27425f" : "#1f2834";
        const QString bubbleBorder = isUser ? "#3f6b95" : "#3a4654";
        const QString bubbleText = isUser ? "#eaf4ff" : "#dcdedf";
        const QString content = message.content.trimmed();
        const QString bodyHtml = markdownToHtml(escapeAnglePromptSyntax(content));
        const QString thinkingText = message.thinking.trimmed();
        const QString thinkingHtml = markdownToHtml(escapeAnglePromptSyntax(thinkingText));

        QTextDocument bodyWidthDoc;
        bodyWidthDoc.setHtml(bodyHtml);
        QTextDocument thinkingWidthDoc;
        thinkingWidthDoc.setHtml(thinkingHtml);
        QFontMetrics titleFm(ui->listChatMessages->font());
        const QString timestampText = formatChatTimestamp(message.createdAt);
        const int naturalBodyWidth = int(std::ceil(bodyWidthDoc.idealWidth())) + 28;
        const int naturalThinkingWidth = int(std::ceil(thinkingWidthDoc.idealWidth())) + 40;
        const int naturalMetaWidth = titleFm.horizontalAdvance(timestampText) + 220;
        const int imageGridWidth = message.imagePaths.isEmpty() ? 0 : 276;
        const int bubbleWidth = qBound(minBubbleWidth,
                                       qMax(qMax(qMax(naturalBodyWidth, naturalThinkingWidth), naturalMetaWidth), imageGridWidth),
                                       maxBubbleWidth);

        QListWidgetItem *item = new QListWidgetItem();
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        item->setData(Qt::UserRole, message.id);
        item->setBackground(QBrush(Qt::transparent));

        QWidget *rowWidget = new QWidget(ui->listChatMessages);
        rowWidget->setAttribute(Qt::WA_StyledBackground, true);
        rowWidget->setStyleSheet("background:transparent;");
        QHBoxLayout *rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(6, 4, 6, 4);
        rowLayout->setSpacing(0);

        QFrame *bubble = new QFrame(rowWidget);
        bubble->setStyleSheet(QString("QFrame{background:%1;border:1px solid %2;border-radius:10px;}").arg(bubbleBg, bubbleBorder));
        bubble->setFixedWidth(bubbleWidth);
        bubble->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        QVBoxLayout *bubbleLayout = new QVBoxLayout(bubble);
        bubbleLayout->setContentsMargins(10, 8, 10, 8);
        bubbleLayout->setSpacing(6);

        auto bindMessageMenu = [&](QWidget *widget) {
            widget->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(widget, &QWidget::customContextMenuRequested, this, [this, message, widget](const QPoint &pos) {
                showChatMessageMenuForId(message.id, widget->mapToGlobal(pos));
            });
        };
        bindMessageMenu(rowWidget);
        bindMessageMenu(bubble);

        if (!content.isEmpty()) {
            QTextBrowser *bodyView = new QTextBrowser(bubble);
            bodyView->setReadOnly(true);
            bodyView->setOpenExternalLinks(false);
            bodyView->setFrameShape(QFrame::NoFrame);
            bodyView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            bodyView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            bodyView->setFocusPolicy(Qt::WheelFocus);
            bodyView->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
            bodyView->setStyleSheet(QString(
                "QTextBrowser{border:none;background:transparent;color:%1;font-size:13px;padding:0px;}"
                "QScrollBar:vertical{background:transparent;width:4px;margin:0px;}"
                "QScrollBar::handle:vertical{background:#5f6f80;min-height:20px;border-radius:2px;}"
                "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0px;}"
                "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}").arg(bubbleText));
            bodyView->document()->setDocumentMargin(0);
            bodyView->viewport()->setAutoFillBackground(false);
            bodyView->viewport()->installEventFilter(this);
            bodyView->setHtml(bodyHtml);
            bodyView->document()->setTextWidth(bubbleWidth - 20);
            const int docHeight = qMax(16, int(bodyView->document()->size().height()) + 4);
            const int finalBodyHeight = qMin(docHeight, bubbleBodyMaxHeight);
            bodyView->setFixedHeight(finalBodyHeight);
            if (docHeight > finalBodyHeight) {
                bodyView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
                if (bodyView->verticalScrollBar()) bodyView->verticalScrollBar()->setSingleStep(16);
            }
            bindInnerScrollState(bodyView, scrollKeyFor(message.id, "body", pending), "body", pending,
                                 pending && m_pendingChatBodyAutoScrollEnabled);
            bindMessageMenu(bodyView);
            bubbleLayout->addWidget(bodyView);
        }

        if (!message.imagePaths.isEmpty()) {
            QWidget *imageGrid = new QWidget(bubble);
            QGridLayout *grid = new QGridLayout(imageGrid);
            grid->setContentsMargins(0, 0, 0, 0);
            grid->setSpacing(6);
            for (int i = 0; i < message.imagePaths.size(); ++i) {
                const QString path = message.imagePaths[i];
                QPushButton *thumb = new QPushButton(imageGrid);
                thumb->setFixedSize(82, 82);
                thumb->setToolTip(path);
                thumb->setText(QFileInfo::exists(path) ? QString() : "图片\n缺失");
                thumb->setStyleSheet("QPushButton{background:#11161c;border:1px solid #3a4654;border-radius:6px;color:#8c96a0;padding:2px;}");
                if (QFileInfo::exists(path)) {
                    QImageReader reader(path);
                    reader.setAutoTransform(true);
                    const QSize originalSize = reader.size();
                    if (originalSize.isValid()) {
                        reader.setScaledSize(originalSize.scaled(76, 76, Qt::KeepAspectRatio));
                    }
                    const QImage image = reader.read();
                    if (!image.isNull()) {
                        thumb->setIcon(QIcon(QPixmap::fromImage(image)));
                        thumb->setIconSize(QSize(76, 76));
                    } else {
                        thumb->setText("图片\n无法读取");
                    }
                    connect(thumb, &QPushButton::clicked, this, [this, path]() { openImagePath(path); });
                } else {
                    thumb->setEnabled(false);
                }
                grid->addWidget(thumb, i / 3, i % 3);
            }
            bubbleLayout->addWidget(imageGrid, 0, Qt::AlignLeft);
        }

        if (message.role == "assistant" && !thinkingText.isEmpty()) {
            QPushButton *btnToggleThinking = new QPushButton(bubble);
            btnToggleThinking->setCursor(Qt::PointingHandCursor);
            btnToggleThinking->setFocusPolicy(Qt::NoFocus);
            btnToggleThinking->setProperty("thinkingToggleButton", true);
            btnToggleThinking->setStyleSheet(QString(
                "QPushButton{background:#223041;border:1px solid #3a4b60;border-radius:4px;padding:2px 8px;color:%1;}"
                "QPushButton:hover{background:#2d3f56;color:#ffffff;}").arg(bubbleText));
            const bool expanded = pending ? m_pendingChatThinkingExpanded : m_expandedThinkingMessageIds.contains(message.id);
            btnToggleThinking->setText(expanded ? "隐藏思考" : "显示思考");
            bubbleLayout->addWidget(btnToggleThinking, 0, Qt::AlignLeft);

            QTextBrowser *thinkingView = new QTextBrowser(bubble);
            thinkingView->setReadOnly(true);
            thinkingView->setOpenExternalLinks(false);
            thinkingView->setFrameShape(QFrame::NoFrame);
            thinkingView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            thinkingView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            thinkingView->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
            thinkingView->setStyleSheet(
                "QTextBrowser{border:1px solid #2e3742;border-radius:6px;background:#11161c;color:#dcdedf;padding:6px;}"
                "QScrollBar:vertical{background:transparent;width:4px;margin:0px;}"
                "QScrollBar::handle:vertical{background:#5f6f80;min-height:20px;border-radius:2px;}"
                "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0px;}"
                "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}");
            thinkingView->viewport()->installEventFilter(this);
            thinkingView->setHtml(thinkingHtml);
            thinkingView->document()->setTextWidth(bubbleWidth - 28);
            const int thinkingDocHeight = qMax(24, int(thinkingView->document()->size().height()) + 8);
            thinkingView->setFixedHeight(qMin(thinkingDocHeight, 220));
            thinkingView->setVisible(expanded);
            bubbleLayout->addWidget(thinkingView);
            bindInnerScrollState(thinkingView, scrollKeyFor(message.id, "thinking", pending), "thinking", pending,
                                 expanded && pending && m_pendingChatThinkingAutoScrollEnabled);
            bindMessageMenu(thinkingView);
            connect(btnToggleThinking, &QPushButton::clicked, this, [this, message, pending, btnToggleThinking, thinkingView, rowWidget, rowLayout, bubble, item, rowWidth]() {
                const bool checked = !thinkingView->isVisible();
                if (pending) {
                    m_pendingChatThinkingExpanded = checked;
                } else if (checked) {
                    m_expandedThinkingMessageIds.insert(message.id);
                } else {
                    m_expandedThinkingMessageIds.remove(message.id);
                }
                btnToggleThinking->setText(checked ? "隐藏思考" : "显示思考");
                thinkingView->setVisible(checked);
                if (checked && thinkingView->verticalScrollBar()) {
                    QPointer<QTextBrowser> safeThinkingView(thinkingView);
                    QTimer::singleShot(0, thinkingView, [safeThinkingView]() {
                        if (safeThinkingView && safeThinkingView->verticalScrollBar()) {
                            safeThinkingView->verticalScrollBar()->setValue(safeThinkingView->verticalScrollBar()->maximum());
                        }
                    });
                }
                bubble->adjustSize();
                const int newRowHeight = bubble->sizeHint().height() + rowLayout->contentsMargins().top() + rowLayout->contentsMargins().bottom();
                rowWidget->setFixedHeight(qMax(28, newRowHeight));
                item->setSizeHint(QSize(rowWidth, rowWidget->height()));
            });
        }

        QHBoxLayout *actionLayout = new QHBoxLayout();
        actionLayout->setContentsMargins(0, 2, 0, 0);
        actionLayout->setSpacing(6);

        QLabel *timeLabel = new QLabel(timestampText, bubble);
        timeLabel->setStyleSheet("font-size:11px; color:#8c96a0;");
        actionLayout->addWidget(timeLabel);
        actionLayout->addStretch(1);

        auto makeActionButton = [&](const QString &text, const QString &tooltip) {
            QPushButton *button = new QPushButton(text, bubble);
            button->setCursor(Qt::PointingHandCursor);
            button->setFocusPolicy(Qt::NoFocus);
            button->setFixedHeight(24);
            button->setToolTip(tooltip);
            button->setStyleSheet(
                "QPushButton{background:#18212b;border:1px solid #344254;border-radius:4px;color:#b9c4d0;padding:2px 7px;font-size:11px;}"
                "QPushButton:hover{background:#263447;color:#ffffff;}"
                "QPushButton:disabled{background:#171b20;border-color:#2a3038;color:#66707c;}");
            return button;
        };

        const bool busy = !m_activeChatReply.isNull() || !m_activeGenerateReply.isNull();
        if (!pending) {
            QPushButton *btnCopy = makeActionButton("复制", "复制这条消息");
            connect(btnCopy, &QPushButton::clicked, this, [this, message]() { copyChatMessage(message.id); });
            actionLayout->addWidget(btnCopy);

            if (message.role == "user") {
                QPushButton *btnEdit = makeActionButton("编辑", "编辑这条用户消息并重新生成后续回复");
                btnEdit->setEnabled(!busy);
                connect(btnEdit, &QPushButton::clicked, this, [this, message]() { editChatMessage(message.id); });
                actionLayout->addWidget(btnEdit);
            } else if (message.role == "assistant") {
                QPushButton *btnRegen = makeActionButton("重新生成", "基于上文重新生成这条回复");
                btnRegen->setEnabled(!busy);
                connect(btnRegen, &QPushButton::clicked, this, [this, message]() { regenerateChatFromMessage(message.id); });
                actionLayout->addWidget(btnRegen);
            }

            QPushButton *btnDelete = makeActionButton("删除", "删除这条消息");
            btnDelete->setEnabled(!busy);
            connect(btnDelete, &QPushButton::clicked, this, [this, message]() { deleteChatMessage(message.id); });
            actionLayout->addWidget(btnDelete);
        }

        bubbleLayout->addLayout(actionLayout);

        if (isUser) {
            rowLayout->addStretch(1);
            rowLayout->addWidget(bubble, 0, Qt::AlignRight);
        } else {
            rowLayout->addWidget(bubble, 0, Qt::AlignLeft);
            rowLayout->addStretch(1);
        }

        rowWidget->setMinimumWidth(rowWidth);
        const int rowHeight = bubble->sizeHint().height() + rowLayout->contentsMargins().top() + rowLayout->contentsMargins().bottom();
        rowWidget->setFixedHeight(qMax(28, rowHeight));
        item->setSizeHint(QSize(rowWidth, rowWidget->height()));
        ui->listChatMessages->addItem(item);
        ui->listChatMessages->setItemWidget(item, rowWidget);
    };

    for (const ChatMessage &message : session->messages) {
        appendBubbleRow(message, false);
    }
    if (session->id == m_pendingChatSessionId
        && (!m_pendingChatAssistantReply.isEmpty() || !m_pendingChatAssistantThinking.trimmed().isEmpty())) {
        appendBubbleRow(makeChatMessage("assistant", m_pendingChatAssistantReply, m_pendingChatAssistantThinking), true);
    }

    ui->listChatMessages->setUpdatesEnabled(true);
    if (scrollToBottom && ui->listChatMessages->verticalScrollBar()) {
        ui->listChatMessages->verticalScrollBar()->setValue(ui->listChatMessages->verticalScrollBar()->maximum());
        m_chatListAutoScrollEnabled = true;
        m_chatListScrollValue = ui->listChatMessages->verticalScrollBar()->value();
    } else if (ui->listChatMessages->verticalScrollBar()) {
        QScrollBar *bar = ui->listChatMessages->verticalScrollBar();
        bar->setValue(qBound(bar->minimum(), m_chatListScrollValue, bar->maximum()));
        m_chatListAutoScrollEnabled = (bar->maximum() - bar->value()) <= autoScrollThreshold;
        m_chatListScrollValue = bar->value();
    }
    updateChatButtons();
}

void LlmPromptWidget::updateChatButtons()
{
    const bool hasSession = activeChatSession() != nullptr;
    const bool busy = !m_activeChatReply.isNull() || !m_activeGenerateReply.isNull();
    const bool generatingActiveSession = hasSession
                                         && !m_activeGenerateReply.isNull()
                                         && !m_currentGeneratedConversationId.isEmpty()
                                         && m_activeChatSessionId == m_currentGeneratedConversationId;
    const bool chattingActiveSession = hasSession
                                       && !m_activeChatReply.isNull()
                                       && !m_pendingChatSessionId.isEmpty()
                                       && m_activeChatSessionId == m_pendingChatSessionId;
    ui->btnRenameChat->setEnabled(hasSession && !busy);
    ui->btnDeleteChat->setEnabled(hasSession && !busy);
    ui->btnClearChats->setEnabled(!m_chatSessions.isEmpty() && !busy);
    ui->btnChatSend->setEnabled(hasSession && !busy);
    ui->btnChatStop->setEnabled(chattingActiveSession || generatingActiveSession);
    ui->btnChatAddImages->setEnabled(hasSession && !busy);
    ui->btnChatClearImages->setEnabled(hasSession && !m_pendingChatImagePaths.isEmpty() && !busy);
    ui->btnContinueConversation->setEnabled(!m_currentGeneratedConversationId.isEmpty()
                                            && chatSessionIndex(m_currentGeneratedConversationId) >= 0);
}

void LlmPromptWidget::updateChatImageInfoLabel()
{
    if (m_pendingChatImagePaths.isEmpty()) {
        ui->lblChatImageInfo->setText("图片：未选择");
        return;
    }
    QStringList names;
    for (const QString &path : std::as_const(m_pendingChatImagePaths)) {
        names.append(QFileInfo(path).fileName());
        if (names.size() >= 3) break;
    }
    ui->lblChatImageInfo->setText(QString("图片：%1 张（%2%3）")
                                      .arg(m_pendingChatImagePaths.size())
                                      .arg(names.join(", "))
                                      .arg(m_pendingChatImagePaths.size() > 3 ? " ..." : ""));
}

void LlmPromptWidget::updateChatStatus(const QString &text, bool isError)
{
    ui->lblChatStatus->setText(text);
    ui->lblChatStatus->setStyleSheet(isError ? "color:#ff7b7b;" : "color:#8c96a0;");
}

QJsonArray LlmPromptWidget::buildOllamaMessagesPayload(const ChatSession &session) const
{
    QJsonArray messages;
    if (!session.systemPrompt.trimmed().isEmpty()) {
        QJsonObject system;
        system["role"] = "system";
        system["content"] = session.systemPrompt;
        messages.append(system);
    }
    for (const ChatMessage &message : session.messages) {
        QJsonObject obj;
        obj["role"] = message.role;
        obj["content"] = message.content;
        if (!message.imagePaths.isEmpty()) {
            QJsonArray images;
            for (const QString &path : message.imagePaths) {
                QFile file(path);
                if (!file.exists() || !file.open(QIODevice::ReadOnly)) continue;
                images.append(QString::fromLatin1(file.readAll().toBase64()));
            }
            if (!images.isEmpty()) obj["images"] = images;
        }
        messages.append(obj);
    }
    return messages;
}

QJsonArray LlmPromptWidget::buildLmStudioMessagesPayload(const ChatSession &session) const
{
    QJsonArray messages;
    if (!session.systemPrompt.trimmed().isEmpty()) {
        QJsonObject system;
        system["role"] = "system";
        system["content"] = session.systemPrompt;
        messages.append(system);
    }
    for (const ChatMessage &message : session.messages) {
        QJsonObject obj;
        obj["role"] = message.role;
        if (message.imagePaths.isEmpty()) {
            obj["content"] = message.content;
        } else {
            QJsonArray content;
            QJsonObject textPart;
            textPart["type"] = "text";
            textPart["text"] = message.content;
            content.append(textPart);
            for (const QString &path : message.imagePaths) {
                QFile file(path);
                if (!file.exists() || !file.open(QIODevice::ReadOnly)) continue;
                QJsonObject imageUrl;
                imageUrl["url"] = QString("data:%1;base64,%2").arg(imageMimeType(path), QString::fromLatin1(file.readAll().toBase64()));
                QJsonObject imagePart;
                imagePart["type"] = "image_url";
                imagePart["image_url"] = imageUrl;
                content.append(imagePart);
            }
            obj["content"] = content;
        }
        messages.append(obj);
    }
    return messages;
}

QJsonObject LlmPromptWidget::buildChatPayload(const ChatSession &session) const
{
    QJsonObject payload;
    payload["model"] = session.modelName;
    payload["stream"] = true;
    if (session.backend == "lmstudio") {
        payload["messages"] = buildLmStudioMessagesPayload(session);
        for (auto it = session.options.constBegin(); it != session.options.constEnd(); ++it) {
            payload[it.key()] = it.value();
        }
    } else {
        payload["messages"] = buildOllamaMessagesPayload(session);
        payload["think"] = ui->chkChatEnableThinking->isChecked();
        payload["options"] = session.options;
    }
    return payload;
}

void LlmPromptWidget::startChatRequest(bool)
{
    ChatSession *session = activeChatSession();
    if (!session || !m_activeChatReply.isNull()) return;
    if (session->modelName.trimmed().isEmpty() || session->endpoint.trimmed().isEmpty()) {
        updateChatStatus("当前对话缺少模型或 Endpoint", true);
        return;
    }

    m_chatStreamBuffer.clear();
    m_pendingChatSessionId = session->id;
    m_pendingChatAssistantReply.clear();
    m_pendingChatAssistantThinking.clear();
    m_pendingChatThinkingExpanded = false;
    m_pendingChatBodyAutoScrollEnabled = true;
    m_pendingChatThinkingAutoScrollEnabled = true;
    m_chatStreamReportedError.clear();
    updateChatView(false);
    updateChatStatus("正在继续对话...");
    ui->btnChatSend->setEnabled(false);
    ui->btnChatStop->setEnabled(true);

    const QJsonObject payload = buildChatPayload(*session);
    ui->textLastRequest->setPlainText(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Indented)));
    QNetworkRequest request(QUrl(session->endpoint + (session->backend == "lmstudio" ? "/v1/chat/completions" : "/api/chat")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_netManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    m_activeChatReply = reply;

    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        if (m_activeChatReply != reply) return;
        processChatStreamChunk(reply->readAll());
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (m_activeChatReply == reply) {
            const QByteArray tail = reply->readAll();
            if (!tail.isEmpty()) processChatStreamChunk(tail);
        }

        const bool canceled = (reply->error() == QNetworkReply::OperationCanceledError);
        const bool hasNetworkError = (reply->error() != QNetworkReply::NoError);
        QString errorText;
        if (hasNetworkError && !canceled) errorText = reply->errorString();
        if (!m_chatStreamReportedError.trimmed().isEmpty()) errorText = m_chatStreamReportedError.trimmed();

        if (m_activeChatReply == reply) m_activeChatReply = nullptr;
        reply->deleteLater();

        if (!m_chatStreamBuffer.trimmed().isEmpty()) {
            processChatStreamLine(m_chatStreamBuffer.toUtf8());
            m_chatStreamBuffer.clear();
        }

        if (!errorText.isEmpty() || canceled) {
            rollbackInflightChatInput();
            updateChatStatus(!errorText.isEmpty() ? ("继续对话失败: " + errorText) : "已停止继续对话", !errorText.isEmpty());
            updateChatButtons();
            return;
        }

        appendPendingAssistantToActiveChat();
        updateChatStatus("继续对话完成");
        updateChatButtons();
        saveConversations();
    });
}

void LlmPromptWidget::appendPendingAssistantToActiveChat()
{
    const QString completedSessionId = m_pendingChatSessionId.isEmpty() ? m_activeChatSessionId : m_pendingChatSessionId;
    const int sessionIndex = chatSessionIndex(completedSessionId);
    if (sessionIndex < 0) {
        m_pendingChatSessionId.clear();
        return;
    }
    ChatSession *session = &m_chatSessions[sessionIndex];
    if (!m_pendingChatAssistantReply.trimmed().isEmpty() || !m_pendingChatAssistantThinking.trimmed().isEmpty()) {
        ChatMessage assistant = makeChatMessage("assistant", m_pendingChatAssistantReply, m_pendingChatAssistantThinking);
        if (m_pendingChatThinkingExpanded && !assistant.thinking.trimmed().isEmpty()) {
            m_expandedThinkingMessageIds.insert(assistant.id);
        }
        session->messages.append(assistant);
        session->updatedAt = QDateTime::currentDateTime();
    }
    m_pendingChatAssistantReply.clear();
    m_pendingChatAssistantThinking.clear();
    m_pendingChatSessionId.clear();
    m_pendingChatThinkingExpanded = false;
    m_pendingChatBodyAutoScrollEnabled = true;
    m_pendingChatThinkingAutoScrollEnabled = true;
    m_inflightChatUserText.clear();
    m_inflightChatUserImages.clear();
    m_inflightChatUserAppended = false;
    std::sort(m_chatSessions.begin(), m_chatSessions.end(), [](const ChatSession &a, const ChatSession &b) {
        return a.updatedAt > b.updatedAt;
    });
    refreshConversationList();
    if (m_activeChatSessionId == completedSessionId) {
        updateChatView(m_chatListAutoScrollEnabled);
    } else {
        updateChatButtons();
    }
}

void LlmPromptWidget::rollbackInflightChatInput()
{
    const QString rolledBackSessionId = m_pendingChatSessionId;
    const bool rollbackIsVisible = m_activeChatSessionId == rolledBackSessionId;
    if (rollbackIsVisible) {
        if (!m_inflightChatUserText.trimmed().isEmpty()) {
            ui->textChatInput->setPlainText(m_inflightChatUserText);
            ui->textChatInput->moveCursor(QTextCursor::End);
        }
        for (const QString &path : std::as_const(m_inflightChatUserImages)) {
            if (!m_pendingChatImagePaths.contains(path)) m_pendingChatImagePaths.append(path);
        }
    }
    const int sessionIndex = chatSessionIndex(m_pendingChatSessionId);
    ChatSession *session = sessionIndex >= 0 ? &m_chatSessions[sessionIndex] : nullptr;
    if (session && m_inflightChatUserAppended && !session->messages.isEmpty()) {
        const ChatMessage &last = session->messages.last();
        if (last.role == "user" && last.content == m_inflightChatUserText) {
            session->messages.removeLast();
        }
    }
    m_pendingChatAssistantReply.clear();
    m_pendingChatAssistantThinking.clear();
    m_pendingChatSessionId.clear();
    m_pendingChatThinkingExpanded = false;
    m_pendingChatBodyAutoScrollEnabled = true;
    m_pendingChatThinkingAutoScrollEnabled = true;
    m_inflightChatUserText.clear();
    m_inflightChatUserImages.clear();
    m_inflightChatUserAppended = false;
    updateChatImageInfoLabel();
    if (m_activeChatSessionId == rolledBackSessionId) updateChatView(false);
    else updateChatButtons();
    saveConversations();
}

void LlmPromptWidget::processChatStreamChunk(const QByteArray &chunk)
{
    if (chunk.isEmpty()) return;
    m_chatStreamBuffer += QString::fromUtf8(chunk);
    int newlineIndex = m_chatStreamBuffer.indexOf('\n');
    while (newlineIndex >= 0) {
        const QString line = m_chatStreamBuffer.left(newlineIndex).trimmed();
        m_chatStreamBuffer.remove(0, newlineIndex + 1);
        if (!line.isEmpty()) processChatStreamLine(line.toUtf8());
        newlineIndex = m_chatStreamBuffer.indexOf('\n');
    }
}

void LlmPromptWidget::processChatStreamLine(const QByteArray &line)
{
    QByteArray payload = line.trimmed();
    if (payload.isEmpty()) return;
    if (payload.startsWith("data:")) payload = payload.mid(5).trimmed();
    if (payload.isEmpty() || payload == "[DONE]") return;

    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject()) return;
    const QJsonObject root = doc.object();
    QString streamError;
    if (root["error"].isString()) streamError = root["error"].toString().trimmed();
    else if (root["error"].isObject()) streamError = root["error"].toObject()["message"].toString().trimmed();
    if (!streamError.isEmpty()) {
        m_chatStreamReportedError = streamError;
        return;
    }

    QString piece;
    QString thinkingPiece;
    if (root["message"].isObject()) {
        const QJsonObject message = root["message"].toObject();
        piece = message["content"].toString();
        thinkingPiece = message["thinking"].toString();
        if (thinkingPiece.isEmpty()) thinkingPiece = message["reasoning"].toString();
        if (thinkingPiece.isEmpty()) thinkingPiece = message["reasoning_content"].toString();
    }
    if (piece.isEmpty()) piece = root["response"].toString();
    if (piece.isEmpty()) piece = root["content"].toString();
    if (thinkingPiece.isEmpty()) thinkingPiece = root["thinking"].toString();
    if (thinkingPiece.isEmpty()) thinkingPiece = root["reasoning"].toString();
    if (thinkingPiece.isEmpty()) thinkingPiece = root["reasoning_content"].toString();

    if ((piece.isEmpty() || thinkingPiece.isEmpty()) && root["choices"].isArray()) {
        const QJsonArray choices = root["choices"].toArray();
        if (!choices.isEmpty() && choices.first().isObject()) {
            const QJsonObject choice = choices.first().toObject();
            const QJsonObject delta = choice["delta"].toObject();
            const QJsonObject messageObj = choice["message"].toObject();
            if (piece.isEmpty()) piece = delta["content"].toString();
            if (piece.isEmpty()) piece = messageObj["content"].toString();
            if (thinkingPiece.isEmpty()) thinkingPiece = delta["reasoning"].toString();
            if (thinkingPiece.isEmpty()) thinkingPiece = delta["thinking"].toString();
            if (thinkingPiece.isEmpty()) thinkingPiece = delta["reasoning_content"].toString();
            if (thinkingPiece.isEmpty()) thinkingPiece = messageObj["reasoning"].toString();
            if (thinkingPiece.isEmpty()) thinkingPiece = messageObj["thinking"].toString();
            if (thinkingPiece.isEmpty()) thinkingPiece = messageObj["reasoning_content"].toString();
        }
    }

    if (!piece.isEmpty()) appendStreamPiece(m_pendingChatAssistantReply, piece);
    if (!thinkingPiece.isEmpty()) appendStreamPiece(m_pendingChatAssistantThinking, thinkingPiece);
    const bool canRefreshPending = m_activeChatSessionId == m_pendingChatSessionId
                                   && m_chatListAutoScrollEnabled
                                   && m_pendingChatBodyAutoScrollEnabled
                                   && m_pendingChatThinkingAutoScrollEnabled;
    if (canRefreshPending) requestChatRefresh(false);
    else m_chatViewDirty = true;
}

void LlmPromptWidget::loadSettings()
{
    QFile file(settingsPath());
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        QJsonObject root = doc.object();
        const QString backend = root["llm_backend"].toString("ollama").toLower();
        QSignalBlocker backendBlocker(ui->comboBackend);
        ui->comboBackend->setCurrentIndex(backend == "lmstudio" ? 1 : 0);
        ui->editEndpoint->setText(root["llm_endpoint"].toString("http://127.0.0.1:11434"));
        ui->comboModel->setEditText(root["llm_model"].toString());
        ui->spinTemperature->setValue(root["llm_temperature"].toDouble(0.4));
        ui->spinCandidateLimit->setValue(root["llm_candidate_limit"].toInt(12));
        ui->spinPreferenceTopCount->setValue(root["llm_preference_top_count"].toInt(15));
        ui->comboPreferencePromptScope->setCurrentIndex(qBound(0, root["llm_preference_prompt_scope"].toInt(0), 2));
        ui->editCustomOptions->setText(root["llm_custom_options"].toString());
        ui->chkEnableThink->setChecked(root["llm_enable_think"].toBool(false));
        QString savedTaskKey = root["llm_task_type"].toString();
        int savedTaskIndex = 0;
        for (int i = 0; i < ui->comboTaskType->count(); ++i) {
            if (taskKeyForIndex(i) == savedTaskKey) {
                savedTaskIndex = i;
                break;
            }
        }
        ui->comboTaskType->setCurrentIndex(savedTaskIndex);
        ui->chkAutoContext->setChecked(root["llm_auto_context"].toBool(true));
        ui->chkUsePreference->setChecked(root["llm_use_preference"].toBool(true));
        ui->chkUseTriggerWords->setChecked(root["llm_use_trigger_words"].toBool(true));
        ui->chkUsePreviewPrompts->setChecked(root["llm_use_preview_prompts"].toBool(true));
        ui->chkIncludeImagePromptsInContext->setChecked(root["llm_include_image_prompts_in_context"].toBool(true));
        ui->chkSendSelectedImages->setChecked(root["llm_send_selected_images"].toBool(false));
        ui->chkStopModelAfterGenerate->setChecked(root["llm_stop_model_after_generate"].toBool(false));
        ui->chkManualTriggerWords->setChecked(root["llm_manual_trigger_words_enabled"].toBool(false));
        ui->chkManualLoraPrompts->setChecked(root["llm_manual_lora_prompts_enabled"].toBool(false));
        ui->textManualTriggerWords->setPlainText(root["llm_manual_trigger_words"].toString());
        ui->textManualLoraPrompts->setPlainText(root["llm_manual_lora_prompts"].toString());
        ui->textSystemPrompt->setPlainText(root["llm_system_prompt"].toString(
            "You are an expert Stable Diffusion prompt engineer. "
            "Rewrite prompts precisely, preserve useful quality tags, and prefer concise, production-ready outputs."
        ));
        QString legacyPromptTemplate = root["llm_prompt_template"].toString();
        const QStringList taskKeys = {kTaskCharacterReplace, kTaskOutfitReplace, kTaskPreferenceGenerate, kTaskImageAdjust};
        for (const QString &taskKey : taskKeys) {
            const QString keyName = "llm_prompt_template_" + taskKey;
            QString value = root[keyName].toString();
            if (value.isEmpty()) value = legacyPromptTemplate;
            if (value.isEmpty()) value = defaultPromptTemplate(taskKey);
            m_taskPromptTemplates.insert(taskKey, value);

            QString guidance = root["llm_task_guidance_" + taskKey].toString();
            if (guidance.isEmpty()) guidance = defaultTaskGuidance(taskKey);
            m_taskGuidances.insert(taskKey, guidance);

            QString attachmentNote = root["llm_image_attachment_note_" + taskKey].toString();
            if (attachmentNote.isEmpty()) attachmentNote = defaultImageAttachmentNote(taskKey);
            m_taskImageAttachmentNotes.insert(taskKey, attachmentNote);
        }
        loadPromptTemplateForTask(currentTaskKey());
        loadTaskPromptFieldsForTask(currentTaskKey());
        return;
    }

    QSignalBlocker backendBlocker(ui->comboBackend);
    ui->comboBackend->setCurrentIndex(0);
    ui->editEndpoint->setText("http://127.0.0.1:11434");
    ui->spinTemperature->setValue(0.4);
    ui->spinCandidateLimit->setValue(12);
    ui->spinPreferenceTopCount->setValue(15);
    ui->comboPreferencePromptScope->setCurrentIndex(0);
    ui->editCustomOptions->clear();
    ui->chkEnableThink->setChecked(false);
    ui->comboTaskType->setCurrentIndex(0);
    ui->chkAutoContext->setChecked(true);
    ui->chkUsePreference->setChecked(true);
    ui->chkUseTriggerWords->setChecked(true);
    ui->chkUsePreviewPrompts->setChecked(true);
    ui->chkIncludeImagePromptsInContext->setChecked(true);
    ui->chkSendSelectedImages->setChecked(false);
    ui->chkStopModelAfterGenerate->setChecked(false);
    ui->chkManualTriggerWords->setChecked(false);
    ui->chkManualLoraPrompts->setChecked(false);
    ui->textManualTriggerWords->clear();
    ui->textManualLoraPrompts->clear();
    ui->textSystemPrompt->setPlainText(
        "You are an expert Stable Diffusion prompt engineer. "
        "Rewrite prompts precisely, preserve useful quality tags, and prefer concise, production-ready outputs."
    );
    m_taskPromptTemplates.insert(kTaskCharacterReplace, defaultPromptTemplate(kTaskCharacterReplace));
    m_taskPromptTemplates.insert(kTaskOutfitReplace, defaultPromptTemplate(kTaskOutfitReplace));
    m_taskPromptTemplates.insert(kTaskPreferenceGenerate, defaultPromptTemplate(kTaskPreferenceGenerate));
    m_taskPromptTemplates.insert(kTaskImageAdjust, defaultPromptTemplate(kTaskImageAdjust));
    m_taskGuidances.insert(kTaskCharacterReplace, defaultTaskGuidance(kTaskCharacterReplace));
    m_taskGuidances.insert(kTaskOutfitReplace, defaultTaskGuidance(kTaskOutfitReplace));
    m_taskGuidances.insert(kTaskPreferenceGenerate, defaultTaskGuidance(kTaskPreferenceGenerate));
    m_taskGuidances.insert(kTaskImageAdjust, defaultTaskGuidance(kTaskImageAdjust));
    m_taskImageAttachmentNotes.insert(kTaskCharacterReplace, defaultImageAttachmentNote(kTaskCharacterReplace));
    m_taskImageAttachmentNotes.insert(kTaskOutfitReplace, defaultImageAttachmentNote(kTaskOutfitReplace));
    m_taskImageAttachmentNotes.insert(kTaskPreferenceGenerate, defaultImageAttachmentNote(kTaskPreferenceGenerate));
    m_taskImageAttachmentNotes.insert(kTaskImageAdjust, defaultImageAttachmentNote(kTaskImageAdjust));
    loadPromptTemplateForTask(currentTaskKey());
    loadTaskPromptFieldsForTask(currentTaskKey());
}

void LlmPromptWidget::saveSettings() const
{
    QFile file(settingsPath());
    QJsonObject root;
    if (file.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
    }

    root["llm_backend"] = currentBackend() == LlmBackend::LmStudio ? "lmstudio" : "ollama";
    root["llm_endpoint"] = ui->editEndpoint->text().trimmed();
    root["llm_model"] = ui->comboModel->currentText().trimmed();
    root["llm_temperature"] = ui->spinTemperature->value();
    root["llm_candidate_limit"] = ui->spinCandidateLimit->value();
    root["llm_preference_top_count"] = ui->spinPreferenceTopCount->value();
    root["llm_preference_prompt_scope"] = ui->comboPreferencePromptScope->currentIndex();
    root["llm_custom_options"] = ui->editCustomOptions->text().trimmed();
    root["llm_enable_think"] = ui->chkEnableThink->isChecked();
    root["llm_task_type"] = currentTaskKey();
    root["llm_auto_context"] = ui->chkAutoContext->isChecked();
    root["llm_use_preference"] = ui->chkUsePreference->isChecked();
    root["llm_use_trigger_words"] = ui->chkUseTriggerWords->isChecked();
    root["llm_use_preview_prompts"] = ui->chkUsePreviewPrompts->isChecked();
    root["llm_include_image_prompts_in_context"] = ui->chkIncludeImagePromptsInContext->isChecked();
    root["llm_send_selected_images"] = ui->chkSendSelectedImages->isChecked();
    root["llm_stop_model_after_generate"] = ui->chkStopModelAfterGenerate->isChecked();
    root["llm_manual_trigger_words_enabled"] = ui->chkManualTriggerWords->isChecked();
    root["llm_manual_lora_prompts_enabled"] = ui->chkManualLoraPrompts->isChecked();
    root["llm_manual_trigger_words"] = ui->textManualTriggerWords->toPlainText();
    root["llm_manual_lora_prompts"] = ui->textManualLoraPrompts->toPlainText();
    root["llm_system_prompt"] = ui->textSystemPrompt->toPlainText();
    const QStringList taskKeys = {kTaskCharacterReplace, kTaskOutfitReplace, kTaskPreferenceGenerate, kTaskImageAdjust};
    for (const QString &taskKey : taskKeys) {
        root["llm_prompt_template_" + taskKey] = m_taskPromptTemplates.value(taskKey, defaultPromptTemplate(taskKey));
        root["llm_task_guidance_" + taskKey] = m_taskGuidances.value(taskKey, defaultTaskGuidance(taskKey));
        root["llm_image_attachment_note_" + taskKey] = m_taskImageAttachmentNotes.value(taskKey, defaultImageAttachmentNote(taskKey));
    }

    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson());
    }
}

void LlmPromptWidget::onNewChatClicked()
{
    if (!m_activeChatReply.isNull() || !m_activeGenerateReply.isNull()) return;
    ChatSession session = createChatSession("新对话", "自由对话");
    m_chatSessions.prepend(session);
    selectChatSession(session.id);
    updateChatStatus("已创建新对话");
}

void LlmPromptWidget::onRenameChatClicked()
{
    if (!m_activeChatReply.isNull() || !m_activeGenerateReply.isNull()) return;
    ChatSession *session = activeChatSession();
    if (!session) return;
    bool ok = false;
    QString title = QInputDialog::getText(nullptr, "重命名对话", "对话标题:", QLineEdit::Normal, session->title, &ok).trimmed();
    if (!ok || title.isEmpty()) return;
    session->title = title;
    session->updatedAt = QDateTime::currentDateTime();
    refreshConversationList();
    updateChatView(false);
    saveConversations();
}

void LlmPromptWidget::onDeleteChatClicked()
{
    if (!m_activeChatReply.isNull() || !m_activeGenerateReply.isNull()) return;
    int index = chatSessionIndex(m_activeChatSessionId);
    if (index < 0) return;
    if (QMessageBox::question(nullptr, "删除对话", "确定删除当前对话吗？") != QMessageBox::Yes) return;
    const QString removedId = m_chatSessions[index].id;
    m_chatSessions.removeAt(index);
    if (m_currentGeneratedConversationId == removedId) m_currentGeneratedConversationId.clear();
    if (m_pendingChatSessionId == removedId) {
        m_pendingChatSessionId.clear();
        m_pendingChatAssistantReply.clear();
        m_pendingChatAssistantThinking.clear();
    }
    m_activeChatSessionId = m_chatSessions.isEmpty() ? QString() : m_chatSessions.first().id;
    refreshConversationList();
    updateChatView(false);
    updateChatButtons();
    saveConversations();
}

void LlmPromptWidget::onClearChatsClicked()
{
    if (!m_activeChatReply.isNull() || !m_activeGenerateReply.isNull() || m_chatSessions.isEmpty()) return;
    if (QMessageBox::question(nullptr, "清空历史", "确定清空所有对话历史吗？") != QMessageBox::Yes) return;
    m_chatSessions.clear();
    m_activeChatSessionId.clear();
    m_currentGeneratedConversationId.clear();
    m_pendingChatSessionId.clear();
    m_pendingChatAssistantReply.clear();
    m_pendingChatAssistantThinking.clear();
    refreshConversationList();
    updateChatView(false);
    updateChatButtons();
    saveConversations();
}

void LlmPromptWidget::onChatSearchChanged(const QString &)
{
    refreshConversationList();
}

void LlmPromptWidget::onChatSelectionChanged()
{
    if (m_syncingChatList) return;
    QListWidgetItem *item = ui->listChatHistory->currentItem();
    if (!item) return;
    selectChatSession(item->data(Qt::UserRole).toString(), false);
}

void LlmPromptWidget::onChatSendClicked()
{
    if (!m_activeChatReply.isNull()) return;
    ChatSession *session = activeChatSession();
    if (!session) {
        onNewChatClicked();
        session = activeChatSession();
    }
    if (!session) return;

    const QString userText = ui->textChatInput->toPlainText().trimmed();
    if (userText.isEmpty()) {
        updateChatStatus("请先输入继续修改的要求", true);
        return;
    }

    session->backend = backendKey(currentBackend());
    session->endpoint = endpointBaseUrl();
    session->modelName = ui->comboModel->currentText().trimmed();
    session->systemPrompt = ui->textSystemPrompt->toPlainText();
    session->options = buildGenerationOptions();
    session->updatedAt = QDateTime::currentDateTime();

    m_inflightChatUserText = userText;
    m_inflightChatUserImages = m_pendingChatImagePaths;
    m_inflightChatUserAppended = false;
    session->messages.append(makeChatMessage("user", userText, QString(), m_inflightChatUserImages));
    m_inflightChatUserAppended = true;
    ui->textChatInput->clear();
    m_pendingChatImagePaths.clear();
    updateChatImageInfoLabel();
    refreshConversationList();
    updateChatView(true);
    saveConversations();
    startChatRequest();
}

void LlmPromptWidget::onChatStopClicked()
{
    if (!m_activeGenerateReply.isNull()
        && !m_currentGeneratedConversationId.isEmpty()
        && m_activeChatSessionId == m_currentGeneratedConversationId) {
        ui->btnChatStop->setEnabled(false);
        updateChatStatus("正在停止提示词生成...");
        onStopGenerateClicked();
        return;
    }

    if (m_activeChatReply.isNull() || m_activeChatSessionId != m_pendingChatSessionId) return;
    ui->btnChatStop->setEnabled(false);
    updateChatStatus("正在停止继续对话...");
    m_activeChatReply->abort();
}

void LlmPromptWidget::onChatAddImagesClicked()
{
    QStringList files = QFileDialog::getOpenFileNames(nullptr, "选择参考图片", QString(), "Images (*.png *.jpg *.jpeg *.webp *.bmp)");
    for (const QString &path : files) {
        if (!m_pendingChatImagePaths.contains(path)) m_pendingChatImagePaths.append(path);
    }
    updateChatImageInfoLabel();
    updateChatButtons();
}

void LlmPromptWidget::onChatClearImagesClicked()
{
    m_pendingChatImagePaths.clear();
    updateChatImageInfoLabel();
    updateChatButtons();
}

void LlmPromptWidget::showChatMessageMenu(const QPoint &pos)
{
    if (!m_activeChatReply.isNull() || !m_activeGenerateReply.isNull()) return;
    QListWidgetItem *item = ui->listChatMessages->itemAt(pos);
    if (!item) return;
    const QString messageId = item->data(Qt::UserRole).toString();
    if (messageId.isEmpty()) return;
    showChatMessageMenuForId(messageId, ui->listChatMessages->viewport()->mapToGlobal(pos));
}

void LlmPromptWidget::showChatMessageMenuForId(const QString &messageId, const QPoint &globalPos)
{
    ChatSession *session = activeChatSession();
    if (!session) return;
    const auto it = std::find_if(session->messages.begin(), session->messages.end(), [&](const ChatMessage &message) {
        return message.id == messageId;
    });
    if (it == session->messages.end()) return;

    QMenu menu(this);
    QAction *copyAction = menu.addAction("复制消息");
    QAction *editAction = nullptr;
    QAction *regenerateAction = nullptr;
    const bool busy = !m_activeChatReply.isNull() || !m_activeGenerateReply.isNull();
    if (it->role == "user") editAction = menu.addAction("编辑并重新生成");
    if (it->role == "assistant") regenerateAction = menu.addAction("重新生成回答");
    QAction *deleteAction = menu.addAction("删除消息");
    if (editAction) editAction->setEnabled(!busy);
    if (regenerateAction) regenerateAction->setEnabled(!busy);
    deleteAction->setEnabled(!busy);
    QAction *chosen = menu.exec(globalPos);
    if (!chosen) return;
    if (chosen == copyAction) copyChatMessage(messageId);
    else if (chosen == editAction) editChatMessage(messageId);
    else if (chosen == regenerateAction) regenerateChatFromMessage(messageId);
    else if (chosen == deleteAction) deleteChatMessage(messageId);
}

void LlmPromptWidget::copyChatMessage(const QString &messageId) const
{
    const ChatSession *session = activeChatSession();
    if (!session) return;
    for (const ChatMessage &message : session->messages) {
        if (message.id == messageId) {
            QApplication::clipboard()->setText(message.content);
            return;
        }
    }
}

void LlmPromptWidget::editChatMessage(const QString &messageId)
{
    ChatSession *session = activeChatSession();
    if (!session || !m_activeChatReply.isNull() || !m_activeGenerateReply.isNull()) return;
    for (int i = 0; i < session->messages.size(); ++i) {
        if (session->messages[i].id != messageId || session->messages[i].role != "user") continue;
        QDialog dialog;
        dialog.setWindowTitle("编辑消息");
        dialog.resize(620, 460);
        QVBoxLayout *layout = new QVBoxLayout(&dialog);

        QPlainTextEdit *textEdit = new QPlainTextEdit(&dialog);
        textEdit->setPlainText(session->messages[i].content);
        textEdit->setMinimumHeight(180);
        layout->addWidget(textEdit);

        QLabel *imageLabel = new QLabel("图片", &dialog);
        layout->addWidget(imageLabel);

        QListWidget *imageList = new QListWidget(&dialog);
        imageList->setSelectionMode(QAbstractItemView::ExtendedSelection);
        imageList->setMinimumHeight(110);
        for (const QString &path : std::as_const(session->messages[i].imagePaths)) {
            QListWidgetItem *imageItem = new QListWidgetItem(QFileInfo(path).fileName());
            imageItem->setData(Qt::UserRole, path);
            imageItem->setToolTip(path);
            imageList->addItem(imageItem);
        }
        layout->addWidget(imageList);

        QHBoxLayout *imageButtons = new QHBoxLayout();
        QPushButton *btnAddImage = new QPushButton("添加图片", &dialog);
        QPushButton *btnRemoveImage = new QPushButton("删除选中", &dialog);
        QPushButton *btnClearImages = new QPushButton("清空图片", &dialog);
        imageButtons->addWidget(btnAddImage);
        imageButtons->addWidget(btnRemoveImage);
        imageButtons->addWidget(btnClearImages);
        imageButtons->addStretch(1);
        layout->addLayout(imageButtons);

        QHBoxLayout *buttons = new QHBoxLayout();
        buttons->addStretch(1);
        QPushButton *btnOk = new QPushButton("保存并重新生成", &dialog);
        QPushButton *btnCancel = new QPushButton("取消", &dialog);
        buttons->addWidget(btnOk);
        buttons->addWidget(btnCancel);
        layout->addLayout(buttons);

        connect(btnAddImage, &QPushButton::clicked, &dialog, [&dialog, imageList]() {
            const QStringList files = QFileDialog::getOpenFileNames(nullptr, "选择参考图片", QString(), "Images (*.png *.jpg *.jpeg *.webp *.bmp)");
            for (const QString &path : files) {
                bool exists = false;
                for (int row = 0; row < imageList->count(); ++row) {
                    if (imageList->item(row)->data(Qt::UserRole).toString() == path) {
                        exists = true;
                        break;
                    }
                }
                if (exists) continue;
                QListWidgetItem *imageItem = new QListWidgetItem(QFileInfo(path).fileName());
                imageItem->setData(Qt::UserRole, path);
                imageItem->setToolTip(path);
                imageList->addItem(imageItem);
            }
        });
        connect(btnRemoveImage, &QPushButton::clicked, &dialog, [imageList]() {
            const QList<QListWidgetItem*> selected = imageList->selectedItems();
            for (QListWidgetItem *item : selected) {
                delete imageList->takeItem(imageList->row(item));
            }
        });
        connect(btnClearImages, &QPushButton::clicked, imageList, &QListWidget::clear);
        connect(btnOk, &QPushButton::clicked, &dialog, &QDialog::accept);
        connect(btnCancel, &QPushButton::clicked, &dialog, &QDialog::reject);

        if (dialog.exec() != QDialog::Accepted) return;
        QString text = textEdit->toPlainText().trimmed();
        if (text.isEmpty()) return;
        QStringList imagePaths;
        for (int row = 0; row < imageList->count(); ++row) {
            const QString path = imageList->item(row)->data(Qt::UserRole).toString();
            if (!path.isEmpty()) imagePaths.append(path);
        }

        session->messages[i].content = text;
        session->messages[i].imagePaths = imagePaths;
        session->messages[i].createdAt = QDateTime::currentDateTime();
        while (session->messages.size() > i + 1) session->messages.removeLast();
        session->updatedAt = QDateTime::currentDateTime();
        m_inflightChatUserText.clear();
        m_inflightChatUserImages.clear();
        m_inflightChatUserAppended = false;
        refreshConversationList();
        updateChatView(false);
        saveConversations();
        startChatRequest();
        return;
    }
}

void LlmPromptWidget::regenerateChatFromMessage(const QString &messageId)
{
    ChatSession *session = activeChatSession();
    if (!session || !m_activeChatReply.isNull() || !m_activeGenerateReply.isNull()) return;
    for (int i = 0; i < session->messages.size(); ++i) {
        if (session->messages[i].id != messageId || session->messages[i].role != "assistant") continue;
        while (session->messages.size() > i) session->messages.removeLast();
        session->updatedAt = QDateTime::currentDateTime();
        refreshConversationList();
        updateChatView(false);
        saveConversations();
        startChatRequest(true);
        return;
    }
}

void LlmPromptWidget::deleteChatMessage(const QString &messageId)
{
    ChatSession *session = activeChatSession();
    if (!session || !m_activeChatReply.isNull() || !m_activeGenerateReply.isNull()) return;
    for (int i = 0; i < session->messages.size(); ++i) {
        if (session->messages[i].id != messageId) continue;
        session->messages.removeAt(i);
        session->updatedAt = QDateTime::currentDateTime();
        refreshConversationList();
        updateChatView(false);
        saveConversations();
        return;
    }
}

QStringList LlmPromptWidget::extractKeywords() const
{
    QString raw = ui->editContextKeywords->text().trimmed();
    if (raw.isEmpty()) {
        raw = ui->textInstruction->toPlainText() + "\n" + ui->textSourcePrompt->toPlainText();
    }

    QStringList keywords;
    QSet<QString> seen;
    QRegularExpression re("[\\p{L}\\p{N}_-]{2,}");
    auto it = re.globalMatch(raw);
    while (it.hasNext()) {
        QString token = it.next().captured(0).trimmed();
        QString lower = token.toLower();
        if (lower.size() < 2 || seen.contains(lower)) continue;
        seen.insert(lower);
        keywords.append(token);
    }
    return keywords;
}

QStringList LlmPromptWidget::readInstalledModelsSync(bool *ok, QString *errorText) const
{
    if (ok) *ok = false;
    QStringList result;

    const bool lmStudio = currentBackend() == LlmBackend::LmStudio;
    QNetworkRequest request(QUrl(endpointBaseUrl() + (lmStudio ? "/v1/models" : "/api/tags")));
    QNetworkReply *reply = m_netManager->get(request);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(5000);
    loop.exec();

    if (!reply->isFinished()) {
        reply->abort();
    }

    if (reply->error() != QNetworkReply::NoError) {
        if (errorText) *errorText = reply->errorString();
        reply->deleteLater();
        return result;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (lmStudio) {
        QJsonArray models = doc.object()["data"].toArray();
        for (const QJsonValue &value : models) {
            QString name = value.toObject()["id"].toString();
            if (!name.isEmpty()) result.append(name);
        }
    } else {
        QJsonArray models = doc.object()["models"].toArray();
        for (const QJsonValue &value : models) {
            QString name = value.toObject()["name"].toString();
            if (!name.isEmpty()) result.append(name);
        }
    }

    reply->deleteLater();
    if (ok) *ok = !result.isEmpty();
    return result;
}

QList<LlmPromptWidget::GalleryCacheItem> LlmPromptWidget::loadGalleryCache() const
{
    QList<GalleryCacheItem> items;
    QFile file(qApp->applicationDirPath() + "/config/user_gallery_cache.json");
    if (!file.open(QIODevice::ReadOnly)) return items;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    QJsonObject root = doc.object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        GalleryCacheItem item;
        item.path = it.key();
        QJsonObject obj = it.value().toObject();
        item.prompt = obj["p"].toString();
        item.negativePrompt = obj["np"].toString();
        item.parameters = obj["param"].toString();
        items.append(item);
    }
    return items;
}

QStringList LlmPromptWidget::collectLocalLoraFiles() const
{
    QStringList files;
    QSet<QString> seen;
    for (const QString &root : m_loraPaths) {
        if (root.isEmpty() || !QDir(root).exists()) continue;
        QDirIterator it(root, QStringList() << "*.safetensors" << "*.pt", QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString path = it.next();
            if (seen.contains(path)) continue;
            seen.insert(path);
            files.append(path);
        }
    }
    return files;
}

LlmPromptWidget::LoraMetadataInfo LlmPromptWidget::readLoraMetadata(const QString &filePath) const
{
    LoraMetadataInfo info;
    info.filePath = filePath;
    info.displayName = QFileInfo(filePath).completeBaseName();
    info.insertionTag = QString("<lora:%1:1>").arg(info.displayName);

    QFileInfo fi(filePath);
    QStringList jsonCandidates = {
        fi.dir().filePath(fi.completeBaseName() + ".json"),
        fi.dir().filePath(fi.completeBaseName() + ".metadata.json")
    };

    for (const QString &jsonPath : jsonCandidates) {
        QFile file(jsonPath);
        if (!file.exists() || !file.open(QIODevice::ReadOnly)) continue;

        QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();

        QString modelName = root["model"].toObject()["name"].toString().trimmed();
        QString versionName = root["name"].toString().trimmed();
        if (!modelName.isEmpty()) {
            info.displayName = versionName.isEmpty() ? modelName : modelName + " [" + versionName + "]";
        }

        QString desc = root["description"].toString().trimmed();
        if (!desc.isEmpty()) info.description = desc;

        QJsonArray trainedWords = root["trainedWords"].toArray();
        for (const QJsonValue &value : trainedWords) {
            QString word = value.toString().trimmed();
            if (!word.isEmpty() && !info.triggerWords.contains(word)) {
                info.triggerWords.append(word);
            }
        }

        QJsonArray images = root["images"].toArray();
        for (const QJsonValue &value : images) {
            QString prompt = value.toObject()["meta"].toObject()["prompt"].toString().trimmed();
            if (!prompt.isEmpty() && !info.previewPrompts.contains(prompt)) {
                info.previewPrompts.append(prompt);
            }
            if (info.previewPrompts.size() >= 3) break;
        }
        break;
    }

    return info;
}

QString LlmPromptWidget::cleanTagText(QString text) const
{
    text = text.trimmed();
    static QRegularExpression weightRegex(":[0-9.]+$");
    static QRegularExpression bracketRegex("[\\{\\}\\[\\]\\(\\)]");
    text.remove(weightRegex);
    text.remove(bracketRegex);
    return text.trimmed();
}

QStringList LlmPromptWidget::parsePromptToTags(const QString &prompt) const
{
    QString normalized = prompt;
    normalized.replace("\r\n", ",");
    normalized.replace("\n", ",");
    QStringList parts = normalized.split(",", Qt::SkipEmptyParts);
    QStringList result;
    QSet<QString> seen;
    for (QString part : parts) {
        part = cleanTagText(part);
        if (part.isEmpty()) continue;
        QString lower = part.toLower();
        if (seen.contains(lower)) continue;
        seen.insert(lower);
        result.append(part);
    }
    return result;
}

QStringList LlmPromptWidget::extractLorasFromPrompt(const QString &prompt) const
{
    QStringList result;
    QRegularExpression re("<lora:([^:>]+)");
    auto it = re.globalMatch(prompt);
    while (it.hasNext()) {
        QString name = it.next().captured(1).trimmed();
        if (!name.isEmpty() && !result.contains(name)) {
            result.append(name);
        }
    }
    return result;
}

QStringList LlmPromptWidget::extractLoraTagsWithWeights(const QString &prompt) const
{
    QStringList result;
    QRegularExpression re("<lora:[^>]+>");
    auto it = re.globalMatch(prompt);
    while (it.hasNext()) {
        QString tag = it.next().captured(0).trimmed();
        if (!tag.isEmpty() && !result.contains(tag)) {
            result.append(tag);
        }
    }
    return result;
}

QStringList LlmPromptWidget::splitPromptTokens(const QString &prompt) const
{
    QString normalized = prompt;
    normalized.replace("\r\n", ",");
    normalized.replace("\n", ",");
    QStringList parts = normalized.split(",", Qt::SkipEmptyParts);
    for (QString &part : parts) {
        part = part.trimmed();
    }
    parts.erase(std::remove_if(parts.begin(), parts.end(), [](const QString &s){ return s.trimmed().isEmpty(); }), parts.end());
    return parts;
}

QString LlmPromptWidget::normalizeLooseText(const QString &text) const
{
    QString out = text.toLower().trimmed();
    out.replace(" ", "");
    out.replace("_", "");
    out.replace("-", "");
    out.replace(".", "");
    out.replace("<", "");
    out.replace(">", "");
    out.replace(":", "");
    out.replace(",", "");
    return out;
}

QStringList LlmPromptWidget::parseReplaceInstruction(QString *oldTarget, QString *newTarget) const
{
    QString instruction = ui->textInstruction->toPlainText().trimmed();
    QString oldValue;
    QString newValue;

    QList<QRegularExpression> patterns = {
        QRegularExpression("将\\s*([^，,\\s]+)\\s*换为\\s*([^，,\\s]+)"),
        QRegularExpression("把\\s*([^，,\\s]+)\\s*换成\\s*([^，,\\s]+)"),
        QRegularExpression("replace\\s+([^,\\s]+)\\s+with\\s+([^,\\s]+)", QRegularExpression::CaseInsensitiveOption)
    };

    for (const QRegularExpression &re : patterns) {
        QRegularExpressionMatch match = re.match(instruction);
        if (match.hasMatch()) {
            oldValue = match.captured(1).trimmed();
            newValue = match.captured(2).trimmed();
            break;
        }
    }

    if (oldTarget) *oldTarget = oldValue;
    if (newTarget) *newTarget = newValue;
    QStringList values;
    if (!oldValue.isEmpty()) values.append(oldValue);
    if (!newValue.isEmpty()) values.append(newValue);
    return values;
}

QString LlmPromptWidget::buildConservativeReplacementPrompt() const
{
    QString sourcePrompt = ui->textSourcePrompt->toPlainText().trimmed();
    if (sourcePrompt.isEmpty()) return QString();

    QString oldTarget;
    QString newTarget;
    parseReplaceInstruction(&oldTarget, &newTarget);
    QString oldNorm = normalizeLooseText(oldTarget);

    QStringList tokens = splitPromptTokens(sourcePrompt);
    QStringList selectedTags;
    for (int i = 0; i < ui->listLoraCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listLoraCandidates->item(i);
        if (item->checkState() != Qt::Checked) continue;
        QString baseName = QFileInfo(item->data(Qt::UserRole).toString()).completeBaseName();
        QString baseNorm = normalizeLooseText(baseName);
        if (!oldNorm.isEmpty() && baseNorm.contains(oldNorm)) continue;
        selectedTags.append(QString("<lora:%1:1>").arg(baseName));
    }

    QStringList kept;
    QSet<QString> seen;
    for (QString token : tokens) {
        QString tokenNorm = normalizeLooseText(token);
        bool isLora = token.contains("<lora:", Qt::CaseInsensitive);
        bool shouldDrop = false;

        if (!oldNorm.isEmpty() && tokenNorm.contains(oldNorm)) {
            shouldDrop = true;
        }

        if (!shouldDrop && isLora) {
            QRegularExpression re("<lora:([^:>]+)");
            QRegularExpressionMatch match = re.match(token);
            if (match.hasMatch() && !oldNorm.isEmpty()) {
                QString loraNameNorm = normalizeLooseText(match.captured(1));
                if (loraNameNorm.contains(oldNorm)) {
                    shouldDrop = true;
                }
            }
        }

        if (shouldDrop) continue;
        QString dedup = token.trimmed().toLower();
        if (dedup.isEmpty() || seen.contains(dedup)) continue;
        seen.insert(dedup);
        kept.append(token.trimmed());
    }

    for (const QString &tag : selectedTags) {
        QString key = tag.toLower();
        if (!seen.contains(key)) {
            kept.prepend(tag);
            seen.insert(key);
        }
    }

    if (!newTarget.isEmpty() && ui->comboTaskType->currentIndex() == 0) {
        QString key = newTarget.toLower();
        if (!seen.contains(key)) {
            kept.append(newTarget);
        }
    }

    return kept.join(", ");
}

QString LlmPromptWidget::defaultPromptTemplate(const QString &taskKey) const
{
    if (taskKey == kTaskCharacterReplace) {
        return
            "Task type:\n{task_type}\n\n"
            "User instruction:\n{instruction}\n\n"
            "Source prompt:\n{source_prompt}\n\n"
            "Parsed replace target:\nOld target: {parsed_old_target}\nNew target: {parsed_new_target}\n\n"
            "Conservative baseline rewrite from source prompt:\n{conservative_prompt}\n\n"
            "Available or selected local LoRAs:\n{lora_context}\n\n"
            "Reference images or prompts:\n{image_context}\n\n"
            "Manual trigger words:\n{manual_trigger_words}\n\n"
            "Manual LoRA preview prompts:\n{manual_lora_prompts}\n\n"
            "{image_attachment_note}\n"
            "{task_guidance}\n"
            "Return in Chinese with this exact structure:\n"
            "推荐LoRA:\n"
            "正向提示词:\n"
            "负向提示词:\n"
            "说明:\n";
    }

    if (taskKey == kTaskOutfitReplace) {
        return
            "Task type:\n{task_type}\n\n"
            "User instruction:\n{instruction}\n\n"
            "Source prompt:\n{source_prompt}\n\n"
            "Conservative baseline rewrite from source prompt:\n{conservative_prompt}\n\n"
            "Reference images or prompts:\n{image_context}\n\n"
            "Available or selected local LoRAs:\n{lora_context}\n\n"
            "Manual LoRA preview prompts:\n{manual_lora_prompts}\n\n"
            "{image_attachment_note}\n"
            "{task_guidance}\n"
            "Return in Chinese with this exact structure:\n"
            "推荐LoRA:\n"
            "正向提示词:\n"
            "负向提示词:\n"
            "说明:\n";
    }

    if (taskKey == kTaskImageAdjust) {
        return
            "Task type:\n{task_type}\n\n"
            "User instruction:\n{instruction}\n\n"
            "Current prompt before adjustment:\n{source_prompt}\n\n"
            "Reference images or prompts:\n{image_context}\n\n"
            "Available or selected local LoRAs:\n{lora_context}\n\n"
            "Manual trigger words:\n{manual_trigger_words}\n\n"
            "Manual LoRA preview prompts:\n{manual_lora_prompts}\n\n"
            "User preference summary:\n{preference_summary}\n\n"
            "{image_attachment_note}\n"
            "{task_guidance}\n"
            "Return in Chinese with this exact structure:\n"
            "推荐LoRA:\n"
            "正向提示词:\n"
            "负向提示词:\n"
            "说明:\n";
    }

    return
        "Task type:\n{task_type}\n\n"
        "User instruction:\n{instruction}\n\n"
        "User preference summary:\n{preference_summary}\n\n"
        "Available or selected local LoRAs:\n{lora_context}\n\n"
        "Reference images or prompts:\n{image_context}\n\n"
        "Manual trigger words:\n{manual_trigger_words}\n\n"
        "Manual LoRA preview prompts:\n{manual_lora_prompts}\n\n"
        "{image_attachment_note}\n"
        "{task_guidance}\n"
        "Return in Chinese with this exact structure:\n"
        "推荐LoRA:\n"
        "正向提示词:\n"
        "负向提示词:\n"
        "说明:\n";
}

QString LlmPromptWidget::defaultTaskGuidance(const QString &taskKey) const
{
    if (taskKey == kTaskCharacterReplace) {
        return
            "Rewrite the Stable Diffusion prompt according to the instruction. "
            "This is character replacement. Preserve composition, camera, outfit, lighting, quality tags, and non-character style LoRAs. "
            "Replace only the old character identity and old character LoRA with the new character. "
            "If a selected replacement LoRA exists, add its exact insertion tag into the positive prompt. "
            "Never place any LoRA syntax or LoRA weight fragments into the negative prompt. "
            "Do not drop clothing tags unless explicitly requested.";
    }

    if (taskKey == kTaskOutfitReplace) {
        return
            "Rewrite the Stable Diffusion prompt according to the instruction. "
            "This is outfit replacement. Preserve character identity, pose, scene, lighting, and style tags unless explicitly changed. "
            "Only replace the clothing-related content. "
            "Keep useful LoRAs, and never place LoRA syntax into the negative prompt.";
    }

    if (taskKey == kTaskImageAdjust) {
        return
            "Rewrite the Stable Diffusion prompt according to the instruction. "
            "This is image-based prompt refinement. Compare the current prompt, the user's complaint, and any attached or referenced image prompts. "
            "Identify missing, inaccurate, or underdescribed visual details, then refine the prompt so the next generation better matches the intended image. "
            "Preserve the existing subject, composition, style, and useful quality tags unless the user explicitly asks to change them. "
            "If the user intended a checkered skirt but wrote striped skirt or omitted the skirt description, correct the clothing description explicitly. "
            "Do not invent large scene changes. Prefer precise tag additions or substitutions over full rewrites when possible.";
    }

    return
        "Generate a user-preferred Stable Diffusion prompt based on history and references. "
        "You may choose suitable local LoRAs from the provided list.";
}

QString LlmPromptWidget::defaultImageAttachmentNote(const QString &taskKey) const
{
    if (taskKey == kTaskImageAdjust) {
        return "Selected reference images may be attached in the request. Use them to identify missing clothing, props, textures, and other visual details that should be corrected in the next prompt.";
    }
    return "Selected reference images may be attached in the request. Use them to refine character features, outfit details, and prompt fidelity.";
}

QString LlmPromptWidget::renderPromptTemplate(const QHash<QString, QString> &values) const
{
    QString taskKey = currentTaskKey();
    QString text = m_taskPromptTemplates.value(taskKey).trimmed();
    if (text.isEmpty()) text = defaultPromptTemplate(taskKey);

    for (auto it = values.begin(); it != values.end(); ++it) {
        text.replace("{" + it.key() + "}", it.value());
    }
    return text;
}

QString LlmPromptWidget::preferenceSummary() const
{
    QList<GalleryCacheItem> items = loadGalleryCache();
    if (items.isEmpty()) {
        return "No gallery history cache available.";
    }

    QMap<QString, int> tagCounts;
    QMap<QString, int> loraCounts;
    QMap<QString, int> negativeCounts;

    for (const GalleryCacheItem &item : items) {
        for (const QString &tag : parsePromptToTags(item.prompt)) {
            tagCounts[tag]++;
        }
        for (const QString &lora : extractLorasFromPrompt(item.prompt)) {
            loraCounts[lora]++;
        }
        for (const QString &tag : parsePromptToTags(item.negativePrompt)) {
            negativeCounts[tag]++;
        }
    }

    const int topLimit = ui->spinPreferenceTopCount->value();
    auto topList = [](const QMap<QString, int> &map, int limit) {
        QList<QPair<QString, int>> pairs;
        for (auto it = map.begin(); it != map.end(); ++it) {
            pairs.append(qMakePair(it.key(), it.value()));
        }
        std::sort(pairs.begin(), pairs.end(), [](const auto &a, const auto &b){
            if (a.second == b.second) return a.first < b.first;
            return a.second > b.second;
        });
        QStringList lines;
        for (int i = 0; i < pairs.size() && i < limit; ++i) {
            lines.append(QString("%1 (%2)").arg(pairs[i].first).arg(pairs[i].second));
        }
        return lines;
    };

    const int scopeIndex = ui->comboPreferencePromptScope->currentIndex();
    const bool includePositive = scopeIndex == 0 || scopeIndex == 1;
    const bool includeNegative = scopeIndex == 0 || scopeIndex == 2;
    const QString scopeText = scopeIndex == 1
        ? "positive prompt only"
        : (scopeIndex == 2 ? "negative prompt only" : "positive and negative prompts");

    QString summary;
    summary += QString("History images: %1\n").arg(items.size());
    summary += QString("Top count setting: %1\n").arg(topLimit);
    summary += "Requested prompt scope: " + scopeText + "\n";
    if (includePositive) {
        summary += "Top positive tags: " + topList(tagCounts, topLimit).join(", ") + "\n";
        summary += "Top LoRAs: " + topList(loraCounts, topLimit).join(", ") + "\n";
    }
    if (includeNegative) {
        summary += "Top negative tags: " + topList(negativeCounts, topLimit).join(", ");
    }
    return summary.trimmed();
}

QString LlmPromptWidget::selectedLoraContext() const
{
    QStringList lines;
    for (int i = 0; i < ui->listLoraCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listLoraCandidates->item(i);
        if (item->checkState() != Qt::Checked) continue;
        LoraMetadataInfo meta = readLoraMetadata(item->data(Qt::UserRole).toString());
        QStringList one;
        one.append(QString("Name: %1").arg(meta.displayName));
        // one.append(QString("Path: %1").arg(meta.filePath));
        one.append(QString("Insertion tag: %1").arg(meta.insertionTag));
        if (ui->chkUseTriggerWords->isChecked() && !meta.triggerWords.isEmpty()) {
            one.append("Trigger words: " + meta.triggerWords.join(", "));
        }
        if (ui->chkUsePreviewPrompts->isChecked() && !meta.previewPrompts.isEmpty()) {
            QStringList prompts;
            for (const QString &prompt : meta.previewPrompts) {
                prompts.append(prompt.left(280));
            }
            one.append("Preview prompts: " + prompts.join(" || "));
        }
        if (!meta.description.isEmpty()) {
            one.append("Description: " + meta.description.left(240));
        }
        lines.append(one.join("\n"));
    }
    return lines.join("\n");
}

QString LlmPromptWidget::selectedImageContext() const
{
    QStringList lines;
    const bool includeImagePrompts = ui->chkIncludeImagePromptsInContext->isChecked();
    for (int i = 0; i < ui->listImageCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listImageCandidates->item(i);
        if (item->checkState() != Qt::Checked) continue;

        const QString imagePath = item->data(Qt::UserRole).toString();
        const QString imageName = QFileInfo(imagePath).fileName();
        const QString prompt = item->data(Qt::UserRole + 1).toString().trimmed();

        QStringList one;
        one.append(QString("Image: %1").arg(imageName.isEmpty() ? item->text() : imageName));
        if (includeImagePrompts && !prompt.isEmpty()) {
            one.append(QString("Prompt: %1").arg(prompt));
        }
        lines.append(one.join("\n"));
    }
    return lines.join("\n\n");
}

QStringList LlmPromptWidget::selectedImagePayloads() const
{
    QStringList images;
    if (!ui->chkSendSelectedImages->isChecked()) return images;

    for (int i = 0; i < ui->listImageCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listImageCandidates->item(i);
        if (item->checkState() != Qt::Checked) continue;
        QString path = item->data(Qt::UserRole).toString();
        QFile file(path);
        if (!file.exists() || !file.open(QIODevice::ReadOnly)) continue;
        images.append(QString::fromLatin1(file.readAll().toBase64()));
        if (images.size() >= 4) break;
    }
    return images;
}

QString LlmPromptWidget::buildGenerationPrompt() const
{
    QString taskKey = currentTaskKey();
    QString taskType = ui->comboTaskType->currentText();
    QString instruction = ui->textInstruction->toPlainText().trimmed();
    QString sourcePrompt = ui->textSourcePrompt->toPlainText().trimmed();
    QString preference = ui->chkUsePreference->isChecked() ? ui->textPreferenceSummary->toPlainText().trimmed() : QString();
    QString loraContext = selectedLoraContext();
    QString imageContext = selectedImageContext();
    QString conservativePrompt = buildConservativeReplacementPrompt();
    QString oldTarget;
    QString newTarget;
    parseReplaceInstruction(&oldTarget, &newTarget);
    QString manualTriggerWords = ui->chkManualTriggerWords->isChecked()
        ? ui->textManualTriggerWords->toPlainText().trimmed()
        : QString();
    QString manualLoraPrompts = ui->chkManualLoraPrompts->isChecked()
        ? ui->textManualLoraPrompts->toPlainText().trimmed()
        : QString();

    QString taskGuidance = m_taskGuidances.value(taskKey, defaultTaskGuidance(taskKey)).trimmed();
    QString imageAttachmentNote = ui->chkSendSelectedImages->isChecked()
        ? m_taskImageAttachmentNotes.value(taskKey, defaultImageAttachmentNote(taskKey)).trimmed()
        : QString();

    QHash<QString, QString> values;
    values.insert("task_type", taskType);
    values.insert("instruction", instruction);
    values.insert("source_prompt", sourcePrompt);
    values.insert("parsed_old_target", oldTarget);
    values.insert("parsed_new_target", newTarget);
    values.insert("conservative_prompt", conservativePrompt);
    values.insert("preference_summary", preference);
    values.insert("lora_context", loraContext);
    values.insert("image_context", imageContext);
    values.insert("manual_trigger_words", manualTriggerWords);
    values.insert("manual_lora_prompts", manualLoraPrompts);
    values.insert("task_guidance", taskGuidance);
    values.insert("image_attachment_note", imageAttachmentNote);
    return renderPromptTemplate(values);
}

QJsonValue LlmPromptWidget::parseOptionValue(QString value) const
{
    value = value.trimmed();
    if (value.isEmpty()) return true;

    QString lower = value.toLower();
    if (lower == "true") return true;
    if (lower == "false") return false;
    if (lower == "null") return QJsonValue(QJsonValue::Null);

    bool intOk = false;
    qlonglong intValue = value.toLongLong(&intOk);
    if (intOk) {
        if (intValue >= std::numeric_limits<int>::min() && intValue <= std::numeric_limits<int>::max()) {
            return static_cast<int>(intValue);
        }
        return static_cast<double>(intValue);
    }

    bool doubleOk = false;
    double doubleValue = value.toDouble(&doubleOk);
    if (doubleOk) return doubleValue;

    return value;
}

QJsonObject LlmPromptWidget::buildGenerationOptions() const
{
    QJsonObject options;
    options["temperature"] = ui->spinTemperature->value();

    const QString customText = ui->editCustomOptions->text().trimmed();
    if (customText.isEmpty()) return options;

    const QStringList tokens = QProcess::splitCommand(customText);
    for (int i = 0; i < tokens.size(); ++i) {
        QString token = tokens[i].trimmed();
        if (!token.startsWith('-')) continue;

        QString key = token;
        while (key.startsWith('-')) key.remove(0, 1);
        key = key.trimmed();
        if (key.isEmpty()) continue;
        key.replace('-', '_');

        QString valueText = "true";
        if (i + 1 < tokens.size()) {
            QString nextToken = tokens[i + 1].trimmed();
            bool nextIsNegativeNumber = QRegularExpression("^-[0-9]+(\\.[0-9]+)?$").match(nextToken).hasMatch();
            if (!nextToken.startsWith('-') || nextIsNegativeNumber) {
                valueText = nextToken;
                ++i;
            }
        }
        options.insert(key, parseOptionValue(valueText));
    }

    return options;
}

QJsonObject LlmPromptWidget::buildGenerationPayload(const QString &modelName) const
{
    if (currentBackend() == LlmBackend::LmStudio) {
        return buildLmStudioGenerationPayload(modelName);
    }

    QJsonObject payload;
    payload["model"] = modelName.trimmed();
    payload["prompt"] = m_lastRenderedPrompt.isEmpty() ? buildGenerationPrompt() : m_lastRenderedPrompt;
    payload["system"] = ui->textSystemPrompt->toPlainText();
    payload["stream"] = true;
    payload["think"] = ui->chkEnableThink->isChecked();

    const QStringList imagePayloads = selectedImagePayloads();
    if (!imagePayloads.isEmpty()) {
        QJsonArray images;
        for (const QString &img : imagePayloads) images.append(img);
        payload["images"] = images;
    }

    payload["options"] = buildGenerationOptions();
    return payload;
}

QJsonObject LlmPromptWidget::buildLmStudioGenerationPayload(const QString &modelName) const
{
    QJsonObject payload;
    payload["model"] = modelName.trimmed();
    payload["stream"] = true;

    QJsonArray messages;
    const QString systemPrompt = ui->textSystemPrompt->toPlainText().trimmed();
    if (!systemPrompt.isEmpty()) {
        QJsonObject system;
        system["role"] = "system";
        system["content"] = systemPrompt;
        messages.append(system);
    }

    QJsonObject user;
    user["role"] = "user";
    const QString prompt = m_lastRenderedPrompt.isEmpty() ? buildGenerationPrompt() : m_lastRenderedPrompt;
    const QStringList imagePayloads = selectedImagePayloads();
    if (imagePayloads.isEmpty()) {
        user["content"] = prompt;
    } else {
        QJsonArray content;
        QJsonObject textPart;
        textPart["type"] = "text";
        textPart["text"] = prompt;
        content.append(textPart);
        for (const QString &img : imagePayloads) {
            QJsonObject imageUrl;
            imageUrl["url"] = "data:image/png;base64," + img;
            QJsonObject imagePart;
            imagePart["type"] = "image_url";
            imagePart["image_url"] = imageUrl;
            content.append(imagePart);
        }
        user["content"] = content;
    }
    messages.append(user);
    payload["messages"] = messages;

    const QJsonObject options = buildGenerationOptions();
    for (auto it = options.constBegin(); it != options.constEnd(); ++it) {
        payload[it.key()] = it.value();
    }
    return payload;
}

QString LlmPromptWidget::postProcessGenerationResult(const QString &text) const
{
    QString result = text;
    if (result.trimmed().isEmpty()) return result;

    QString oldTarget;
    QString newTarget;
    parseReplaceInstruction(&oldTarget, &newTarget);
    const QString oldNorm = normalizeLooseText(oldTarget);
    const QString newNorm = normalizeLooseText(newTarget);

    auto normalizedSectionName = [](const QString &line) {
        QString trimmed = line.trimmed();
        trimmed.remove(QRegularExpression("^#+\\s*"));
        trimmed.remove(QRegularExpression("^[-*]+\\s*"));
        trimmed.replace("**", "");
        trimmed.replace("__", "");
        trimmed.replace('`', "");
        trimmed.replace("：", ":");
        QString compact = trimmed;
        compact.remove(' ');
        if (compact.startsWith("推荐LoRA") || compact.startsWith("推荐Lora")) return QString("推荐LoRA");
        if (compact.startsWith("正向提示词") || compact.startsWith("正面提示词")) return QString("正向提示词");
        if (compact.startsWith("负向提示词") || compact.startsWith("负面提示词")) return QString("负向提示词");
        if (compact.startsWith("关键修改说明") || compact.startsWith("修改说明") || compact.startsWith("说明") || compact.startsWith("为什么这样修改")) return QString("说明");
        if (compact.startsWith("风格理由")) return QString("风格理由");
        return QString();
    };

    auto extractBodyAfterHeading = [](const QString &line) {
        QString normalized = line;
        normalized.replace("：", ":");
        int colon = normalized.indexOf(':');
        return colon >= 0 ? normalized.mid(colon + 1).trimmed() : QString();
    };

    auto joinPromptSection = [](const QStringList &lines) {
        QStringList cleaned;
        for (QString line : lines) {
            QString trimmed = line.trimmed();
            if (trimmed.isEmpty()) continue;
            cleaned.append(trimmed);
        }
        return cleaned.join(" ");
    };

    QStringList originalLines = result.split('\n');
    QHash<QString, QStringList> sections;
    QString currentSection;
    for (const QString &line : originalLines) {
        QString section = normalizedSectionName(line);
        if (!section.isEmpty()) {
            currentSection = section;
            QString body = extractBodyAfterHeading(line);
            if (!body.isEmpty()) sections[section].append(body);
        } else if (!currentSection.isEmpty() && !line.trimmed().isEmpty()) {
            sections[currentSection].append(line.trimmed());
        }
    }

    QStringList selectedTags;
    QStringList mandatoryPositiveTags;
    for (int i = 0; i < ui->listLoraCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listLoraCandidates->item(i);
        if (item->checkState() != Qt::Checked) continue;
        QString baseName = QFileInfo(item->data(Qt::UserRole).toString()).completeBaseName();
        QString baseNorm = normalizeLooseText(baseName);
        if (!oldNorm.isEmpty() && baseNorm.contains(oldNorm)) continue;

        QString tag = QString("<lora:%1:1>").arg(baseName);
        selectedTags.append(tag);
        if (!newNorm.isEmpty() && baseNorm.contains(newNorm)) {
            mandatoryPositiveTags.append(tag);
        }
    }

    if (sections.isEmpty()) {
        return result.trimmed();
    }

    QString recoBody = sections.value("推荐LoRA").join("\n").trimmed();
    QString posBody = joinPromptSection(sections.value("正向提示词")).trimmed();
    QString negBody = joinPromptSection(sections.value("负向提示词")).trimmed();
    QString explainBody = sections.value("说明").join("\n").trimmed();
    if (explainBody.isEmpty()) explainBody = sections.value("风格理由").join("\n").trimmed();

    if (recoBody.isEmpty() && !mandatoryPositiveTags.isEmpty()) {
        recoBody = mandatoryPositiveTags.join(", ");
    } else if (recoBody.isEmpty() && !selectedTags.isEmpty()) {
        recoBody = selectedTags.join(", ");
    }

    if (!mandatoryPositiveTags.isEmpty()) {
        for (const QString &tag : mandatoryPositiveTags) {
            if (!posBody.contains(tag, Qt::CaseInsensitive)) {
                posBody = posBody.isEmpty() ? tag : (tag + ", " + posBody);
            }
        }
    }

    if (isReplacementTask(currentTaskKey())) {
        QString baseline = buildConservativeReplacementPrompt();
        int baselineCount = splitPromptTokens(baseline).size();
        int posCount = splitPromptTokens(posBody).size();
        if (!baseline.isEmpty() && (posCount < qMax(6, baselineCount * 2 / 3))) {
            posBody = baseline;
        }
    }

    {
        QStringList tokens = splitPromptTokens(negBody);
        QStringList cleaned;
        QSet<QString> seen;
        for (QString token : tokens) {
            QString trimmed = token.trimmed();
            if (trimmed.contains("<lora:", Qt::CaseInsensitive)) continue;
            if (QRegularExpression("^[^,<>]+:[0-9.]+$").match(trimmed).hasMatch()) continue;
            QString lower = trimmed.toLower();
            if (seen.contains(lower)) continue;
            seen.insert(lower);
            cleaned.append(trimmed);
        }
        negBody = cleaned.join(", ");
    }

    QStringList out;
    out << ("推荐LoRA: " + recoBody);
    out << ("正向提示词: " + posBody);
    out << ("负向提示词: " + negBody);
    out << ("说明: " + explainBody);
    return out.join("\n");
}

void LlmPromptWidget::updateStatus(const QString &text, bool isError)
{
    ui->lblGenerateStatus->setText(text);
    ui->lblGenerateStatus->setStyleSheet(isError ? "color:#ff7b7b;" : "color:#8c96a0;");
}

void LlmPromptWidget::populateModels(const QStringList &models)
{
    QString current = ui->comboModel->currentText().trimmed();
    ui->comboModel->blockSignals(true);
    ui->comboModel->clear();
    ui->comboModel->addItems(models);
    ui->comboModel->setEditText(current);
    ui->comboModel->blockSignals(false);
}

void LlmPromptWidget::appendUniqueImageCandidate(const QString &label, const QString &path, const QString &prompt, bool checked)
{
    for (int i = 0; i < ui->listImageCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listImageCandidates->item(i);
        if (item->data(Qt::UserRole).toString() == path) {
            if (checked) item->setCheckState(Qt::Checked);
            return;
        }
    }

    QListWidgetItem *item = new QListWidgetItem(label);
    item->setData(Qt::UserRole, path);
    item->setData(Qt::UserRole + 1, prompt);
    item->setToolTip(path);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    ui->listImageCandidates->addItem(item);
}

void LlmPromptWidget::onFetchModelsClicked()
{
    const bool lmStudio = currentBackend() == LlmBackend::LmStudio;
    updateStatus(QString("正在读取 %1 模型...").arg(currentBackendName()));
    QNetworkRequest request(QUrl(endpointBaseUrl() + (lmStudio ? "/v1/models" : "/api/tags")));
    QNetworkReply *reply = m_netManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, lmStudio]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            updateStatus(QString("读取 %1 模型失败: %2").arg(currentBackendName(), reply->errorString()), true);
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QStringList names;
        if (lmStudio) {
            QJsonArray models = doc.object()["data"].toArray();
            for (const QJsonValue &value : models) {
                QString name = value.toObject()["id"].toString();
                if (!name.isEmpty()) names.append(name);
            }
        } else {
            QJsonArray models = doc.object()["models"].toArray();
            for (const QJsonValue &value : models) {
                QString name = value.toObject()["name"].toString();
                if (!name.isEmpty()) names.append(name);
            }
        }
        populateModels(names);
        updateStatus(names.isEmpty() ? "未发现可用模型" : QString("已加载 %1 个本地模型").arg(names.size()));
        saveSettings();
    });
}

void LlmPromptWidget::onRefreshCandidatesClicked()
{
    const bool hadPreviousLoraCandidates = (ui->listLoraCandidates->count() > 0);
    const bool hadPreviousImageCandidates = (ui->listImageCandidates->count() > 0);

    QSet<QString> previouslyCheckedLoraPaths;
    for (int i = 0; i < ui->listLoraCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listLoraCandidates->item(i);
        if (!item || item->checkState() != Qt::Checked) continue;
        const QString path = item->data(Qt::UserRole).toString();
        if (!path.isEmpty()) previouslyCheckedLoraPaths.insert(path);
    }

    struct PreviousImageCandidate {
        QString label;
        QString prompt;
    };
    QHash<QString, PreviousImageCandidate> previouslyCheckedImageCandidates;
    for (int i = 0; i < ui->listImageCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listImageCandidates->item(i);
        if (!item || item->checkState() != Qt::Checked) continue;
        const QString path = item->data(Qt::UserRole).toString();
        if (path.isEmpty()) continue;
        previouslyCheckedImageCandidates.insert(path, {item->text(), item->data(Qt::UserRole + 1).toString()});
    }

    ui->listLoraCandidates->clear();
    ui->listImageCandidates->clear();

    QStringList keywords = extractKeywords();
    QString oldTarget;
    QString newTarget;
    parseReplaceInstruction(&oldTarget, &newTarget);
    QString newTargetNorm = normalizeLooseText(newTarget);
    const int limit = ui->spinCandidateLimit->value();

    QList<QPair<int, QString>> loraScores;
    for (const QString &path : collectLocalLoraFiles()) {
        QString baseName = QFileInfo(path).completeBaseName();
        LoraMetadataInfo meta = readLoraMetadata(path);
        int score = 0;
        QString baseNorm = normalizeLooseText(baseName);
        QString displayNorm = normalizeLooseText(meta.displayName);
        if (!newTargetNorm.isEmpty()) {
            if (baseNorm.contains(newTargetNorm) || displayNorm.contains(newTargetNorm)) score += 20;
            for (const QString &word : meta.triggerWords) {
                if (normalizeLooseText(word).contains(newTargetNorm)) score += 10;
            }
        }
        for (const QString &keyword : keywords) {
            if (baseName.contains(keyword, Qt::CaseInsensitive)) score += 2;
            if (path.contains(keyword, Qt::CaseInsensitive)) score += 1;
            for (const QString &word : meta.triggerWords) {
                if (word.contains(keyword, Qt::CaseInsensitive) || keyword.contains(word, Qt::CaseInsensitive)) score += 3;
            }
            for (const QString &prompt : meta.previewPrompts) {
                if (prompt.contains(keyword, Qt::CaseInsensitive)) score += 2;
            }
        }
        if (keywords.isEmpty()) score = 1;
        if (score > 0) loraScores.append(qMakePair(score, path));
    }
    std::sort(loraScores.begin(), loraScores.end(), [](const auto &a, const auto &b){
        if (a.first == b.first) return a.second < b.second;
        return a.first > b.first;
    });

    for (int i = 0; i < loraScores.size() && i < limit; ++i) {
        QString path = loraScores[i].second;
        LoraMetadataInfo meta = readLoraMetadata(path);
        QListWidgetItem *item = new QListWidgetItem(meta.displayName.isEmpty() ? QFileInfo(path).completeBaseName() : meta.displayName);
        item->setData(Qt::UserRole, path);
        QStringList tip;
        tip << path << meta.insertionTag;
        if (!meta.triggerWords.isEmpty()) tip << ("Trigger: " + meta.triggerWords.join(", "));
        if (!meta.previewPrompts.isEmpty()) tip << ("Preview: " + meta.previewPrompts.first().left(200));
        item->setToolTip(tip.join("\n"));
        const bool autoCheck = ui->chkAutoContext->isChecked() && !hadPreviousLoraCandidates && i < 3;
        const bool checked = previouslyCheckedLoraPaths.contains(path) || autoCheck;
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        ui->listLoraCandidates->addItem(item);
    }

    auto listContainsPath = [](QListWidget *list, const QString &path) {
        for (int i = 0; i < list->count(); ++i) {
            QListWidgetItem *item = list->item(i);
            if (item && item->data(Qt::UserRole).toString() == path) return true;
        }
        return false;
    };

    for (const QString &path : std::as_const(previouslyCheckedLoraPaths)) {
        if (path.isEmpty() || listContainsPath(ui->listLoraCandidates, path) || !QFileInfo::exists(path)) continue;
        LoraMetadataInfo meta = readLoraMetadata(path);
        QListWidgetItem *item = new QListWidgetItem(meta.displayName.isEmpty() ? QFileInfo(path).completeBaseName() : meta.displayName);
        item->setData(Qt::UserRole, path);
        QStringList tip;
        tip << path << meta.insertionTag;
        if (!meta.triggerWords.isEmpty()) tip << ("Trigger: " + meta.triggerWords.join(", "));
        if (!meta.previewPrompts.isEmpty()) tip << ("Preview: " + meta.previewPrompts.first().left(200));
        item->setToolTip(tip.join("\n"));
        item->setCheckState(Qt::Checked);
        ui->listLoraCandidates->addItem(item);
    }

    QList<QPair<int, GalleryCacheItem>> imageScores;
    for (const GalleryCacheItem &item : loadGalleryCache()) {
        int score = 0;
        for (const QString &keyword : keywords) {
            if (item.prompt.contains(keyword, Qt::CaseInsensitive)) score += 2;
            if (item.path.contains(keyword, Qt::CaseInsensitive)) score += 1;
        }
        if (keywords.isEmpty()) score = 1;
        if (score > 0) imageScores.append(qMakePair(score, item));
    }
    std::sort(imageScores.begin(), imageScores.end(), [](const auto &a, const auto &b){
        if (a.first == b.first) return a.second.path < b.second.path;
        return a.first > b.first;
    });

    for (int i = 0; i < imageScores.size() && i < limit; ++i) {
        const GalleryCacheItem &item = imageScores[i].second;
        QString label = QFileInfo(item.path).fileName();
        if (!item.prompt.isEmpty()) {
            label += " | " + item.prompt.left(80);
        }
        const bool autoCheck = ui->chkAutoContext->isChecked() && !hadPreviousImageCandidates && i < 3;
        const bool checked = previouslyCheckedImageCandidates.contains(item.path) || autoCheck;
        appendUniqueImageCandidate(label, item.path, item.prompt, checked);
    }

    for (auto it = previouslyCheckedImageCandidates.constBegin(); it != previouslyCheckedImageCandidates.constEnd(); ++it) {
        if (it.key().isEmpty()) continue;
        if (!listContainsPath(ui->listImageCandidates, it.key())) {
            appendUniqueImageCandidate(it.value().label, it.key(), it.value().prompt, true);
        }
    }

    updateContextSelectionSummary();
    updateStatus(QString("已刷新候选上下文: %1 个 LoRA, %2 张图片").arg(ui->listLoraCandidates->count()).arg(ui->listImageCandidates->count()));
}

void LlmPromptWidget::onClearCandidateSelectionsClicked()
{
    const QSignalBlocker blockerLora(ui->listLoraCandidates);
    const QSignalBlocker blockerImage(ui->listImageCandidates);

    for (int i = 0; i < ui->listLoraCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listLoraCandidates->item(i);
        if (item) item->setCheckState(Qt::Unchecked);
    }
    for (int i = 0; i < ui->listImageCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listImageCandidates->item(i);
        if (item) item->setCheckState(Qt::Unchecked);
    }

    updateContextSelectionSummary();
    updateStatus("已取消选择所有候选 LoRA 和图片");
}

void LlmPromptWidget::onAnalyzePreferenceClicked()
{
    QString summary = preferenceSummary();
    ui->textPreferenceSummary->setPlainText(summary);
    updateStatus("已根据历史图库缓存生成人物偏好摘要");
}

void LlmPromptWidget::onGenerateClicked()
{
    if (m_activeGenerateReply) {
        updateStatus("已有生成任务正在进行，请先停止当前任务", true);
        return;
    }

    saveSettings();

    if (ui->textInstruction->toPlainText().trimmed().isEmpty()) {
        QMessageBox::information(nullptr, "提示", "请先输入生成指令。");
        return;
    }

    QString modelName = ui->comboModel->currentText().trimmed();
    if (modelName.isEmpty()) {
        bool ok = false;
        QString errorText;
        QStringList models = readInstalledModelsSync(&ok, &errorText);
        if (!ok || models.isEmpty()) {
            updateStatus(QString("未能自动获取 %1 模型: %2").arg(currentBackendName(), errorText), true);
            QMessageBox::warning(nullptr, "错误", QString("没有可用的 %1 模型，请先启动服务或手动填写模型名。").arg(currentBackendName()));
            return;
        }
        populateModels(models);
        modelName = models.first();
        ui->comboModel->setEditText(modelName);
    }

    m_lastRenderedPrompt = buildGenerationPrompt();
    m_streamBuffer.clear();
    m_streamResponseText.clear();
    m_streamThinkingText.clear();
    m_lastAssistantVisibleText.clear();
    m_activeModelName = modelName;

    QStringList contextLines;
    const QString instruction = ui->textInstruction->toPlainText().trimmed();
    if (!instruction.isEmpty()) contextLines << instruction;
    const QString sourcePrompt = ui->textSourcePrompt->toPlainText().trimmed();
    if (!sourcePrompt.isEmpty()) contextLines << sourcePrompt;
    if (!m_lastRenderedPrompt.trimmed().isEmpty()) contextLines << m_lastRenderedPrompt.trimmed();

    ChatSession generatedSession = createChatSession(makeChatTitle(ui->comboTaskType->currentText(), instruction), ui->comboTaskType->currentText());
    generatedSession.modelName = modelName;
    QStringList generatedImagePaths;
    if (ui->chkSendSelectedImages->isChecked()) {
        for (int i = 0; i < ui->listImageCandidates->count(); ++i) {
            QListWidgetItem *item = ui->listImageCandidates->item(i);
            if (!item || item->checkState() != Qt::Checked) continue;
            const QString path = item->data(Qt::UserRole).toString();
            if (!path.isEmpty()) generatedImagePaths.append(path);
            if (generatedImagePaths.size() >= 4) break;
        }
    }
    generatedSession.messages.append(makeChatMessage("user", contextLines.join("\n\n"), QString(), generatedImagePaths));
    m_chatSessions.prepend(generatedSession);
    m_activeChatSessionId = generatedSession.id;
    m_currentGeneratedConversationId = generatedSession.id;
    m_pendingChatSessionId = generatedSession.id;
    m_pendingChatAssistantReply.clear();
    m_pendingChatAssistantThinking.clear();
    m_pendingChatBodyAutoScrollEnabled = true;
    m_pendingChatThinkingAutoScrollEnabled = true;
    refreshConversationList();
    ui->tabMain->setCurrentWidget(ui->pageChat);
    updateChatView(true);
    saveConversations();

    updateStatus(QString("正在调用 %1 生成提示词...").arg(currentBackendName()));
    updateChatStatus(QString("正在调用 %1 生成提示词...").arg(currentBackendName()));
    ui->textResult->clear();
    ui->textThinking->clear();
    ui->btnToggleThinking->setChecked(false);
    ui->btnToggleThinking->setEnabled(false);
    ui->btnContinueConversation->setEnabled(false);
    ui->btnGenerate->setEnabled(false);
    ui->btnStopGenerate->setEnabled(true);

    const QJsonObject payload = buildGenerationPayload(modelName);
    ui->textLastRequest->setPlainText(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Indented)));

    const bool lmStudio = currentBackend() == LlmBackend::LmStudio;
    QNetworkRequest request(QUrl(endpointBaseUrl() + (lmStudio ? "/v1/chat/completions" : "/api/generate")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_netManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    m_activeGenerateReply = reply;
    updateChatButtons();
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        if (m_activeGenerateReply != reply) return;
        processStreamChunk(reply->readAll());
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, modelName]() {
        if (m_activeGenerateReply == reply) {
            QByteArray tail = reply->readAll();
            if (!tail.isEmpty()) processStreamChunk(tail);
        }

        bool canceled = (reply->error() == QNetworkReply::OperationCanceledError);
        bool hasError = (reply->error() != QNetworkReply::NoError);
        QString errorText = reply->errorString();

        if (m_activeGenerateReply == reply) {
            m_activeGenerateReply = nullptr;
        }
        reply->deleteLater();

        if (hasError && !canceled) {
            ui->btnGenerate->setEnabled(true);
            ui->btnStopGenerate->setEnabled(false);
            ParsedThinkingBlocks parsed = splitThinkingBlocks(m_streamResponseText);
            m_lastAssistantVisibleText = parsed.visibleText;
            ui->textResult->setMarkdown(escapeAnglePromptSyntax(parsed.visibleText));
            ui->textThinking->setPlainText(combineThinkingText(m_streamThinkingText, parsed.thinkingText));
            ui->btnToggleThinking->setEnabled(!ui->textThinking->toPlainText().trimmed().isEmpty());
            ui->btnContinueConversation->setEnabled(!m_lastAssistantVisibleText.trimmed().isEmpty());
            m_pendingChatSessionId = m_currentGeneratedConversationId;
            m_pendingChatAssistantReply = m_lastAssistantVisibleText;
            m_pendingChatAssistantThinking = ui->textThinking->toPlainText();
            if (m_activeChatSessionId == m_pendingChatSessionId) requestChatRefresh(false);
            else m_chatViewDirty = true;
            m_activeModelName.clear();
            updateStatus(QString("%1 调用失败: %2").arg(currentBackendName(), errorText), true);
            updateChatStatus(QString("%1 调用失败: %2").arg(currentBackendName(), errorText), true);
            updateChatButtons();
            return;
        }

        finishStreaming(modelName, canceled);
    });
}

void LlmPromptWidget::onStopGenerateClicked()
{
    if (!m_activeGenerateReply) return;

    updateStatus("正在停止生成...");
    ui->btnStopGenerate->setEnabled(false);
    if (currentBackend() == LlmBackend::Ollama && !m_activeModelName.isEmpty()) {
        QProcess::startDetached("ollama", QStringList() << "stop" << m_activeModelName);
    }
    m_activeGenerateReply->abort();
}

void LlmPromptWidget::onUnloadModelClicked()
{
    QString modelName = ui->comboModel->currentText().trimmed();
    if (modelName.isEmpty()) {
        updateStatus("请先选择或输入要停止的模型", true);
        QMessageBox::information(nullptr, "提示", "请先选择或输入模型名。");
        return;
    }

    if (currentBackend() == LlmBackend::LmStudio) {
        updateStatus("LM Studio 暂不支持通过此按钮卸载模型，请在 LM Studio 中管理模型。", true);
        QMessageBox::information(nullptr, "提示", "LM Studio 的 OpenAI 兼容接口没有等价的 unload/stop 命令，请在 LM Studio 界面中卸载模型。");
        return;
    }

    bool started = QProcess::startDetached("ollama", QStringList() << "stop" << modelName);
    if (started) {
        updateStatus("已请求停止模型: " + modelName);
    } else {
        updateStatus("停止模型失败: " + modelName, true);
        QMessageBox::warning(nullptr, "错误", "无法执行 ollama stop，请检查 Ollama 是否可用。");
    }
}

void LlmPromptWidget::onContinueConversationClicked()
{
    if (m_currentGeneratedConversationId.isEmpty() || chatSessionIndex(m_currentGeneratedConversationId) < 0) {
        QMessageBox::information(nullptr, "提示", "当前还没有可继续修改的模型输出。");
        return;
    }
    selectChatSession(m_currentGeneratedConversationId, true);
}

void LlmPromptWidget::onCopyPromptClicked()
{
    m_lastRenderedPrompt = buildGenerationPrompt();
    QApplication::clipboard()->setText(m_lastRenderedPrompt);

    const QJsonObject payload = buildGenerationPayload(ui->comboModel->currentText().trimmed());
    ui->textLastRequest->setPlainText(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Indented)));
    updateStatus("已复制交给 LLM 的提示词");
}

void LlmPromptWidget::onCopyResultClicked()
{
    QString rawResult = splitThinkingBlocks(m_streamResponseText).visibleText;
    QApplication::clipboard()->setText(rawResult.isEmpty() ? ui->textResult->toMarkdown() : rawResult);
    updateStatus("结果已复制到剪贴板");
}

void LlmPromptWidget::onAddManualImagesClicked()
{
    QStringList files = QFileDialog::getOpenFileNames(nullptr, "选择参考图片", QString(), "Images (*.png *.jpg *.jpeg *.webp)");
    for (const QString &path : files) {
        QImageReader reader(path);
        QString prompt = reader.text("parameters");
        QString label = "[Manual] " + QFileInfo(path).fileName();
        appendUniqueImageCandidate(label, path, prompt, true);
    }
    updateContextSelectionSummary();
    updateStatus("已添加手动参考图片");
}

void LlmPromptWidget::onResetPromptTemplateClicked()
{
    QString taskKey = currentTaskKey();
    QString text = defaultPromptTemplate(taskKey);
    m_taskPromptTemplates.insert(taskKey, text);
    m_taskGuidances.insert(taskKey, defaultTaskGuidance(taskKey));
    m_taskImageAttachmentNotes.insert(taskKey, defaultImageAttachmentNote(taskKey));
    loadPromptTemplateForTask(taskKey);
    loadTaskPromptFieldsForTask(taskKey);
    updateStatus(taskLabelForKey(taskKey) + " 的提示词模板已重置");
}

void LlmPromptWidget::onTaskTypeChanged(int)
{
    if (!m_syncingTaskTypeSelectors) {
        m_syncingTaskTypeSelectors = true;
        ui->comboTemplateTaskType->setCurrentIndex(ui->comboTaskType->currentIndex());
        m_syncingTaskTypeSelectors = false;
    }

    loadPromptTemplateForTask(currentTaskKey());
    loadTaskPromptFieldsForTask(currentTaskKey());

    if (currentTaskKey() == kTaskImageAdjust) {
        ui->textInstruction->setPlaceholderText("例如：这张图想保留整体构图，但把裙子从 striped skirt 改成更明确的 checkered skirt，并补全袜子与袖口细节。");
        ui->textSourcePrompt->setPlaceholderText("输入生成这张图时使用的原始 prompt，便于 AI 找出哪里描述不足或写错。");
    } else if (currentTaskKey() == kTaskPreferenceGenerate) {
        ui->textInstruction->setPlaceholderText("例如：根据我历史常用 tag，生成一条偏向二次元、雨天窗边、细节丰富的人物 prompt。");
        ui->textSourcePrompt->setPlaceholderText("原始提示词可留空；如果有想延续的 prompt，也可以粘贴进来。");
    } else {
        ui->textInstruction->setPlaceholderText("例如：把提示词里的人物 Arisu 换成 Mika，保留教室构图与蓝白色调，并尽量选择合适的 LoRA。");
        ui->textSourcePrompt->setPlaceholderText("原始提示词，可为空；如果是人物/服装替换，建议粘贴原 prompt。");
    }
}

void LlmPromptWidget::onPromptTemplateEdited()
{
    persistCurrentPromptTemplate();
}

void LlmPromptWidget::onTaskGuidanceEdited()
{
    persistCurrentTaskPromptFields();
}

void LlmPromptWidget::onImageAttachmentNoteEdited()
{
    persistCurrentTaskPromptFields();
}

void LlmPromptWidget::onTemplateTaskTypeChanged(int index)
{
    if (m_syncingTaskTypeSelectors) return;
    m_syncingTaskTypeSelectors = true;
    ui->comboTaskType->setCurrentIndex(index);
    loadPromptTemplateForTask(taskKeyForIndex(index));
    loadTaskPromptFieldsForTask(taskKeyForIndex(index));
    m_syncingTaskTypeSelectors = false;
}

void LlmPromptWidget::onCandidateItemChanged(QListWidgetItem *)
{
    updateContextSelectionSummary();
}

void LlmPromptWidget::onThinkingToggled(bool checked)
{
    bool hasThinking = !ui->textThinking->toPlainText().trimmed().isEmpty();
    ui->textThinking->setVisible(checked && hasThinking);
    ui->btnToggleThinking->setText(checked ? "隐藏思考内容" : "显示思考内容");
}

void LlmPromptWidget::processStreamChunk(const QByteArray &chunk)
{
    if (chunk.isEmpty()) return;

    m_streamBuffer += QString::fromUtf8(chunk);
    int newlineIndex = m_streamBuffer.indexOf('\n');
    while (newlineIndex >= 0) {
        QString line = m_streamBuffer.left(newlineIndex).trimmed();
        m_streamBuffer.remove(0, newlineIndex + 1);
        if (!line.isEmpty()) processStreamLine(line.toUtf8());
        newlineIndex = m_streamBuffer.indexOf('\n');
    }
}

void LlmPromptWidget::processStreamLine(const QByteArray &line)
{
    QByteArray trimmed = line.trimmed();
    if (trimmed.isEmpty()) return;
    if (trimmed.startsWith("data:")) {
        trimmed = trimmed.mid(5).trimmed();
    }
    if (trimmed.isEmpty() || trimmed == "[DONE]") return;

    QJsonDocument doc = QJsonDocument::fromJson(trimmed);
    if (!doc.isObject()) {
        appendStreamPiece(m_streamResponseText, QString::fromUtf8(trimmed));
    } else {
        QJsonObject root = doc.object();
        if (root["error"].isString()) {
            appendStreamPiece(m_streamResponseText, "\n" + root["error"].toString());
        } else if (root["error"].isObject()) {
            appendStreamPiece(m_streamResponseText, "\n" + root["error"].toObject()["message"].toString());
        }

        QString responsePiece = root["response"].toString();
        if (responsePiece.isEmpty() && root["message"].isObject()) {
            responsePiece = root["message"].toObject()["content"].toString();
        }
        if (responsePiece.isEmpty() && root["choices"].isArray()) {
            const QJsonArray choices = root["choices"].toArray();
            if (!choices.isEmpty() && choices.first().isObject()) {
                const QJsonObject choice = choices.first().toObject();
                const QJsonObject delta = choice["delta"].toObject();
                const QJsonObject message = choice["message"].toObject();
                responsePiece = delta["content"].toString();
                if (responsePiece.isEmpty()) responsePiece = message["content"].toString();
            }
        }
        if (!responsePiece.isEmpty()) {
            appendStreamPiece(m_streamResponseText, responsePiece);
        }

        QString thinkingPiece = root["thinking"].toString();
        if (thinkingPiece.isEmpty()) thinkingPiece = root["reasoning"].toString();
        if (thinkingPiece.isEmpty() && root["message"].isObject()) {
            QJsonObject message = root["message"].toObject();
            if (thinkingPiece.isEmpty()) thinkingPiece = message["thinking"].toString();
            if (thinkingPiece.isEmpty()) thinkingPiece = message["reasoning"].toString();
        }
        if (thinkingPiece.isEmpty() && root["choices"].isArray()) {
            const QJsonArray choices = root["choices"].toArray();
            if (!choices.isEmpty() && choices.first().isObject()) {
                const QJsonObject choice = choices.first().toObject();
                const QJsonObject delta = choice["delta"].toObject();
                const QJsonObject message = choice["message"].toObject();
                thinkingPiece = delta["reasoning"].toString();
                if (thinkingPiece.isEmpty()) thinkingPiece = delta["thinking"].toString();
                if (thinkingPiece.isEmpty()) thinkingPiece = delta["reasoning_content"].toString();
                if (thinkingPiece.isEmpty()) thinkingPiece = message["reasoning"].toString();
                if (thinkingPiece.isEmpty()) thinkingPiece = message["thinking"].toString();
                if (thinkingPiece.isEmpty()) thinkingPiece = message["reasoning_content"].toString();
            }
        }
        if (!thinkingPiece.isEmpty()) {
            appendStreamPiece(m_streamThinkingText, thinkingPiece);
        }
    }

    ParsedThinkingBlocks parsed = splitThinkingBlocks(m_streamResponseText);
    QString combinedThinking = combineThinkingText(m_streamThinkingText, parsed.thinkingText);
    m_lastAssistantVisibleText = parsed.visibleText;
    ui->textResult->setMarkdown(escapeAnglePromptSyntax(parsed.visibleText));
    ui->textThinking->setPlainText(combinedThinking);
    if (!m_currentGeneratedConversationId.isEmpty()) {
        m_pendingChatSessionId = m_currentGeneratedConversationId;
        m_pendingChatAssistantReply = parsed.visibleText;
        m_pendingChatAssistantThinking = combinedThinking;
        const bool canRefreshPending = m_activeChatSessionId == m_pendingChatSessionId
                                       && m_chatListAutoScrollEnabled
                                       && m_pendingChatBodyAutoScrollEnabled
                                       && m_pendingChatThinkingAutoScrollEnabled;
        if (canRefreshPending) requestChatRefresh(false);
        else m_chatViewDirty = true;
    }
    scrollTextViewportToBottom(ui->textResult);
    scrollTextViewportToBottom(ui->textThinking);
    bool hasThinking = !combinedThinking.isEmpty();
    ui->btnToggleThinking->setEnabled(hasThinking);
    ui->btnContinueConversation->setEnabled(!m_lastAssistantVisibleText.trimmed().isEmpty());
    if (hasThinking && ui->btnToggleThinking->isChecked()) {
        ui->textThinking->setVisible(true);
    } else if (!hasThinking) {
        ui->textThinking->setVisible(false);
    }
}

void LlmPromptWidget::finishStreaming(const QString &modelName, bool canceled)
{
    if (!m_streamBuffer.trimmed().isEmpty()) {
        processStreamLine(m_streamBuffer.toUtf8());
        m_streamBuffer.clear();
    }

    ParsedThinkingBlocks parsed = splitThinkingBlocks(m_streamResponseText);
    QString finalThinking = combineThinkingText(m_streamThinkingText, parsed.thinkingText);
    QString finalResponse = parsed.visibleText;
    m_lastAssistantVisibleText = finalResponse;

    ui->btnGenerate->setEnabled(true);
    ui->btnStopGenerate->setEnabled(false);
    ui->textThinking->setPlainText(finalThinking);
    ui->btnToggleThinking->setEnabled(!finalThinking.isEmpty());
    ui->btnContinueConversation->setEnabled(!m_lastAssistantVisibleText.trimmed().isEmpty());
    if (finalThinking.isEmpty()) {
        ui->btnToggleThinking->setChecked(false);
        ui->textThinking->setVisible(false);
    }

    if (!finalResponse.isEmpty()) {
        ui->textResult->setMarkdown(escapeAnglePromptSyntax(finalResponse));
    }
    scrollTextViewportToBottom(ui->textResult);
    scrollTextViewportToBottom(ui->textThinking);

    if (canceled) {
        m_activeModelName.clear();
        const QString canceledSessionId = m_pendingChatSessionId;
        m_pendingChatAssistantReply.clear();
        m_pendingChatAssistantThinking.clear();
        m_pendingChatSessionId.clear();
        if (m_activeChatSessionId == canceledSessionId) requestChatRefresh(false);
        else updateChatButtons();
        updateStatus("已停止生成");
        updateChatStatus("已停止生成");
        updateChatButtons();
        return;
    }

    if (!m_currentGeneratedConversationId.isEmpty()) {
        m_pendingChatSessionId = m_currentGeneratedConversationId;
        m_pendingChatAssistantReply = finalResponse;
        m_pendingChatAssistantThinking = finalThinking;
        appendPendingAssistantToActiveChat();
        saveConversations();
    }

    if (ui->chkStopModelAfterGenerate->isChecked() && currentBackend() == LlmBackend::Ollama) {
        bool started = QProcess::startDetached("ollama", QStringList() << "stop" << modelName);
        m_activeModelName.clear();
        updateStatus(started ? "提示词生成完成，已请求停止模型" : "提示词生成完成，但停止模型失败", !started);
        updateChatStatus(started ? "提示词生成完成，已请求停止模型" : "提示词生成完成，但停止模型失败", !started);
    } else if (ui->chkStopModelAfterGenerate->isChecked()) {
        m_activeModelName.clear();
        updateStatus("提示词生成完成；LM Studio 需在其界面中手动停止或卸载模型");
        updateChatStatus("提示词生成完成；LM Studio 需在其界面中手动停止或卸载模型");
    } else {
        m_activeModelName.clear();
        updateStatus("提示词生成完成");
        updateChatStatus("提示词生成完成");
    }
    updateChatButtons();
}
