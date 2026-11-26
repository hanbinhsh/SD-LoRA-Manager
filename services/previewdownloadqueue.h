#ifndef PREVIEWDOWNLOADQUEUE_H
#define PREVIEWDOWNLOADQUEUE_H

#include "core/modeltypes.h"
#include "utils/previewimagestore.h"

#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QPushButton>
#include <QQueue>
#include <QString>
#include <QUrl>
#include <QVector>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QThreadPool;

struct PreviewDownloadTask {
    QString url;
    QString savePath;
    QString localBaseName;
    QPointer<QPushButton> button;
    int imageIndex = -1;
    PreviewMetadataPayload previewMeta;
    bool allowNoButton = false;
    bool metadataOnly = false;
    bool countForMetadataSync = false;
};

class PreviewDownloadQueue : public QObject
{
    Q_OBJECT

public:
    using RequestFactory = std::function<QNetworkRequest(const QUrl &, bool allowCivitaiAuth)>;
    using ApiKeyProvider = std::function<QString()>;

    PreviewDownloadQueue(QNetworkAccessManager *netManager,
                         QThreadPool *backgroundThreadPool,
                         QThreadPool *iconThreadPool,
                         RequestFactory requestFactory,
                         ApiKeyProvider apiKeyProvider,
                         QObject *parent = nullptr);

    void enqueue(const PreviewDownloadTask &task);
    void syncPreviewImagesFromMetadata(const QString &modelDir,
                                       const QString &baseName,
                                       const QVector<ImageInfo> &images,
                                       bool forceNonCoverDownload,
                                       bool countForMetadataSync = false);
    void downloadThumbnail(const QString &url,
                           const QString &savePath,
                           QPushButton *button,
                           QObject *iconReceiver);

    void cancelActiveAndQueued();
    void clearQueueOnly();
    void resetMetadataSyncCounter();
    int metadataPreviewTasksPending() const { return m_metadataPreviewTasksPending; }

signals:
    void previewSaved(const QString &localBaseName, const QString &savePath);
    void statusMessage(const QString &message, int timeoutMs);
    void metadataStatusText(const QString &text);
    void metadataPreviewTaskFinished(int remainingCount);

private:
    void processNext();
    void markMetadataPreviewTaskFinished();
    void handleThumbnailReply(QNetworkReply *reply,
                              const QString &savePath,
                              const QPointer<QPushButton> &safeBtn,
                              const QUrl &requestUrl,
                              QObject *iconReceiver);

    QNetworkAccessManager *m_netManager = nullptr;
    QThreadPool *m_backgroundThreadPool = nullptr;
    QThreadPool *m_iconThreadPool = nullptr;
    RequestFactory m_requestFactory;
    ApiKeyProvider m_apiKeyProvider;

    QQueue<PreviewDownloadTask> m_queue;
    bool m_isDownloading = false;
    int m_metadataPreviewTasksPending = 0;
};

#endif // PREVIEWDOWNLOADQUEUE_H
