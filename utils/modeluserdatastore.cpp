#include "modeluserdatastore.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <cmath>

namespace ModelUserDataStore {
namespace {

bool writeJsonObjectAtomically(const QString &filePath,
                               const QJsonObject &root,
                               QJsonDocument::JsonFormat format = QJsonDocument::Indented,
                               const char *warningContext = nullptr)
{
    const QFileInfo fi(filePath);
    if (!fi.absolutePath().isEmpty()) {
        QDir().mkpath(fi.absolutePath());
    }
    QSaveFile file(filePath);
    const QByteArray payload = QJsonDocument(root).toJson(format);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(payload) != payload.size()
        || !file.commit()) {
        if (warningContext) {
            qWarning() << warningContext << file.errorString();
        }
        return false;
    }
    return true;
}

} // namespace

QStringList normalizeDistinctTrimmedList(const QStringList &items)
{
    QStringList result;
    QSet<QString> seen;
    for (const QString &raw : items) {
        const QString item = raw.trimmed();
        if (item.isEmpty()) continue;
        const QString key = item.toCaseFolded();
        if (seen.contains(key)) continue;
        seen.insert(key);
        result.append(item);
    }
    return result;
}

QStringList normalizeUserTagsText(const QString &text)
{
    QString normalizedText = text;
    normalizedText.replace(QLatin1Char('\r'), QLatin1Char(','));
    normalizedText.replace(QLatin1Char('\n'), QLatin1Char(','));
    return normalizeDistinctTrimmedList(normalizedText.split(QLatin1Char(','), Qt::SkipEmptyParts));
}

QString formatRating(double rating, const QString &unratedLabel)
{
    if (rating <= 0.0) return unratedLabel;
    return QStringLiteral("%1 / 5").arg(rating, 0, 'f', (std::fmod(rating, 1.0) == 0.0) ? 0 : 1);
}

QString formatUserNoteTooltip(const ModelUserNote &note,
                              const QString &baseTooltip,
                              const QString &unratedLabel)
{
    QStringList lines;
    if (!baseTooltip.trimmed().isEmpty()) lines << baseTooltip.trimmed();
    if (note.rating > 0.0) lines << QStringLiteral("评分: ") + formatRating(note.rating, unratedLabel);
    if (!note.tags.isEmpty()) lines << QStringLiteral("标签: ") + note.tags.join(QStringLiteral(", "));
    if (!note.customTriggers.isEmpty()) {
        lines << QStringLiteral("自定义触发词: ") + note.customTriggers.join(QStringLiteral(" | "));
    }
    QString noteText = note.note.simplified();
    if (!noteText.isEmpty()) {
        if (noteText.size() > 120) noteText = noteText.left(117) + QStringLiteral("...");
        lines << QStringLiteral("备注: ") + noteText;
    }
    return lines.join(QLatin1Char('\n'));
}

QMap<QString, QStringList> loadCollections(const QString &filePath)
{
    QMap<QString, QStringList> collections;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return collections;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    const QJsonObject root = doc.object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        QStringList files;
        const QJsonArray arr = it.value().toArray();
        files.reserve(arr.size());
        for (const QJsonValue &v : arr) files << v.toString();
        collections.insert(it.key(), files);
    }
    return collections;
}

bool saveCollections(const QString &filePath, const QMap<QString, QStringList> &collections)
{
    QJsonObject root;
    for (auto it = collections.cbegin(); it != collections.cend(); ++it) {
        QJsonArray arr;
        for (const QString &f : it.value()) arr.append(f);
        root.insert(it.key(), arr);
    }
    return writeJsonObjectAtomically(filePath, root, QJsonDocument::Indented,
                                     "Unable to save collections atomically:");
}

QHash<QString, QColor> loadHighlightColors(const QString &filePath)
{
    QHash<QString, QColor> colors;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return colors;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return colors;

    const QJsonObject root = doc.object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const QString normalizedPath = QFileInfo(it.key()).absoluteFilePath();
        const QColor color(it.value().toString());
        if (!normalizedPath.isEmpty() && color.isValid()) {
            colors.insert(normalizedPath, color);
        }
    }
    return colors;
}

