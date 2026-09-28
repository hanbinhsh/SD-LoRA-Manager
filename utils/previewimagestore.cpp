#include "previewimagestore.h"

#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QSaveFile>

namespace PreviewImageStore {

namespace {
bool writeRawImage(const QByteArray &data, const QString &path)
{
    QSaveFile output(path);
    return output.open(QIODevice::WriteOnly)
           && output.write(data) == data.size() && output.commit();
}

bool writePng(const QImage &image, const QString &path, const QString &parameters,
              const QString &prompt, const QString &negativePrompt)
{
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) return false;
    QImageWriter writer(&output, "png");
    writer.setText("parameters", parameters);
    writer.setText("civitai_prompt", prompt);
    writer.setText("civitai_negative_prompt", negativePrompt);
    return writer.write(image) && output.commit();
}
}

PreviewMetadataPayload previewPayloadFromImageInfo(const ImageInfo &img)
{
    PreviewMetadataPayload payload;
    payload.prompt = img.prompt;
    payload.negativePrompt = img.negativePrompt;
    payload.sampler = img.sampler;
    payload.cfgScale = img.cfgScale;
    payload.steps = img.steps;
    payload.seed = img.seed;
    payload.width = img.width;
    payload.height = img.height;
    payload.nsfwLevel = img.nsfwLevel;
    return payload;
}

bool writePreviewMetadataToPath(const QString &path,
                                const QString &parameters,
                                const QString &prompt,
                                const QString &negativePrompt)
{
    if (path.isEmpty() || parameters.isEmpty() || !QFile::exists(path)) return false;

    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) return false;
    const QByteArray data = input.readAll();
    input.close();
    if (data.size() < 12) return false;

    const bool isPng = data.startsWith("\x89PNG\r\n\x1a\n");
    const bool isJpeg = data.startsWith("\xff\xd8");
    const bool isWebp = data.size() >= 12 && data.left(4) == "RIFF" && data.mid(8, 4) == "WEBP";
    if (!isPng && !isJpeg && !isWebp) return false;

    // Avoid libpng spam on partial/corrupt downloads. A complete PNG must contain IEND.
    if (isPng && !data.contains("IEND")) {
        return false;
    }

    QImage image;
    if (!image.loadFromData(data)) return false;
    if (image.isNull()) return false;

    return writePng(image, path, parameters, prompt, negativePrompt);
}

bool previewFileAlreadyHasPromptMetadata(const QString &path)
{
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) return false;
    const QByteArray data = file.readAll();
    return data.contains("parameters") || data.contains("civitai_prompt");
}

QString buildPreviewParametersText(const PreviewMetadataPayload &payload)
{
    QStringList lines;
    if (!payload.prompt.trimmed().isEmpty()) {
        lines << payload.prompt.trimmed();
    }
    if (!payload.negativePrompt.trimmed().isEmpty()) {
        lines << "Negative prompt: " + payload.negativePrompt.trimmed();
    }

    QStringList params;
    if (!payload.steps.trimmed().isEmpty() && payload.steps.trimmed() != "0") params << "Steps: " + payload.steps.trimmed();
    if (!payload.sampler.trimmed().isEmpty()) params << "Sampler: " + payload.sampler.trimmed();
    if (!payload.cfgScale.trimmed().isEmpty() && payload.cfgScale.trimmed() != "0") params << "CFG scale: " + payload.cfgScale.trimmed();
    if (!payload.seed.trimmed().isEmpty() && payload.seed.trimmed() != "0") params << "Seed: " + payload.seed.trimmed();
    if (payload.width > 0 && payload.height > 0) params << QString("Size: %1x%2").arg(payload.width).arg(payload.height);
    if (!params.isEmpty()) lines << params.join(", ");

    return lines.join('\n').trimmed();
}

bool savePreviewImageWithMetadata(const QByteArray &data, const QString &savePath, const PreviewMetadataPayload &payload)
{
    const QString parameters = buildPreviewParametersText(payload);
    if (parameters.isEmpty()) {
        return writeRawImage(data, savePath);
    }

    QImage image;
    image.loadFromData(data);
    if (!image.isNull() && writePng(image, savePath, parameters, payload.prompt, payload.negativePrompt)) {
        return true;
    }

    // Preserve downloaded bytes if PNG conversion fails; false reports missing metadata.
    writeRawImage(data, savePath);
    return false;
}

bool ensurePreviewImageMetadata(const QString &path, const PreviewMetadataPayload &payload)
{
    const QString parameters = buildPreviewParametersText(payload);
    return writePreviewMetadataToPath(path, parameters, payload.prompt, payload.negativePrompt);
}

}
