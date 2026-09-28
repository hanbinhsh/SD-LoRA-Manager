#include "downloadmanager.h"

#include "downloadspage.h"
#include "fileutils.h"
#include "utils/downloadstatus.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QPixmap>
#include <QThreadPool>
#include <QUrlQuery>
#include <QUuid>
#include <QtConcurrent>
#include <QNetworkAccessManager>
#include <QNetworkRequest>

DownloadManager::DownloadManager(DownloadsPage *page,
                                 QNetworkAccessManager *network,
                                 QThreadPool *previewThreadPool,
                                 QObject *parent, const QString &cachePath)
    : QObject(parent)
    , m_page(page)
    , m_network(network)
    , m_previewThreadPool(previewThreadPool)
    , m_cachePath(cachePath.isEmpty() ? qApp->applicationDirPath() + "/config/downloads.json" : cachePath)
{
    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    connect(m_previewTimer, &QTimer::timeout, this, &DownloadManager::processPreviewLoadBatch);
    m_restoreTimer = new QTimer(this);
    m_restoreTimer->setSingleShot(true);
    connect(m_restoreTimer, &QTimer::timeout, this, &DownloadManager::restoreCacheBatch);
}

DownloadManager::~DownloadManager()
{
    shutdown();
}

void DownloadManager::setNetworkCallbacks(MakeRequestCallback makeRequest,
                                          ReplyErrorCallback replyError,
                                          TokenUrlCallback tokenUrl,
                                          ApiKeyCallback apiKey,
                                          HashCallback hash)
{
    m_makeRequest = std::move(makeRequest);
    m_replyError = std::move(replyError);
    m_tokenUrl = std::move(tokenUrl);
    m_apiKey = std::move(apiKey);
    m_hash = std::move(hash);
}

void DownloadManager::setTargetPathCallback(TargetPathCallback callback)
{
    m_targetPath = std::move(callback);
}

void DownloadManager::setPreviewPathCallback(PreviewPathCallback callback)
{
    m_previewPath = std::move(callback);
}

bool DownloadManager::containsInfo(const QString &filePath) const
{
    return m_infos.contains(filePath);
}

ModelUpdateInfo DownloadManager::info(const QString &filePath) const
{
    return m_infos.value(filePath);
}

void DownloadManager::setInfo(const ModelUpdateInfo &info)
{
    if (!info.filePath.isEmpty()) m_infos.insert(info.filePath, info);
}

void DownloadManager::addOrUpdateCard(const ModelUpdateInfo &info, const QString &status, bool sourceAvailable)
{
    if (!m_page || info.filePath.isEmpty()) return;
    m_infos.insert(info.filePath, info);
    const QString effectiveStatus = DownloadStatus::preserveIgnored(m_page->cardStatusText(info.filePath), status);

    m_page->addOrUpdateCard(info, effectiveStatus, sourceAvailable);

    if (effectiveStatus == "检查中..." || effectiveStatus == "计算 Hash 中...") {
        resetPreview(info.filePath);
    } else {
        if (m_restoringCache) resetPreview(info.filePath);
        schedulePreviewLoad(info.filePath);
    }
}

void DownloadManager::updateStatus(const QString &filePath, const QString &status)
{
    if (m_page) m_page->updateCardStatus(filePath, status);
}

void DownloadManager::updateProgress(const QString &filePath, int percent, const QString &speedText)
{
    if (m_page) m_page->updateCardProgress(filePath, percent, speedText);
}

void DownloadManager::removeCard(const QString &filePath)
{
    if (m_restoringCache) m_removedDuringRestore.insert(filePath);
    m_infos.remove(filePath);
    if (m_page) m_page->removeCard(filePath);
    saveCache();
}

void DownloadManager::clearCompleted()
{
    if (!m_page) return;
    m_page->beginCardBatch();
    const QStringList keys = m_infos.keys();
    for (const QString &filePath : keys) {
        const QString status = m_page->cardStatusText(filePath);
        if (status.contains("完成") || status.contains("已是最新")) {
            if (m_restoringCache) m_removedDuringRestore.insert(filePath);
            m_infos.remove(filePath);
            m_page->removeCard(filePath);
        }
    }
    m_page->endCardBatch();
    saveCache();
}

