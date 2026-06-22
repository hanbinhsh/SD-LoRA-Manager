#ifndef DOWNLOADMODELS_H
#define DOWNLOADMODELS_H

#include <QJsonObject>
#include <QImage>
#include <QPointer>
#include <QString>
#include <QVector>

class QFrame;
class QLabel;
class QProgressBar;
class QPushButton;

struct ModelUpdateInfo {
    QString filePath;
    QString modelDir;
    QString baseName;
    QString displayName;
    QString currentVersion;
    QString latestVersion;
    QString downloadUrl;
    QString downloadFileName;
    QString sha256;
    QString metadataSource;
    QString sourceUrl;
    QJsonObject latestVersionJson;
    int modelId = 0;
    int currentVersionId = 0;
    int latestVersionId = 0;
    double sizeMB = 0.0;
    bool hasUpdate = false;
    bool latestFileExistsLocally = false;
};

struct DownloadCardWidgets {
    QPointer<QFrame> card;
    QPointer<QLabel> previewLabel;
    QPointer<QLabel> titleLabel;
    QPointer<QLabel> versionLabel;
    QPointer<QLabel> sizeLabel;
    QPointer<QLabel> speedLabel;
    QPointer<QLabel> statusLabel;
    QPointer<QLabel> targetLabel;
    QPointer<QProgressBar> progressBar;
    QPointer<QPushButton> sourceButton;
    QPointer<QPushButton> civitaiButton;
    QPointer<QPushButton> downloadButton;
    QPointer<QPushButton> ignoreButton;
    QString statusText;
    QString targetPath;
    QString category;
    QString displayName;
    bool selected = false;
    bool hasUpdate = false;
    bool showingPlaceholder = false; // 当前预览是"加载失败占位X"，切主题时需要重绘
};

struct ModelFileDownloadTask {
    ModelUpdateInfo info;
    QString targetPath;
    QString tempPath;
    QString filePath;
    bool overwrite = false;
};

struct DownloadPreviewLoadResult {
    QString filePath;
    QString previewPath;
    QImage image;
    bool valid = false;
};

struct MetadataScanItem {
    QString filePath;
    QString displayName;
    QString jsonPath;
    QString previewPath;
    QString modelIdText;
    QString versionIdText;
    QString sha256;
    QString status;
    QString category;
    QString lastSyncedAt;
    QString lastSyncedSource;
    QString syncFailure;
    QString errorText;
    bool localEdited = false;
    bool checked = false;
};

struct MetadataHealthIssue {
    QString severity;
    QString modelName;
    QString issue;
    QString suggestion;
    QString filePath;
};

#endif // DOWNLOADMODELS_H
