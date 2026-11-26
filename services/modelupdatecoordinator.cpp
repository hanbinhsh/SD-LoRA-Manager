#include "modelupdatecoordinator.h"
#include "utils/applanguage.h"
#include "utils/civitaiclient.h"
#include "utils/fileutils.h"
#include "utils/modelmetadatacodec.h"

#include <QDateTime>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent/QtConcurrent>

ModelUpdateCoordinator::ModelUpdateCoordinator(QNetworkAccessManager *netManager,
                                               QThreadPool *backgroundThreadPool,
                                               RequestFactory requestFactory,
                                               QObject *parent)
    : QObject(parent)
    , m_netManager(netManager)
    , m_backgroundThreadPool(backgroundThreadPool)
    , m_requestFactory(std::move(requestFactory))
{
}

void ModelUpdateCoordinator::cancelAll()
{
    ++m_token;
    m_pendingChecksQueue.clear();
    m_pendingHashChecks.clear();
    m_activeNetworkChecks = 0;
    m_activeHashChecks = 0;
    m_pendingChecks = 0;
    m_completedChecks = 0;
    m_detailPrompt = false;
    m_detailFilePath.clear();
}

void ModelUpdateCoordinator::startChecks(const QList<UpdateCheckSnapshot> &snapshots, bool detailPrompt)
{
    ++m_token;
    m_detailPrompt = detailPrompt && snapshots.size() == 1;
    m_detailFilePath = m_detailPrompt ? snapshots.first().filePath : QString();
    m_pendingChecksQueue.clear();
    m_pendingHashChecks.clear();
    m_activeNetworkChecks = 0;
    m_activeHashChecks = 0;
    m_completedChecks = 0;
    emit buttonsEnabledChanged(false);

    for (const UpdateCheckSnapshot &snapshot : snapshots) {
        if (!snapshot.filePath.isEmpty()) {
            m_pendingChecksQueue.enqueue(snapshot);
        }
    }
    m_pendingChecks = m_pendingChecksQueue.size();
    if (m_pendingChecks <= 0) {
        emit statusChanged(AppText::tr("没有可检查的模型。"));
        emit buttonsEnabledChanged(true);
        m_detailPrompt = false;
        m_detailFilePath.clear();
        return;
    }

    emit statusChanged(AppText::tr("正在检查 %1 个模型的更新...").arg(m_pendingChecks));
    QTimer::singleShot(0, this, &ModelUpdateCoordinator::dispatchQueuedChecks);
}

void ModelUpdateCoordinator::checkSnapshot(const UpdateCheckSnapshot &snapshot)
{
    if (snapshot.filePath.isEmpty()) {
        markCheckFinished();
        return;
    }

    ModelUpdateInfo pending;
    pending.filePath = snapshot.filePath;
    pending.modelDir = snapshot.modelDir;
    pending.baseName = snapshot.baseName;
    pending.displayName = snapshot.displayName;
    pending.modelId = snapshot.modelId;
    pending.currentVersionId = snapshot.currentVersionId;
    pending.previewState = snapshot.previewState;

    if (snapshot.localEdited) {
        emit cardUpdated(pending, QStringLiteral("本地/已编辑模型，已跳过"));
        markCheckFinished();
        return;
    }
    emit cardUpdated(pending, QStringLiteral("检查中..."));

    if (snapshot.modelId > 0) {
        const int token = m_token;
        const QUrl url(QStringLiteral("https://civitai.com/api/v1/models/%1").arg(snapshot.modelId));
        QNetworkReply *reply = m_netManager->get(m_requestFactory(url, true));
        connect(reply, &QNetworkReply::finished, this, [this, reply, snapshot, token]() {
            handleReply(reply, snapshot, token, false);
        });
        return;
    }

    enqueueHashCheck(snapshot);
}

void ModelUpdateCoordinator::dispatchQueuedChecks()
{
    // Civitai rate-limits bursts aggressively. Keep model checks serial; the next
    // item is dispatched after a short cooldown in markCheckFinished().
    if (m_activeNetworkChecks > 0 || m_pendingChecksQueue.isEmpty()) return;
    const UpdateCheckSnapshot snapshot = m_pendingChecksQueue.dequeue();
    ++m_activeNetworkChecks;
    checkSnapshot(snapshot);
}

void ModelUpdateCoordinator::enqueueHashCheck(const UpdateCheckSnapshot &snapshot)
{
    m_pendingHashChecks.enqueue(snapshot);
    ModelUpdateInfo pending;
    pending.filePath = snapshot.filePath;
    pending.modelDir = snapshot.modelDir;
    pending.baseName = snapshot.baseName;
    pending.displayName = snapshot.displayName;
    pending.previewState = snapshot.previewState;
    emit cardUpdated(pending, QStringLiteral("计算 Hash 中..."));
    dispatchHashChecks();
}

