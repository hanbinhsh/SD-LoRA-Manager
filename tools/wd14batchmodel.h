#ifndef WD14BATCHMODEL_H
#define WD14BATCHMODEL_H

#include <QAbstractListModel>
#include <QHash>
#include <QImage>
#include <QSet>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QVector>

enum class Wd14BatchStatus {
    Waiting,
    Running,
    Success,
    Skipped,
    Conflict,
    Failed,
    Stopped
};

struct Wd14BatchItem
{
    QString imagePath;
    QString txtPath;
    Wd14BatchStatus status = Wd14BatchStatus::Waiting;
    QString finalTags;
    QString error;
    int tagCount = 0;
    double elapsedSec = 0.0;
};

QVector<Wd14BatchItem> scanWd14BatchFolder(const QString &folder, bool recursive);
QString wd14BatchStatusText(Wd14BatchStatus status);

class Wd14BatchModel final : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role {
        ImagePathRole = Qt::UserRole + 1,
        TxtPathRole,
        StatusRole,
        ThumbnailRole
    };

    explicit Wd14BatchModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    void setItems(QVector<Wd14BatchItem> items);
    void clear();
    const Wd14BatchItem *itemAt(int row) const;
    QVector<Wd14BatchItem> &items();
    const QVector<Wd14BatchItem> &items() const;
    int rowForPath(const QString &path) const;
    void notifyRow(int row);
    void notifyAll();

private:
    void rebuildPathRows();
    void requestThumbnail(const QString &path) const;

    QVector<Wd14BatchItem> m_items;
    QHash<QString, int> m_pathRows;
    mutable QHash<QString, QImage> m_thumbnailCache;
    mutable QStringList m_thumbnailLru;
    mutable QSet<QString> m_pendingThumbnails;
};

class Wd14BatchDelegate final : public QStyledItemDelegate
{
public:
    explicit Wd14BatchDelegate(QObject *parent = nullptr);
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;
};

#endif // WD14BATCHMODEL_H
