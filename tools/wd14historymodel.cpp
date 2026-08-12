#include "wd14historymodel.h"

#include "styleconstants.h"

#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPainterPath>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>

namespace {

QJsonObject scoreToJson(const Wd14TagScore &score)
{
    QJsonObject object;
    object["tag"] = score.tag;
    object["confidence"] = score.confidence;
    if (!score.category.isEmpty()) object["category"] = score.category;
    return object;
}

Wd14TagScore scoreFromJson(const QJsonObject &object)
{
    Wd14TagScore score;
    score.tag = object.value("tag").toString();
    score.confidence = float(object.value("confidence").toDouble());
    score.category = object.value("category").toString();
    return score;
}

QJsonArray scoresToJson(const QVector<Wd14TagScore> &scores)
{
    QJsonArray array;
    for (const Wd14TagScore &score : scores) array.append(scoreToJson(score));
    return array;
}

QVector<Wd14TagScore> scoresFromJson(const QJsonArray &array)
{
    QVector<Wd14TagScore> scores;
    scores.reserve(array.size());
    for (const QJsonValue &value : array) {
        const Wd14TagScore score = scoreFromJson(value.toObject());
        if (!score.tag.isEmpty()) scores.append(score);
    }
    return scores;
}

QString normalizedSearchText(QString text)
{
    text = text.trimmed().toLower();
    text.replace('_', ' ');
    return text.simplified();
}

QString elidedText(const QFontMetrics &metrics, const QString &text, int width)
{
    return metrics.elidedText(text, Qt::ElideRight, qMax(1, width));
}

} // namespace

QJsonObject wd14HistoryEntryToJson(const Wd14HistoryEntry &entry)
{
    QJsonObject settings;
    settings["model_dir"] = entry.settings.modelDir;
    settings["preset"] = entry.settings.presetName;
    settings["threshold"] = entry.settings.threshold;
    settings["additional_tags"] = entry.settings.additionalTags;
    settings["exclude_tags"] = entry.settings.excludeTags;
    settings["default_exclude"] = entry.settings.defaultExclude;
    settings["sort_alpha"] = entry.settings.sortAlphabetically;
    settings["include_confidence"] = entry.settings.includeConfidence;
    settings["replace_underscore"] = entry.settings.replaceUnderscore;
    settings["escape_brackets"] = entry.settings.escapeBrackets;

    QJsonObject object;
    object["version"] = 1;
    object["id"] = entry.id;
    object["created_at"] = entry.createdAt.toUTC().toString(Qt::ISODateWithMs);
    object["image_path"] = entry.imagePath;
    object["settings"] = settings;
    object["elapsed_sec"] = entry.result.elapsedSec;
    object["total_memory"] = QString::number(entry.result.totalMemory);
    object["available_memory"] = QString::number(entry.result.availableMemory);
    object["ratings"] = scoresToJson(entry.result.ratings);
    object["tags"] = scoresToJson(entry.result.tags);
    object["final_tags"] = entry.finalTags;
    return object;
}

bool wd14HistoryEntryFromJson(const QJsonObject &object, Wd14HistoryEntry *entry)
{
    if (!entry) return false;
    Wd14HistoryEntry parsed;
    parsed.id = object.value("id").toString();
    parsed.createdAt = QDateTime::fromString(object.value("created_at").toString(), Qt::ISODateWithMs);
    if (!parsed.createdAt.isValid()) {
        parsed.createdAt = QDateTime::fromString(object.value("created_at").toString(), Qt::ISODate);
    }
    parsed.imagePath = object.value("image_path").toString();
    parsed.finalTags = object.value("final_tags").toString();
    const QJsonObject settings = object.value("settings").toObject();
    parsed.settings.modelDir = settings.value("model_dir").toString();
    parsed.settings.presetName = settings.value("preset").toString();
    parsed.settings.threshold = settings.value("threshold").toDouble(0.35);
    parsed.settings.additionalTags = settings.value("additional_tags").toString();
    parsed.settings.excludeTags = settings.value("exclude_tags").toString();
    parsed.settings.defaultExclude = settings.value("default_exclude").toString();
    parsed.settings.sortAlphabetically = settings.value("sort_alpha").toBool(false);
    parsed.settings.includeConfidence = settings.value("include_confidence").toBool(false);
    parsed.settings.replaceUnderscore = settings.value("replace_underscore").toBool(true);
    parsed.settings.escapeBrackets = settings.value("escape_brackets").toBool(false);
    parsed.result.ok = true;
    parsed.result.elapsedSec = object.value("elapsed_sec").toDouble();
    parsed.result.totalMemory = object.value("total_memory").toString().toULongLong();
    parsed.result.availableMemory = object.value("available_memory").toString().toULongLong();
    parsed.result.ratings = scoresFromJson(object.value("ratings").toArray());
    parsed.result.tags = scoresFromJson(object.value("tags").toArray());
    parsed.result.finalTags = parsed.finalTags;
    if (parsed.id.isEmpty() || !parsed.createdAt.isValid()) return false;
    *entry = parsed;
    return true;
}

