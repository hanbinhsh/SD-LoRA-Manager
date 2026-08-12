#include "wd14batchmodel.h"

#include "styleconstants.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>

namespace {

bool isSupportedImage(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == "png" || suffix == "jpg" || suffix == "jpeg" || suffix == "webp";
}

QString canonicalKey(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower();
}

QString elided(const QFontMetrics &metrics, const QString &text, int width)
{
    return metrics.elidedText(text, Qt::ElideRight, qMax(1, width));
}

} // namespace

QVector<Wd14BatchItem> scanWd14BatchFolder(const QString &folder, bool recursive)
{
    QVector<Wd14BatchItem> items;
    const QDirIterator::IteratorFlags flags = recursive
        ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
    QDirIterator iterator(folder, QDir::Files | QDir::NoSymLinks, flags);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        if (!isSupportedImage(path)) continue;
        Wd14BatchItem item;
        item.imagePath = QFileInfo(path).absoluteFilePath();
        item.txtPath = QFileInfo(path).absolutePath() + '/' + QFileInfo(path).completeBaseName() + ".txt";
        items.append(item);
    }
    std::sort(items.begin(), items.end(), [](const Wd14BatchItem &a, const Wd14BatchItem &b) {
        return QString::localeAwareCompare(a.imagePath, b.imagePath) < 0;
    });

    QHash<QString, int> outputCounts;
    for (const Wd14BatchItem &item : std::as_const(items)) ++outputCounts[canonicalKey(item.txtPath)];
    for (Wd14BatchItem &item : items) {
        if (outputCounts.value(canonicalKey(item.txtPath)) > 1) {
            item.status = Wd14BatchStatus::Conflict;
            item.error = "多个图片映射到同一个 TXT。";
        }
    }
    return items;
}

QString wd14BatchStatusText(Wd14BatchStatus status)
{
    switch (status) {
    case Wd14BatchStatus::Waiting: return "等待";
    case Wd14BatchStatus::Running: return "运行中";
    case Wd14BatchStatus::Success: return "成功";
    case Wd14BatchStatus::Skipped: return "已有 TXT，已跳过";
    case Wd14BatchStatus::Conflict: return "输出冲突";
    case Wd14BatchStatus::Failed: return "失败";
    case Wd14BatchStatus::Stopped: return "已停止";
    }
    return "未知";
}

Wd14BatchModel::Wd14BatchModel(QObject *parent) : QAbstractListModel(parent) {}

int Wd14BatchModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

QVariant Wd14BatchModel::data(const QModelIndex &index, int role) const
{
    const Wd14BatchItem *item = itemAt(index.row());
    if (!item) return {};
    if (role == Qt::DisplayRole) return QFileInfo(item->imagePath).fileName();
    if (role == ImagePathRole) return item->imagePath;
    if (role == TxtPathRole) return item->txtPath;
    if (role == StatusRole) return int(item->status);
    if (role == Qt::ToolTipRole) {
        return QString("%1\n%2\n%3").arg(item->imagePath, item->txtPath,
            item->error.isEmpty() ? item->finalTags : item->error);
    }
    if (role == ThumbnailRole) {
        const auto it = m_thumbnailCache.constFind(item->imagePath);
        if (it != m_thumbnailCache.cend()) return *it;
        requestThumbnail(item->imagePath);
    }
    return {};
}

void Wd14BatchModel::setItems(QVector<Wd14BatchItem> items)
{
    beginResetModel();
    m_items = std::move(items);
    rebuildPathRows();
    endResetModel();
}

void Wd14BatchModel::clear()
{
    beginResetModel();
    m_items.clear();
    m_pathRows.clear();
    m_thumbnailCache.clear();
    m_thumbnailLru.clear();
    endResetModel();
}

const Wd14BatchItem *Wd14BatchModel::itemAt(int row) const
{
    return row >= 0 && row < m_items.size() ? &m_items.at(row) : nullptr;
}

QVector<Wd14BatchItem> &Wd14BatchModel::items() { return m_items; }
const QVector<Wd14BatchItem> &Wd14BatchModel::items() const { return m_items; }

int Wd14BatchModel::rowForPath(const QString &path) const
{
    return m_pathRows.value(canonicalKey(path), -1);
}

void Wd14BatchModel::notifyRow(int row)
{
    if (row >= 0 && row < m_items.size()) emit dataChanged(index(row), index(row));
}

void Wd14BatchModel::notifyAll()
{
    if (!m_items.isEmpty()) emit dataChanged(index(0), index(m_items.size() - 1));
}

