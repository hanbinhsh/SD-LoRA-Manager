#pragma once

#include <QList>
#include <QString>
#include <QStringList>

enum class ModelPreviewState : int {
    MissingOrUnknown = 0,
    RealPreview = 1,
    KnownNoPreview = 2,
};

struct ModelUserNote {
    double rating = 0.0;
    QString note;
    QStringList tags;
    QStringList customTriggers;
    QString updatedAt;
};

struct UpdateCheckSnapshot {
    QString filePath;
    QString modelDir;
    QString baseName;
    QString displayName;
    QString currentSha256;
    int modelId = 0;
    int currentVersionId = 0;
    bool localEdited = false;
    ModelPreviewState previewState = ModelPreviewState::MissingOrUnknown;
};

struct MetadataSyncJob {
    UpdateCheckSnapshot snapshot;
    bool updateExisting = false;
    bool civArchiveOnly = false;
    bool detailFallback = false;
};

struct PreviewMetadataPayload {
    QString prompt;
    QString negativePrompt;
    QString sampler;
    QString cfgScale;
    QString steps;
    QString seed;
    int width = 0;
    int height = 0;
    int nsfwLevel = 0;
};

struct ImageInfo {
    QString url;
    QString hash;
    QString prompt;
    QString negativePrompt;
    QString sampler;
    QString cfgScale;
    QString steps;
    QString seed;
    QString model;
    int nsfwLevel = 0;
    int width = 0;
    int height = 0;
    bool nsfw = false;
};

struct UserImageInfo {
    QString path;
    QString prompt;
    QStringList cleanTags;
    QStringList negativeCleanTags;
    QString negativePrompt;
    QString parameters;
    qint64 lastModified = 0;
    int parserVersion = 0;
};

struct ModelMeta {
    QString fileName;
    QString modelName;
    QString versionName;
    QString name;
    QString filePath;
    QString previewPath;
    QStringList trainedWordsGroups;
    QString modelUrl;
    QString baseModel;
    QString type;
    QString description;
    QString createdAt;
    bool nsfw = false;
    int downloadCount = 0;
    int thumbsUpCount = 0;
    double fileSizeMB = 0.0;
    QString sha256;
    QString fileNameServer;
    QString creatorName;
    QString creatorAvatarUrl;
    QStringList modelTags;
    int modelId = 0;
    int versionId = 0;
    bool isLocalEdited = false;
    bool isLocalOnly = false;
    QList<ImageInfo> images;
};

