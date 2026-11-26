#include "utils/foldersyncengine.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QUuid>
#include <algorithm>
#include <cstdlib>

namespace {

constexpr qint64 kTimestampToleranceMs = 2000;

#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kPathCaseSensitivity = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCaseSensitivity = Qt::CaseSensitive;
#endif

struct ScannedFileEntry {
    QString relativePath;
    QString absolutePath;
    qint64 size = 0;
    qint64 modifiedMs = 0;
};

QString lookupKeyForRelativePath(const QString &relativePath)
{
#ifdef Q_OS_WIN
    return relativePath.toLower();
#else
    return relativePath;
#endif
}

QHash<QString, ScannedFileEntry> scanDirectoryFiles(const QString &rootDirPath, bool includeSubdirs)
{
    QHash<QString, ScannedFileEntry> result;
    const QFileInfo rootInfo(rootDirPath);
    if (!rootInfo.exists() || !rootInfo.isDir()) {
        return result;
    }

    const QDir rootDir(rootInfo.absoluteFilePath());
    const QDirIterator::IteratorFlags flags = includeSubdirs
        ? QDirIterator::Subdirectories
        : QDirIterator::NoIteratorFlags;

    QDirIterator it(rootDir.absolutePath(), QDir::Files | QDir::NoDotAndDotDot, flags);
    while (it.hasNext()) {
        const QString absPath = it.next();
        const QFileInfo fi(absPath);
        const QString relPath = QDir::fromNativeSeparators(rootDir.relativeFilePath(absPath));
        if (relPath.isEmpty() || relPath.startsWith(QStringLiteral("../"))) {
            continue;
        }
        ScannedFileEntry entry;
        entry.relativePath = relPath;
        entry.absolutePath = fi.absoluteFilePath();
        entry.size = fi.size();
        entry.modifiedMs = fi.lastModified().toMSecsSinceEpoch();
        result.insert(lookupKeyForRelativePath(relPath), entry);
    }
    return result;
}

bool copyFileAtomically(const QString &sourcePath, const QString &targetPath, QString *errorOut)
{
    const QFileInfo srcInfo(sourcePath);
    if (!srcInfo.exists() || !srcInfo.isFile()) {
        if (errorOut) *errorOut = QStringLiteral("Source file does not exist: %1").arg(sourcePath);
        return false;
    }

    const QFileInfo dstInfo(targetPath);
    const QDir dstDir = dstInfo.dir();
    if (!dstDir.exists() && !dstDir.mkpath(QStringLiteral("."))) {
        if (errorOut) *errorOut = QStringLiteral("Failed to create target directory: %1").arg(dstDir.absolutePath());
        return false;
    }

    const QString suffix = QUuid::createUuid().toString(QUuid::Id128).left(8);
    const QString tempPath = targetPath + QStringLiteral(".sdlm_tmp_") + suffix;
    QFile::remove(tempPath);

    if (!QFile::copy(sourcePath, tempPath)) {
        QFile::remove(tempPath);
        if (errorOut) *errorOut = QStringLiteral("Failed to copy '%1' to '%2'").arg(sourcePath, targetPath);
        return false;
    }

    // Preserve modification timestamp so subsequent sync comparisons skip unchanged files cleanly.
    const QDateTime srcMtime = srcInfo.lastModified();
    if (srcMtime.isValid()) {
        QFile tmpFile(tempPath);
        if (tmpFile.open(QIODevice::ReadWrite | QIODevice::ExistingOnly)) {
            tmpFile.setFileTime(srcMtime, QFileDevice::FileModificationTime);
            tmpFile.close();
        }
    }

    if (QFile::exists(targetPath)) {
        const QString backupPath = targetPath + QStringLiteral(".sdlm_bak_") + suffix;
        QFile::remove(backupPath);
        if (!QFile::rename(targetPath, backupPath)) {
            QFile::remove(tempPath);
            if (errorOut) *errorOut = QStringLiteral("Failed to replace existing target file: %1").arg(targetPath);
            return false;
        }
        if (!QFile::rename(tempPath, targetPath)) {
            QFile::rename(backupPath, targetPath);
            QFile::remove(tempPath);
            if (errorOut) *errorOut = QStringLiteral("Failed to finalize target file: %1").arg(targetPath);
            return false;
        }
        QFile::remove(backupPath);
    } else {
        if (!QFile::rename(tempPath, targetPath)) {
            QFile::remove(tempPath);
            if (errorOut) *errorOut = QStringLiteral("Failed to move staged file into place: %1").arg(targetPath);
            return false;
        }
    }

    return true;
}

bool moveFileSafely(const QString &sourcePath, const QString &targetPath, QString *errorOut)
{
    const QFileInfo dstInfo(targetPath);
    const QDir dstDir = dstInfo.dir();
    if (!dstDir.exists() && !dstDir.mkpath(QStringLiteral("."))) {
        if (errorOut) *errorOut = QStringLiteral("Failed to create target directory: %1").arg(dstDir.absolutePath());
        return false;
    }

    // Fast path: same volume and destination does not exist yet.
    if (!QFile::exists(targetPath) && QFile::rename(sourcePath, targetPath)) {
        return true;
    }

    // Fallback: atomic copy + remove source (works across volumes and when replacing existing files).
    if (!copyFileAtomically(sourcePath, targetPath, errorOut)) {
        return false;
    }

    if (!QFile::remove(sourcePath)) {
        if (errorOut) *errorOut = QStringLiteral("Copied to target but failed to remove source file: %1").arg(sourcePath);
        return false;
    }

    return true;
}

int removeEmptySubdirectories(const QString &rootDirPath)
{
    QStringList subDirs;
    QDirIterator it(rootDirPath, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        subDirs.append(it.next());
    }
    std::sort(subDirs.begin(), subDirs.end(), [](const QString &a, const QString &b) {
        return a.size() > b.size();
    });

    int removed = 0;
    QDir helper;
    for (const QString &dirPath : subDirs) {
        if (helper.rmdir(dirPath)) {
            ++removed;
        }
    }
    return removed;
}

} // namespace

