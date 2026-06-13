#ifndef DOWNLOADMANAGER_H
#define DOWNLOADMANAGER_H

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QIcon>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QObject>
#include <QUrl>
#include <functional>

#include "downloadmodels.h"

class DownloadsPage;
class QFile;
class QNetworkAccessManager;
class QThreadPool;

class DownloadManager : public QObject
{
    Q_OBJECT

public:
    explicit DownloadManager(DownloadsPage *page,
                             QNetworkAccessManager *network,
                             QThreadPool *previewThreadPool,
                             QObject *parent = nullptr);
    ~DownloadManager() override;

    using MakeRequestCallback = std::function<QNetworkRequest(const QUrl &, bool)>;
    using ReplyErrorCallback = std::function<QString(QNetworkReply *)>;
    using TokenUrlCallback = std::function<QUrl(const QUrl &)>;
    using ApiKeyCallback = std::function<QString()>;
    using HashCallback = std::function<QString(const QString &)>;
    using TargetPathCallback = std::function<QString(const ModelUpdateInfo &, bool *)>;
    using PreviewPathCallback = std::function<QString(const ModelUpdateInfo &)>;

    void setNetworkCallbacks(MakeRequestCallback makeRequest,
                             ReplyErrorCallback replyError,
                             TokenUrlCallback tokenUrl,
                             ApiKeyCallback apiKey,
                             HashCallback hash);
    void setTargetPathCallback(TargetPathCallback callback);
    void setPreviewPathCallback(PreviewPathCallback callback);
    void setPlaceholderIcon(const QIcon &icon);

    bool cacheLoaded() const { return m_cacheLoaded; }
    void ensureCacheLoaded();
    void saveCache() const;
    void shutdown();

    bool containsInfo(const QString &filePath) const;
    ModelUpdateInfo info(const QString &filePath) const;
    void setInfo(const ModelUpdateInfo &info);

    QStringList selectedFilePaths() const;
    QString currentCategory() const;
    QStringList filePathsForCategory(const QString &category) const;
    QStringList sortedFilePathsForCategory(const QString &category) const;
    QString cardStatusText(const QString &filePath) const;
    QString cardTargetPath(const QString &filePath) const;
    bool containsCard(const QString &filePath) const;

    void addOrUpdateCard(const ModelUpdateInfo &info, const QString &status, bool sourceAvailable);
    void updateStatus(const QString &filePath, const QString &status);
    void updateProgress(const QString &filePath, int percent, const QString &speedText);
    void updateSelectionSummary();
    void filterCards();
    void sortCards();
    void removeCard(const QString &filePath);
    void clearCompleted();
    void toggleIgnore(const QString &filePath);
    void setPreview(const QString &filePath, const QString &previewPath = QString());
    void schedulePreviewLoad(const QString &filePath);

    void startSelectedDownloads();
    void enqueueModelDownload(const ModelUpdateInfo &info);
    void ignoreSelectedUpdates();
    void retryFailedDownloads();

signals:
    void statusMessageChanged(const QString &message);
    void modelFileReady(const ModelFileDownloadTask &task);
    void modelFileDownloaded(const ModelUpdateInfo &info, const QString &targetPath);

private slots:
    void processPreviewLoadBatch();
    void onPreviewLoaded();
    void processNextModelDownload();

private:
    static DownloadPreviewLoadResult processPreviewTask(const QString &filePath, const QString &previewPath);
    static QString uniqueFilePath(const QString &dirPath, const QString &fileName);

    QString chooseTargetPath(const ModelUpdateInfo &info, bool *overwrite) const;
    void finishModelDownload(const ModelFileDownloadTask &task);

    DownloadsPage *m_page = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    QThreadPool *m_previewThreadPool = nullptr;

    MakeRequestCallback m_makeRequest;
    ReplyErrorCallback m_replyError;
    TokenUrlCallback m_tokenUrl;
    ApiKeyCallback m_apiKey;
    HashCallback m_hash;
    TargetPathCallback m_targetPath;
    PreviewPathCallback m_previewPath;

    QIcon m_placeholderIcon;
    QHash<QString, ModelUpdateInfo> m_infos;
    bool m_cacheLoaded = false;
    bool m_restoringCache = false;
    bool m_shuttingDown = false;

    QQueue<QString> m_pendingPreviewLoads;
    QSet<QString> m_queuedPreviewLoads;
    QSet<QString> m_activePreviewLoads;
    QList<QPointer<QFutureWatcher<DownloadPreviewLoadResult>>> m_previewWatchers;
    QTimer *m_previewTimer = nullptr;

    QQueue<ModelFileDownloadTask> m_downloadQueue;
    QSet<QString> m_canceledPaths;
    QPointer<QNetworkReply> m_activeReply;
    ModelFileDownloadTask m_activeTask;
    QFile *m_activeFile = nullptr;
    bool m_downloading = false;
    QElapsedTimer m_downloadTimer;
};

#endif // DOWNLOADMANAGER_H
