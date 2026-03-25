#include "continueconversationdialog.h"
#include "ui_continueconversationdialog.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QApplication>
#include <QCursor>
#include <QBrush>
#include <QLabel>
#include <QListWidgetItem>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QFrame>
#include <QHBoxLayout>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
#include <utility>

namespace {
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

QString roleLabel(const QString &role)
{
    if (role == "user") return "用户";
    if (role == "assistant") return "助手";
    return "系统";
}
}

ContinueConversationDialog::ContinueConversationDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::ContinueConversationDialog)
    , m_netManager(new QNetworkAccessManager(this))
{
    ui->setupUi(this);

    connect(ui->btnSend, &QPushButton::clicked, this, &ContinueConversationDialog::onSendClicked);
    connect(ui->btnStop, &QPushButton::clicked, this, &ContinueConversationDialog::onStopClicked);
    connect(ui->btnAddImages, &QPushButton::clicked, this, &ContinueConversationDialog::onAddImagesClicked);
    connect(ui->btnClearImages, &QPushButton::clicked, this, &ContinueConversationDialog::onClearImagesClicked);

    ui->btnStop->setEnabled(false);
    ui->btnToggleThinking->setVisible(false);
    ui->btnToggleThinking->setEnabled(false);
    ui->textThinking->setVisible(false);
    ui->textUserInput->setFixedHeight(88);
    ui->textConversation->setSelectionMode(QAbstractItemView::NoSelection);
    ui->textConversation->setFocusPolicy(Qt::NoFocus);
    ui->textConversation->setAlternatingRowColors(false);
    ui->textConversation->setUniformItemSizes(false);
    ui->textConversation->setSpacing(4);
    ui->textConversation->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    ui->textConversation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->textConversation->setStyleSheet(
        "QListWidget{background:#16191e;border:1px solid #31363d;padding:2px;outline:none;}"
        "QListWidget::item{background:transparent;border:none;margin:0px;padding:0px;}"
        "QListWidget::item:hover{background:transparent;}"
        "QListWidget::item:selected{background:transparent;color:inherit;}"
        "QScrollBar:vertical{background:#12161c;width:4px;margin:2px 0 2px 0;border-radius:5px;}"
        "QScrollBar::handle:vertical{background:#3a4654;min-height:24px;border-radius:5px;}"
        "QScrollBar::handle:vertical:hover{background:#4b5a6b;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0px;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}"
        "QScrollBar:horizontal{background:#12161c;height:4px;margin:0 2px 0 2px;border-radius:5px;}"
        "QScrollBar::handle:horizontal{background:#3a4654;min-width:24px;border-radius:5px;}"
        "QScrollBar::handle:horizontal:hover{background:#4b5a6b;}"
        "QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{width:0px;}"
        "QScrollBar::add-page:horizontal,QScrollBar::sub-page:horizontal{background:transparent;}"
    );
    if (ui->textConversation->verticalScrollBar()) {
        ui->textConversation->verticalScrollBar()->setSingleStep(18);
    }

    m_conversationRefreshTimer = new QTimer(this);
    m_conversationRefreshTimer->setSingleShot(true);
    m_conversationRefreshTimer->setInterval(60);
    connect(m_conversationRefreshTimer, &QTimer::timeout, this, [this]() {
        QWidget *hovered = QApplication::widgetAt(QCursor::pos());
        bool hoveringToggleButton = false;
        for (QWidget *w = hovered; w; w = w->parentWidget()) {
            if (w->property("thinkingToggleButton").toBool()) {
                hoveringToggleButton = true;
                break;
            }
        }
        if (hoveringToggleButton) {
            m_conversationRefreshTimer->start();
            return;
        }

        const bool scrollToBottom = m_refreshScrollToBottomPending;
        m_refreshScrollToBottomPending = false;
        m_conversationRefreshScheduled = false;
        updateConversationView(scrollToBottom);
    });

    updateImageInfoLabel();
}

ContinueConversationDialog::~ContinueConversationDialog()
{
    delete ui;
}

void ContinueConversationDialog::setConnectionContext(const QString &endpointBaseUrl,
                                                      const QString &modelName,
                                                      const QString &systemPrompt,
                                                      const QJsonObject &options)
{
    m_endpointBaseUrl = endpointBaseUrl;
    m_modelName = modelName;
    m_systemPrompt = systemPrompt;
    m_options = options;
    ui->lblModelInfo->setText(QString("模型：%1").arg(modelName.isEmpty() ? "未设置" : modelName));
}

