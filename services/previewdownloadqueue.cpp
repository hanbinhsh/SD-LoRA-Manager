#include "previewdownloadqueue.h"
#include "utils/applanguage.h"
#include "utils/civitaiclient.h"
#include "utils/imageloader.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSharedPointer>
#include <QThreadPool>
#include <QTimer>
#include <QUrlQuery>
#include <QtConcurrent/QtConcurrent>

PreviewDownloadQueue::PreviewDownloadQueue(QNetworkAccessManager *netManager,
                                           QThreadPool *backgroundThreadPool,
                                           QThreadPool *iconThreadPool,
                                           RequestFactory requestFactory,
                                           ApiKeyProvider apiKeyProvider,
                                           QObject *parent)
    : QObject(parent)
    , m_netManager(netManager)
    , m_backgroundThreadPool(backgroundThreadPool)
    , m_iconThreadPool(iconThreadPool)
    , m_requestFactory(std::move(requestFactory))
    , m_apiKeyProvider(std::move(apiKeyProvider))
{
}

void PreviewDownloadQueue::enqueue(const PreviewDownloadTask &task)
{
    if (task.countForMetadataSync) {
        ++m_metadataPreviewTasksPending;
    }
    m_queue.enqueue(task);
    if (!m_isDownloading) {
        processNext();
    }
}

void PreviewDownloadQueue::syncPreviewImagesFromMetadata(const QString &modelDir,
                                                         const QString &baseName,
                                                         const QVector<ImageInfo> &images,
                                                         bool forceNonCoverDownload,
                                                         bool countForMetadataSync)
{
    if (modelDir.isEmpty() || baseName.isEmpty()) return;
    for (int index = 0; index < images.size(); ++index) {
        const ImageInfo &img = images.at(index);
        const QString suffix = (index == 0) ? QStringLiteral(".preview.png")
                                            : QStringLiteral(".preview.%1.png").arg(index);
        const QString savePath = QFileInfo(QDir(modelDir).filePath(baseName + suffix)).absoluteFilePath();
        const PreviewMetadataPayload payload = PreviewImageStore::previewPayloadFromImageInfo(img);
        const bool exists = QFile::exists(savePath);

        PreviewDownloadTask task;
        task.url = img.url;
        task.savePath = savePath;
        task.localBaseName = baseName;
        task.imageIndex = index;
        task.previewMeta = payload;
        task.allowNoButton = true;
        task.countForMetadataSync = countForMetadataSync;

        if (exists && !(forceNonCoverDownload && index > 0)) {
            task.metadataOnly = true;
            enqueue(task);
            continue;
        }

        if (img.url.isEmpty()) continue;
        if (exists && index > 0 && forceNonCoverDownload) {
            QFile::remove(savePath);
        }
        task.metadataOnly = false;
        enqueue(task);
    }
}

void PreviewDownloadQueue::downloadThumbnail(const QString &url,
                                             const QString &savePath,
                                             QPushButton *button,
                                             QObject *iconReceiver)
{
    if (!m_netManager || !m_requestFactory) return;
    const QUrl requestUrl(url);
    QNetworkReply *reply = m_netManager->get(m_requestFactory(requestUrl, true));
    reply->setProperty("tokenRetry", false);
    const QPointer<QPushButton> safeBtn = button;

    connect(reply, &QNetworkReply::finished, this, [this, reply, savePath, safeBtn, requestUrl, iconReceiver]() {
        handleThumbnailReply(reply, savePath, safeBtn, requestUrl, iconReceiver);
    });
}