void DownloadManager::toggleIgnore(const QString &filePath)
{
    if (!m_page || !m_infos.contains(filePath)) return;
    const QString status = m_page->cardStatusText(filePath);
    if (status.contains("已忽略")) {
        const ModelUpdateInfo info = m_infos.value(filePath);
        const QString restoredStatus = info.latestFileExistsLocally
            ? QStringLiteral("旧版共存：本地已存在新版本")
            : (info.hasUpdate ? QStringLiteral("发现新版本") : QStringLiteral("已是最新"));
        updateStatus(filePath, restoredStatus);
        emit statusMessageChanged("已取消忽略更新。");
    } else {
        updateStatus(filePath, "已忽略更新");
        emit statusMessageChanged("已忽略该模型更新。");
    }
    saveCache();
}

void DownloadManager::ensureCacheLoaded()
{
    if (m_cacheLoaded || m_restoringCache || m_shuttingDown) return;
    m_restoringCache = true;
    if (m_page) m_page->setCacheRestoring(true);
    emit statusMessageChanged("正在后台读取上次下载列表...");
    auto *watcher = new QFutureWatcher<DownloadCache::LoadResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher]() {
        const DownloadCache::LoadResult result = watcher->result();
        watcher->deleteLater();
        if (m_shuttingDown) return;
        m_cacheError = result.error;
        m_pendingCacheEntries = result.entries;
        m_restoreIndex = 0;
        m_restoreTimer->start(0);
    });
    watcher->setFuture(QtConcurrent::run(m_previewThreadPool, &DownloadCache::load, m_cachePath));
}

void DownloadManager::restoreCacheBatch()
{
    if (m_shuttingDown || !m_page) return;
    QElapsedTimer budget;
    budget.start();
    m_page->beginCardBatch();
    int processed = 0;
    while (m_restoreIndex < m_pendingCacheEntries.size() && processed < 8 && budget.elapsed() < 8) {
        const DownloadCache::Entry &entry = m_pendingCacheEntries.at(m_restoreIndex++);
        // A live update always wins over a result read before that update.
        if (!m_infos.contains(entry.info.filePath) && !m_removedDuringRestore.contains(entry.info.filePath))
            addOrUpdateCard(entry.info, entry.status, entry.sourceAvailable);
        ++processed;
    }
    m_page->endCardBatch();

    if (m_restoreIndex < m_pendingCacheEntries.size()) {
        emit statusMessageChanged(QString("正在恢复下载列表... %1/%2")
                                      .arg(m_restoreIndex).arg(m_pendingCacheEntries.size()));
        m_restoreTimer->start(16);
        return;
    }

    m_pendingCacheEntries.clear();
    m_removedDuringRestore.clear();
    m_restoringCache = false;
    m_cacheLoaded = true;
    m_page->setCacheRestoring(false);
    if (!m_cacheError.isEmpty()) {
        emit statusMessageChanged("下载缓存读取失败，原文件已保留: " + m_cacheError);
    } else {
        emit statusMessageChanged(m_infos.isEmpty() ? "暂无下载检查缓存。"
                                    : QString("已恢复上次下载列表，共 %1 个模型。").arg(m_infos.size()));
    }
    if (m_saveAfterRestore) {
        m_saveAfterRestore = false;
        saveCache();
    }
    if (!m_pendingPreviewLoads.isEmpty()) m_previewTimer->start(0);
    emit cacheReady();
}

void DownloadManager::saveCache() const
{
    if (m_restoringCache) {
        m_saveAfterRestore = true;
        return;
    }
    // Never replace a cache with a partially restored list or a failed read.
    if (!m_cacheLoaded || !m_page || !m_cacheError.isEmpty()) return;
    QVector<DownloadCache::Entry> entries;
    const QStringList categories = {"updates", "coexisting", "ignored", "latest", "errors", "local"};
    for (const QString &category : categories) {
        for (const QString &filePath : m_page->sortedFilePathsForCategory(category)) {
            const auto it = m_infos.constFind(filePath);
            if (it == m_infos.cend()) continue;
            const QString status = DownloadStatus::persistedStatus(m_page->cardStatusText(filePath), it->hasUpdate);
            if (status.isEmpty()) continue;
            entries.append({it.value(), status, false});
        }
    }
    QString error;
    if (!DownloadCache::save(m_cachePath, entries, &error)) qWarning() << "Unable to save download cache:" << error;
}

