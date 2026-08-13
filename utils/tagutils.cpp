#include "tagutils.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace TagUtils {

QString cleanPromptTag(QString text, bool preserveEmoticons)
{
    text = text.trimmed();
    if (text.isEmpty()) return QString();

    static const QSet<QString> emoticons = {":)", ":-)", ":(", ":-(", "^_^", "T_T", "o_o", "O_O"};
    if (preserveEmoticons && emoticons.contains(text)) return text;

    static const QRegularExpression weightRegex(":[0-9.]+$");
    text.remove(weightRegex);

    static const QRegularExpression bracketRegex("[\\{\\}\\[\\]\\(\\)]");
    text.remove(bracketRegex);

    return text.trimmed();
}

QString normalizedPromptTagKey(QString text)
{
    text = cleanPromptTag(text, false).toCaseFolded().trimmed();
    text.replace('_', ' ');
    text.replace('-', ' ');
    static const QRegularExpression separators(QStringLiteral("\\s+"));
    text.replace(separators, QStringLiteral(" "));
    return text;
}

QStringList splitPromptParts(const QString &rawPrompt, bool splitOnNewline)
{
    const QString trimmed = rawPrompt.trimmed();
    if (trimmed.isEmpty()) return {};
    if (trimmed.startsWith('{') || trimmed.startsWith('[')) {
        const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8());
        if (!doc.isNull()) return {};
    }

    QString processText = trimmed;
    if (splitOnNewline) {
        processText.replace("\r\n", ",");
        processText.replace("\n", ",");
        processText.replace("\r", ",");
    }
    return processText.split(',', Qt::SkipEmptyParts);
}

}
