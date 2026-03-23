#ifndef LLMPROMPTWIDGET_H
#define LLMPROMPTWIDGET_H

#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QWidget>
#include <QHash>

namespace Ui {
class LlmPromptWidget;
}

class QListWidgetItem;
class QNetworkReply;

class LlmPromptWidget : public QWidget
{
    Q_OBJECT

public:
    explicit LlmPromptWidget(QWidget *parent = nullptr);
    ~LlmPromptWidget();

    void setLibraryPaths(const QStringList &loraPaths, const QStringList &galleryPaths);

private slots:
    void onFetchModelsClicked();
    void onRefreshCandidatesClicked();
    void onAnalyzePreferenceClicked();
    void onGenerateClicked();
    void onStopGenerateClicked();
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

    Ui::LlmPromptWidget *ui;
    QNetworkAccessManager *m_netManager;
    QPointer<QNetworkReply> m_activeGenerateReply;
    QStringList m_loraPaths;
    QStringList m_galleryPaths;
    QString m_streamBuffer;
    QString m_streamResponseText;
    QString m_streamThinkingText;
    QString m_lastRenderedPrompt;
    QString m_activeModelName;
    QHash<QString, QString> m_taskPromptTemplates;
    QHash<QString, QString> m_taskGuidances;
    QHash<QString, QString> m_taskImageAttachmentNotes;
    bool m_syncingPromptTemplateEditor = false;
    bool m_syncingTaskPromptFields = false;
    bool m_syncingTaskTypeSelectors = false;

    void loadSettings();
    void saveSettings() const;
    QString settingsPath() const;
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
    QJsonValue parseOptionValue(QString value) const;
    QString selectedLoraContext() const;
    QString selectedImageContext() const;
    QStringList selectedImagePayloads() const;
    QString postProcessGenerationResult(const QString &text) const;
    void processStreamChunk(const QByteArray &chunk);
    void processStreamLine(const QByteArray &line);
    void finishStreaming(const QString &modelName, bool canceled);
    void updateStatus(const QString &text, bool isError = false);
    void populateModels(const QStringList &models);
    void appendUniqueImageCandidate(const QString &label, const QString &path, const QString &prompt, bool checked);
};

#endif // LLMPROMPTWIDGET_H