void ModelUpdateCoordinator::dispatchHashChecks()
{
    constexpr int kMaxConcurrentHashChecks = 2;
    while (m_activeHashChecks < kMaxConcurrentHashChecks && !m_pendingHashChecks.isEmpty()) {
        const UpdateCheckSnapshot snapshot = m_pendingHashChecks.dequeue();
        const int token = m_token;
        ++m_activeHashChecks;
        auto *watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, snapshot, token]() {
            const QString hash = watcher->result();
            watcher->deleteLater();
            m_activeHashChecks = qMax(0, m_activeHashChecks - 1);

            if (token != m_token) {
                dispatchHashChecks();
                return;
            }

            if (hash.isEmpty()) {
                ModelUpdateInfo pending;
                pending.filePath = snapshot.filePath;
                pending.modelDir = snapshot.modelDir;
                pending.baseName = snapshot.baseName;
                pending.displayName = snapshot.displayName;
                pending.previewState = snapshot.previewState;
                emit cardUpdated(pending, QStringLiteral("无法计算 Hash，无法判断"));
                markCheckFinished();
                dispatchHashChecks();
                return;
            }

            UpdateCheckSnapshot hashedSnapshot = snapshot;
            hashedSnapshot.currentSha256 = hash;
            const QUrl url(QStringLiteral("https://civitai.com/api/v1/model-versions/by-hash/%1").arg(hash));
            QNetworkReply *reply = m_netManager->get(m_requestFactory(url, true));
            connect(reply, &QNetworkReply::finished, this, [this, reply, hashedSnapshot, token]() {
                handleReply(reply, hashedSnapshot, token, true);
            });
            dispatchHashChecks();
        });
        watcher->setFuture(QtConcurrent::run(m_backgroundThreadPool, [filePath = snapshot.filePath]() {
            return FileUtils::calculateSha256Hex(filePath);
        }));
    }
}

void ModelUpdateCoordinator::markCheckFinished()
{
    if (m_pendingChecks <= 0) return;
    m_activeNetworkChecks = qMax(0, m_activeNetworkChecks - 1);
    ++m_completedChecks;
    emit statusChanged(AppText::tr("正在检查更新... %1/%2").arg(m_completedChecks).arg(m_pendingChecks));

    if (m_completedChecks >= m_pendingChecks && m_activeHashChecks == 0 && m_pendingHashChecks.isEmpty()) {
        emit statusChanged(AppText::tr("更新检查完成，共 %1 个模型。").arg(m_pendingChecks));
        emit buttonsEnabledChanged(true);
        const bool detailPrompt = m_detailPrompt;
        const QString detailFilePath = m_detailFilePath;
        m_detailPrompt = false;
        m_detailFilePath.clear();
        emit allChecksCompleted(m_pendingChecks, detailPrompt, detailFilePath);
        return;
    }

    constexpr int kUpdateCheckIntervalMs = 1200;
    QTimer::singleShot(kUpdateCheckIntervalMs, this, &ModelUpdateCoordinator::dispatchQueuedChecks);
}