void DownloadManager::shutdown()
{
    if (m_shuttingDown) return;
    m_shuttingDown = true;
    m_restoreTimer->stop();
    if (m_previewTimer) m_previewTimer->stop();
    if (m_activeReply) {
        m_activeReply->disconnect(this);
        m_activeReply->abort();
        m_activeReply = nullptr;
    }
    if (m_activeFile) {
        m_activeFile->close();
        delete m_activeFile;
        m_activeFile = nullptr;
    }
    m_downloadQueue.clear();
    m_queuedDownloadPaths.clear();
    m_pendingPreviewLoads.clear();
    m_queuedPreviewLoads.clear();
    for (const QPointer<QFutureWatcher<DownloadPreviewLoadResult>> &watcher : std::as_const(m_previewWatchers)) {
        if (!watcher) continue;
        watcher->disconnect(this);
        if (watcher->isRunning()) watcher->waitForFinished();
    }
    m_previewWatchers.clear();
    m_activePreviewLoads.clear();
    if (m_cacheLoaded) saveCache();
}

void DownloadManager::resetPreview(const QString &filePath)
{
    if (!m_page) return;
    ModelPreviewState state = m_infos.value(filePath).previewState;
    if (state == ModelPreviewState::RealPreview) state = ModelPreviewState::MissingOrUnknown;
    if (m_infos.contains(filePath)) m_infos[filePath].previewState = state;
    m_page->setCardPreviewPlaceholder(filePath, state);
}

void DownloadManager::schedulePreviewLoad(const QString &filePath)
{
    if (filePath.isEmpty() || m_queuedPreviewLoads.contains(filePath)) return;
    if (!m_page || !m_page->containsCard(filePath) || !m_infos.contains(filePath)) return;
    m_queuedPreviewLoads.insert(filePath);
    m_pendingPreviewLoads.enqueue(filePath);
    if (!m_restoringCache && m_previewTimer && !m_previewTimer->isActive()) m_previewTimer->start(0);
}

void DownloadManager::processPreviewLoadBatch()
{
    if (m_shuttingDown) return;
    constexpr int kBatchSize = 8;
    constexpr int kMaxActive = 4;
    int processed = 0;
    while (processed < kBatchSize && m_activePreviewLoads.size() < kMaxActive && !m_pendingPreviewLoads.isEmpty()) {
        const QString filePath = m_pendingPreviewLoads.dequeue();
        m_queuedPreviewLoads.remove(filePath);
        if (m_page && m_page->containsCard(filePath) && m_infos.contains(filePath) && m_previewPath) {
            const QString previewPath = m_previewPath(m_infos.value(filePath));
            if (!previewPath.isEmpty() && QFile::exists(previewPath)) {
                m_activePreviewLoads.insert(filePath);
                auto *watcher = new QFutureWatcher<DownloadPreviewLoadResult>(this);
                m_previewWatchers.append(watcher);
                connect(watcher, &QFutureWatcher<DownloadPreviewLoadResult>::finished,
                        this, &DownloadManager::onPreviewLoaded);
                watcher->setFuture(QtConcurrent::run(m_previewThreadPool,
                                                     &DownloadManager::processPreviewTask,
                                                     filePath,
                                                     previewPath));
            } else {
                ModelPreviewState state = m_infos.value(filePath).previewState;
                if (state == ModelPreviewState::RealPreview) {
                    state = ModelPreviewState::MissingOrUnknown;
                    m_infos[filePath].previewState = state;
                }
                m_page->setCardPreviewPlaceholder(filePath, state);
            }
        }
        ++processed;
    }
    if (m_previewTimer && !m_pendingPreviewLoads.isEmpty()) m_previewTimer->start(16);
}

