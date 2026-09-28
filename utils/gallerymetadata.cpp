#include "gallerymetadata.h"
#include "imagemetadataparser.h"
#include "tagutils.h"
#include <QImageReader>

namespace GalleryMetadata {

void parseImage(const QString &path, UserImageInfo &info, bool splitOnNewline, const QStringList &filterTags)
{
    const ParsedImageMetadata parsed = parseImageMetadataFromFile(path);
    if (!parsed.hasContent()) {
        // A readable image without metadata is a stable result. Incomplete,
        // locked, or damaged files stay retryable on the next scan.
        QImageReader reader(path);
        if (reader.canRead()) info.parserVersion = ParserVersion;
        return;
    }

    info.parserVersion = ParserVersion;
    info.prompt = parsed.positivePrompt.trimmed();
    info.negativePrompt = parsed.negativePrompt.trimmed();
    if (info.negativePrompt.isEmpty() && !info.prompt.isEmpty()) {
        info.negativePrompt = "(empty)";
    }
    info.parameters = parsed.parametersText.trimmed();
    info.cleanTags = TagUtils::parsePromptTags(info.prompt, splitOnNewline, filterTags);
    info.negativeCleanTags = TagUtils::parsePromptTags(info.negativePrompt, splitOnNewline, filterTags);
}

}
