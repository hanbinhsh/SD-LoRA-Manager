#include "tagusage.h"
#include "tagutils.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>

namespace TagUsage {

QStringList promptTags(const QString &prompt)
{
    if (prompt.trimmed().compare("(empty)", Qt::CaseInsensitive) == 0) return {};
    static const QStringList blockedTags = {"BREAK", "ADDCOMM", "ADDBASE", "ADDCOL", "ADDROW"};
    QStringList tags;
    QSet<QString> seen;
    for (const QString &tag : TagUtils::parsePromptTags(prompt, true, blockedTags)) {
        const QString key = TagUtils::normalizedPromptTagKey(tag);
        if (key.isEmpty() || seen.contains(key)) continue;
        seen.insert(key);
        tags.append(tag);
    }
    return tags;
}

void Counts::addImage(const QStringList &tags)
{
    QSet<QString> seen;
    for (const QString &tag : tags) {
        const QString key = TagUtils::normalizedPromptTagKey(tag);
        if (key.isEmpty() || seen.contains(key)) continue;
        seen.insert(key);
        if (!displayTags.contains(key)) displayTags.insert(key, tag);
        ++values[key];
    }
}

QVector<Row> Statistics::rows(int scope) const
{
    QVector<Row> result;
    const auto append = [&result](const Counts &counts, const QString &kind) {
        for (auto it = counts.values.cbegin(); it != counts.values.cend(); ++it)
            result.append({counts.displayTags.value(it.key(), it.key()), kind, it.value()});
    };
    if (scope == 0 || scope == 2) append(positive, QStringLiteral("正面"));
    if (scope == 1 || scope == 2) append(negative, QStringLiteral("负面"));
    std::sort(result.begin(), result.end(), [](const Row &a, const Row &b) {
        if (a.count != b.count) return a.count > b.count;
        const int byName = QString::compare(a.tag, b.tag, Qt::CaseInsensitive);
        if (byName != 0) return byName < 0;
        if (a.tag != b.tag) return a.tag < b.tag;
        return a.kind < b.kind;
    });
    return result;
}

Statistics readCache(const QString &path, Scope scope)
{
    Statistics result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return result;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return result;
    const QJsonObject root = doc.object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        if (it.key().startsWith("__") || !it.value().isObject()) continue;
        const QJsonObject image = it.value().toObject();
        const QStringList positive = scope == Negative ? QStringList{} : promptTags(image.value("p").toString());
        const QStringList negative = scope == Positive ? QStringList{} : promptTags(image.value("np").toString());
        if (scope == Combined) {
            result.combined.addImage(positive + negative);
        } else {
            result.positive.addImage(positive);
            result.negative.addImage(negative);
        }
    }
    return result;
}

}