DownloadPreviewLoadResult DownloadManager::processPreviewTask(const QString &filePath, const QString &previewPath)
{
    DownloadPreviewLoadResult result;
    result.filePath = filePath;
    result.previewPath = previewPath;
    const QSize targetSize(96, 128);
    QImageReader reader(previewPath);
    const QSize originalSize = reader.size();
    if (originalSize.isValid()) {
        const QSize decodeSize = originalSize.scaled(targetSize, Qt::KeepAspectRatioByExpanding);
        if (decodeSize.width() < originalSize.width() || decodeSize.height() < originalSize.height())
            reader.setScaledSize(decodeSize);
    }
    const QImage image = reader.read();
    if (image.isNull()) return result;
    const QImage scaled = image.scaled(targetSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    result.image = scaled.copy(qMax(0, (scaled.width() - targetSize.width()) / 2),
                                qMax(0, (scaled.height() - targetSize.height()) / 2),
                                targetSize.width(), targetSize.height());
    result.valid = true;
    return result;
}

void DownloadManager::onPreviewLoaded()
{
    auto *watcher = static_cast<QFutureWatcher<DownloadPreviewLoadResult>*>(sender());
    if (!watcher) return;
    const DownloadPreviewLoadResult result = watcher->result();
    m_previewWatchers.removeAll(watcher);
    watcher->deleteLater();
    m_activePreviewLoads.remove(result.filePath);

    if (!m_shuttingDown && m_page && m_page->containsCard(result.filePath)) {
        if (result.valid) {
            if (m_infos.contains(result.filePath)) {
                m_infos[result.filePath].previewState = ModelPreviewState::RealPreview;
            }
            m_page->setCardPreview(result.filePath, QPixmap::fromImage(result.image));
        } else {
            if (m_infos.contains(result.filePath)) {
                m_infos[result.filePath].previewState = ModelPreviewState::MissingOrUnknown;
            }
            m_page->setCardPreviewPlaceholder(result.filePath, ModelPreviewState::MissingOrUnknown);
        }
    }

    if (m_previewTimer && (!m_pendingPreviewLoads.isEmpty() || !m_activePreviewLoads.isEmpty())) {
        m_previewTimer->start(16);
    }
}

void DownloadManager::startSelectedDownloads()
{
    if (!m_page) return;
    bool queued = false;
    for (const QString &filePath : m_page->selectedFilePaths()) {
        if (!m_infos.contains(filePath)) continue;
        const ModelUpdateInfo info = m_infos.value(filePath);
        const QString status = m_page->cardStatusText(filePath);
        if (DownloadStatus::cardAction(status, info.hasUpdate) != DownloadStatus::CardAction::Download) continue;
        enqueueModelDownload(info);
        queued = true;
    }
    if (!queued) emit statusMessageChanged("没有可下载的选中更新。");
}

void DownloadManager::enqueueModelDownload(const ModelUpdateInfo &info)
{
    if (info.filePath.isEmpty()) return;
    if ((m_downloading && m_activeTask.filePath == info.filePath)
        || m_queuedDownloadPaths.contains(info.filePath)) {
        emit statusMessageChanged("该模型已经在下载队列中。");
        return;
    }

    bool overwrite = false;
    const QString targetPath = chooseTargetPath(info, &overwrite);
    if (targetPath.isEmpty()) return;

    ModelFileDownloadTask task;
    task.info = info;
    task.targetPath = targetPath;
    task.tempPath = targetPath + ".part";
    task.filePath = info.filePath;
    task.overwrite = overwrite;
    if (m_page) m_page->updateCardTargetPath(info.filePath, targetPath);
    m_downloadQueue.enqueue(task);
    m_queuedDownloadPaths.insert(info.filePath);
    m_canceledPaths.remove(info.filePath);
    updateStatus(info.filePath, "已加入下载队列");
    if (!m_downloading) processNextModelDownload();
}

QString DownloadManager::chooseTargetPath(const ModelUpdateInfo &info, bool *overwrite) const
{
    if (m_targetPath) return m_targetPath(info, overwrite);
    if (overwrite) *overwrite = false;
    QString fileName = info.downloadFileName;
    if (fileName.isEmpty()) fileName = QFileInfo(info.filePath).fileName();
    return FileUtils::uniqueFilePath(info.modelDir, fileName);
}

void DownloadManager::ignoreSelectedUpdates()
{
    if (!m_page) return;
    bool changed = false;
    m_page->beginCardBatch();
    for (const QString &filePath : m_page->selectedFilePaths()) {
        if (!m_infos.contains(filePath)) continue;
        const QString status = m_page->cardStatusText(filePath);
        if (status.contains("已忽略") || DownloadStatus::isDownloading(status)) continue;
        updateStatus(filePath, "已忽略更新");
        changed = true;
    }
    m_page->endCardBatch();
    if (changed) {
        emit statusMessageChanged("已忽略选中的模型更新。");
        saveCache();
    }
}

void DownloadManager::retryFailedDownloads()
{
    if (!m_page) return;
    bool queued = false;
    const QStringList keys = m_infos.keys();
    for (const QString &filePath : keys) {
        const QString status = m_page->cardStatusText(filePath);
        if (!DownloadStatus::isDownloadFailure(status)) continue;
        enqueueModelDownload(m_infos.value(filePath));
        queued = true;
    }
    if (!queued) emit statusMessageChanged("当前没有下载失败任务可重试。");
}

void DownloadManager::processNextModelDownload()
{
    if (m_downloadQueue.isEmpty()) {
        m_downloading = false;
        m_activeTask = ModelFileDownloadTask();
        return;
    }

    m_downloading = true;
    m_activeTask = m_downloadQueue.dequeue();
    m_queuedDownloadPaths.remove(m_activeTask.filePath);
    if (m_canceledPaths.contains(m_activeTask.info.filePath)) {
        m_canceledPaths.remove(m_activeTask.info.filePath);
        QTimer::singleShot(0, this, &DownloadManager::processNextModelDownload);
        return;
    }
    const QUrl downloadUrl(m_activeTask.info.downloadUrl);
    const bool downloadHasToken = QUrlQuery(downloadUrl).hasQueryItem("token");
    if (!m_network || !m_makeRequest) {
        updateStatus(m_activeTask.info.filePath, "下载失败: 下载器未初始化");
        QTimer::singleShot(0, this, &DownloadManager::processNextModelDownload);
        return;
    }

    QFile::remove(m_activeTask.tempPath);
    QDir().mkpath(QFileInfo(m_activeTask.tempPath).absolutePath());

    m_activeFile = new QFile(m_activeTask.tempPath, this);
    if (!m_activeFile->open(QIODevice::WriteOnly)) {
        updateStatus(m_activeTask.info.filePath, "下载失败: 无法写入目标路径");
        m_activeFile->deleteLater();
        m_activeFile = nullptr;
        QTimer::singleShot(0, this, &DownloadManager::processNextModelDownload);
        return;
    }
    m_activeWriteFailed = false;
    m_activeWriteError.clear();

    m_downloadTimer.restart();
    updateStatus(m_activeTask.info.filePath, "下载中...");
    updateProgress(m_activeTask.info.filePath, 0, "--");

    QNetworkReply *reply = m_network->get(m_makeRequest(downloadUrl, !downloadHasToken));
    m_activeReply = reply;

    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        if (!writeActiveReplyData(reply)) reply->abort();
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        const int percent = total > 0 ? int((received * 100) / total) : 0;
        const double seconds = qMax(0.1, double(m_downloadTimer.elapsed()) / 1000.0);
        const double mbps = (double(received) / 1024.0 / 1024.0) / seconds;
        updateProgress(m_activeTask.info.filePath, percent, QString::number(mbps, 'f', 2) + " MB/s");
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        writeActiveReplyData(reply);
        closeActiveFile();
        reply->deleteLater();
        m_activeReply = nullptr;

        if (m_activeWriteFailed) {
            QFile::remove(m_activeTask.tempPath);
            updateStatus(m_activeTask.info.filePath,
                         "下载失败: 写入文件失败" +
                             (m_activeWriteError.isEmpty() ? QString() : " (" + m_activeWriteError + ")"));
            QTimer::singleShot(0, this, &DownloadManager::processNextModelDownload);
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool canTokenRetry = (status == 401 || status == 403) &&
                                      !m_activeTask.info.downloadUrl.contains("token=", Qt::CaseInsensitive) &&
                                      m_apiKey && !m_apiKey().isEmpty() && m_tokenUrl;
            if (canTokenRetry) {
                QFile::remove(m_activeTask.tempPath);
                m_activeTask.info.downloadUrl = m_tokenUrl(QUrl(m_activeTask.info.downloadUrl)).toString();
                m_downloadQueue.enqueue(m_activeTask);
                m_queuedDownloadPaths.insert(m_activeTask.filePath);
                updateStatus(m_activeTask.info.filePath, "认证重试中...");
                QTimer::singleShot(0, this, &DownloadManager::processNextModelDownload);
                return;
            }
            QFile::remove(m_activeTask.tempPath);
            updateStatus(m_activeTask.info.filePath, "下载失败: " + (m_replyError ? m_replyError(reply) : reply->errorString()));
            QTimer::singleShot(0, this, &DownloadManager::processNextModelDownload);
            return;
        }

        verifyAndFinishDownload(m_activeTask);
    });
}