void ContinueConversationDialog::setInitialConversation(const QString &taskLabel,
                                                        const QString &contextMessage,
                                                        const QString &assistantReply)
{
    m_taskLabel = taskLabel;
    m_messages.clear();
    m_pendingAssistantReply.clear();
    m_pendingAssistantThinking.clear();
    m_pendingImagePaths.clear();
    m_inflightUserText.clear();
    m_inflightUserImages.clear();
    m_inflightUserAppended = false;
    m_streamReportedError.clear();
    m_expandedThinkingMessageIndices.clear();
    m_pendingThinkingExpanded = false;

    if (!m_systemPrompt.trimmed().isEmpty()) {
        m_messages.append({"system", m_systemPrompt.trimmed(), QString(), {}});
    }
    if (!contextMessage.trimmed().isEmpty()) {
        m_messages.append({"user", contextMessage.trimmed(), QString(), {}});
    }
    if (!assistantReply.trimmed().isEmpty()) {
        m_messages.append({"assistant", assistantReply, QString(), {}});
    }

    ui->lblConversationInfo->setText(QString("任务：%1").arg(taskLabel.isEmpty() ? "继续修改" : taskLabel));
    if (isVisible()) {
        updateConversationView();
    } else {
        ui->textConversation->clear();
    }
    updateThinkingView();
    updateImageInfoLabel();
}

void ContinueConversationDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    requestConversationRefresh(false);
}

void ContinueConversationDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    updateConversationView(false);
}

void ContinueConversationDialog::updateStatus(const QString &text, bool isError)
{
    ui->lblStatus->setText(text);
    ui->lblStatus->setStyleSheet(isError ? "color:#ff7b7b;" : "color:#8c96a0;");
}

void ContinueConversationDialog::requestConversationRefresh(bool scrollToBottom)
{
    m_refreshScrollToBottomPending = m_refreshScrollToBottomPending || scrollToBottom;
    if (m_conversationRefreshScheduled) return;
    if (!m_conversationRefreshTimer) {
        updateConversationView(m_refreshScrollToBottomPending);
        m_refreshScrollToBottomPending = false;
        return;
    }
    m_conversationRefreshScheduled = true;
    m_conversationRefreshTimer->start();
}

