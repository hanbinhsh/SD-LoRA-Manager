#pragma once

#include "utils/tagsearchindex.h"
#include <QFutureWatcher>
#include <QSortFilterProxyModel>
#include <QTimer>

// Flat editable tables: index source text once, filter immutable snapshots off-thread.
class TagSearchProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    explicit TagSearchProxyModel(QObject *parent = nullptr);
    ~TagSearchProxyModel() override;
    void setSourceModel(QAbstractItemModel *model) override;
    void setSearchColumns(int tagColumn, int translationColumn);
    void setSearchText(const QString &text);
    void setMatchMode(int mode);
    void applySearchNow();
    bool isSearching() const { return m_searching; }

signals:
    void searchStateChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    TagSearch::Row readRow(QAbstractItemModel *model, int row) const;
    void rebuildIndex(QAbstractItemModel *model);
    void sourceChanged();
    void scheduleSearch(int delay);
    void startSearch();
    void setSearching(bool searching);
    void applyMatches(QVector<quint8> matches, const TagSearch::Query &query);

    int m_tagColumn = 0;
    int m_translationColumn = 1;
    QVector<TagSearch::Row> m_rows;
    QVector<quint8> m_matches;
    TagSearch::Query m_query;
    TagSearch::Query m_appliedQuery;
    QVector<QMetaObject::Connection> m_sourceConnections;
    QTimer m_timer;
    QFutureWatcher<QVector<quint8>> *m_watcher = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    quint64 m_generation = 0;
    bool m_searching = false;
};
