#include "continueconversationdialog.h"
#include "ui_continueconversationdialog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
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

    ui->btnStop->setEnabled(false);
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

    if (!m_systemPrompt.trimmed().isEmpty()) {
        m_messages.append({"system", m_systemPrompt.trimmed()});
    }
    if (!contextMessage.trimmed().isEmpty()) {
        m_messages.append({"user", contextMessage.trimmed()});
    }
    if (!assistantReply.trimmed().isEmpty()) {
        m_messages.append({"assistant", assistantReply});
    }

    ui->lblConversationInfo->setText(QString("任务：%1").arg(taskLabel.isEmpty() ? "继续修改" : taskLabel));
    updateConversationView();
}

void ContinueConversationDialog::updateStatus(const QString &text, bool isError)
{
    ui->lblStatus->setText(text);
    ui->lblStatus->setStyleSheet(isError ? "color:#ff7b7b;" : "color:#8c96a0;");
}

void ContinueConversationDialog::updateConversationView()
{
    QStringList parts;
    for (const ChatMessage &message : std::as_const(m_messages)) {
        if (message.role == "system") continue;
        QString content = message.content;
        if (message.role == "assistant") {
            content = escapeAnglePromptSyntax(content);
        }
        parts << QString("### %1\n\n%2").arg(roleLabel(message.role), content);
    }

    if (!m_pendingAssistantReply.isEmpty()) {
        parts << QString("### 助手\n\n%1").arg(escapeAnglePromptSyntax(m_pendingAssistantReply));
    }

    ui->textConversation->setMarkdown(parts.join("\n\n"));
    if (ui->textConversation->verticalScrollBar()) {
        ui->textConversation->verticalScrollBar()->setValue(ui->textConversation->verticalScrollBar()->maximum());
    }
}

QJsonArray ContinueConversationDialog::buildMessagesPayload() const
{
    QJsonArray messages;
    for (const ChatMessage &message : m_messages) {
        QJsonObject obj;
        obj["role"] = message.role;
        obj["content"] = message.content;
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

    m_messages.append({"user", userText});
    m_pendingAssistantReply.clear();
    updateConversationView();
    ui->textUserInput->clear();
    ui->btnSend->setEnabled(false);
    ui->btnStop->setEnabled(true);
    updateStatus("正在继续对话...");

    QJsonObject payload;
    payload["model"] = m_modelName;
    payload["messages"] = buildMessagesPayload();
    payload["stream"] = true;
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
            m_messages.append({"assistant", m_pendingAssistantReply});
            m_pendingAssistantReply.clear();
            updateConversationView();
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
    if (root["message"].isObject()) {
        piece = root["message"].toObject()["content"].toString();
    }
    if (piece.isEmpty()) {
        piece = root["response"].toString();
    }
    if (!piece.isEmpty()) {
        m_pendingAssistantReply += piece;
        updateConversationView();
    }
}