bool DownloadManager::writeActiveReplyData(QNetworkReply *reply)
{
    if (!reply || !m_activeFile || m_activeWriteFailed) return !m_activeWriteFailed;
    const QByteArray data = reply->readAll();
    if (data.isEmpty()) return true;
    const qint64 written = m_activeFile->write(data);
    if (written == data.size()) return true;

    m_activeWriteFailed = true;
    m_activeWriteError = m_activeFile->errorString();
    if (m_activeWriteError.isEmpty()) m_activeWriteError = QStringLiteral("写入长度不完整");
    return false;
}

void DownloadManager::closeActiveFile()
{
    if (!m_activeFile) return;
    if (!m_activeFile->flush() && !m_activeWriteFailed) {
        m_activeWriteFailed = true;
        m_activeWriteError = m_activeFile->errorString();
    }
    m_activeFile->close();
    m_activeFile->deleteLater();
    m_activeFile = nullptr;
}

void DownloadManager::verifyAndFinishDownload(const ModelFileDownloadTask &task)
{
    const QFileInfo tempInfo(task.tempPath);
    if (!tempInfo.exists() || tempInfo.size() <= 0) {
        QFile::remove(task.tempPath);
        updateStatus(task.info.filePath, "下载失败: 下载文件为空");
        QTimer::singleShot(0, this, &DownloadManager::processNextModelDownload);
        return;
    }

    const QString expected = task.info.sha256.trimmed();
    if (expected.isEmpty()) {
        finishModelDownload(task);
        QTimer::singleShot(0, this, &DownloadManager::processNextModelDownload);
        return;
    }

    updateStatus(task.info.filePath, "校验中...");
    const HashCallback hashCallback = m_hash;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, task, expected]() {
        const QString actual = watcher->result();
        watcher->deleteLater();
        if (m_shuttingDown) return;

        if (actual.isEmpty()) {
            QFile::remove(task.tempPath);
            updateStatus(task.info.filePath, "下载失败: 无法计算 SHA256");
        } else if (actual.compare(expected, Qt::CaseInsensitive) != 0) {
            QFile::remove(task.tempPath);
            updateStatus(task.info.filePath, "下载失败: SHA256 校验失败");
        } else {
            finishModelDownload(task);
        }
        QTimer::singleShot(0, this, &DownloadManager::processNextModelDownload);
    });
    watcher->setFuture(QtConcurrent::run([hashCallback, path = task.tempPath]() {
        return hashCallback ? hashCallback(path) : FileUtils::calculateSha256Hex(path);
    }));
}

