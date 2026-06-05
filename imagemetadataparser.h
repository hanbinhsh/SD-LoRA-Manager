#ifndef IMAGEMETADATAPARSER_H
#define IMAGEMETADATAPARSER_H

#include <QMap>
#include <QString>
#include <QStringList>

struct ParsedImageMetadata
{
    QString sourceType;
    QString positivePrompt;
    QString negativePrompt;
    QString parametersText;
    QString seed;
    QString steps;
    QString cfg;
    QString sampler;
    QString scheduler;
    QString checkpoint;
    QStringList loraDescriptions;
    QMap<QString, QString> loraHashes;

    bool hasContent() const
    {
        return !positivePrompt.trimmed().isEmpty()
               || !negativePrompt.trimmed().isEmpty()
               || !parametersText.trimmed().isEmpty();
    }
};

ParsedImageMetadata parseImageMetadataFromFile(const QString &filePath);
QString extractPngParametersText(const QString &filePath);
QMap<QString, QString> extractImageMetadataTextChunks(const QString &filePath);

#endif // IMAGEMETADATAPARSER_H
