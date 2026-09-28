#include "tagsearchproxymodel.h"

#include <QtConcurrent>
#include <utility>

TagSearchProxyModel::TagSearchProxyModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(false);
    setSortRole(Qt::UserRole);
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &TagSearchProxyModel::startSearch);
}

TagSearchProxyModel::~TagSearchProxyModel()
{
    if (m_cancelled) m_cancelled->store(true, std::memory_order_relaxed);
}

TagSearch::Row TagSearchProxyModel::readRow(QAbstractItemModel *model, int row) const
{
    return {TagSearch::normalizedText(model->index(row, m_tagColumn).data().toString()),
            TagSearch::normalizedText(model->index(row, m_translationColumn).data().toString())};
}

void TagSearchProxyModel::rebuildIndex(QAbstractItemModel *model)
{
    m_rows.clear();
    m_matches.clear();
    if (model) {
        const int count = model->rowCount();
        m_rows.reserve(count);
        m_matches.reserve(count);
        for (int row = 0; row < count; ++row) {
            m_rows.append(readRow(model, row));
            m_matches.append(TagSearch::matches(m_rows.last(), m_appliedQuery));
        }
    }
    sourceChanged();
}

void TagSearchProxyModel::setSourceModel(QAbstractItemModel *model)
{
    if (model == sourceModel()) return;
    for (const auto &connection : std::as_const(m_sourceConnections)) disconnect(connection);
    m_sourceConnections.clear();
    rebuildIndex(model);
    if (model) {
        // Update row-aligned caches before the base proxy handles source notifications.
        m_sourceConnections.append(connect(model, &QAbstractItemModel::rowsInserted, this,
            [this, model](const QModelIndex &parent, int first, int last) {
                if (parent.isValid()) return;
                m_rows.insert(first, last - first + 1, {});
                m_matches.insert(first, last - first + 1, 0);
                for (int row = first; row <= last; ++row) {
                    m_rows[row] = readRow(model, row);
                    m_matches[row] = TagSearch::matches(m_rows.at(row), m_appliedQuery);
                }
                sourceChanged();
            }));
        m_sourceConnections.append(connect(model, &QAbstractItemModel::rowsRemoved, this,
            [this](const QModelIndex &parent, int first, int last) {
                if (parent.isValid()) return;
                m_rows.remove(first, last - first + 1);
                m_matches.remove(first, last - first + 1);
                sourceChanged();
            }));
        m_sourceConnections.append(connect(model, &QAbstractItemModel::dataChanged, this,
            [this, model](const QModelIndex &first, const QModelIndex &last, const QList<int> &roles) {
                if (first.parent().isValid()
                    || (!roles.isEmpty() && !roles.contains(Qt::DisplayRole) && !roles.contains(Qt::EditRole))) return;
                if ((m_tagColumn < first.column() || m_tagColumn > last.column())
                    && (m_translationColumn < first.column() || m_translationColumn > last.column())) return;
                for (int row = first.row(); row <= last.row(); ++row) m_rows[row] = readRow(model, row);
                sourceChanged();
            }));
        const auto rebuild = [this, model]() { rebuildIndex(model); };
        m_sourceConnections.append(connect(model, &QAbstractItemModel::modelReset, this, rebuild));
        m_sourceConnections.append(connect(model, &QAbstractItemModel::layoutChanged, this, rebuild));
        m_sourceConnections.append(connect(model, &QAbstractItemModel::rowsMoved, this, rebuild));
        m_sourceConnections.append(connect(model, &QAbstractItemModel::columnsInserted, this, rebuild));
        m_sourceConnections.append(connect(model, &QAbstractItemModel::columnsRemoved, this, rebuild));
        m_sourceConnections.append(connect(model, &QObject::destroyed, this, [this]() { rebuildIndex(nullptr); }));
    }
    QSortFilterProxyModel::setSourceModel(model);
}

void TagSearchProxyModel::setSearchColumns(int tagColumn, int translationColumn)
{
    if (m_tagColumn == tagColumn && m_translationColumn == translationColumn) return;
    m_tagColumn = tagColumn;
    m_translationColumn = translationColumn;
    rebuildIndex(sourceModel());
}

void TagSearchProxyModel::sourceChanged()
{
    if (m_query.text.isEmpty() && m_appliedQuery.text.isEmpty()) return;
    scheduleSearch(0);
}

void TagSearchProxyModel::setSearchText(const QString &text)
{
    const QString normalized = TagSearch::normalizedText(text);
    if (m_query.text == normalized) return;
    m_query.text = normalized;
    scheduleSearch(normalized.isEmpty() ? 0 : 180);
}

void TagSearchProxyModel::setMatchMode(int mode)
{
    const auto next = mode == TagSearch::WordMatch ? TagSearch::WordMatch
        : (mode == TagSearch::ExactMatch ? TagSearch::ExactMatch : TagSearch::ContainsMatch);
    if (m_query.mode == next) return;
    m_query.mode = next;
    scheduleSearch(0);
}

void TagSearchProxyModel::applySearchNow()
{
    if (m_searching) {
        m_timer.stop();
        startSearch();
    }
}

void TagSearchProxyModel::scheduleSearch(int delay)
{
    ++m_generation;
    if (m_cancelled) m_cancelled->store(true, std::memory_order_relaxed);
    m_timer.start(delay);
    setSearching(true);
}

void TagSearchProxyModel::startSearch()
{
    if (m_query.text.isEmpty()) {
        applyMatches(QVector<quint8>(m_rows.size(), 1), m_query);
        return;
    }
    if (m_watcher) return; // Coalesce requests; at most one worker per table.
    const quint64 generation = m_generation;
    const auto query = m_query;
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    m_watcher = new QFutureWatcher<QVector<quint8>>(this);
    connect(m_watcher, &QFutureWatcherBase::finished, this, [this, generation, query]() {
        const auto result = m_watcher->result();
        m_watcher->deleteLater();
        m_watcher = nullptr;
        if (generation != m_generation) {
            if (m_searching && !m_timer.isActive()) m_timer.start(0);
            return;
        }
        applyMatches(result, query);
    });
    // No model, widget or this pointer is accessed by the worker.
    m_watcher->setFuture(QtConcurrent::run(&TagSearch::matchRows, m_rows, query, m_cancelled));
}

void TagSearchProxyModel::applyMatches(QVector<quint8> matches, const TagSearch::Query &query)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif
    m_matches = std::move(matches);
    m_appliedQuery = query;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(Direction::Rows);
#else
    invalidateFilter();
#endif
    setSearching(false);
}

void TagSearchProxyModel::setSearching(bool searching)
{
    if (m_searching == searching) return;
    m_searching = searching;
    emit searchStateChanged();
}

bool TagSearchProxyModel::filterAcceptsRow(int row, const QModelIndex &parent) const
{
    if (parent.isValid()) return false;
    return row >= 0 && row < m_matches.size() && m_matches.at(row);
}
