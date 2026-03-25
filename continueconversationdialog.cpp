#include "continueconversationdialog.h"
#include "ui_continueconversationdialog.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextDocumentFragment>
#include <QToolButton>
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
    connect(ui->btnToggleThinking, &QToolButton::toggled, this, &ContinueConversationDialog::onThinkingToggled);

    ui->btnStop->setEnabled(false);
    ui->btnToggleThinking->setEnabled(false);
    ui->textThinking->setVisible(false);
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
    updateConversationView();
    updateThinkingView();
    updateImageInfoLabel();
}

void ContinueConversationDialog::updateStatus(const QString &text, bool isError)
{
    ui->lblStatus->setText(text);
    ui->lblStatus->setStyleSheet(isError ? "color:#ff7b7b;" : "color:#8c96a0;");
}

QString ContinueConversationDialog::markdownToHtml(const QString &markdown)
{
    QString normalized = markdown;
    normalized.replace("\r\n", "\n");
    normalized.replace('\r', '\n');
    return QTextDocumentFragment::fromMarkdown(normalized).toHtml();
}

void ContinueConversationDialog::updateConversationView()
{
    QStringList bubbles;
    for (const ChatMessage &message : std::as_const(m_messages)) {
        if (message.role == "system") continue;
        QString contentMarkdown = message.content;
        if (message.role == "assistant") {
            contentMarkdown = escapeAnglePromptSyntax(contentMarkdown);
        }

        QStringList imageLines;
        for (const QString &path : message.imagePaths) {
            imageLines.append(QString("`[图片] %1`").arg(QFileInfo(path).fileName()));
        }
        if (!imageLines.isEmpty()) {
            if (!contentMarkdown.trimmed().isEmpty()) contentMarkdown += "\n\n";
            contentMarkdown += imageLines.join("\n");
        }

        const bool isUser = (message.role == "user");
        const QString bubbleClass = isUser ? "user" : "assistant";
        const QString title = QString("<div class='title'>%1</div>").arg(roleLabel(message.role));
        const QString body = QString("<div class='body'>%1</div>").arg(markdownToHtml(contentMarkdown));
        bubbles << QString("<div class='row %1'><div class='bubble %1'>%2%3</div></div>")
                       .arg(bubbleClass, title, body);
    }

    if (!m_pendingAssistantReply.isEmpty()) {
        QString body = QString("<div class='body'>%1</div>").arg(markdownToHtml(escapeAnglePromptSyntax(m_pendingAssistantReply)));
        bubbles << QString("<div class='row assistant'><div class='bubble assistant'><div class='title'>助手</div>%1</div></div>").arg(body);
    }

    const QString html =
        "<html><head><style>"
        "body{background:#16191e;color:#dcdedf;font-family:'Microsoft YaHei UI',sans-serif;}"
        ".row{margin:10px 0;display:flex;}"
        ".row.user{justify-content:flex-end;}"
        ".row.assistant{justify-content:flex-start;}"
        ".bubble{max-width:78%;border-radius:10px;padding:10px 12px;line-height:1.5;border:1px solid #31363d;}"
        ".bubble.user{background:#27425f;color:#eaf4ff;border-color:#3f6b95;}"
        ".bubble.assistant{background:#1f2834;color:#dcdedf;border-color:#3a4654;}"
        ".title{font-size:12px;opacity:0.85;margin-bottom:4px;}"
        ".body{font-size:13px;}"
        ".body pre{background:#11161c;border:1px solid #2e3742;border-radius:6px;padding:8px;white-space:pre-wrap;}"
        ".body code{background:#11161c;border-radius:4px;padding:1px 4px;}"
        "</style></head><body>" + bubbles.join("\n") + "</body></html>";
    ui->textConversation->setHtml(html);
    if (ui->textConversation->verticalScrollBar()) {
        ui->textConversation->verticalScrollBar()->setValue(ui->textConversation->verticalScrollBar()->maximum());
    }
}

void ContinueConversationDialog::updateThinkingView()
{
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

    m_messages.append({"user", userText, QString(), m_pendingImagePaths});
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
        const bool hasError = (reply->error() != QNetworkReply::NoError);
        const QString errorText = reply->errorString();

        if (m_activeReply == reply) m_activeReply = nullptr;
        reply->deleteLater();

        ui->btnSend->setEnabled(true);
        ui->btnStop->setEnabled(false);

        if (!m_streamBuffer.trimmed().isEmpty()) {
            processStreamLine(m_streamBuffer.toUtf8());
            m_streamBuffer.clear();
        }

        if (hasError && !canceled) {
            updateStatus("继续对话失败: " + errorText, true);
            return;
        }

        if (!m_pendingAssistantReply.trimmed().isEmpty()) {
            m_messages.append({"assistant", m_pendingAssistantReply, m_pendingAssistantThinking, {}});
            m_pendingAssistantReply.clear();
            m_pendingAssistantThinking.clear();
            updateConversationView();
            updateThinkingView();
        }

        updateStatus(canceled ? "已停止继续对话" : "继续对话完成");
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
    const QByteArray trimmed = line.trimmed();
    if (trimmed.isEmpty()) return;

    const QJsonDocument doc = QJsonDocument::fromJson(trimmed);
    if (!doc.isObject()) return;

    const QJsonObject root = doc.object();
    QString piece;
    QString thinkingPiece;
    if (root["message"].isObject()) {
        const QJsonObject message = root["message"].toObject();
        piece = message["content"].toString();
        thinkingPiece = message["thinking"].toString();
        if (thinkingPiece.isEmpty()) thinkingPiece = message["reasoning"].toString();
    }
    if (piece.isEmpty()) {
        piece = root["response"].toString();
    }
    if (thinkingPiece.isEmpty()) {
        thinkingPiece = root["thinking"].toString();
    }
    if (thinkingPiece.isEmpty()) {
        thinkingPiece = root["reasoning"].toString();
    }
    if (!piece.isEmpty()) {
        m_pendingAssistantReply += piece;
        updateConversationView();
    }
    if (!thinkingPiece.isEmpty()) {
        m_pendingAssistantThinking += thinkingPiece;
        updateThinkingView();
    }
}