namespace FolderSyncEngine {

QString normalizeDirPath(const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    const QFileInfo info(trimmed);
    const QString canonical = info.canonicalFilePath();
    const QString raw = canonical.isEmpty() ? info.absoluteFilePath() : canonical;
    QString cleaned = QDir::cleanPath(QDir::fromNativeSeparators(raw));
    while (cleaned.size() > 1 && cleaned.endsWith(QLatin1Char('/'))) {
        if (cleaned.size() == 3 && cleaned.at(1) == QLatin1Char(':')) {
            break;
        }
        cleaned.chop(1);
    }
    return cleaned;
}

bool isSameOrNestedDirectory(const QString &dirA, const QString &dirB)
{
    const QString normA = normalizeDirPath(dirA);
    const QString normB = normalizeDirPath(dirB);
    if (normA.isEmpty() || normB.isEmpty()) {
        return false;
    }
    if (normA.compare(normB, kPathCaseSensitivity) == 0) {
        return true;
    }
    const QString prefixA = normA.endsWith(QLatin1Char('/')) ? normA : (normA + QLatin1Char('/'));
    const QString prefixB = normB.endsWith(QLatin1Char('/')) ? normB : (normB + QLatin1Char('/'));
    return normB.startsWith(prefixA, kPathCaseSensitivity)
        || normA.startsWith(prefixB, kPathCaseSensitivity);
}

FolderSyncPlan planSync(const QString &folderA,
                        const QString &folderB,
                        FolderSyncMode mode,
                        bool includeSubdirs)
{
    FolderSyncPlan plan;
    plan.mode = mode;
    plan.includeSubdirs = includeSubdirs;

    const QString trimmedA = folderA.trimmed();
    const QString trimmedB = folderB.trimmed();
    if (trimmedA.isEmpty() || trimmedB.isEmpty()) {
        plan.errorMessage = QStringLiteral("请先选择有效的文件夹 A 和文件夹 B。");
        return plan;
    }

    const QFileInfo infoA(trimmedA);
    if (!infoA.exists() || !infoA.isDir()) {
        plan.errorMessage = QStringLiteral("文件夹 A 不存在或不是有效目录：%1").arg(trimmedA);
        return plan;
    }

    const QFileInfo infoB(trimmedB);
    if (infoB.exists() && !infoB.isDir()) {
        plan.errorMessage = QStringLiteral("文件夹 B 路径指向的是文件而不是目录：%1").arg(trimmedB);
        return plan;
    }

    if (isSameOrNestedDirectory(trimmedA, trimmedB)) {
        plan.errorMessage = QStringLiteral("文件夹 A 与文件夹 B 不能相同，也不能互相嵌套。");
        return plan;
    }

    plan.folderA = normalizeDirPath(trimmedA);
    plan.folderB = normalizeDirPath(trimmedB);

    const QHash<QString, ScannedFileEntry> filesA = scanDirectoryFiles(plan.folderA, includeSubdirs);
    const QHash<QString, ScannedFileEntry> filesB = infoB.exists()
        ? scanDirectoryFiles(plan.folderB, includeSubdirs)
        : QHash<QString, ScannedFileEntry>{};

    const QDir dirA(plan.folderA);
    const QDir dirB(plan.folderB);

    QStringList allKeys = filesA.keys();
    if (mode == FolderSyncMode::TwoWaySync) {
        for (auto it = filesB.cbegin(); it != filesB.cend(); ++it) {
            if (!filesA.contains(it.key())) {
                allKeys.append(it.key());
            }
        }
    }
    std::sort(allKeys.begin(), allKeys.end());

    plan.actions.reserve(allKeys.size());

    for (const QString &key : allKeys) {
        const bool inA = filesA.contains(key);
        const bool inB = filesB.contains(key);

        if (mode == FolderSyncMode::OneWayCopy) {
            const ScannedFileEntry &entryA = filesA[key];
            FolderSyncAction action;
            action.relativePath = entryA.relativePath;
            action.sourcePath = entryA.absolutePath;
            action.targetPath = QDir::cleanPath(dirB.filePath(entryA.relativePath));
            action.fileSize = entryA.size;

            if (!inB) {
                action.type = FolderSyncActionType::CopyAtoB;
                action.reason = QStringLiteral("B 中不存在");
                ++plan.copyAtoBCount;
                plan.totalTransferBytes += entryA.size;
            } else {
                const ScannedFileEntry &entryB = filesB[key];
                if (entryA.size == entryB.size && entryB.modifiedMs + kTimestampToleranceMs >= entryA.modifiedMs) {
                    action.type = FolderSyncActionType::Skip;
                    action.reason = QStringLiteral("目标已是最新且大小一致");
                    ++plan.skipCount;
                } else {
                    action.type = FolderSyncActionType::CopyAtoB;
                    action.reason = QStringLiteral("A 文件更新或大小不同");
                    ++plan.copyAtoBCount;
                    plan.totalTransferBytes += entryA.size;
                }
            }
            plan.actions.append(action);
        } else if (mode == FolderSyncMode::OneWayMove) {
            const ScannedFileEntry &entryA = filesA[key];
            FolderSyncAction action;
            action.type = FolderSyncActionType::MoveAtoB;
            action.relativePath = entryA.relativePath;
            action.sourcePath = entryA.absolutePath;
            action.targetPath = QDir::cleanPath(dirB.filePath(entryA.relativePath));
            action.fileSize = entryA.size;
            action.reason = inB ? QStringLiteral("移动并覆盖 B 中同名文件") : QStringLiteral("移动到 B");
            ++plan.moveAtoBCount;
            plan.totalTransferBytes += entryA.size;
            plan.actions.append(action);
        } else { // TwoWaySync
            if (inA && !inB) {
                const ScannedFileEntry &entryA = filesA[key];
                FolderSyncAction action;
                action.type = FolderSyncActionType::CopyAtoB;
                action.relativePath = entryA.relativePath;
                action.sourcePath = entryA.absolutePath;
                action.targetPath = QDir::cleanPath(dirB.filePath(entryA.relativePath));
                action.fileSize = entryA.size;
                action.reason = QStringLiteral("仅存在于 A");
                ++plan.copyAtoBCount;
                plan.totalTransferBytes += entryA.size;
                plan.actions.append(action);
            } else if (!inA && inB) {
                const ScannedFileEntry &entryB = filesB[key];
                FolderSyncAction action;
                action.type = FolderSyncActionType::CopyBtoA;
                action.relativePath = entryB.relativePath;
                action.sourcePath = entryB.absolutePath;
                action.targetPath = QDir::cleanPath(dirA.filePath(entryB.relativePath));
                action.fileSize = entryB.size;
                action.reason = QStringLiteral("仅存在于 B");
                ++plan.copyBtoACount;
                plan.totalTransferBytes += entryB.size;
                plan.actions.append(action);
            } else if (inA && inB) {
                const ScannedFileEntry &entryA = filesA[key];
                const ScannedFileEntry &entryB = filesB[key];
                const qint64 timeDiff = entryA.modifiedMs - entryB.modifiedMs;

                FolderSyncAction action;
                action.relativePath = entryA.relativePath;
                if (entryA.size == entryB.size && std::llabs(timeDiff) <= kTimestampToleranceMs) {
                    action.type = FolderSyncActionType::Skip;
                    action.sourcePath = entryA.absolutePath;
                    action.targetPath = entryB.absolutePath;
                    action.fileSize = entryA.size;
                    action.reason = QStringLiteral("两端一致");
                    ++plan.skipCount;
                } else if (timeDiff > kTimestampToleranceMs
                           || (std::llabs(timeDiff) <= kTimestampToleranceMs && entryA.modifiedMs >= entryB.modifiedMs)) {
                    action.type = FolderSyncActionType::CopyAtoB;
                    action.sourcePath = entryA.absolutePath;
                    action.targetPath = entryB.absolutePath;
                    action.fileSize = entryA.size;
                    action.reason = QStringLiteral("A 较新");
                    ++plan.copyAtoBCount;
                    plan.totalTransferBytes += entryA.size;
                } else {
                    action.type = FolderSyncActionType::CopyBtoA;
                    action.sourcePath = entryB.absolutePath;
                    action.targetPath = entryA.absolutePath;
                    action.fileSize = entryB.size;
                    action.reason = QStringLiteral("B 较新");
                    ++plan.copyBtoACount;
                    plan.totalTransferBytes += entryB.size;
                }
                plan.actions.append(action);
            }
        }
    }

    plan.valid = true;
    return plan;
}

FolderSyncResult executeSync(
    const FolderSyncPlan &plan,
    const std::function<void(int done, int total, const QString &message)> &progressCallback,
    const std::atomic_bool *cancelFlag)
{
    FolderSyncResult result;
    if (!plan.valid) {
        result.errors.append(plan.errorMessage.isEmpty() ? QStringLiteral("同步计划无效。") : plan.errorMessage);
        result.failedCount = 1;
        return result;
    }

    // Ensure destination directory B exists (even if folder A is currently empty).
    QDir dirB(plan.folderB);
    if (!dirB.exists() && !dirB.mkpath(QStringLiteral("."))) {
        result.errors.append(QStringLiteral("无法创建目标文件夹 B：%1").arg(plan.folderB));
        result.failedCount = 1;
        return result;
    }

    const int totalTransfers = plan.pendingTransferCount();
    int doneTransfers = 0;

    for (const FolderSyncAction &action : plan.actions) {
        if (cancelFlag && cancelFlag->load()) {
            result.canceled = true;
            break;
        }

        if (action.type == FolderSyncActionType::Skip) {
            ++result.skippedCount;
            continue;
        }

        QString err;
        bool ok = false;
        QString verb;
        if (action.type == FolderSyncActionType::CopyAtoB) {
            ok = copyFileAtomically(action.sourcePath, action.targetPath, &err);
            verb = QStringLiteral("[A → B 复制]");
            if (ok) {
                ++result.copiedCount;
                result.transferredBytes += action.fileSize;
            }
        } else if (action.type == FolderSyncActionType::CopyBtoA) {
            ok = copyFileAtomically(action.sourcePath, action.targetPath, &err);
            verb = QStringLiteral("[B → A 复制]");
            if (ok) {
                ++result.copiedCount;
                result.transferredBytes += action.fileSize;
            }
        } else if (action.type == FolderSyncActionType::MoveAtoB) {
            ok = moveFileSafely(action.sourcePath, action.targetPath, &err);
            verb = QStringLiteral("[A → B 移动]");
            if (ok) {
                ++result.movedCount;
                result.transferredBytes += action.fileSize;
            }
        }

        ++doneTransfers;
        if (!ok) {
            ++result.failedCount;
            const QString errMsg = QStringLiteral("%1 失败: %2 (%3)").arg(verb, action.relativePath, err);
            result.errors.append(errMsg);
            if (progressCallback) {
                progressCallback(doneTransfers, totalTransfers, errMsg);
            }
        } else if (progressCallback) {
            progressCallback(doneTransfers, totalTransfers,
                             QStringLiteral("%1 %2").arg(verb, action.relativePath));
        }
    }

    if (plan.mode == FolderSyncMode::OneWayMove && !result.canceled && plan.includeSubdirs) {
        result.removedEmptyDirs = removeEmptySubdirectories(plan.folderA);
    }

    result.success = (!result.canceled && result.failedCount == 0);
    return result;
}

} // namespace FolderSyncEngine