void Wd14BatchModel::rebuildPathRows()
{
    m_pathRows.clear();
    for (int i = 0; i < m_items.size(); ++i) m_pathRows.insert(canonicalKey(m_items.at(i).imagePath), i);
}

void Wd14BatchModel::requestThumbnail(const QString &path) const
{
    if (path.isEmpty() || m_pendingThumbnails.contains(path)) return;
    auto *self = const_cast<Wd14BatchModel *>(this);
    m_pendingThumbnails.insert(path);
    auto *watcher = new QFutureWatcher<QImage>(self);
    connect(watcher, &QFutureWatcher<QImage>::finished, self, [self, watcher, path]() {
        const QImage image = watcher->result();
        watcher->deleteLater();
        self->m_pendingThumbnails.remove(path);
        self->m_thumbnailCache.insert(path, image);
        self->m_thumbnailLru.removeAll(path);
        self->m_thumbnailLru.append(path);
        while (self->m_thumbnailLru.size() > 128)
            self->m_thumbnailCache.remove(self->m_thumbnailLru.takeFirst());
        const int row = self->rowForPath(path);
        if (row >= 0) emit self->dataChanged(self->index(row), self->index(row), {ThumbnailRole});
    });
    watcher->setFuture(QtConcurrent::run([path]() {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QSize size = reader.size();
        if (size.isValid()) reader.setScaledSize(size.scaled(64, 64, Qt::KeepAspectRatioByExpanding));
        return reader.read();
    }));
}

Wd14BatchDelegate::Wd14BatchDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

void Wd14BatchDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                               const QModelIndex &index) const
{
    const auto *model = qobject_cast<const Wd14BatchModel *>(index.model());
    const Wd14BatchItem *item = model ? model->itemAt(index.row()) : nullptr;
    if (!item) return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    QRect card = option.rect.adjusted(4, 3, -4, -3);
    QColor bg = option.state.testFlag(QStyle::State_Selected)
        ? AppStyle::color("selectionBg")
        : option.state.testFlag(QStyle::State_MouseOver)
            ? AppStyle::color("templateHoverBg") : AppStyle::color("templateCardBg");
    painter->setPen(AppStyle::color("inputBorder"));
    painter->setBrush(bg);
    painter->drawRoundedRect(card, 7, 7);

    QRect thumb(card.left() + 9, card.top() + 8, 64, 64);
    QPainterPath clip;
    clip.addRoundedRect(thumb, 6, 6);
    painter->save();
    painter->setClipPath(clip);
    painter->fillRect(thumb, AppStyle::color("inputBg"));
    const QImage image = index.data(Wd14BatchModel::ThumbnailRole).value<QImage>();
    if (!image.isNull()) {
        const QImage scaled = image.scaled(thumb.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        const QPoint offset((scaled.width() - thumb.width()) / 2, (scaled.height() - thumb.height()) / 2);
        painter->drawImage(thumb, scaled, QRect(offset, thumb.size()));
    } else {
        painter->setPen(AppStyle::color("placeholderText"));
        painter->drawText(thumb, Qt::AlignCenter, "加载中");
    }
    painter->restore();

    const int left = thumb.right() + 11;
    const int width = card.right() - left - 10;
    QFont title = option.font;
    title.setBold(true);
    painter->setFont(title);
    painter->setPen(AppStyle::color("bodyText"));
    painter->drawText(QRect(left, card.top() + 7, width, 20), Qt::AlignVCenter,
                      elided(QFontMetrics(title), QFileInfo(item->imagePath).fileName(), width));
    painter->setFont(option.font);
    QColor statusColor = AppStyle::color("mutedText");
    if (item->status == Wd14BatchStatus::Success) statusColor = AppStyle::color("successGreen");
    if (item->status == Wd14BatchStatus::Failed || item->status == Wd14BatchStatus::Conflict)
        statusColor = AppStyle::color("errorRed");
    painter->setPen(statusColor);
    const QString status = QString("%1  |  %2 Tags  |  %3 sec.")
        .arg(wd14BatchStatusText(item->status)).arg(item->tagCount).arg(item->elapsedSec, 0, 'f', 2);
    painter->drawText(QRect(left, card.top() + 30, width, 18), Qt::AlignVCenter,
                      elided(option.fontMetrics, status, width));
    painter->setPen(AppStyle::color("mutedText"));
    const QString detail = item->error.isEmpty() ? item->txtPath : item->error;
    painter->drawText(QRect(left, card.top() + 52, width, 18), Qt::AlignVCenter,
                      elided(option.fontMetrics, detail, width));
    painter->restore();
}

QSize Wd14BatchDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return QSize(600, 84);
}
