#ifndef METADATASYNCSERVICE_H
#define METADATASYNCSERVICE_H

#include "core/metadatascantypes.h"
#include "core/modelupdateinfo.h"

#include <QJsonObject>
#include <QNetworkRequest>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QThreadPool;
class PreviewDownloadQueue;

struct MetadataSyncOptions {
    bool recalculateKnownHash = false;
    bool tryCivArchiveOnFail = true;
};

class MetadataSyncService : public QObject
{
    Q_OBJECT

public:
    using RequestFactory = std::function<QNetworkRequest(const QUrl &, bool allowCivitaiAuth)>;
    using OptionsProvider = std::function<MetadataSyncOptions()>;
    using SaveMetadataCallback = std::function<bool(const MetadataSyncJob &,
                                                    const QJsonObject &modelRoot,
                                                    const QJsonObject &versionHint,
                                                    bool syncPreviewImages,
                                                    bool batchRunning)>;

    MetadataSyncService(QNetworkAccessManager *netManager,
                        QThreadPool *backgroundThreadPool,
                        PreviewDownloadQueue *previewQueue,
                        RequestFactory requestFactory,
                        OptionsProvider optionsProvider,
                        SaveMetadataCallback saveCallback,
                        QObject *parent = nullptr);

    bool isRunning() const { return m_running; }
    bool syncPreviewImagesEnabled() const { return m_syncPreviewImages; }

    void startBatch(const QQueue<MetadataSyncJob> &jobs, bool syncPreviewImages, const QString &sourceLabel);
    void startDetailCivArchiveFallback(const MetadataSyncJob &job, const QString &reason, bool syncPreviewImages);
    void onMetadataPreviewTaskFinished(int remainingCount);

signals:
    void itemStatusChanged(const QString &filePath,
                           const QString &status,
                           const QString &category,
                           const QString &timestamp = QString(),
                           const QString &sourceLabel = QString());
    void batchStatusChanged(const QString &text);
    void hashCalculated(const QString &filePath, const QString &sha256);
    void jobFailed(const MetadataSyncJob &job, const QString &message, const QString &category);
    void batchCompleted(int totalCount);

private:
    void processNextJob();
    void fetchForJob(const MetadataSyncJob &job);
    void handleVersionReply(QNetworkReply *reply, MetadataSyncJob job);
    void handleHashReply(QNetworkReply *reply, MetadataSyncJob job);
    void handleModelReply(QNetworkReply *reply, const MetadataSyncJob &job, const QJsonObject &versionHint);
    bool tryStartCivArchiveHashCalculation(const MetadataSyncJob &job, const QString &reason);
    void fetchFromCivArchive(const MetadataSyncJob &job, const QString &reason, bool directOnly = false);
    void handleCivArchiveReply(QNetworkReply *reply,
                               const MetadataSyncJob &job,
                               const QString &reason,
                               const QString &sourceUrl);
    void finishJobWithFailure(const MetadataSyncJob &job,
                              const QString &message,
                              const QString &category = QStringLiteral("failed"));
    void finishBatch();

    QNetworkAccessManager *m_netManager = nullptr;
    QThreadPool *m_backgroundThreadPool = nullptr;
    PreviewDownloadQueue *m_previewQueue = nullptr;
    RequestFactory m_requestFactory;
    OptionsProvider m_optionsProvider;
    SaveMetadataCallback m_saveCallback;

    QQueue<MetadataSyncJob> m_pendingJobs;
    int m_total = 0;
    int m_done = 0;
    bool m_running = false;
    bool m_syncPreviewImages = false;
    bool m_waitingForPreviews = false;
};

#endif // METADATASYNCSERVICE_H