QVector<Wd14HistoryEntry> loadWd14HistoryFile(const QString &path)
{
    QVector<Wd14HistoryEntry> entries;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return entries;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) continue;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) continue;
        Wd14HistoryEntry entry;
        if (wd14HistoryEntryFromJson(document.object(), &entry)) entries.append(entry);
    }
    return entries;
}

Wd14HistoryModel::Wd14HistoryModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int Wd14HistoryModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_visibleRows.size();
}

QVariant Wd14HistoryModel::data(const QModelIndex &index, int role) const
{
    const Wd14HistoryEntry *entry = entryAt(index.row());
    if (!entry) return {};
    if (role == Qt::DisplayRole) return QFileInfo(entry->imagePath).fileName();
    if (role == EntryIdRole) return entry->id;
    if (role == MissingImageRole) return !QFileInfo::exists(entry->imagePath);
    if (role == Qt::ToolTipRole) {
        return QString("%1\n%2\n%3")
            .arg(QFileInfo(entry->imagePath).fileName(), entry->imagePath,
                 entry->createdAt.toLocalTime().toString("yyyy-MM-dd HH:mm:ss"));
    }
    if (role == ThumbnailRole) {
        const auto it = m_thumbnailCache.constFind(entry->imagePath);
        if (it != m_thumbnailCache.cend()) return *it;
        requestThumbnail(entry->imagePath);
    }
    return {};
}

void Wd14HistoryModel::setEntries(QVector<Wd14HistoryEntry> entries)
{
    beginResetModel();
    m_entries = std::move(entries);
    m_visibleRows.clear();
    for (int i = 0; i < m_entries.size(); ++i) {
        if (matchesSearch(m_entries.at(i))) m_visibleRows.append(i);
    }
    std::sort(m_visibleRows.begin(), m_visibleRows.end(), [this](int left, int right) {
        const QDateTime a = m_entries.at(left).createdAt;
        const QDateTime b = m_entries.at(right).createdAt;
        if (a == b) return m_entries.at(left).id < m_entries.at(right).id;
        return m_newestFirst ? a > b : a < b;
    });
    endResetModel();
}

void Wd14HistoryModel::prependEntry(const Wd14HistoryEntry &entry)
{
    m_entries.prepend(entry);
    rebuildVisibleRows();
}

void Wd14HistoryModel::setSearchText(const QString &text)
{
    const QString normalized = normalizedSearchText(text);
    if (m_searchText == normalized) return;
    m_searchText = normalized;
    rebuildVisibleRows();
}

void Wd14HistoryModel::setNewestFirst(bool newestFirst)
{
    if (m_newestFirst == newestFirst) return;
    m_newestFirst = newestFirst;
    rebuildVisibleRows();
}

const Wd14HistoryEntry *Wd14HistoryModel::entryAt(int row) const
{
    if (row < 0 || row >= m_visibleRows.size()) return nullptr;
    const int sourceRow = m_visibleRows.at(row);
    return sourceRow >= 0 && sourceRow < m_entries.size() ? &m_entries.at(sourceRow) : nullptr;
}

QVector<Wd14HistoryEntry> Wd14HistoryModel::allEntries() const
{
    return m_entries;
}

void Wd14HistoryModel::removeIds(const QSet<QString> &ids)
{
    if (ids.isEmpty()) return;
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), [&ids](const Wd14HistoryEntry &entry) {
        return ids.contains(entry.id);
    }), m_entries.end());
    rebuildVisibleRows();
}

void Wd14HistoryModel::clear()
{
    beginResetModel();
    m_entries.clear();
    m_visibleRows.clear();
    m_thumbnailCache.clear();
    m_thumbnailLru.clear();
    endResetModel();
}

void Wd14HistoryModel::rebuildVisibleRows()
{
    beginResetModel();
    m_visibleRows.clear();
    for (int i = 0; i < m_entries.size(); ++i) {
        if (matchesSearch(m_entries.at(i))) m_visibleRows.append(i);
    }
    std::sort(m_visibleRows.begin(), m_visibleRows.end(), [this](int left, int right) {
        const QDateTime a = m_entries.at(left).createdAt;
        const QDateTime b = m_entries.at(right).createdAt;
        if (a == b) return m_entries.at(left).id < m_entries.at(right).id;
        return m_newestFirst ? a > b : a < b;
    });
    endResetModel();
}

bool Wd14HistoryModel::matchesSearch(const Wd14HistoryEntry &entry) const
{
    if (m_searchText.isEmpty()) return true;
    QString haystack = entry.imagePath + ' ' + QFileInfo(entry.settings.modelDir).fileName() + ' ' + entry.finalTags;
    for (const Wd14TagScore &tag : entry.result.tags) haystack += ' ' + tag.tag;
    return normalizedSearchText(haystack).contains(m_searchText);
}