void DownloadManager::finishModelDownload(const ModelFileDownloadTask &task)
{
    if (QFile::exists(task.targetPath) && !task.overwrite && task.targetPath != task.tempPath) {
        QFile::remove(task.tempPath);
        updateStatus(task.info.filePath, "下载失败: 目标文件已存在");
        return;
    }

    QString backupPath;
    if (task.overwrite && QFile::exists(task.targetPath)) {
        backupPath = task.targetPath + ".replace-backup-" +
                     QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!QFile::rename(task.targetPath, backupPath)) {
            updateStatus(task.info.filePath, "下载失败: 无法备份原模型文件");
            return;
        }
    }

    if (!QFile::rename(task.tempPath, task.targetPath)) {
        const bool restored = backupPath.isEmpty() || QFile::rename(backupPath, task.targetPath);
        updateStatus(task.info.filePath,
                     restored ? "下载失败: 无法移动下载文件，原模型已恢复"
                              : "下载失败: 无法移动下载文件，原模型保存在 " + backupPath);
        return;
    }
    if (!backupPath.isEmpty() && !QFile::remove(backupPath)) {
        qWarning() << "Downloaded model installed, but old backup could not be removed:" << backupPath;
    }

    updateProgress(task.info.filePath, 100, "--");
    updateStatus(task.info.filePath, "下载完成");
    saveCache();
    emit modelFileReady(task);
    emit modelFileDownloaded(task.info, task.targetPath);
}
