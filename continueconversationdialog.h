#ifndef CONTINUECONVERSATIONDIALOG_H
#define CONTINUECONVERSATIONDIALOG_H

#include <QDialog>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>

namespace Ui {
class ContinueConversationDialog;
}

class QNetworkReply;

class ContinueConversationDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ContinueConversationDialog(QWidget *parent = nullptr);
    ~ContinueConversationDialog();

    void setConnectionContext(const QString &endpointBaseUrl,
                              const QString &modelName,
                              const QString &systemPrompt,
                              const QJsonObject &options);
    void setInitialConversation(const QString &taskLabel,
                                const QString &contextMessage,
                                const QString &assistantReply);

private slots:
    void onSendClicked();
    void onStopClicked();

private:
    struct ChatMessage {
        QString role;
        QString content;
    };

    Ui::ContinueConversationDialog *ui;
    QNetworkAccessManager *m_netManager;
    QPointer<QNetworkReply> m_activeReply;
    QList<ChatMessage> m_messages;
    QString m_endpointBaseUrl;
    QString m_modelName;
    QString m_systemPrompt;
    QJsonObject m_options;
    QString m_streamBuffer;
    QString m_pendingAssistantReply;
    QString m_taskLabel;

    void updateStatus(const QString &text, bool isError = false);
    void updateConversationView();
    QJsonArray buildMessagesPayload() const;
    void processStreamChunk(const QByteArray &chunk);
    void processStreamLine(const QByteArray &line);
};

#endif // CONTINUECONVERSATIONDIALOG_H
