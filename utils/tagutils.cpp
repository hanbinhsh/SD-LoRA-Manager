#include "tagutils.h"

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

}
