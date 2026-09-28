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

#include "core/modeltypes.h"
#include "core/modelupdateinfo.h"
#include "core/metadatascantypes.h"


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
    QString searchText;
    bool selected = false;
    bool hasUpdate = false;
    bool showingPlaceholder = false; // 当前预览为缺图占位，切主题时需要重绘
    ModelPreviewState previewState = ModelPreviewState::MissingOrUnknown;
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

#endif // DOWNLOADMODELS_H
