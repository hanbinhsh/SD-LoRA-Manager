#include "metadatasyncservice.h"
#include "previewdownloadqueue.h"
#include "utils/applanguage.h"
#include "utils/civarchiveparser.h"
#include "utils/civitaiclient.h"
#include "utils/fileutils.h"

#include <QDateTime>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrent>

MetadataSyncService::MetadataSyncService(QNetworkAccessManager *netManager,
                                         QThreadPool *backgroundThreadPool,
                                         PreviewDownloadQueue *previewQueue,
                                         RequestFactory requestFactory,
                                         OptionsProvider optionsProvider,
                                         SaveMetadataCallback saveCallback,
                                         QObject *parent)
    : QObject(parent)
    , m_netManager(netManager)
    , m_backgroundThreadPool(backgroundThreadPool)
    , m_previewQueue(previewQueue)
    , m_requestFactory(std::move(requestFactory))
    , m_optionsProvider(std::move(optionsProvider))
    , m_saveCallback(std::move(saveCallback))
{
}

void MetadataSyncService::startBatch(const QQueue<MetadataSyncJob> &jobs,
                                     bool syncPreviewImages,
                                     const QString &sourceLabel)
{
    m_pendingJobs = jobs;
    m_syncPreviewImages = syncPreviewImages;
    m_total = m_pendingJobs.size();
    m_done = 0;
    m_waitingForPreviews = false;
    if (m_previewQueue) {
        m_previewQueue->resetMetadataSyncCounter();
    }
    m_running = m_total > 0;
    if (!m_running) {
        m_syncPreviewImages = false;
        emit batchStatusChanged(AppText::tr("没有可同步的模型。"));
        return;
    }
    emit batchStatusChanged(AppText::tr("正在从 %1 同步元信息... 0/%2").arg(sourceLabel).arg(m_total));
    processNextJob();
}

void MetadataSyncService::startDetailCivArchiveFallback(const MetadataSyncJob &job,
                                                        const QString &reason,
                                                        bool syncPreviewImages)
{
    m_total = qMax(m_total, 1);
    m_done = 0;
    m_running = false;
    m_syncPreviewImages = syncPreviewImages;
    fetchFromCivArchive(job, reason, false);
}

void MetadataSyncService::onMetadataPreviewTaskFinished(int remainingCount)
{
    if (m_waitingForPreviews && remainingCount > 0) {
        emit batchStatusChanged(AppText::tr("正在同步预览图元信息... 剩余 %1 个任务").arg(remainingCount));
    }
    if (m_waitingForPreviews && remainingCount <= 0) {
        finishBatch();
    }
}

void MetadataSyncService::finishBatch()
{
    m_waitingForPreviews = false;
    m_syncPreviewImages = false;
    emit batchStatusChanged(AppText::tr("元信息同步完成，共处理 %1 个模型。").arg(m_total));
    emit batchCompleted(m_total);
}

void MetadataSyncService::processNextJob()
{
    if (!m_running) return;
    if (m_pendingJobs.isEmpty()) {
        m_running = false;
        const int pendingPreviews = m_previewQueue ? m_previewQueue->metadataPreviewTasksPending() : 0;
        if (m_syncPreviewImages && pendingPreviews > 0) {
            m_waitingForPreviews = true;
            emit batchStatusChanged(AppText::tr("正在同步预览图元信息... 剩余 %1 个任务").arg(pendingPreviews));
            return;
        }
        finishBatch();
        return;
    }

    const MetadataSyncJob job = m_pendingJobs.dequeue();
    emit itemStatusChanged(job.snapshot.filePath, QStringLiteral("同步中..."), QString());
    fetchForJob(job);
}

