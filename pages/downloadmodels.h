#ifndef DOWNLOADMODELS_H
#define DOWNLOADMODELS_H

#include <QJsonObject>
#include <QPointer>
#include <QString>

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
};

#endif // DOWNLOADMODELS_H
