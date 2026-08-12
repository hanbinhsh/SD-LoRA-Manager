#ifndef WD14HISTORYMODEL_H
#define WD14HISTORYMODEL_H

#include <QAbstractListModel>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QSet>
#include <QStyledItemDelegate>
#include <QVector>

struct Wd14TagScore
{
    QString tag;
    QString category;
    float confidence = 0.0f;
    QString translation;
    QString priority;
    int usageCount = 0;
};

struct Wd14InferenceResult
{
    bool ok = false;
    QString error;
    QString finalTags;
    QVector<Wd14TagScore> ratings;
    QVector<Wd14TagScore> tags;
    double elapsedSec = 0.0;
    quint64 totalMemory = 0;
    quint64 availableMemory = 0;
};

struct Wd14RenderSettings
{
    QString modelDir;
    QString presetName;
    double threshold = 0.35;
    QString additionalTags;
    QString excludeTags;
    QString defaultExclude;
    bool sortAlphabetically = false;
    bool includeConfidence = false;
    bool replaceUnderscore = true;
    bool escapeBrackets = false;
};

struct Wd14HistoryEntry
{
    QString id;
    QDateTime createdAt;
    QString imagePath;
    Wd14RenderSettings settings;
    Wd14InferenceResult result;
    QString finalTags;
};

QJsonObject wd14HistoryEntryToJson(const Wd14HistoryEntry &entry);
bool wd14HistoryEntryFromJson(const QJsonObject &object, Wd14HistoryEntry *entry);
QVector<Wd14HistoryEntry> loadWd14HistoryFile(const QString &path);

class Wd14HistoryModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        EntryIdRole = Qt::UserRole + 1,
        ThumbnailRole,
        MissingImageRole
    };

    explicit Wd14HistoryModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    void setEntries(QVector<Wd14HistoryEntry> entries);
    void prependEntry(const Wd14HistoryEntry &entry);
    void setSearchText(const QString &text);
    void setNewestFirst(bool newestFirst);
    const Wd14HistoryEntry *entryAt(int row) const;
    QVector<Wd14HistoryEntry> allEntries() const;
    void removeIds(const QSet<QString> &ids);
    void clear();

private:
    void rebuildVisibleRows();
    void requestThumbnail(const QString &path) const;
    bool matchesSearch(const Wd14HistoryEntry &entry) const;

    QVector<Wd14HistoryEntry> m_entries;
    QVector<int> m_visibleRows;
    QString m_searchText;
    bool m_newestFirst = true;
    mutable QHash<QString, QImage> m_thumbnailCache;
    mutable QStringList m_thumbnailLru;
    mutable QSet<QString> m_pendingThumbnails;
};

class Wd14HistoryDelegate final : public QStyledItemDelegate
{
public:
    explicit Wd14HistoryDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;
};

#endif // WD14HISTORYMODEL_H
