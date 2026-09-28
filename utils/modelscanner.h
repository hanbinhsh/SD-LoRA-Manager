#pragma once
#include "core/modeltypes.h"
#include <QList>

namespace ModelScanner {
struct ModelListMetadata {
    qint64 sortDate = 0;
    qint64 sortAdded = 0;
    int downloads = 0;
    int likes = 0;
    QString filterBase = QStringLiteral("Unknown");
    int nsfwLevel = 1;
    bool localEdited = false;
    int modelId = 0;
    int versionId = 0;
    QString civitaiSha256;
    QString creator;
    QStringList modelTags;
    QString modelType;
    QStringList trainedWords;
    QString civitaiName; // 为空表示不覆盖已有的 ROLE_CIVITAI_NAME
    ModelPreviewState previewState = ModelPreviewState::MissingOrUnknown;
};

struct ScannedModelEntry {
    QString baseName;
    QString fullPath;
    QString previewPath;
    QString rootPath;
    QString rootName;
    ModelListMetadata meta;
};
ModelListMetadata parseModelListMetadata(const QString &filePath, const QString &jsonPath,
                                        bool *cacheable = nullptr);
QList<ScannedModelEntry> scanModelsWorker(const QStringList &paths, bool recursive, const QString &cachePath);
}