QString ContinueConversationDialog::markdownToHtml(const QString &markdown)
{
    QString normalized = markdown;
    normalized.replace("\r\n", "\n");
    normalized.replace('\r', '\n');

    const QString html = QTextDocumentFragment::fromMarkdown(normalized).toHtml();
    static const QRegularExpression bodyRe("<body[^>]*>(.*)</body>",
                                           QRegularExpression::CaseInsensitiveOption
                                           | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = bodyRe.match(html);
    if (match.hasMatch()) {
        return match.captured(1);
    }
    return html;
}

void ContinueConversationDialog::updateConversationView(bool scrollToBottom)
{
    ui->textConversation->setUpdatesEnabled(false);
    ui->textConversation->clear();

    const int viewportWidth = qMax(320, ui->textConversation->viewport()->width());
    const int maxBubbleWidth = qBound(280, int(viewportWidth * 0.66), qMax(280, viewportWidth - 56));
    const int minBubbleWidth = 150;
    const int bubbleBodyMaxHeight = 300;
    auto appendBubbleRow = [&](int messageIndex,
                               const QString &role,
                               const QString &contentMarkdown,
                               const QString &thinkingMarkdown,
                               const QStringList &imagePaths) {
        QString content = contentMarkdown;
        QStringList imageLines;
        for (const QString &path : imagePaths) {
            imageLines.append(QString("`[图片] %1`").arg(QFileInfo(path).fileName()));
        }
        if (!imageLines.isEmpty()) {
            if (!content.trimmed().isEmpty()) content += "\n\n";
            content += imageLines.join("\n");
        }

        const bool isUser = (role == "user");
        const QString bubbleBg = isUser ? "#27425f" : "#1f2834";
        const QString bubbleBorder = isUser ? "#3f6b95" : "#3a4654";
        const QString bubbleText = isUser ? "#eaf4ff" : "#dcdedf";
        const QString bodyText = content.trimmed();
        const QString bodyHtml = markdownToHtml(content);
        const QString thinkingText = thinkingMarkdown.trimmed();
        const QString thinkingHtml = markdownToHtml(thinkingText);

        QTextDocument bodyWidthDoc;
        bodyWidthDoc.setHtml(bodyHtml);
        const int naturalBodyWidth = int(std::ceil(bodyWidthDoc.idealWidth())) + 28;
        QTextDocument thinkingWidthDoc;
        thinkingWidthDoc.setHtml(thinkingHtml);
        const int naturalThinkingWidth = int(std::ceil(thinkingWidthDoc.idealWidth())) + 40;
        QFontMetrics titleFm(ui->textConversation->font());
        const int naturalTitleWidth = titleFm.horizontalAdvance(roleLabel(role)) + 36;
        const int bubbleWidth = qBound(minBubbleWidth,
                                       qMax(qMax(naturalBodyWidth, naturalThinkingWidth), naturalTitleWidth),
                                       maxBubbleWidth);

        QWidget *rowWidget = new QWidget(ui->textConversation);
        rowWidget->setAttribute(Qt::WA_StyledBackground, true);
        rowWidget->setStyleSheet("background:transparent;");
        QHBoxLayout *rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(6, 4, 6, 4);
        rowLayout->setSpacing(0);
        const int rowWidth = qMax(260, ui->textConversation->viewport()->width() - 8);
        QListWidgetItem *item = new QListWidgetItem();
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        item->setBackground(QBrush(Qt::transparent));

        QFrame *bubble = new QFrame(rowWidget);
        bubble->setStyleSheet(QString("QFrame{background:%1;border:1px solid %2;border-radius:10px;}").arg(bubbleBg, bubbleBorder));
        bubble->setFixedWidth(bubbleWidth);
        bubble->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

        QVBoxLayout *bubbleLayout = new QVBoxLayout(bubble);
        bubbleLayout->setContentsMargins(10, 8, 10, 8);
        bubbleLayout->setSpacing(4);

        QLabel *titleLabel = new QLabel(roleLabel(role), bubble);
        titleLabel->setStyleSheet(QString("font-size:12px; color:%1; opacity:0.85;").arg(bubbleText));
        titleLabel->setTextInteractionFlags(Qt::NoTextInteraction);
        bubbleLayout->addWidget(titleLabel);

        if (!bodyText.isEmpty()) {
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
                "QTextBrowser:focus{outline:none;}"
                "QScrollBar:vertical{background:transparent;width:4px;margin:0px;}"
                "QScrollBar::handle:vertical{background:#5f6f80;min-height:20px;border-radius:2px;}"
                "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0px;}"
                "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}").arg(bubbleText));
            bodyView->setContentsMargins(0, 0, 0, 0);
            bodyView->document()->setDocumentMargin(0);
            bodyView->viewport()->setAutoFillBackground(false);
            bodyView->setHtml(bodyHtml);
            bodyView->document()->setTextWidth(bubbleWidth - 20);
            const int docHeight = qMax(16, int(bodyView->document()->size().height()) + 4);
            const int finalBodyHeight = qMin(docHeight, bubbleBodyMaxHeight);
            bodyView->setFixedHeight(finalBodyHeight);
            if (docHeight > finalBodyHeight) {
                bodyView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
                if (bodyView->verticalScrollBar()) {
                    bodyView->verticalScrollBar()->setSingleStep(16);
                    bodyView->verticalScrollBar()->setValue(bodyView->verticalScrollBar()->maximum());
                }
            }
            bubbleLayout->addWidget(bodyView);
        }

        if (role == "assistant" && !thinkingText.isEmpty()) {
            QPushButton *btnToggleThinking = new QPushButton(bubble);
            btnToggleThinking->setCheckable(false);
            btnToggleThinking->setCursor(Qt::PointingHandCursor);
            btnToggleThinking->setFocusPolicy(Qt::NoFocus);
            btnToggleThinking->setMinimumHeight(24);
            btnToggleThinking->setProperty("thinkingToggleButton", true);
            btnToggleThinking->setStyleSheet(QString(
                "QPushButton{background:#223041;border:1px solid #3a4b60;border-radius:4px;padding:2px 8px;color:%1;}"
                "QPushButton:hover{background:#2d3f56;color:#ffffff;}").arg(bubbleText));

            const bool isPending = (messageIndex < 0);
            const bool expanded = isPending
                                      ? m_pendingThinkingExpanded
                                      : m_expandedThinkingMessageIndices.contains(messageIndex);
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
            thinkingView->setHtml(thinkingHtml);
            thinkingView->document()->setTextWidth(bubbleWidth - 28);
            const int thinkingDocHeight = qMax(24, int(thinkingView->document()->size().height()) + 8);
            const int thinkingViewHeight = qMin(thinkingDocHeight, 220);
            thinkingView->setFixedHeight(thinkingViewHeight);
            if (thinkingDocHeight > thinkingViewHeight && thinkingView->verticalScrollBar()) {
                thinkingView->verticalScrollBar()->setSingleStep(14);
            }
            thinkingView->setVisible(expanded);
            if (expanded && thinkingView->verticalScrollBar()) {
                thinkingView->verticalScrollBar()->setValue(thinkingView->verticalScrollBar()->maximum());
                QTimer::singleShot(0, thinkingView, [thinkingView]() {
                    if (thinkingView->verticalScrollBar()) {
                        thinkingView->verticalScrollBar()->setValue(thinkingView->verticalScrollBar()->maximum());
                    }
                });
            }
            bubbleLayout->addWidget(thinkingView);

            connect(btnToggleThinking, &QPushButton::clicked, this, [this, messageIndex, btnToggleThinking, thinkingView, rowWidget, rowLayout, bubble, item, rowWidth]() {
                const bool checked = !thinkingView->isVisible();
                if (messageIndex < 0) {
                    m_pendingThinkingExpanded = checked;
                } else if (checked) {
                    m_expandedThinkingMessageIndices.insert(messageIndex);
                } else {
                    m_expandedThinkingMessageIndices.remove(messageIndex);
                }
                btnToggleThinking->setText(checked ? "隐藏思考" : "显示思考");
                thinkingView->setVisible(checked);
                if (checked && thinkingView->verticalScrollBar()) {
                    thinkingView->verticalScrollBar()->setValue(thinkingView->verticalScrollBar()->maximum());
                    QTimer::singleShot(0, thinkingView, [thinkingView]() {
                        if (thinkingView->verticalScrollBar()) {
                            thinkingView->verticalScrollBar()->setValue(thinkingView->verticalScrollBar()->maximum());
                        }
                    });
                }
                bubble->adjustSize();
                const int newRowHeight = bubble->sizeHint().height()
                                         + rowLayout->contentsMargins().top()
                                         + rowLayout->contentsMargins().bottom();
                rowWidget->setFixedHeight(qMax(28, newRowHeight));
                item->setSizeHint(QSize(rowWidth, rowWidget->height()));
            });
        }

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
        ui->textConversation->addItem(item);
        ui->textConversation->setItemWidget(item, rowWidget);
    };

    for (int i = 0; i < m_messages.size(); ++i) {
        const ChatMessage &message = m_messages[i];
        if (message.role == "system") continue;
        appendBubbleRow(i,
                        message.role,
                        escapeAnglePromptSyntax(message.content),
                        escapeAnglePromptSyntax(message.thinking),
                        message.imagePaths);
    }

    if (!m_pendingAssistantReply.isEmpty() || !m_pendingAssistantThinking.trimmed().isEmpty()) {
        appendBubbleRow(-1,
                        "assistant",
                        escapeAnglePromptSyntax(m_pendingAssistantReply),
                        escapeAnglePromptSyntax(m_pendingAssistantThinking),
                        {});
    }

    ui->textConversation->setUpdatesEnabled(true);
    if (scrollToBottom && ui->textConversation->verticalScrollBar()) {
        ui->textConversation->verticalScrollBar()->setValue(ui->textConversation->verticalScrollBar()->maximum());
    }
}

