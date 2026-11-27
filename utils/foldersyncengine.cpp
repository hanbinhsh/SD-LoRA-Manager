#include "utils/foldersyncengine.h"

#include "utils/imagemetadataparser.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
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
    QString parentDir;
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

QHash<QString, ScannedFileEntry> scanDirectoryFiles(
    const QString &rootDirPath,
    bool includeSubdirs,
    const QString &sideLabel,
    const FolderSyncEngine::ProgressCallback &progressCallback,
    const std::atomic_bool *cancelFlag,
    bool *canceledOut)
{
    QHash<QString, ScannedFileEntry> result;
    if (canceledOut) *canceledOut = false;

    const QFileInfo rootInfo(rootDirPath);
    if (!rootInfo.exists() || !rootInfo.isDir()) {
        return result;
    }

    const QDir rootDir(rootInfo.absoluteFilePath());
    const QDirIterator::IteratorFlags flags = includeSubdirs
        ? QDirIterator::Subdirectories
        : QDirIterator::NoIteratorFlags;

    QElapsedTimer throttle;
    throttle.start();
    int scanned = 0;

    QDirIterator it(rootDir.absolutePath(), QDir::Files | QDir::NoDotAndDotDot, flags);
    while (it.hasNext()) {
        if (cancelFlag && cancelFlag->load()) {
            if (canceledOut) *canceledOut = true;
            return {};
        }
        const QString absPath = it.next();
        const QFileInfo fi(absPath);
        const QString relPath = QDir::fromNativeSeparators(rootDir.relativeFilePath(absPath));
        if (relPath.isEmpty() || relPath.startsWith(QStringLiteral("../"))) {
            continue;
        }
        ScannedFileEntry entry;
        entry.relativePath = relPath;
        entry.absolutePath = fi.absoluteFilePath();
        entry.parentDir = QDir::cleanPath(QDir::fromNativeSeparators(fi.absolutePath()));
        entry.size = fi.size();
        entry.modifiedMs = fi.lastModified().toMSecsSinceEpoch();
        result.insert(lookupKeyForRelativePath(relPath), entry);
        ++scanned;

        if (progressCallback && (scanned % 200 == 0 || throttle.elapsed() >= 150)) {
            throttle.restart();
            progressCallback(scanned, 0,
                             QStringLiteral("正在扫描文件夹 %1（已发现 %2 个文件）...").arg(sideLabel).arg(scanned));
        }
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

    if (!QFile::exists(targetPath) && QFile::rename(sourcePath, targetPath)) {
        return true;
    }

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

bool isSupportedImageFile(const QString &fileName)
{
    static const QSet<QString> kImageExtensions = {
        QStringLiteral("png"),
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("webp"),
        QStringLiteral("bmp"),
        QStringLiteral("gif"),
        QStringLiteral("tif"),
        QStringLiteral("tiff"),
        QStringLiteral("avif"),
    };
    const QString ext = QFileInfo(fileName).suffix().toLower();
    return !ext.isEmpty() && kImageExtensions.contains(ext);
}

QStringList parseNameKeywords(const QString &text)
{
    static const QRegularExpression sep(QStringLiteral("[,，;；\\r\\n]+"));
    const QStringList raw = text.split(sep, Qt::SkipEmptyParts);
    QStringList out;
    out.reserve(raw.size());
    for (const QString &part : raw) {
        const QString clean = part.trimmed();
        if (!clean.isEmpty()) {
            out.append(clean);
        }
    }
    return out;
}

QStringList parseExtensions(const QString &text)
{
    static const QRegularExpression sep(QStringLiteral("[,，;；\\s]+"));
    const QStringList raw = text.split(sep, Qt::SkipEmptyParts);
    QStringList out;
    QSet<QString> seen;
    for (const QString &part : raw) {
        QString clean = part.trimmed().toLower();
        while (clean.startsWith(QLatin1Char('*')) || clean.startsWith(QLatin1Char('.'))) {
            clean.remove(0, 1);
        }
        if (!clean.isEmpty() && !seen.contains(clean)) {
            seen.insert(clean);
            out.append(clean);
        }
    }
    return out;
}

QString validateInputs(const QString &folderA, const QString &folderB, const FolderSyncFilter &filter)
{
    const QString trimmedA = folderA.trimmed();
    const QString trimmedB = folderB.trimmed();
    if (trimmedA.isEmpty() || trimmedB.isEmpty()) {
        return QStringLiteral("请先选择有效的文件夹 A 和文件夹 B。");
    }

    const QFileInfo infoA(trimmedA);
    if (!infoA.exists() || !infoA.isDir()) {
        return QStringLiteral("文件夹 A 不存在或不是有效目录：%1").arg(trimmedA);
    }

    const QFileInfo infoB(trimmedB);
    if (infoB.exists() && !infoB.isDir()) {
        return QStringLiteral("文件夹 B 路径指向的是文件而不是目录：%1").arg(trimmedB);
    }

    if (isSameOrNestedDirectory(trimmedA, trimmedB)) {
        return QStringLiteral("文件夹 A 与文件夹 B 不能相同，也不能互相嵌套。");
    }

    if (filter.timeMode == FolderSyncTimeFilterMode::Before && !filter.endTime.isValid()) {
        return QStringLiteral("请设置有效的截止时间。");
    }
    if (filter.timeMode == FolderSyncTimeFilterMode::After && !filter.startTime.isValid()) {
        return QStringLiteral("请设置有效的起始时间。");
    }
    if (filter.timeMode == FolderSyncTimeFilterMode::Between) {
        if (!filter.startTime.isValid() || !filter.endTime.isValid()) {
            return QStringLiteral("请设置有效的起始时间和截止时间。");
        }
        if (filter.startTime > filter.endTime) {
            return QStringLiteral("起始时间不能晚于截止时间。");
        }
    }

    if (filter.minSizeKB > 0 && filter.maxSizeKB > 0 && filter.minSizeKB > filter.maxSizeKB) {
        return QStringLiteral("最小文件大小不能大于最大文件大小。");
    }

    return QString();
}

bool matchesTimeRange(qint64 modifiedMs, const FolderSyncFilter &filter)
{
    switch (filter.timeMode) {
    case FolderSyncTimeFilterMode::All:
        return true;
    case FolderSyncTimeFilterMode::Before:
        if (!filter.endTime.isValid()) return false;
        return modifiedMs <= filter.endTime.toMSecsSinceEpoch() + 999;
    case FolderSyncTimeFilterMode::After:
        if (!filter.startTime.isValid()) return false;
        return modifiedMs >= filter.startTime.toMSecsSinceEpoch();
    case FolderSyncTimeFilterMode::Between:
        if (!filter.startTime.isValid() || !filter.endTime.isValid()) return false;
        return modifiedMs >= filter.startTime.toMSecsSinceEpoch()
            && modifiedMs <= filter.endTime.toMSecsSinceEpoch() + 999;
    }
    return true;
}

bool evaluateNonTimeFilter(const QString &absPath,
                           const QString &fileName,
                           qint64 sizeBytes,
                           const FolderSyncFilter &filter,
                           QString *excludeReasonOut)
{
    const bool isImg = isSupportedImageFile(fileName);
    if (filter.imagesOnly && !isImg) {
        if (excludeReasonOut) *excludeReasonOut = QStringLiteral("非图片文件");
        return false;
    }

    if (filter.extMode != FolderSyncExtFilterMode::None && !filter.extensions.isEmpty()) {
        const QString ext = QFileInfo(fileName).suffix().toLower();
        const bool matched = !ext.isEmpty() && filter.extensions.contains(ext);
        if (filter.extMode == FolderSyncExtFilterMode::IncludeOnly && !matched) {
            if (excludeReasonOut) *excludeReasonOut = QStringLiteral("文件后缀不在允许列表");
            return false;
        }
        if (filter.extMode == FolderSyncExtFilterMode::Exclude && matched) {
            if (excludeReasonOut) *excludeReasonOut = QStringLiteral("文件后缀命中排除列表: .%1").arg(ext);
            return false;
        }
    }

    if (filter.minSizeKB > 0 && sizeBytes < filter.minSizeKB * 1024LL) {
        if (excludeReasonOut) *excludeReasonOut = QStringLiteral("文件小于最小大小限制");
        return false;
    }
    if (filter.maxSizeKB > 0 && sizeBytes > filter.maxSizeKB * 1024LL) {
        if (excludeReasonOut) *excludeReasonOut = QStringLiteral("文件超过最大大小限制");
        return false;
    }

    if (filter.nameMode != FolderSyncNameFilterMode::None && !filter.nameKeywords.isEmpty()) {
        const QString baseFileName = QFileInfo(fileName).fileName();
        QString matchedKeyword;
        for (const QString &kw : filter.nameKeywords) {
            if (!kw.isEmpty() && baseFileName.contains(kw, Qt::CaseInsensitive)) {
                matchedKeyword = kw;
                break;
            }
        }
        if (filter.nameMode == FolderSyncNameFilterMode::ExcludeContains && !matchedKeyword.isEmpty()) {
            if (excludeReasonOut) *excludeReasonOut = QStringLiteral("文件名包含排除词: %1").arg(matchedKeyword);
            return false;
        }
        if (filter.nameMode == FolderSyncNameFilterMode::IncludeContains && matchedKeyword.isEmpty()) {
            if (excludeReasonOut) *excludeReasonOut = QStringLiteral("文件名未命中包含词");
            return false;
        }
    }

    if (filter.requireImageMetadata && isImg) {
        const ParsedImageMetadata meta = parseImageMetadataFromFile(absPath);
        if (!meta.hasContent()) {
            if (excludeReasonOut) *excludeReasonOut = QStringLiteral("图片无生成元信息");
            return false;
        }
    }

    return true;
}

bool matchesFilter(const QString &absPath,
                   const QString &fileName,
                   qint64 sizeBytes,
                   qint64 modifiedMs,
                   const FolderSyncFilter &filter,
                   QString *excludeReasonOut)
{
    if (!matchesTimeRange(modifiedMs, filter)) {
        if (excludeReasonOut) *excludeReasonOut = QStringLiteral("不在选定时间范围内");
        return false;
    }
    return evaluateNonTimeFilter(absPath, fileName, sizeBytes, filter, excludeReasonOut);
}

QStringList collectWatchDirectories(const QStringList &rootDirs,
                                    bool includeSubdirs,
                                    int maxDirectories,
                                    const std::atomic_bool *cancelFlag)
{
    QStringList dirs;
    QSet<QString> seen;

    auto addUnique = [&](const QString &dirPath) {
        const QString clean = normalizeDirPath(dirPath);
        if (clean.isEmpty()) return;
        const QString key = lookupKeyForRelativePath(clean);
        if (!seen.contains(key)) {
            seen.insert(key);
            dirs.append(clean);
        }
    };

    for (const QString &root : rootDirs) {
        if (cancelFlag && cancelFlag->load()) return {};
        const QFileInfo fi(root);
        if (!fi.exists() || !fi.isDir()) continue;
        addUnique(fi.absoluteFilePath());
        if (maxDirectories > 0 && dirs.size() >= maxDirectories) break;

        if (includeSubdirs) {
            QDirIterator it(fi.absoluteFilePath(), QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                if (cancelFlag && cancelFlag->load()) return {};
                addUnique(it.next());
                if (maxDirectories > 0 && dirs.size() >= maxDirectories) break;
            }
        }
    }
    return dirs;
}

FolderSyncPlan planSync(const QString &folderA,
                        const QString &folderB,
                        FolderSyncMode mode,
                        bool includeSubdirs,
                        const FolderSyncFilter &filter,
                        const ProgressCallback &progressCallback,
                        const std::atomic_bool *cancelFlag)
{
    FolderSyncPlan plan;
    plan.mode = mode;
    plan.includeSubdirs = includeSubdirs;
    plan.filter = filter;

    const QString validationError = validateInputs(folderA, folderB, filter);
    if (!validationError.isEmpty()) {
        plan.errorMessage = validationError;
        return plan;
    }

    plan.folderA = normalizeDirPath(folderA);
    plan.folderB = normalizeDirPath(folderB);

    bool canceled = false;
    const QHash<QString, ScannedFileEntry> filesA = scanDirectoryFiles(
        plan.folderA, includeSubdirs, QStringLiteral("A"), progressCallback, cancelFlag, &canceled);
    if (canceled) {
        plan.canceled = true;
        return plan;
    }

    const QFileInfo infoB(plan.folderB);
    const QHash<QString, ScannedFileEntry> filesB = infoB.exists()
        ? scanDirectoryFiles(plan.folderB, includeSubdirs, QStringLiteral("B"), progressCallback, cancelFlag, &canceled)
        : QHash<QString, ScannedFileEntry>{};
    if (canceled) {
        plan.canceled = true;
        return plan;
    }

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

    const int totalKeys = allKeys.size();
    plan.actions.reserve(totalKeys);

    QElapsedTimer throttle;
    throttle.start();
    int processed = 0;

    for (const QString &key : allKeys) {
        if (cancelFlag && cancelFlag->load()) {
            plan.canceled = true;
            return plan;
        }

        const bool inA = filesA.contains(key);
        const bool inB = filesB.contains(key);

        if (mode == FolderSyncMode::OneWayCopy) {
            const ScannedFileEntry &entryA = filesA[key];
            if (!matchesFilter(entryA.absolutePath, entryA.relativePath, entryA.size, entryA.modifiedMs, filter)) {
                ++plan.filteredOutCount;
            } else {
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
            }
        } else if (mode == FolderSyncMode::OneWayMove) {
            const ScannedFileEntry &entryA = filesA[key];
            if (!matchesFilter(entryA.absolutePath, entryA.relativePath, entryA.size, entryA.modifiedMs, filter)) {
                ++plan.filteredOutCount;
            } else {
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
            }
        } else { // TwoWaySync
            if (inA && !inB) {
                const ScannedFileEntry &entryA = filesA[key];
                if (!matchesFilter(entryA.absolutePath, entryA.relativePath, entryA.size, entryA.modifiedMs, filter)) {
                    ++plan.filteredOutCount;
                } else {
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
                }
            } else if (!inA && inB) {
                const ScannedFileEntry &entryB = filesB[key];
                if (!matchesFilter(entryB.absolutePath, entryB.relativePath, entryB.size, entryB.modifiedMs, filter)) {
                    ++plan.filteredOutCount;
                } else {
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
                }
            } else if (inA && inB) {
                const ScannedFileEntry &entryA = filesA[key];
                const ScannedFileEntry &entryB = filesB[key];
                const qint64 timeDiff = entryA.modifiedMs - entryB.modifiedMs;

                if (entryA.size == entryB.size && std::llabs(timeDiff) <= kTimestampToleranceMs) {
                    if (!matchesFilter(entryA.absolutePath, entryA.relativePath, entryA.size, entryA.modifiedMs, filter)
                        && !matchesFilter(entryB.absolutePath, entryB.relativePath, entryB.size, entryB.modifiedMs, filter)) {
                        ++plan.filteredOutCount;
                    } else {
                        FolderSyncAction action;
                        action.relativePath = entryA.relativePath;
                        action.type = FolderSyncActionType::Skip;
                        action.sourcePath = entryA.absolutePath;
                        action.targetPath = entryB.absolutePath;
                        action.fileSize = entryA.size;
                        action.reason = QStringLiteral("两端一致");
                        ++plan.skipCount;
                        plan.actions.append(action);
                    }
                } else if (timeDiff > kTimestampToleranceMs
                           || (std::llabs(timeDiff) <= kTimestampToleranceMs && entryA.modifiedMs >= entryB.modifiedMs)) {
                    if (!matchesFilter(entryA.absolutePath, entryA.relativePath, entryA.size, entryA.modifiedMs, filter)) {
                        ++plan.filteredOutCount;
                    } else {
                        FolderSyncAction action;
                        action.relativePath = entryA.relativePath;
                        action.type = FolderSyncActionType::CopyAtoB;
                        action.sourcePath = entryA.absolutePath;
                        action.targetPath = entryB.absolutePath;
                        action.fileSize = entryA.size;
                        action.reason = QStringLiteral("A 较新");
                        ++plan.copyAtoBCount;
                        plan.totalTransferBytes += entryA.size;
                        plan.actions.append(action);
                    }
                } else {
                    if (!matchesFilter(entryB.absolutePath, entryB.relativePath, entryB.size, entryB.modifiedMs, filter)) {
                        ++plan.filteredOutCount;
                    } else {
                        FolderSyncAction action;
                        action.relativePath = entryA.relativePath;
                        action.type = FolderSyncActionType::CopyBtoA;
                        action.sourcePath = entryB.absolutePath;
                        action.targetPath = entryA.absolutePath;
                        action.fileSize = entryB.size;
                        action.reason = QStringLiteral("B 较新");
                        ++plan.copyBtoACount;
                        plan.totalTransferBytes += entryB.size;
                        plan.actions.append(action);
                    }
                }
            }
        }

        ++processed;
        if (progressCallback && (processed % 100 == 0 || throttle.elapsed() >= 150)) {
            throttle.restart();
            progressCallback(processed, totalKeys,
                             QStringLiteral("正在分析差异 %1 / %2 ...").arg(processed).arg(totalKeys));
        }
    }

    plan.valid = true;
    return plan;
}

FolderSyncResult executeSync(const FolderSyncPlan &plan,
                             const ProgressCallback &progressCallback,
                             const std::atomic_bool *cancelFlag)
{
    FolderSyncResult result;
    if (!plan.valid) {
        result.errors.append(plan.errorMessage.isEmpty() ? QStringLiteral("同步计划无效。") : plan.errorMessage);
        result.failedCount = 1;
        return result;
    }

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

FolderCompareResult compareFolders(const QString &folderA,
                                   const QString &folderB,
                                   bool includeSubdirs,
                                   const FolderSyncFilter &filter,
                                   const ProgressCallback &progressCallback,
                                   const std::atomic_bool *cancelFlag)
{
    FolderCompareResult result;
    const QString validationError = validateInputs(folderA, folderB, filter);
    if (!validationError.isEmpty()) {
        result.errorMessage = validationError;
        return result;
    }

    result.folderA = normalizeDirPath(folderA);
    result.folderB = normalizeDirPath(folderB);

    bool canceled = false;
    const QHash<QString, ScannedFileEntry> filesA = scanDirectoryFiles(
        result.folderA, includeSubdirs, QStringLiteral("A"), progressCallback, cancelFlag, &canceled);
    if (canceled) {
        result.canceled = true;
        return result;
    }

    const QFileInfo infoB(result.folderB);
    const QHash<QString, ScannedFileEntry> filesB = infoB.exists()
        ? scanDirectoryFiles(result.folderB, includeSubdirs, QStringLiteral("B"), progressCallback, cancelFlag, &canceled)
        : QHash<QString, ScannedFileEntry>{};
    if (canceled) {
        result.canceled = true;
        return result;
    }

    QHash<QString, ScannedFileEntry> qualifiedA;
    QHash<QString, ScannedFileEntry> qualifiedB;

    const int totalToEval = filesA.size() + filesB.size();
    int evaluated = 0;
    QElapsedTimer throttle;
    throttle.start();

    auto classifySide = [&](const QHash<QString, ScannedFileEntry> &rawFiles,
                            QHash<QString, ScannedFileEntry> &qualifiedOut,
                            bool isSideA) -> bool {
        QStringList keys = rawFiles.keys();
        std::sort(keys.begin(), keys.end());
        for (const QString &key : keys) {
            if (cancelFlag && cancelFlag->load()) return false;
            const ScannedFileEntry &entry = rawFiles[key];
            if (matchesTimeRange(entry.modifiedMs, filter)) {
                QString reason;
                if (evaluateNonTimeFilter(entry.absolutePath, entry.relativePath, entry.size, filter, &reason)) {
                    qualifiedOut.insert(key, entry);
                } else {
                    FolderCompareEntry excluded;
                    excluded.kind = isSideA ? FolderCompareEntryKind::ExcludedInA : FolderCompareEntryKind::ExcludedInB;
                    excluded.sideLabel = isSideA ? QStringLiteral("文件夹 A") : QStringLiteral("文件夹 B");
                    excluded.absolutePath = entry.absolutePath;
                    excluded.parentDir = entry.parentDir;
                    excluded.relativePath = entry.relativePath;
                    excluded.fileSize = entry.size;
                    excluded.modifiedMs = entry.modifiedMs;
                    excluded.reason = reason;
                    result.entries.append(excluded);
                    if (isSideA) ++result.excludedInACount;
                    else ++result.excludedInBCount;
                }
            }
            ++evaluated;
            if (progressCallback && (evaluated % 100 == 0 || throttle.elapsed() >= 150)) {
                throttle.restart();
                progressCallback(evaluated, totalToEval,
                                 QStringLiteral("正在筛选与检测元信息 %1 / %2 ...").arg(evaluated).arg(totalToEval));
            }
        }
        return true;
    };

    if (!classifySide(filesA, qualifiedA, true) || !classifySide(filesB, qualifiedB, false)) {
        result.canceled = true;
        return result;
    }

    QStringList allQualifiedKeys = qualifiedA.keys();
    for (auto it = qualifiedB.cbegin(); it != qualifiedB.cend(); ++it) {
        if (!qualifiedA.contains(it.key())) {
            allQualifiedKeys.append(it.key());
        }
    }
    std::sort(allQualifiedKeys.begin(), allQualifiedKeys.end());

    for (const QString &key : allQualifiedKeys) {
        if (cancelFlag && cancelFlag->load()) {
            result.canceled = true;
            return result;
        }
        const bool inA = qualifiedA.contains(key);
        const bool inB = qualifiedB.contains(key);

        if (inA && !inB) {
            const ScannedFileEntry &e = qualifiedA[key];
            FolderCompareEntry item;
            item.kind = FolderCompareEntryKind::OnlyInA;
            item.sideLabel = QStringLiteral("文件夹 A");
            item.absolutePath = e.absolutePath;
            item.parentDir = e.parentDir;
            item.relativePath = e.relativePath;
            item.fileSize = e.size;
            item.modifiedMs = e.modifiedMs;
            item.reason = QStringLiteral("仅存在于 A（B 缺失）");
            result.entries.append(item);
            ++result.onlyInACount;
        } else if (!inA && inB) {
            const ScannedFileEntry &e = qualifiedB[key];
            FolderCompareEntry item;
            item.kind = FolderCompareEntryKind::OnlyInB;
            item.sideLabel = QStringLiteral("文件夹 B");
            item.absolutePath = e.absolutePath;
            item.parentDir = e.parentDir;
            item.relativePath = e.relativePath;
            item.fileSize = e.size;
            item.modifiedMs = e.modifiedMs;
            item.reason = QStringLiteral("仅存在于 B（多余）");
            result.entries.append(item);
            ++result.onlyInBCount;
        } else if (inA && inB) {
            const ScannedFileEntry &eA = qualifiedA[key];
            const ScannedFileEntry &eB = qualifiedB[key];
            const qint64 timeDiff = std::llabs(eA.modifiedMs - eB.modifiedMs);
            if (eA.size == eB.size && timeDiff <= kTimestampToleranceMs) {
                ++result.matchedCount;
            } else {
                FolderCompareEntry item;
                item.kind = FolderCompareEntryKind::ContentDiff;
                item.sideLabel = QStringLiteral("A / B");
                item.absolutePath = eA.absolutePath;
                item.parentDir = eA.parentDir;
                item.relativePath = eA.relativePath;
                item.fileSize = eA.size;
                item.modifiedMs = eA.modifiedMs;
                item.reason = (eA.size != eB.size)
                    ? QStringLiteral("两端大小不一致 (A: %1 B, B: %2 B)").arg(eA.size).arg(eB.size)
                    : QStringLiteral("两端修改时间不一致");
                result.entries.append(item);
                ++result.differentCount;
            }
        }
    }

    result.valid = true;
    return result;
}

TrashDeleteResult deleteFiles(const QStringList &filePaths,
                              FolderDeleteMode mode,
                              const ProgressCallback &progressCallback,
                              const std::atomic_bool *cancelFlag)
{
    TrashDeleteResult result;
    const int total = filePaths.size();
    int done = 0;
    const bool permanent = (mode == FolderDeleteMode::PermanentDelete);

    for (const QString &path : filePaths) {
        if (cancelFlag && cancelFlag->load()) {
            result.canceled = true;
            break;
        }
        ++done;
        if (!QFileInfo::exists(path)) {
            ++result.failedCount;
            const QString msg = permanent
                ? QStringLiteral("[直接删除] 文件不存在: %1").arg(path)
                : QStringLiteral("[回收站] 文件不存在: %1").arg(path);
            result.errors.append(msg);
            if (progressCallback) progressCallback(done, total, msg);
            continue;
        }

        bool ok = false;
        if (permanent) {
            ok = QFile::remove(path);
        } else {
            QString trashPath;
            ok = QFile::moveToTrash(path, &trashPath);
        }

        if (ok) {
            ++result.deletedCount;
            if (progressCallback) {
                progressCallback(done, total,
                                 permanent
                                     ? QStringLiteral("[已直接删除] %1").arg(path)
                                     : QStringLiteral("[已移入回收站] %1").arg(path));
            }
        } else {
            ++result.failedCount;
            const QString msg = permanent
                ? QStringLiteral("[直接删除失败] %1").arg(path)
                : QStringLiteral("[移入回收站失败] %1").arg(path);
            result.errors.append(msg);
            if (progressCallback) progressCallback(done, total, msg);
        }
    }
    return result;
}

} // namespace FolderSyncEngine