void PreviewDownloadQueue::handleThumbnailReply(QNetworkReply *reply,
                                                const QString &savePath,
                                                const QPointer<QPushButton> &safeBtn,
                                                const QUrl &requestUrl,
                                                QObject *iconReceiver)
{
    if (!reply) return;
    reply->deleteLater();

    const QString apiKey = m_apiKeyProvider ? m_apiKeyProvider() : QString();
    if (reply->error() != QNetworkReply::NoError) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if ((status == 401 || status == 403)
            && !reply->property("tokenRetry").toBool()
            && !apiKey.isEmpty()) {
            const QUrl retryUrl = CivitaiClient::urlWithToken(requestUrl, apiKey);
            if (retryUrl != requestUrl) {
                QNetworkReply *retryReply = m_netManager->get(m_requestFactory(retryUrl, false));
                retryReply->setProperty("tokenRetry", true);
                connect(retryReply, &QNetworkReply::finished, this,
                        [this, retryReply, savePath, safeBtn, requestUrl, iconReceiver]() {
                    handleThumbnailReply(retryReply, savePath, safeBtn, requestUrl, iconReceiver);
                });
                return;
            }
        }
        if (safeBtn) safeBtn->setText(AppText::tr("Error"));
        qDebug() << "Download error:" << CivitaiClient::networkErrorMessage(reply);
        return;
    }

    const QByteArray data = reply->readAll();
    if (data.isEmpty()) {
        if (safeBtn) safeBtn->setText(AppText::tr("Empty"));
        return;
    }

    QFile file(savePath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(data);
        file.close();
        if (m_iconThreadPool && iconReceiver) {
            auto *task = new IconLoaderTask(savePath, 100, 0, iconReceiver, savePath, true);
            task->setAutoDelete(true);
            m_iconThreadPool->start(task);
        }
        if (safeBtn) {
            safeBtn->setText(QString());
        }
    }
}

void PreviewDownloadQueue::cancelActiveAndQueued()
{
    if (m_netManager) {
        const QList<QNetworkReply*> replies = m_netManager->findChildren<QNetworkReply*>();
        for (QNetworkReply *reply : replies) {
            if (reply && reply->property("isGalleryDownload").toBool()) {
                reply->abort();
            }
        }
    }
    m_queue.clear();
    m_isDownloading = false;
}

void PreviewDownloadQueue::clearQueueOnly()
{
    m_queue.clear();
    m_isDownloading = false;
}

void PreviewDownloadQueue::resetMetadataSyncCounter()
{
    m_metadataPreviewTasksPending = 0;
}

void PreviewDownloadQueue::markMetadataPreviewTaskFinished()
{
    if (m_metadataPreviewTasksPending > 0) {
        --m_metadataPreviewTasksPending;
    }
    emit metadataPreviewTaskFinished(m_metadataPreviewTasksPending);
}

