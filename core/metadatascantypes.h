#pragma once

#include <QString>

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