void ContinueConversationDialog::updateThinkingView()
{
    if (!ui->btnToggleThinking->isVisible() && !ui->textThinking->isVisible()) {
        return;
    }
    QString thinkingText;
    if (!m_pendingAssistantThinking.trimmed().isEmpty()) {
        thinkingText = m_pendingAssistantThinking;
    } else {
        for (int i = m_messages.size() - 1; i >= 0; --i) {
            if (m_messages[i].role == "assistant" && !m_messages[i].thinking.trimmed().isEmpty()) {
                thinkingText = m_messages[i].thinking;
                break;
            }
        }
    }

    ui->textThinking->setPlainText(thinkingText.trimmed());
    if (ui->textThinking->verticalScrollBar()) {
        ui->textThinking->verticalScrollBar()->setValue(ui->textThinking->verticalScrollBar()->maximum());
    }
    const bool hasThinking = !thinkingText.trimmed().isEmpty();
    ui->btnToggleThinking->setEnabled(hasThinking);
    if (!hasThinking) {
        ui->btnToggleThinking->setChecked(false);
        ui->textThinking->setVisible(false);
        ui->btnToggleThinking->setText("显示思考内容");
    } else if (ui->btnToggleThinking->isChecked()) {
        ui->textThinking->setVisible(true);
        ui->btnToggleThinking->setText("隐藏思考内容");
    } else {
        ui->textThinking->setVisible(false);
        ui->btnToggleThinking->setText("显示思考内容");
    }
}