void MetadataSyncService::fetchForJob(const MetadataSyncJob &job)
{
    if (!m_netManager || !m_requestFactory) return;

    if (job.civArchiveOnly) {
        fetchFromCivArchive(job, QStringLiteral("手动从 CivArchive 补充"), true);
        return;
    }

    if (job.snapshot.modelId > 0) {
        if (job.snapshot.currentVersionId > 0) {
            emit itemStatusChanged(job.snapshot.filePath, QStringLiteral("正在获取完整版本元信息..."), QString());
            const QUrl url(QStringLiteral("https://civitai.com/api/v1/model-versions/%1").arg(job.snapshot.currentVersionId));
            QNetworkReply *reply = m_netManager->get(m_requestFactory(url, true));
            connect(reply, &QNetworkReply::finished, this, [this, reply, job]() {
                handleVersionReply(reply, job);
            });
            return;
        }

        const QUrl url(QStringLiteral("https://civitai.com/api/v1/models/%1").arg(job.snapshot.modelId));
        QNetworkReply *reply = m_netManager->get(m_requestFactory(url, true));
        connect(reply, &QNetworkReply::finished, this, [this, reply, job]() {
            handleModelReply(reply, job, QJsonObject());
        });
        return;
    }

    const MetadataSyncOptions opts = m_optionsProvider ? m_optionsProvider() : MetadataSyncOptions{};
    const QString cachedHash = job.snapshot.currentSha256.trimmed();
    if (!opts.recalculateKnownHash && !cachedHash.isEmpty()) {
        emit itemStatusChanged(job.snapshot.filePath, QStringLiteral("使用缓存 Hash 同步中..."), QString());
        MetadataSyncJob hashJob = job;
        hashJob.snapshot.currentSha256 = cachedHash;
        const QUrl url(QStringLiteral("https://civitai.com/api/v1/model-versions/by-hash/%1").arg(cachedHash));
        QNetworkReply *reply = m_netManager->get(m_requestFactory(url, true));
        connect(reply, &QNetworkReply::finished, this, [this, reply, hashJob]() {
            handleHashReply(reply, hashJob);
        });
        return;
    }

    emit itemStatusChanged(job.snapshot.filePath, QStringLiteral("正在计算 Hash..."), QString());
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, job]() {
        const QString hash = watcher->result();
        watcher->deleteLater();
        if (hash.isEmpty()) {
            fetchFromCivArchive(job, QStringLiteral("无法计算 Hash，尝试使用 ID 查询 CivArchive"));
            return;
        }
        MetadataSyncJob hashJob = job;
        hashJob.snapshot.currentSha256 = hash;
        emit hashCalculated(hashJob.snapshot.filePath, hash);
        const QUrl url(QStringLiteral("https://civitai.com/api/v1/model-versions/by-hash/%1").arg(hash));
        QNetworkReply *reply = m_netManager->get(m_requestFactory(url, true));
        connect(reply, &QNetworkReply::finished, this, [this, reply, hashJob]() {
            handleHashReply(reply, hashJob);
        });
    });
    watcher->setFuture(QtConcurrent::run(m_backgroundThreadPool, [filePath = job.snapshot.filePath]() {
        return FileUtils::calculateSha256Hex(filePath);
    }));
}

void MetadataSyncService::handleVersionReply(QNetworkReply *reply, MetadataSyncJob job)
{
    if (!reply) return;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        fetchFromCivArchive(job, QStringLiteral("版本元信息获取失败: ") + CivitaiClient::networkErrorMessage(reply));
        return;
    }

    const QJsonObject versionRoot = QJsonDocument::fromJson(reply->readAll()).object();
    const int modelId = versionRoot.value(QStringLiteral("modelId")).toInt(job.snapshot.modelId);
    job.snapshot.currentVersionId = versionRoot.value(QStringLiteral("id")).toInt(job.snapshot.currentVersionId);
    if (modelId <= 0) {
        fetchFromCivArchive(job, QStringLiteral("版本元信息缺少模型 ID"));
        return;
    }

    const QUrl url(QStringLiteral("https://civitai.com/api/v1/models/%1").arg(modelId));
    QNetworkReply *detailReply = m_netManager->get(m_requestFactory(url, true));
    connect(detailReply, &QNetworkReply::finished, this, [this, detailReply, job, versionRoot]() {
        handleModelReply(detailReply, job, versionRoot);
    });
}

void MetadataSyncService::handleHashReply(QNetworkReply *reply, MetadataSyncJob job)
{
    if (!reply) return;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        fetchFromCivArchive(job, QStringLiteral("Hash 匹配失败: ") + CivitaiClient::networkErrorMessage(reply));
        return;
    }

    const QJsonObject versionRoot = QJsonDocument::fromJson(reply->readAll()).object();
    const int modelId = versionRoot.value(QStringLiteral("modelId")).toInt();
    job.snapshot.currentVersionId = versionRoot.value(QStringLiteral("id")).toInt();
    if (modelId <= 0) {
        fetchFromCivArchive(job, QStringLiteral("无法从 Hash 匹配 Civitai 模型"));
        return;
    }

    const QUrl url(QStringLiteral("https://civitai.com/api/v1/models/%1").arg(modelId));
    QNetworkReply *detailReply = m_netManager->get(m_requestFactory(url, true));
    connect(detailReply, &QNetworkReply::finished, this, [this, detailReply, job, versionRoot]() {
        handleModelReply(detailReply, job, versionRoot);
    });
}

void MetadataSyncService::handleModelReply(QNetworkReply *reply,
                                           const MetadataSyncJob &job,
                                           const QJsonObject &versionHint)
{
    if (!reply) return;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        fetchFromCivArchive(job, QStringLiteral("同步失败: ") + CivitaiClient::networkErrorMessage(reply));
        return;
    }

    const QJsonObject modelRoot = QJsonDocument::fromJson(reply->readAll()).object();
    const bool ok = m_saveCallback
        ? m_saveCallback(job, modelRoot, versionHint, m_syncPreviewImages, m_running)
        : false;
    if (!ok && !job.civArchiveOnly) {
        fetchFromCivArchive(job, QStringLiteral("Civitai 返回成功但无法匹配当前版本"));
        return;
    }
    if (!ok) {
        finishJobWithFailure(job, QStringLiteral("同步失败: 未找到可保存的版本"));
        return;
    }

    const QString nowText = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    emit itemStatusChanged(job.snapshot.filePath,
                           QStringLiteral("metadata 已同步"),
                           QStringLiteral("existing"),
                           nowText,
                           QStringLiteral("同步时间"));
    ++m_done;
    emit batchStatusChanged(AppText::tr("正在同步元信息... %1/%2").arg(m_done).arg(m_total));
    processNextJob();
}