void ModelUpdateCoordinator::handleReply(QNetworkReply *reply,
                                         const UpdateCheckSnapshot &snapshot,
                                         int token,
                                         bool byHash)
{
    if (!reply) return;
    reply->deleteLater();
    if (token > 0 && token != m_token) return;

    ModelUpdateInfo fallback;
    fallback.filePath = snapshot.filePath;
    fallback.baseName = snapshot.baseName;
    fallback.modelDir = snapshot.modelDir;
    fallback.displayName = snapshot.displayName.isEmpty()
        ? QFileInfo(snapshot.filePath).completeBaseName()
        : snapshot.displayName;
    fallback.previewState = snapshot.previewState;

    if (reply->error() != QNetworkReply::NoError) {
        emit cardUpdated(fallback, QStringLiteral("检查失败: ") + CivitaiClient::networkErrorMessage(reply));
        markCheckFinished();
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
    if (byHash) {
        const int modelId = root.value(QStringLiteral("modelId")).toInt();
        if (modelId <= 0) {
            emit cardUpdated(fallback, QStringLiteral("无法从 Hash 匹配 Civitai 模型"));
            markCheckFinished();
            return;
        }
        UpdateCheckSnapshot nextSnapshot = snapshot;
        nextSnapshot.modelId = modelId;
        nextSnapshot.currentVersionId = root.value(QStringLiteral("id")).toInt();
        QTimer::singleShot(600, this, [this, nextSnapshot, token]() {
            if (token > 0 && token != m_token) return;
            const QUrl url(QStringLiteral("https://civitai.com/api/v1/models/%1").arg(nextSnapshot.modelId));
            QNetworkReply *detailReply = m_netManager->get(m_requestFactory(url, true));
            connect(detailReply, &QNetworkReply::finished, this, [this, detailReply, nextSnapshot, token]() {
                handleReply(detailReply, nextSnapshot, token, false);
            });
        });
        return;
    }

    emit modelRootReceived(snapshot.filePath, snapshot.currentVersionId, root, fallback);
    markCheckFinished();
}

ModelUpdateInfo ModelUpdateCoordinator::parseModelUpdateInfo(const UpdateCheckSnapshot &snapshot,
                                                             const QJsonObject &modelRoot,
                                                             const CoexistenceCheck &hasLocalVersion)
{
    ModelUpdateInfo info;
    info.filePath = snapshot.filePath;
    info.modelDir = snapshot.modelDir;
    info.baseName = snapshot.baseName;
    info.displayName = snapshot.displayName;
    info.modelId = modelRoot.value(QStringLiteral("id")).toInt(snapshot.modelId);
    info.currentVersionId = snapshot.currentVersionId;
    const QString currentSha = snapshot.currentSha256;
    info.metadataSource = modelRoot.value(QStringLiteral("metadataSource")).toString();
    info.sourceUrl = modelRoot.value(QStringLiteral("sourceUrl")).toString();
    info.previewState = snapshot.previewState;

    const QJsonArray versions = modelRoot.value(QStringLiteral("modelVersions")).toArray();
    QJsonObject currentVersionObj;
    QJsonObject latestVersionObj;
    QDateTime latestTime;
    for (const QJsonValue &val : versions) {
        const QJsonObject version = val.toObject();
        const int versionId = version.value(QStringLiteral("id")).toInt();
        if (versionId == info.currentVersionId) currentVersionObj = version;
        if (info.currentVersionId <= 0 && !currentSha.isEmpty()) {
            for (const QJsonValue &fileVal : version.value(QStringLiteral("files")).toArray()) {
                const QString sha = fileVal.toObject().value(QStringLiteral("hashes")).toObject().value(QStringLiteral("SHA256")).toString();
                if (!sha.isEmpty() && sha.compare(currentSha, Qt::CaseInsensitive) == 0) {
                    currentVersionObj = version;
                    info.currentVersionId = versionId;
                }
            }
        }
        QDateTime t = QDateTime::fromString(version.value(QStringLiteral("publishedAt")).toString(), Qt::ISODate);
        if (!t.isValid()) t = QDateTime::fromString(version.value(QStringLiteral("createdAt")).toString(), Qt::ISODate);
        if (!latestVersionObj.isEmpty() && (!t.isValid() || (latestTime.isValid() && t <= latestTime))) continue;
        latestVersionObj = version;
        latestTime = t;
    }

    if (latestVersionObj.isEmpty() && !versions.isEmpty()) latestVersionObj = versions.first().toObject();
    latestVersionObj = ModelMetadata::mergeCivitaiModelIntoVersion(latestVersionObj, modelRoot);
    if (!latestVersionObj.contains(QStringLiteral("modelId"))) latestVersionObj[QStringLiteral("modelId")] = info.modelId;
    info.latestVersionJson = latestVersionObj;
    info.latestVersionId = latestVersionObj.value(QStringLiteral("id")).toInt();
    info.latestVersion = latestVersionObj.value(QStringLiteral("name")).toString();
    info.currentVersion = currentVersionObj.value(QStringLiteral("name")).toString();
    if (info.currentVersion.isEmpty()) info.currentVersion = QStringLiteral("版本ID %1").arg(info.currentVersionId);
    if (info.latestVersion.isEmpty()) info.latestVersion = QStringLiteral("版本ID %1").arg(info.latestVersionId);

    QJsonObject selectedFile;
    for (const QJsonValue &fileVal : latestVersionObj.value(QStringLiteral("files")).toArray()) {
        const QJsonObject f = fileVal.toObject();
        if (selectedFile.isEmpty() || f.value(QStringLiteral("primary")).toBool()) selectedFile = f;
        if (f.value(QStringLiteral("primary")).toBool()) break;
    }
    info.downloadUrl = selectedFile.value(QStringLiteral("downloadUrl")).toString();
    info.downloadFileName = selectedFile.value(QStringLiteral("name")).toString();
    info.sha256 = selectedFile.value(QStringLiteral("hashes")).toObject().value(QStringLiteral("SHA256")).toString();
    if (info.sha256.isEmpty()) info.sha256 = currentSha;
    info.sizeMB = selectedFile.value(QStringLiteral("sizeKB")).toDouble() / 1024.0;
    info.hasUpdate = info.latestVersionId > 0
        && info.latestVersionId != info.currentVersionId
        && !info.downloadUrl.isEmpty();
    if (info.hasUpdate && hasLocalVersion) {
        info.latestFileExistsLocally = hasLocalVersion(info.modelId, info.latestVersionId);
    }
    return info;
}
