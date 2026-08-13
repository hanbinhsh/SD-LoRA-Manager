#include "downloadmanager.h"

#include "downloadspage.h"
#include "fileutils.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSaveFile>
#include <QThreadPool>
#include <QUrlQuery>
#include <QUuid>
#include <QtConcurrent>
#include <QNetworkAccessManager>
#include <QNetworkRequest>

DownloadManager::DownloadManager(DownloadsPage *page,
                                 QNetworkAccessManager *network,
                                 QThreadPool *previewThreadPool,
                                 QObject *parent)
    : QObject(parent)
    , m_page(page)
    , m_network(network)
    , m_previewThreadPool(previewThreadPool)
{
    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    connect(m_previewTimer, &QTimer::timeout, this, &DownloadManager::processPreviewLoadBatch);
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

void DownloadManager::setPlaceholderIcon(const QIcon &icon)
{
    m_placeholderIcon = icon;
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

QStringList DownloadManager::selectedFilePaths() const
{
    return m_page ? m_page->selectedFilePaths() : QStringList();
}

QString DownloadManager::currentCategory() const
{
    return m_page ? m_page->currentCategory() : QString();
}

QStringList DownloadManager::filePathsForCategory(const QString &category) const
{
    return m_page ? m_page->filePathsForCategory(category) : QStringList();
}

QStringList DownloadManager::sortedFilePathsForCategory(const QString &category) const
{
    return m_page ? m_page->sortedFilePathsForCategory(category) : QStringList();
}

QString DownloadManager::cardStatusText(const QString &filePath) const
{
    return m_page ? m_page->cardStatusText(filePath) : QString();
}

QString DownloadManager::cardTargetPath(const QString &filePath) const
{
    return m_page ? m_page->cardTargetPath(filePath) : QString();
}

bool DownloadManager::containsCard(const QString &filePath) const
{
    return m_page && m_page->containsCard(filePath);
}

void DownloadManager::addOrUpdateCard(const ModelUpdateInfo &info, const QString &status, bool sourceAvailable)
{
    if (!m_page || info.filePath.isEmpty()) return;
    m_infos.insert(info.filePath, info);
    QString effectiveStatus = status;
    if (m_page->cardStatusText(info.filePath).contains("已忽略") &&
        !status.contains("下载") &&
        !status.contains("完成") &&
        !status.contains("失败")) {
        effectiveStatus = "已忽略更新";
    }

    m_page->addOrUpdateCard(info, effectiveStatus, sourceAvailable);

    if (effectiveStatus == "检查中..." || effectiveStatus == "计算 Hash 中...") {
        setPreview(info.filePath);
    } else {
        if (m_restoringCache) setPreview(info.filePath);
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

void DownloadManager::updateSelectionSummary()
{
    if (m_page) m_page->updateSelectionSummary();
}

void DownloadManager::filterCards()
{
    if (!m_page) return;
    for (const QString &filePath : m_infos.keys()) {
        m_page->placeCardInCategory(filePath, m_page->categoryForStatus(m_page->cardStatusText(filePath)));
    }
}

void DownloadManager::sortCards()
{
    if (m_page) m_page->sortAllCards();
}

void DownloadManager::removeCard(const QString &filePath)
{
    m_infos.remove(filePath);
    if (m_page) m_page->removeCard(filePath);
    updateSelectionSummary();
    saveCache();
}

void DownloadManager::clearCompleted()
{
    if (!m_page) return;
    const QStringList keys = m_infos.keys();
    for (const QString &filePath : keys) {
        const QString status = m_page->cardStatusText(filePath);
        if (status.contains("完成") || status.contains("已是最新")) removeCard(filePath);
    }
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
    if (m_cacheLoaded) return;
    m_cacheLoaded = true;
    const QString cachePath = qApp->applicationDirPath() + "/config/downloads.json";
    QFile file(cachePath);
    if (!file.open(QIODevice::ReadOnly)) return;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    const QJsonArray items = doc.object().value("items").toArray();
    m_restoringCache = true;
    for (const QJsonValue &val : items) {
        const QJsonObject obj = val.toObject();
        ModelUpdateInfo info;
        info.filePath = obj.value("filePath").toString();
        if (info.filePath.isEmpty()) continue;
        info.modelDir = obj.value("modelDir").toString(QFileInfo(info.filePath).absolutePath());
        info.baseName = obj.value("baseName").toString(QFileInfo(info.filePath).completeBaseName());
        info.displayName = obj.value("displayName").toString(info.baseName);
        info.currentVersion = obj.value("currentVersion").toString();
        info.latestVersion = obj.value("latestVersion").toString();
        info.downloadUrl = obj.value("downloadUrl").toString();
        info.downloadFileName = obj.value("downloadFileName").toString();
        info.sha256 = obj.value("sha256").toString();
        info.metadataSource = obj.value("metadataSource").toString();
        info.sourceUrl = obj.value("sourceUrl").toString();
        info.latestVersionJson = obj.value("latestVersionJson").toObject();
        info.modelId = obj.value("modelId").toInt();
        info.currentVersionId = obj.value("currentVersionId").toInt();
        info.latestVersionId = obj.value("latestVersionId").toInt();
        info.sizeMB = obj.value("sizeMB").toDouble();
        info.hasUpdate = obj.value("hasUpdate").toBool(false);
        info.latestFileExistsLocally = obj.value("latestFileExistsLocally").toBool(false);
        info.previewState = static_cast<ModelPreviewState>(
            obj.value("previewState").toInt(static_cast<int>(ModelPreviewState::MissingOrUnknown)));

        const QString status = obj.value("status").toString(info.hasUpdate ? "发现新版本" : "已是最新");
        addOrUpdateCard(info, status, QFile::exists(info.filePath));
    }
    m_restoringCache = false;
    sortCards();
    emit statusMessageChanged(items.isEmpty() ? "暂无下载检查缓存。"
                                              : QString("已恢复上次下载列表，共 %1 个模型。").arg(items.size()));
    if (m_previewTimer && !m_pendingPreviewLoads.isEmpty()) m_previewTimer->start(0);
}

void DownloadManager::saveCache() const
{
    if (!m_cacheLoaded || !m_page) return;
    QJsonArray items;
    const QStringList categories = {"updates", "coexisting", "ignored", "latest", "errors", "local"};
    for (const QString &category : categories) {
        for (const QString &filePath : m_page->sortedFilePathsForCategory(category)) {
            if (!m_infos.contains(filePath)) continue;
            const ModelUpdateInfo info = m_infos.value(filePath);
            QString status = m_page->cardStatusText(filePath);
            if (status.contains("检查中") || status.contains("计算 Hash")) continue;
            if (status.contains("下载中") || status.contains("认证重试") || status.contains("队列")) {
                status = info.hasUpdate ? "发现新版本" : "已是最新";
            }

            QJsonObject obj;
            obj["filePath"] = info.filePath;
            obj["modelDir"] = info.modelDir;
            obj["baseName"] = info.baseName;
            obj["displayName"] = info.displayName;
            obj["currentVersion"] = info.currentVersion;
            obj["latestVersion"] = info.latestVersion;
            obj["downloadUrl"] = info.downloadUrl;
            obj["downloadFileName"] = info.downloadFileName;
            obj["sha256"] = info.sha256;
            obj["metadataSource"] = info.metadataSource;
            obj["sourceUrl"] = info.sourceUrl;
            obj["latestVersionJson"] = info.latestVersionJson;
            obj["modelId"] = info.modelId;
            obj["currentVersionId"] = info.currentVersionId;
            obj["latestVersionId"] = info.latestVersionId;
            obj["sizeMB"] = info.sizeMB;
            obj["hasUpdate"] = info.hasUpdate;
            obj["latestFileExistsLocally"] = info.latestFileExistsLocally;
            obj["previewState"] = static_cast<int>(info.previewState);
            obj["status"] = status;
            items.append(obj);
        }
    }

    const QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);
    QSaveFile file(configDir + "/downloads.json");
    QJsonObject root;
    root["version"] = 1;
    root["savedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    root["items"] = items;
    const QByteArray payload = QJsonDocument(root).toJson();
    if (!file.open(QIODevice::WriteOnly)
        || file.write(payload) != payload.size()
        || !file.commit()) {
        qWarning() << "Unable to save download cache atomically:" << file.errorString();
    }
}

void DownloadManager::shutdown()
{
    if (m_shuttingDown) return;
    m_shuttingDown = true;
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

void DownloadManager::setPreview(const QString &filePath, const QString &previewPath)
{
    if (!m_page) return;
    QPixmap pix;
    if (!previewPath.isEmpty() && QFile::exists(previewPath)) pix.load(previewPath);
    if (pix.isNull()) {
        // 预览缺失/加载失败：由 DownloadsPage 按模型状态绘制圆圈或叉号。
        ModelPreviewState state = m_infos.contains(filePath)
            ? m_infos.value(filePath).previewState
            : ModelPreviewState::MissingOrUnknown;
        if (state == ModelPreviewState::RealPreview) state = ModelPreviewState::MissingOrUnknown;
        if (m_infos.contains(filePath)) m_infos[filePath].previewState = state;
        m_page->setCardPreviewPlaceholder(filePath, state);
        return;
    }
    if (m_infos.contains(filePath)) m_infos[filePath].previewState = ModelPreviewState::RealPreview;
    m_page->setCardPreview(filePath, pix);
}

void DownloadManager::schedulePreviewLoad(const QString &filePath)
{
    if (filePath.isEmpty() || m_queuedPreviewLoads.contains(filePath)) return;
    if (!m_page || !m_page->containsCard(filePath) || !m_infos.contains(filePath)) return;
    m_queuedPreviewLoads.insert(filePath);
    m_pendingPreviewLoads.enqueue(filePath);
    if (m_previewTimer && !m_previewTimer->isActive()) m_previewTimer->start(0);
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
    QImage src(previewPath);
    if (src.isNull()) return result;

    const QSize targetSize(96, 128);
    QImage scaled = src.scaled(targetSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const int x = qMax(0, (scaled.width() - targetSize.width()) / 2);
    const int y = qMax(0, (scaled.height() - targetSize.height()) / 2);
    scaled = scaled.copy(x, y, targetSize.width(), targetSize.height());

    QImage rounded(targetSize, QImage::Format_ARGB32_Premultiplied);
    rounded.fill(Qt::transparent);
    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(QPointF(0, 0), QSizeF(targetSize)), 6, 6);
    painter.setClipPath(path);
    painter.drawImage(0, 0, scaled);
    painter.end();

    result.image = rounded;
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
    bool queued = false;
    for (const QString &filePath : selectedFilePaths()) {
        if (!m_infos.contains(filePath)) continue;
        const ModelUpdateInfo info = m_infos.value(filePath);
        if (!info.hasUpdate) continue;
        const QString status = cardStatusText(filePath);
        if (status.contains("已忽略") || status.contains("下载中") ||
            status.contains("认证重试") || status.contains("队列") ||
            status.contains("完成")) {
            continue;
        }
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
    return uniqueFilePath(info.modelDir, fileName);
}

void DownloadManager::ignoreSelectedUpdates()
{
    bool changed = false;
    for (const QString &filePath : selectedFilePaths()) {
        if (!m_infos.contains(filePath)) continue;
        if (cardStatusText(filePath).contains("已忽略")) continue;
        updateStatus(filePath, "已忽略更新");
        changed = true;
    }
    if (changed) {
        emit statusMessageChanged("已忽略选中的模型更新。");
        saveCache();
    }
}

void DownloadManager::retryFailedDownloads()
{
    bool queued = false;
    const QStringList keys = m_infos.keys();
    for (const QString &filePath : keys) {
        const QString status = cardStatusText(filePath);
        const bool legacyDownloadFailure = status.startsWith(QStringLiteral("失败:"))
                                           && !status.startsWith(QStringLiteral("检查失败:"));
        if (!status.startsWith(QStringLiteral("下载失败:")) && !legacyDownloadFailure) continue;
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

QString DownloadManager::uniqueFilePath(const QString &dirPath, const QString &fileName)
{
    QDir dir(dirPath);
    QString base = QFileInfo(fileName).completeBaseName();
    QString suffix = QFileInfo(fileName).suffix();
    QString candidate = dir.filePath(fileName);
    int index = 1;
    while (QFile::exists(candidate)) {
        const QString nextName = suffix.isEmpty()
            ? QString("%1_%2").arg(base).arg(index)
            : QString("%1_%2.%3").arg(base).arg(index).arg(suffix);
        candidate = dir.filePath(nextName);
        ++index;
    }
    return QFileInfo(candidate).absoluteFilePath();
}