void ContinueConversationDialog::updateImageInfoLabel()
{
    if (m_pendingImagePaths.isEmpty()) {
        ui->lblImageInfo->setText("图片：未选择");
        return;
    }

    QStringList names;
    for (const QString &path : std::as_const(m_pendingImagePaths)) {
        names.append(QFileInfo(path).fileName());
        if (names.size() >= 3) break;
    }
    QString suffix = m_pendingImagePaths.size() > 3 ? " ..." : "";
    ui->lblImageInfo->setText(QString("图片：%1 张（%2%3）")
                                  .arg(m_pendingImagePaths.size())
                                  .arg(names.join(", "))
                                  .arg(suffix));
}

void ContinueConversationDialog::rollbackInflightUserInput()
{
    if (!m_inflightUserText.trimmed().isEmpty()) {
        ui->textUserInput->setPlainText(m_inflightUserText);
        ui->textUserInput->moveCursor(QTextCursor::End);
    }

    for (const QString &path : std::as_const(m_inflightUserImages)) {
        if (!m_pendingImagePaths.contains(path)) {
            m_pendingImagePaths.append(path);
        }
    }
    updateImageInfoLabel();

    if (m_inflightUserAppended && !m_messages.isEmpty()) {
        const ChatMessage &last = m_messages.last();
        if (last.role == "user" && last.content == m_inflightUserText) {
            m_messages.removeLast();
        }
    }

    m_pendingAssistantReply.clear();
    m_pendingAssistantThinking.clear();
    m_pendingThinkingExpanded = false;
    updateConversationView();
    updateThinkingView();

    m_inflightUserText.clear();
    m_inflightUserImages.clear();
    m_inflightUserAppended = false;
}

