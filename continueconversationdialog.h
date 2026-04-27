#ifndef CONTINUECONVERSATIONDIALOG_H
#define CONTINUECONVERSATIONDIALOG_H

#include <QDialog>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSet>

namespace Ui {
class ContinueConversationDialog;
}

class QNetworkReply;
class QResizeEvent;
class QShowEvent;
class QTimer;

class ContinueConversationDialog : public QDialog
{
    Q_OBJECT

public:
    enum class LlmBackend {
        Ollama,
        LmStudio
    };

    explicit ContinueConversationDialog(QWidget *parent = nullptr);
    ~ContinueConversationDialog();

    void setConnectionContext(const QString &endpointBaseUrl,
                              const QString &modelName,
                              const QString &systemPrompt,
                              const QJsonObject &options,
                              LlmBackend backend);
    void setInitialConversation(const QString &taskLabel,
                                const QString &contextMessage,
                                const QString &assistantReply);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private slots:
    void onSendClicked();
    void onStopClicked();
    void onAddImagesClicked();
    void onClearImagesClicked();
    void onThinkingToggled(bool checked);

private:
    struct ChatMessage {
        QString role;
        QString content;
        QString thinking;
        QStringList imagePaths;
    };

    Ui::ContinueConversationDialog *ui;
    QNetworkAccessManager *m_netManager;
    QPointer<QNetworkReply> m_activeReply;
    QList<ChatMessage> m_messages;
    QString m_endpointBaseUrl;
    QString m_modelName;
    QString m_systemPrompt;
    QJsonObject m_options;
    LlmBackend m_backend = LlmBackend::Ollama;
    QString m_streamBuffer;
    QString m_pendingAssistantReply;
    QString m_pendingAssistantThinking;
    QStringList m_pendingImagePaths;
    QString m_taskLabel;
    QString m_inflightUserText;
    QStringList m_inflightUserImages;
    bool m_inflightUserAppended = false;
    QString m_streamReportedError;
    QSet<int> m_expandedThinkingMessageIndices;
    bool m_pendingThinkingExpanded = false;
    bool m_conversationRefreshScheduled = false;
    bool m_refreshScrollToBottomPending = false;
    QTimer *m_conversationRefreshTimer = nullptr;

    void updateStatus(const QString &text, bool isError = false);
    void updateConversationView(bool scrollToBottom = true);
    void requestConversationRefresh(bool scrollToBottom = true);
    void updateThinkingView();
    void updateImageInfoLabel();
    QJsonArray buildMessagesPayload() const;
    QJsonArray buildLmStudioMessagesPayload() const;
    static QString markdownToHtml(const QString &markdown);
    void processStreamChunk(const QByteArray &chunk);
    void processStreamLine(const QByteArray &line);
    void rollbackInflightUserInput();
};

#endif // CONTINUECONVERSATIONDIALOG_H
