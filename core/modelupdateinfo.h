#pragma once

#include "modeltypes.h"
#include <QJsonObject>

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
    ModelPreviewState previewState = ModelPreviewState::MissingOrUnknown;
};
