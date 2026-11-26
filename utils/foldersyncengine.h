#ifndef FOLDERSYNCENGINE_H
#define FOLDERSYNCENGINE_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <functional>

enum class FolderSyncMode {
    OneWayCopy = 0,
    OneWayMove = 1,
    TwoWaySync = 2
};

enum class FolderSyncActionType {
    CopyAtoB,
    MoveAtoB,
    CopyBtoA,
    Skip
};

struct FolderSyncAction {
    FolderSyncActionType type = FolderSyncActionType::Skip;
    QString relativePath;
    QString sourcePath;
    QString targetPath;
    qint64 fileSize = 0;
    QString reason;
};

struct FolderSyncPlan {
    bool valid = false;
    QString errorMessage;
    QString folderA;
    QString folderB;
    FolderSyncMode mode = FolderSyncMode::OneWayCopy;
    bool includeSubdirs = true;
    QVector<FolderSyncAction> actions;
    int copyAtoBCount = 0;
    int moveAtoBCount = 0;
    int copyBtoACount = 0;
    int skipCount = 0;
    qint64 totalTransferBytes = 0;

    int pendingTransferCount() const {
        return copyAtoBCount + moveAtoBCount + copyBtoACount;
    }
};

struct FolderSyncResult {
    bool success = false;
    bool canceled = false;
    int copiedCount = 0;
    int movedCount = 0;
    int skippedCount = 0;
    int failedCount = 0;
    int removedEmptyDirs = 0;
    qint64 transferredBytes = 0;
    QStringList errors;
};

namespace FolderSyncEngine {

QString normalizeDirPath(const QString &path);
bool isSameOrNestedDirectory(const QString &dirA, const QString &dirB);

FolderSyncPlan planSync(const QString &folderA,
                        const QString &folderB,
                        FolderSyncMode mode,
                        bool includeSubdirs = true);

FolderSyncResult executeSync(
    const FolderSyncPlan &plan,
    const std::function<void(int done, int total, const QString &message)> &progressCallback = {},
    const std::atomic_bool *cancelFlag = nullptr);

} // namespace FolderSyncEngine

#endif // FOLDERSYNCENGINE_H