QJsonArray ContinueConversationDialog::buildMessagesPayload() const
{
    QJsonArray messages;
    for (const ChatMessage &message : m_messages) {
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

void ContinueConversationDialog::onSendClicked()
{
    if (m_activeReply) return;

    const QString userText = ui->textUserInput->toPlainText().trimmed();
    if (userText.isEmpty()) {
        updateStatus("请先输入继续修改的要求", true);
        return;
    }
    if (m_modelName.trimmed().isEmpty() || m_endpointBaseUrl.trimmed().isEmpty()) {
        updateStatus("当前未配置可用的大模型连接", true);
        return;
    }

    m_streamReportedError.clear();
    m_inflightUserText = userText;
    m_inflightUserImages = m_pendingImagePaths;
    m_inflightUserAppended = false;

    m_messages.append({"user", userText, QString(), m_inflightUserImages});
    m_inflightUserAppended = true;
    m_pendingAssistantReply.clear();
    m_pendingAssistantThinking.clear();
    updateConversationView();
    updateThinkingView();
    ui->textUserInput->clear();
    m_pendingImagePaths.clear();
    updateImageInfoLabel();
    ui->btnSend->setEnabled(false);
    ui->btnStop->setEnabled(true);
    updateStatus("正在继续对话...");

    QJsonObject payload;
    payload["model"] = m_modelName;
    payload["messages"] = buildMessagesPayload();
    payload["stream"] = true;
    payload["think"] = ui->chkEnableThinking->isChecked();
    payload["options"] = m_options;

    QNetworkRequest request(QUrl(m_endpointBaseUrl + "/api/chat"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_netManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    m_activeReply = reply;
    m_streamBuffer.clear();

    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        if (m_activeReply != reply) return;
        processStreamChunk(reply->readAll());
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (m_activeReply == reply) {
            const QByteArray tail = reply->readAll();
            if (!tail.isEmpty()) processStreamChunk(tail);
        }

        const bool canceled = (reply->error() == QNetworkReply::OperationCanceledError);
        const bool hasNetworkError = (reply->error() != QNetworkReply::NoError);
        QString errorText;
        if (hasNetworkError && !canceled) {
            errorText = reply->errorString();
        }
        if (!m_streamReportedError.trimmed().isEmpty()) {
            errorText = m_streamReportedError.trimmed();
        }
        const bool hasError = !errorText.isEmpty();

        if (m_activeReply == reply) m_activeReply = nullptr;
        reply->deleteLater();

        ui->btnSend->setEnabled(true);
        ui->btnStop->setEnabled(false);

        if (!m_streamBuffer.trimmed().isEmpty()) {
            processStreamLine(m_streamBuffer.toUtf8());
            m_streamBuffer.clear();
        }

        if (hasError || canceled) {
            rollbackInflightUserInput();
            if (hasError) {
                updateStatus("继续对话失败: " + errorText, true);
            } else {
                updateStatus("已停止继续对话");
            }
            return;
        }

        if (!m_pendingAssistantReply.trimmed().isEmpty() || !m_pendingAssistantThinking.trimmed().isEmpty()) {
            m_messages.append({"assistant", m_pendingAssistantReply, m_pendingAssistantThinking, {}});
            if (m_pendingThinkingExpanded && !m_pendingAssistantThinking.trimmed().isEmpty()) {
                m_expandedThinkingMessageIndices.insert(m_messages.size() - 1);
            }
        }
        m_pendingAssistantReply.clear();
        m_pendingAssistantThinking.clear();
        m_pendingThinkingExpanded = false;
        m_inflightUserText.clear();
        m_inflightUserImages.clear();
        m_inflightUserAppended = false;
        updateConversationView();
        updateThinkingView();

        updateStatus("继续对话完成");
    });
}

void ContinueConversationDialog::onStopClicked()
{
    if (!m_activeReply) return;
    ui->btnStop->setEnabled(false);
    updateStatus("正在停止继续对话...");
    m_activeReply->abort();
}

void ContinueConversationDialog::onAddImagesClicked()
{
    QStringList files = QFileDialog::getOpenFileNames(this,
                                                      "选择参考图片",
                                                      QString(),
                                                      "Images (*.png *.jpg *.jpeg *.webp *.bmp)");
    if (files.isEmpty()) return;

    for (const QString &path : files) {
        if (!m_pendingImagePaths.contains(path)) {
            m_pendingImagePaths.append(path);
        }
    }
    updateImageInfoLabel();
    updateStatus(QString("已选择 %1 张图片，将随下一条消息发送").arg(m_pendingImagePaths.size()));
}

void ContinueConversationDialog::onClearImagesClicked()
{
    m_pendingImagePaths.clear();
    updateImageInfoLabel();
    updateStatus("已清空待发送图片");
}

void ContinueConversationDialog::onThinkingToggled(bool checked)
{
    const bool hasThinking = !ui->textThinking->toPlainText().trimmed().isEmpty();
    ui->textThinking->setVisible(checked && hasThinking);
    ui->btnToggleThinking->setText(checked ? "隐藏思考内容" : "显示思考内容");
}

void ContinueConversationDialog::processStreamChunk(const QByteArray &chunk)
{
    if (chunk.isEmpty()) return;

    m_streamBuffer += QString::fromUtf8(chunk);
    int newlineIndex = m_streamBuffer.indexOf('\n');
    while (newlineIndex >= 0) {
        const QString line = m_streamBuffer.left(newlineIndex).trimmed();
        m_streamBuffer.remove(0, newlineIndex + 1);
        if (!line.isEmpty()) processStreamLine(line.toUtf8());
        newlineIndex = m_streamBuffer.indexOf('\n');
    }
}

void ContinueConversationDialog::processStreamLine(const QByteArray &line)
{
    QByteArray payload = line.trimmed();
    if (payload.isEmpty()) return;
    if (payload.startsWith("data:")) {
        payload = payload.mid(5).trimmed();
    }
    if (payload.isEmpty() || payload == "[DONE]") return;

    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject()) return;

    const QJsonObject root = doc.object();
    QString streamError;
    if (root["error"].isString()) {
        streamError = root["error"].toString().trimmed();
    } else if (root["error"].isObject()) {
        streamError = root["error"].toObject()["message"].toString().trimmed();
    }
    if (!streamError.isEmpty()) {
        m_streamReportedError = streamError;
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

    if (!piece.isEmpty()) {
        m_pendingAssistantReply += piece;
        requestConversationRefresh();
    }
    if (!thinkingPiece.isEmpty()) {
        m_pendingAssistantThinking += thinkingPiece;
        requestConversationRefresh();
        updateThinkingView();
    }
}
