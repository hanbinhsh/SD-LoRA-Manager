#ifndef FOLDERSYNCENGINE_H
#define FOLDERSYNCENGINE_H

#include <QDateTime>
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

enum class FolderSyncTimeFilterMode {
    All = 0,
    Before = 1,
    After = 2,
    Between = 3
};

enum class FolderSyncNameFilterMode {
    None = 0,
    ExcludeContains = 1,
    IncludeContains = 2
};

enum class FolderSyncExtFilterMode {
    None = 0,
    IncludeOnly = 1,
    Exclude = 2
};

enum class FolderDeleteMode {
    MoveToTrash = 0,
    PermanentDelete = 1
};

struct FolderSyncFilter {
    FolderSyncTimeFilterMode timeMode = FolderSyncTimeFilterMode::All;
    QDateTime startTime;
    QDateTime endTime;
    bool imagesOnly = false;
    FolderSyncExtFilterMode extMode = FolderSyncExtFilterMode::None;
    QStringList extensions;
    qint64 minSizeKB = 0; // <= 0 means no minimum
    qint64 maxSizeKB = 0; // <= 0 means no maximum
    FolderSyncNameFilterMode nameMode = FolderSyncNameFilterMode::None;
    QStringList nameKeywords;
    bool requireImageMetadata = false;
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
    bool canceled = false;
    QString errorMessage;
    QString folderA;
    QString folderB;
    FolderSyncMode mode = FolderSyncMode::OneWayCopy;
    bool includeSubdirs = true;
    FolderSyncFilter filter;
    QVector<FolderSyncAction> actions;
    int copyAtoBCount = 0;
    int moveAtoBCount = 0;
    int copyBtoACount = 0;
    int skipCount = 0;
    int filteredOutCount = 0;
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

enum class FolderCompareEntryKind {
    OnlyInA,
    OnlyInB,
    ContentDiff,
    ExcludedInA,
    ExcludedInB
};

struct FolderCompareEntry {
    FolderCompareEntryKind kind = FolderCompareEntryKind::OnlyInA;
    QString sideLabel;
    QString absolutePath;
    QString parentDir;
    QString relativePath;
    qint64 fileSize = 0;
    qint64 modifiedMs = 0;
    QString reason;
};

struct FolderCompareResult {
    bool valid = false;
    bool canceled = false;
    QString errorMessage;
    QString folderA;
    QString folderB;
    int matchedCount = 0;
    int differentCount = 0;
    int onlyInACount = 0;
    int onlyInBCount = 0;
    int excludedInACount = 0;
    int excludedInBCount = 0;
    QVector<FolderCompareEntry> entries;

    bool isFilteredExactMatch() const {
        return valid && !canceled && differentCount == 0 && onlyInACount == 0 && onlyInBCount == 0;
    }

    int deletableCandidateCount() const {
        return onlyInACount + onlyInBCount + excludedInACount + excludedInBCount;
    }
};

struct TrashDeleteResult {
    bool canceled = false;
    int deletedCount = 0;
    int failedCount = 0;
    QStringList errors;
};

namespace FolderSyncEngine {

using ProgressCallback = std::function<void(int done, int total, const QString &message)>;

QString normalizeDirPath(const QString &path);
bool isSameOrNestedDirectory(const QString &dirA, const QString &dirB);
bool isSupportedImageFile(const QString &fileName);
QStringList parseNameKeywords(const QString &text);
QStringList parseExtensions(const QString &text);
QString validateInputs(const QString &folderA, const QString &folderB, const FolderSyncFilter &filter);

bool matchesTimeRange(qint64 modifiedMs, const FolderSyncFilter &filter);
bool evaluateNonTimeFilter(const QString &absPath,
                           const QString &fileName,
                           qint64 sizeBytes,
                           const FolderSyncFilter &filter,
                           QString *excludeReasonOut = nullptr);
bool matchesFilter(const QString &absPath,
                   const QString &fileName,
                   qint64 sizeBytes,
                   qint64 modifiedMs,
                   const FolderSyncFilter &filter,
                   QString *excludeReasonOut = nullptr);

QStringList collectWatchDirectories(const QStringList &rootDirs,
                                    bool includeSubdirs,
                                    int maxDirectories = 1500,
                                    const std::atomic_bool *cancelFlag = nullptr);

FolderSyncPlan planSync(const QString &folderA,
                        const QString &folderB,
                        FolderSyncMode mode,
                        bool includeSubdirs = true,
                        const FolderSyncFilter &filter = FolderSyncFilter(),
                        const ProgressCallback &progressCallback = {},
                        const std::atomic_bool *cancelFlag = nullptr);

FolderSyncResult executeSync(const FolderSyncPlan &plan,
                             const ProgressCallback &progressCallback = {},
                             const std::atomic_bool *cancelFlag = nullptr);

FolderCompareResult compareFolders(const QString &folderA,
                                   const QString &folderB,
                                   bool includeSubdirs = true,
                                   const FolderSyncFilter &filter = FolderSyncFilter(),
                                   const ProgressCallback &progressCallback = {},
                                   const std::atomic_bool *cancelFlag = nullptr);

TrashDeleteResult deleteFiles(const QStringList &filePaths,
                              FolderDeleteMode mode,
                              const ProgressCallback &progressCallback = {},
                              const std::atomic_bool *cancelFlag = nullptr);

} // namespace FolderSyncEngine

#endif // FOLDERSYNCENGINE_H
