#ifndef DOWNLOADMANAGER_H
#define DOWNLOADMANAGER_H

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
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
#include "utils/downloadcache.h"

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
                             QObject *parent = nullptr,
                             const QString &cachePath = QString());
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

    bool cacheLoaded() const { return m_cacheLoaded; }
    void ensureCacheLoaded();
    void saveCache() const;
    void shutdown();

    bool containsInfo(const QString &filePath) const;
    ModelUpdateInfo info(const QString &filePath) const;
    void setInfo(const ModelUpdateInfo &info);

    void addOrUpdateCard(const ModelUpdateInfo &info, const QString &status, bool sourceAvailable);
    void updateStatus(const QString &filePath, const QString &status);
    void updateProgress(const QString &filePath, int percent, const QString &speedText);
    void removeCard(const QString &filePath);
    void clearCompleted();
    void toggleIgnore(const QString &filePath);
    void resetPreview(const QString &filePath);
    void schedulePreviewLoad(const QString &filePath);

    void startSelectedDownloads();
    void enqueueModelDownload(const ModelUpdateInfo &info);
    void ignoreSelectedUpdates();
    void retryFailedDownloads();

signals:
    void cacheReady();
    void statusMessageChanged(const QString &message);
    void modelFileReady(const ModelFileDownloadTask &task);
    void modelFileDownloaded(const ModelUpdateInfo &info, const QString &targetPath);

private slots:
    void restoreCacheBatch();
    void processPreviewLoadBatch();
    void onPreviewLoaded();
    void processNextModelDownload();

private:
    static DownloadPreviewLoadResult processPreviewTask(const QString &filePath, const QString &previewPath);

    QString chooseTargetPath(const ModelUpdateInfo &info, bool *overwrite) const;
    bool writeActiveReplyData(QNetworkReply *reply);
    void closeActiveFile();
    void verifyAndFinishDownload(const ModelFileDownloadTask &task);
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

    QString m_cachePath;
    QString m_cacheError;
    QVector<DownloadCache::Entry> m_pendingCacheEntries;
    qsizetype m_restoreIndex = 0;
    QSet<QString> m_removedDuringRestore;
    QTimer *m_restoreTimer = nullptr;
    mutable bool m_saveAfterRestore = false;
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
    QSet<QString> m_queuedDownloadPaths;
    QSet<QString> m_canceledPaths;
    QPointer<QNetworkReply> m_activeReply;
    ModelFileDownloadTask m_activeTask;
    QFile *m_activeFile = nullptr;
    bool m_activeWriteFailed = false;
    QString m_activeWriteError;
    bool m_downloading = false;
    QElapsedTimer m_downloadTimer;
};

#endif // DOWNLOADMANAGER_H