bool saveHighlightColors(const QString &filePath, const QHash<QString, QColor> &colors)
{
    QJsonObject root;
    for (auto it = colors.cbegin(); it != colors.cend(); ++it) {
        if (it.value().isValid()) {
            root.insert(it.key(), it.value().name(QColor::HexArgb));
        }
    }
    return writeJsonObjectAtomically(filePath, root, QJsonDocument::Indented,
                                     "Unable to save model highlight colors atomically:");
}

QHash<QString, ModelUserNote> loadUserNotes(const QString &filePath)
{
    QHash<QString, ModelUserNote> notes;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return notes;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return notes;

    const QJsonObject root = doc.object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const QString normalizedPath = QFileInfo(it.key()).absoluteFilePath();
        const QJsonObject obj = it.value().toObject();
        ModelUserNote note;
        note.rating = obj.value(QStringLiteral("rating")).toDouble(0.0);
        if (note.rating < 0.5) note.rating = 0.0;
        if (note.rating > 5.0) note.rating = 5.0;
        note.rating = std::round(note.rating * 2.0) / 2.0;
        note.note = obj.value(QStringLiteral("note")).toString();
        note.updatedAt = obj.value(QStringLiteral("updatedAt")).toString();

        QStringList tags;
        const QJsonArray arr = obj.value(QStringLiteral("tags")).toArray();
        for (const QJsonValue &val : arr) tags.append(val.toString());
        note.tags = normalizeDistinctTrimmedList(tags);

        QStringList customTriggers;
        const QJsonArray triggerArr = obj.value(QStringLiteral("customTriggers")).toArray();
        for (const QJsonValue &val : triggerArr) customTriggers.append(val.toString());
        note.customTriggers = normalizeDistinctTrimmedList(customTriggers);

        if (!normalizedPath.isEmpty()) notes.insert(normalizedPath, note);
    }
    return notes;
}

bool saveUserNotes(const QString &filePath, const QHash<QString, ModelUserNote> &notes)
{
    QJsonObject root;
    for (auto it = notes.cbegin(); it != notes.cend(); ++it) {
        const ModelUserNote &note = it.value();
        if (note.rating <= 0.0 && note.note.trimmed().isEmpty()
            && note.tags.isEmpty() && note.customTriggers.isEmpty()) {
            continue;
        }

        QJsonObject obj;
        obj[QStringLiteral("rating")] = note.rating;
        obj[QStringLiteral("note")] = note.note;
        obj[QStringLiteral("updatedAt")] = note.updatedAt;
        QJsonArray tags;
        for (const QString &tag : note.tags) tags.append(tag);
        obj[QStringLiteral("tags")] = tags;
        QJsonArray triggers;
        for (const QString &trigger : note.customTriggers) triggers.append(trigger);
        obj[QStringLiteral("customTriggers")] = triggers;
        root.insert(it.key(), obj);
    }
    return writeJsonObjectAtomically(filePath, root, QJsonDocument::Indented,
                                     "Unable to save model user notes atomically:");
}

QHash<QString, QJsonObject> loadSyncFailures(const QString &filePath)
{
    QHash<QString, QJsonObject> failures;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return failures;

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const QString normalizedPath = QFileInfo(it.key()).absoluteFilePath();
        if (!normalizedPath.isEmpty()) failures.insert(normalizedPath, it.value().toObject());
    }
    return failures;
}

bool saveSyncFailures(const QString &filePath, const QHash<QString, QJsonObject> &failures)
{
    QJsonObject root;
    for (auto it = failures.cbegin(); it != failures.cend(); ++it) {
        root.insert(QFileInfo(it.key()).absoluteFilePath(), it.value());
    }
    return writeJsonObjectAtomically(filePath, root, QJsonDocument::Indented,
                                     "Unable to save model sync failures atomically:");
}

} // namespace ModelUserDataStore