void PreviewDownloadQueue::processNext()
{
    if (!m_netManager || !m_requestFactory) {
        m_isDownloading = false;
        return;
    }

    while (!m_queue.isEmpty()) {
        PreviewDownloadTask task = m_queue.dequeue();
        if (task.button.isNull() && !task.allowNoButton) {
            continue;
        }

        m_isDownloading = true;
        if (task.button) task.button->setText(AppText::tr("Waiting..."));

        const QString cleanedSavePath = QFileInfo(task.savePath).absoluteFilePath();

        if (task.metadataOnly || task.url.trimmed().isEmpty()) {
            if (!task.url.trimmed().isEmpty()) {
                if (PreviewImageStore::previewFileAlreadyHasPromptMetadata(cleanedSavePath)) {
                    if (task.countForMetadataSync) markMetadataPreviewTaskFinished();
                } else {
                    PreviewDownloadTask retryTask = task;
                    retryTask.metadataOnly = false;
                    retryTask.allowNoButton = true;
                    m_queue.enqueue(retryTask);
                }
                QTimer::singleShot(0, this, &PreviewDownloadQueue::processNext);
                return;
            }

            auto *watcher = new QFutureWatcher<bool>(this);
            connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher, task]() {
                const bool ok = watcher->result();
                watcher->deleteLater();

                if (!ok && !task.url.trimmed().isEmpty()) {
                    PreviewDownloadTask retryTask = task;
                    retryTask.metadataOnly = false;
                    retryTask.allowNoButton = true;
                    m_queue.enqueue(retryTask);
                    if (task.countForMetadataSync) {
                        emit metadataStatusText(AppText::tr("本地预览图元信息写入失败，正在重新下载: %1")
                                                    .arg(QFileInfo(task.savePath).fileName()));
                    }
                } else if (task.countForMetadataSync) {
                    markMetadataPreviewTaskFinished();
                }

                QTimer::singleShot(0, this, &PreviewDownloadQueue::processNext);
            });
            watcher->setFuture(QtConcurrent::run(m_backgroundThreadPool, [path = cleanedSavePath, payload = task.previewMeta]() {
                return PreviewImageStore::ensurePreviewImageMetadata(path, payload);
            }));
            return;
        }

        const QUrl taskUrl(task.url);
        const bool hasToken = QUrlQuery(taskUrl).hasQueryItem(QStringLiteral("token"));
        QNetworkReply *reply = m_netManager->get(m_requestFactory(taskUrl, !hasToken));
        reply->setProperty("isGalleryDownload", true);

        auto timer = QSharedPointer<QElapsedTimer>::create();
        timer->start();

        connect(reply, &QNetworkReply::downloadProgress, this, [task, timer](qint64 received, qint64 total) {
            if (task.button.isNull()) return;
            if (total > 0) {
                const int percent = static_cast<int>((received * 100) / total);
                const double seconds = timer->elapsed() / 1000.0;
                QString speedStr;
                if (seconds > 0.1) {
                    const double speedKB = (received / 1024.0) / seconds;
                    if (speedKB > 1024.0) {
                        speedStr = QString::number(speedKB / 1024.0, 'f', 1) + QStringLiteral(" MB/s");
                    } else {
                        speedStr = QString::number(speedKB, 'f', 0) + QStringLiteral(" KB/s");
                    }
                }
                task.button->setText(QStringLiteral("%1%\n%2").arg(percent).arg(speedStr));
            } else if (received > 0) {
                task.button->setText(AppText::tr("%1 KB").arg(received / 1024));
            }
        });

        connect(reply, &QNetworkReply::finished, this, [this, reply, task, cleanedSavePath]() {
            reply->deleteLater();

            if (reply->error() == QNetworkReply::OperationCanceledError) {
                if (task.countForMetadataSync) markMetadataPreviewTaskFinished();
                return;
            }

            const QString apiKey = m_apiKeyProvider ? m_apiKeyProvider() : QString();
            if (reply->error() != QNetworkReply::NoError) {
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if ((status == 401 || status == 403)
                    && !task.url.contains(QStringLiteral("token="), Qt::CaseInsensitive)
                    && !apiKey.isEmpty()) {
                    PreviewDownloadTask retryTask = task;
                    retryTask.url = CivitaiClient::urlWithToken(QUrl(task.url), apiKey).toString();
                    m_queue.enqueue(retryTask);
                    if (task.button) task.button->setText(AppText::tr("Retrying..."));
                    QTimer::singleShot(0, this, &PreviewDownloadQueue::processNext);
                    return;
                }

                const QString errStr = reply->errorString();
                if (errStr.contains(QStringLiteral("TLS"), Qt::CaseInsensitive)
                    || errStr.contains(QStringLiteral("SSL"), Qt::CaseInsensitive)
                    || errStr.contains(QStringLiteral("handshake"), Qt::CaseInsensitive)) {
                    if (task.button) task.button->setText(AppText::tr("SSL Error"));
                    emit statusMessage(AppText::tr("HTTPS 连接失败: 缺失 OpenSSL 运行库 (libcrypto / libssl)"), 8000);
                } else if (task.button) {
                    task.button->setText(AppText::tr("Err: %1").arg(status > 0 ? QString::number(status) : QStringLiteral("Net")));
                }

                qDebug() << "Queued preview download failed:" << CivitaiClient::networkErrorMessage(reply) << errStr;
                if (task.countForMetadataSync) markMetadataPreviewTaskFinished();
                QTimer::singleShot(500, this, &PreviewDownloadQueue::processNext);
                return;
            }

            QByteArray data = reply->readAll();
            if (data.isEmpty()) {
                if (task.countForMetadataSync) markMetadataPreviewTaskFinished();
                QTimer::singleShot(0, this, &PreviewDownloadQueue::processNext);
                return;
            }

            auto *saveWatcher = new QFutureWatcher<bool>(this);
            connect(saveWatcher, &QFutureWatcher<bool>::finished, this,
                    [this, saveWatcher, task, cleanedSavePath]() {
                const bool saved = saveWatcher->result();
                saveWatcher->deleteLater();

                if (saved) {
                    emit previewSaved(task.localBaseName, cleanedSavePath);
                } else {
                    emit statusMessage(
                        AppText::tr("预览图已保存，但未能写入可解析元信息: ")
                            + QFileInfo(cleanedSavePath).fileName(),
                        5000);
                }

                if (task.button) task.button->setText(QString());
                if (task.countForMetadataSync) markMetadataPreviewTaskFinished();
                QTimer::singleShot(0, this, &PreviewDownloadQueue::processNext);
            });
            saveWatcher->setFuture(QtConcurrent::run(
                m_backgroundThreadPool,
                [data = std::move(data),
                 savePath = cleanedSavePath,
                 payload = task.previewMeta]() {
                    return PreviewImageStore::savePreviewImageWithMetadata(data, savePath, payload);
                }));
        });

        return;
    }

    m_isDownloading = false;
}