void Wd14HistoryModel::requestThumbnail(const QString &path) const
{
    if (path.isEmpty() || m_pendingThumbnails.contains(path)) return;
    m_pendingThumbnails.insert(path);
    auto *self = const_cast<Wd14HistoryModel *>(this);
    auto *watcher = new QFutureWatcher<QImage>(self);
    connect(watcher, &QFutureWatcher<QImage>::finished, self,
            [self, watcher, path]() {
        const QImage image = watcher->result();
        watcher->deleteLater();
        self->m_pendingThumbnails.remove(path);
        self->m_thumbnailCache.insert(path, image);
        self->m_thumbnailLru.removeAll(path);
        self->m_thumbnailLru.append(path);
        while (self->m_thumbnailLru.size() > 128) {
            self->m_thumbnailCache.remove(self->m_thumbnailLru.takeFirst());
        }
        for (int row = 0; row < self->m_visibleRows.size(); ++row) {
            if (self->m_entries.at(self->m_visibleRows.at(row)).imagePath == path) {
                emit self->dataChanged(self->index(row), self->index(row), {ThumbnailRole});
            }
        }
    });
    watcher->setFuture(QtConcurrent::run([path]() {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QSize original = reader.size();
        if (original.isValid()) reader.setScaledSize(original.scaled(72, 72, Qt::KeepAspectRatioByExpanding));
        return reader.read();
    }));
}

Wd14HistoryDelegate::Wd14HistoryDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

void Wd14HistoryDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const
{
    const auto *model = qobject_cast<const Wd14HistoryModel *>(index.model());
    const Wd14HistoryEntry *entry = model ? model->entryAt(index.row()) : nullptr;
    if (!entry) return;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    QRect card = option.rect.adjusted(4, 4, -4, -4);
    QColor background = AppStyle::color("templateCardBg");
    if (option.state.testFlag(QStyle::State_Selected)) background = AppStyle::color("selectionBg");
    else if (option.state.testFlag(QStyle::State_MouseOver)) background = AppStyle::color("templateHoverBg");
    painter->setPen(QPen(AppStyle::color("inputBorder")));
    painter->setBrush(background);
    painter->drawRoundedRect(card, 7, 7);

    const QRect thumbRect(card.left() + 10, card.top() + 10, 72, 72);
    const QImage thumbnail = index.data(Wd14HistoryModel::ThumbnailRole).value<QImage>();
    QPainterPath clip;
    clip.addRoundedRect(thumbRect, 6, 6);
    painter->save();
    painter->setClipPath(clip);
    painter->fillRect(thumbRect, AppStyle::color("inputBg"));
    if (!thumbnail.isNull()) {
        const QImage scaled = thumbnail.scaled(thumbRect.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        const QPoint offset((scaled.width() - thumbRect.width()) / 2, (scaled.height() - thumbRect.height()) / 2);
        painter->drawImage(thumbRect, scaled, QRect(offset, thumbRect.size()));
    } else {
        painter->setPen(AppStyle::color("placeholderText"));
        painter->drawText(thumbRect, Qt::AlignCenter, index.data(Wd14HistoryModel::MissingImageRole).toBool() ? "图片已丢失" : "加载中");
    }
    painter->restore();

    const int textLeft = thumbRect.right() + 12;
    const int textWidth = card.right() - textLeft - 10;
    QFont titleFont = option.font;
    titleFont.setBold(true);
    painter->setFont(titleFont);
    painter->setPen(AppStyle::color("bodyText"));
    painter->drawText(QRect(textLeft, card.top() + 9, textWidth, 20), Qt::AlignVCenter,
                      elidedText(QFontMetrics(titleFont), QFileInfo(entry->imagePath).fileName(), textWidth));

    painter->setFont(option.font);
    painter->setPen(AppStyle::color("mutedText"));
    const QString modelName = QFileInfo(entry->settings.modelDir).fileName();
    const QString detail = QString("%1  |  阈值 %2  |  %3 sec.  |  %4 Tags")
        .arg(modelName.isEmpty() ? "未知模型" : modelName)
        .arg(entry->settings.threshold, 0, 'f', 2)
        .arg(entry->result.elapsedSec, 0, 'f', 2)
        .arg(entry->result.tags.size());
    painter->drawText(QRect(textLeft, card.top() + 32, textWidth, 18), Qt::AlignVCenter,
                      elidedText(option.fontMetrics, detail, textWidth));
    painter->drawText(QRect(textLeft, card.top() + 54, textWidth, 18), Qt::AlignVCenter,
                      elidedText(option.fontMetrics, entry->finalTags, textWidth));
    painter->drawText(QRect(textLeft, card.top() + 75, textWidth, 16), Qt::AlignVCenter,
                      entry->createdAt.toLocalTime().toString("yyyy-MM-dd HH:mm:ss"));
    painter->restore();
}

QSize Wd14HistoryDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return QSize(360, 104);
}