bool MetadataSyncService::tryStartCivArchiveHashCalculation(const MetadataSyncJob &job, const QString &reason)
{
    const MetadataSyncOptions opts = m_optionsProvider ? m_optionsProvider() : MetadataSyncOptions{};
    if (job.snapshot.filePath.isEmpty() || !QFileInfo::exists(job.snapshot.filePath)) return false;
    if (!job.snapshot.currentSha256.trimmed().isEmpty()) return false;
    if (!opts.recalculateKnownHash && !job.civArchiveOnly) return false;

    emit itemStatusChanged(job.snapshot.filePath, QStringLiteral("正在计算 Hash 以查询 CivArchive..."), QString());
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, job, reason]() {
        MetadataSyncJob retryJob = job;
        retryJob.snapshot.currentSha256 = watcher->result();
        watcher->deleteLater();
        if (!retryJob.snapshot.currentSha256.trimmed().isEmpty()) {
            emit hashCalculated(retryJob.snapshot.filePath, retryJob.snapshot.currentSha256.trimmed());
        }
        fetchFromCivArchive(retryJob, reason);
    });
    watcher->setFuture(QtConcurrent::run(m_backgroundThreadPool, [filePath = job.snapshot.filePath]() {
        return FileUtils::calculateSha256Hex(filePath);
    }));
    return true;
}

void MetadataSyncService::fetchFromCivArchive(const MetadataSyncJob &job, const QString &reason, bool directOnly)
{
    const MetadataSyncOptions opts = m_optionsProvider ? m_optionsProvider() : MetadataSyncOptions{};
    if (!directOnly && !job.civArchiveOnly && !opts.tryCivArchiveOnFail) {
        finishJobWithFailure(job, reason);
        return;
    }

    const QUrl url = CivArchiveParser::civArchiveLookupUrl(job);
    if (!url.isValid()) {
        if (tryStartCivArchiveHashCalculation(job, reason)) return;
        finishJobWithFailure(job, reason + QStringLiteral("；CivArchive 查询缺少 Hash 或模型 ID"), QStringLiteral("no_ids"));
        return;
    }

    emit itemStatusChanged(job.snapshot.filePath, QStringLiteral("正在查询 CivArchive..."), QString());
    QNetworkReply *reply = m_netManager->get(m_requestFactory(url, false));
    const QString sourceUrl = url.toString();
    connect(reply, &QNetworkReply::finished, this, [this, reply, job, reason, sourceUrl]() {
        handleCivArchiveReply(reply, job, reason, sourceUrl);
    });
}

void MetadataSyncService::handleCivArchiveReply(QNetworkReply *reply,
                                                const MetadataSyncJob &job,
                                                const QString &reason,
                                                const QString &sourceUrl)
{
    if (!reply) return;
    const QByteArray body = reply->readAll();
    const QString networkError = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    if (!networkError.isEmpty()) {
        QString message = reason + QStringLiteral("；CivArchive 查询失败: %1").arg(networkError);
        if (status > 0) message += QStringLiteral(" (HTTP %1)").arg(status);
        finishJobWithFailure(job, message);
        return;
    }

    QJsonObject modelRoot;
    QJsonObject versionHint;
    if (!CivArchiveParser::parseCivArchivePayload(body, sourceUrl, modelRoot, versionHint)) {
        finishJobWithFailure(job, reason + QStringLiteral("；CivArchive 返回内容中未找到可识别的模型元信息"));
        return;
    }

    const bool ok = m_saveCallback
        ? m_saveCallback(job, modelRoot, versionHint, m_syncPreviewImages, m_running)
        : false;
    if (!ok) {
        finishJobWithFailure(job, reason + QStringLiteral("；CivArchive 元信息无法匹配当前模型版本"));
        return;
    }

    const QString nowText = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    emit itemStatusChanged(job.snapshot.filePath,
                           QStringLiteral("metadata 已从 CivArchive 补充"),
                           QStringLiteral("existing"),
                           nowText,
                           QStringLiteral("CivArchive"));
    ++m_done;
    emit batchStatusChanged(AppText::tr("正在同步元信息... %1/%2").arg(m_done).arg(m_total));
    processNextJob();
}

void MetadataSyncService::finishJobWithFailure(const MetadataSyncJob &job,
                                               const QString &message,
                                               const QString &category)
{
    emit itemStatusChanged(job.snapshot.filePath, message, category);
    emit batchStatusChanged(AppText::tr("正在同步元信息... %1/%2").arg(m_done + 1).arg(m_total));
    emit jobFailed(job, message, category);
    ++m_done;
    processNextJob();
}
