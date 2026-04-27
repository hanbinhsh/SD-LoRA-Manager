#ifndef LLMPROMPTWIDGET_H
#define LLMPROMPTWIDGET_H

#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QWidget>
#include <QHash>
#include <QDateTime>
#include <QSet>

namespace Ui {
class LlmPromptWidget;
}

class QListWidgetItem;
class QNetworkReply;
class QTimer;
class QEvent;

class LlmPromptWidget : public QWidget
{
    Q_OBJECT

public:
    explicit LlmPromptWidget(QWidget *parent = nullptr);
    ~LlmPromptWidget();

    void setLibraryPaths(const QStringList &loraPaths, const QStringList &galleryPaths);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onFetchModelsClicked();
    void onRefreshCandidatesClicked();
    void onClearCandidateSelectionsClicked();
    void onAnalyzePreferenceClicked();
    void onGenerateClicked();
    void onStopGenerateClicked();
    void onUnloadModelClicked();
    void onContinueConversationClicked();
    void onNewChatClicked();
    void onRenameChatClicked();
    void onDeleteChatClicked();
    void onClearChatsClicked();
    void onChatSearchChanged(const QString &text);
    void onChatSelectionChanged();
    void onChatSendClicked();
    void onChatStopClicked();
    void onChatAddImagesClicked();
    void onChatClearImagesClicked();
    void onCopyPromptClicked();
    void onCopyResultClicked();
    void onAddManualImagesClicked();
    void onResetPromptTemplateClicked();
    void onTaskTypeChanged(int index);
    void onTemplateTaskTypeChanged(int index);
    void onPromptTemplateEdited();
    void onTaskGuidanceEdited();
    void onImageAttachmentNoteEdited();
    void onCandidateItemChanged(QListWidgetItem *item);
    void onThinkingToggled(bool checked);

private:
    enum class LlmBackend {
        Ollama,
        LmStudio
    };

    struct GalleryCacheItem {
        QString path;
        QString prompt;
        QString negativePrompt;
        QString parameters;
    };

    struct LoraMetadataInfo {
        QString displayName;
        QString filePath;
        QString insertionTag;
        QStringList triggerWords;
        QStringList previewPrompts;
        QString description;
    };

    struct ChatMessage {
        QString id;
        QString role;
        QString content;
        QString thinking;
        QStringList imagePaths;
        QDateTime createdAt;
    };

    struct ChatSession {
        QString id;
        QString title;
        QString taskLabel;
        QDateTime createdAt;
        QDateTime updatedAt;
        QString backend;
        QString endpoint;
        QString modelName;
        QString systemPrompt;
        QJsonObject options;
        QList<ChatMessage> messages;
    };

    Ui::LlmPromptWidget *ui;
    QNetworkAccessManager *m_netManager;
    QPointer<QNetworkReply> m_activeGenerateReply;
    QPointer<QNetworkReply> m_activeChatReply;
    QStringList m_loraPaths;
    QStringList m_galleryPaths;
    QString m_streamBuffer;
    QString m_streamResponseText;
    QString m_streamThinkingText;
    QString m_lastRenderedPrompt;
    QString m_activeModelName;
    QString m_lastAssistantVisibleText;
    QString m_currentGeneratedConversationId;
    QString m_activeChatSessionId;
    QString m_pendingChatSessionId;
    QList<ChatSession> m_chatSessions;
    QString m_chatStreamBuffer;
    QString m_pendingChatAssistantReply;
    QString m_pendingChatAssistantThinking;
    QStringList m_pendingChatImagePaths;
    QString m_inflightChatUserText;
    QStringList m_inflightChatUserImages;
    bool m_inflightChatUserAppended = false;
    QString m_chatStreamReportedError;
    QSet<QString> m_expandedThinkingMessageIds;
    bool m_pendingChatThinkingExpanded = false;
    bool m_chatRefreshScheduled = false;
    bool m_chatRefreshScrollToBottomPending = false;
    bool m_chatViewDirty = false;
    bool m_destroying = false;
    bool m_chatListAutoScrollEnabled = false;
    bool m_pendingChatBodyAutoScrollEnabled = true;
    bool m_pendingChatThinkingAutoScrollEnabled = true;
    int m_chatListScrollValue = 0;
    QHash<QString, int> m_chatInnerScrollValues;
    QTimer *m_chatRefreshTimer = nullptr;
    bool m_syncingChatList = false;
    QHash<QString, QString> m_taskPromptTemplates;
    QHash<QString, QString> m_taskGuidances;
    QHash<QString, QString> m_taskImageAttachmentNotes;
    bool m_syncingPromptTemplateEditor = false;
    bool m_syncingTaskPromptFields = false;
    bool m_syncingTaskTypeSelectors = false;

