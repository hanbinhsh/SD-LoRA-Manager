#pragma once

#include "core/modeltypes.h"
#include <QByteArray>

namespace PreviewImageStore {

// File-only operations; safe to call from a worker without a window instance.
PreviewMetadataPayload previewPayloadFromImageInfo(const ImageInfo &image);
QString buildPreviewParametersText(const PreviewMetadataPayload &payload);
bool savePreviewImageWithMetadata(const QByteArray &data, const QString &savePath,
                                  const PreviewMetadataPayload &payload);
bool ensurePreviewImageMetadata(const QString &path, const PreviewMetadataPayload &payload);
bool previewFileAlreadyHasPromptMetadata(const QString &path);

}