    void loadSettings();
    void saveSettings() const;
    QString settingsPath() const;
    QString conversationsPath() const;
    LlmBackend currentBackend() const;
    QString currentBackendName() const;
    QString backendKey(LlmBackend backend) const;
    QString endpointBaseUrl() const;
    QString currentTaskKey() const;
    QString taskKeyForIndex(int index) const;
    QString taskLabelForKey(const QString &taskKey) const;
    bool isReplacementTask(const QString &taskKey) const;
    void loadPromptTemplateForTask(const QString &taskKey);
    void persistCurrentPromptTemplate();
    void loadTaskPromptFieldsForTask(const QString &taskKey);
    void persistCurrentTaskPromptFields();
    void updateContextSelectionSummary();

    QStringList extractKeywords() const;
    QStringList readInstalledModelsSync(bool *ok = nullptr, QString *errorText = nullptr) const;
    QList<GalleryCacheItem> loadGalleryCache() const;
    QStringList collectLocalLoraFiles() const;
    LoraMetadataInfo readLoraMetadata(const QString &filePath) const;
    QString preferenceSummary() const;
    QString cleanTagText(QString text) const;
    QStringList parsePromptToTags(const QString &prompt) const;
    QStringList extractLorasFromPrompt(const QString &prompt) const;
    QStringList extractLoraTagsWithWeights(const QString &prompt) const;
    QStringList splitPromptTokens(const QString &prompt) const;
    QString normalizeLooseText(const QString &text) const;
    QStringList parseReplaceInstruction(QString *oldTarget = nullptr, QString *newTarget = nullptr) const;
    QString buildConservativeReplacementPrompt() const;
    QString defaultPromptTemplate(const QString &taskKey) const;
    QString defaultTaskGuidance(const QString &taskKey) const;
    QString defaultImageAttachmentNote(const QString &taskKey) const;
    QString renderPromptTemplate(const QHash<QString, QString> &values) const;
    QString buildGenerationPrompt() const;
    QJsonObject buildGenerationOptions() const;
    QJsonObject buildGenerationPayload(const QString &modelName) const;
    QJsonObject buildLmStudioGenerationPayload(const QString &modelName) const;
    QJsonObject buildChatPayload(const ChatSession &session) const;
    QJsonValue parseOptionValue(QString value) const;
    QString selectedLoraContext() const;
    QString selectedImageContext() const;
    QStringList selectedImagePayloads() const;
    QString postProcessGenerationResult(const QString &text) const;
    void processStreamChunk(const QByteArray &chunk);
    void processStreamLine(const QByteArray &line);
    void finishStreaming(const QString &modelName, bool canceled);
    void processChatStreamChunk(const QByteArray &chunk);
    void processChatStreamLine(const QByteArray &line);
    void updateStatus(const QString &text, bool isError = false);
    void updateChatStatus(const QString &text, bool isError = false);
    void populateModels(const QStringList &models);
    void appendUniqueImageCandidate(const QString &label, const QString &path, const QString &prompt, bool checked);
    void setupChatUi();
    void loadConversations();
    void saveConversations() const;
    void refreshConversationList();
    void selectChatSession(const QString &sessionId, bool switchToChatTab = true);
    ChatSession *activeChatSession();
    const ChatSession *activeChatSession() const;
    int chatSessionIndex(const QString &sessionId) const;
    ChatSession createChatSession(const QString &title, const QString &taskLabel) const;
    QString makeChatTitle(const QString &taskLabel, const QString &instruction) const;
    ChatMessage makeChatMessage(const QString &role, const QString &content, const QString &thinking = QString(), const QStringList &imagePaths = {}) const;
    void appendPendingAssistantToActiveChat();
    void requestChatRefresh(bool scrollToBottom = true);
    void updateChatView(bool scrollToBottom = true);
    void updateChatButtons();
    void updateChatImageInfoLabel();
    QJsonArray buildOllamaMessagesPayload(const ChatSession &session) const;
    QJsonArray buildLmStudioMessagesPayload(const ChatSession &session) const;
    void startChatRequest(bool fromRegenerate = false);
    void rollbackInflightChatInput();
    void showChatMessageMenu(const QPoint &pos);
    void showChatMessageMenuForId(const QString &messageId, const QPoint &globalPos);
    void copyChatMessage(const QString &messageId) const;
    void editChatMessage(const QString &messageId);
    void regenerateChatFromMessage(const QString &messageId);
    void deleteChatMessage(const QString &messageId);
    QString markdownToHtml(const QString &markdown) const;
    QString imageMimeType(const QString &path) const;
    void openImagePath(const QString &path) const;
    QString formatChatTimestamp(const QDateTime &time) const;
};

#endif // LLMPROMPTWIDGET_H
