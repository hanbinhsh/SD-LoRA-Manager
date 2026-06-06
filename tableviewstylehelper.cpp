#include "tableviewstylehelper.h"

#include <QAbstractItemView>
#include <QEvent>
#include <QHeaderView>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QWidget>

class UnifiedTableRowDelegate : public QStyledItemDelegate
{
public:
    explicit UnifiedTableRowDelegate(QTableView *view)
        : QStyledItemDelegate(view)
        , m_view(view)
    {
        if (m_view && m_view->viewport()) {
            m_view->viewport()->setMouseTracking(true);
            m_view->viewport()->installEventFilter(this);
        }
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (!m_view || watched != m_view->viewport()) {
            return QStyledItemDelegate::eventFilter(watched, event);
        }

        int nextHoverRow = m_hoverRow;
        if (event->type() == QEvent::MouseMove) {
            const auto *mouseEvent = static_cast<QMouseEvent*>(event);
            nextHoverRow = m_view->indexAt(mouseEvent->position().toPoint()).row();
        } else if (event->type() == QEvent::Leave) {
            nextHoverRow = -1;
        }

        if (nextHoverRow != m_hoverRow) {
            const int previousRow = m_hoverRow;
            m_hoverRow = nextHoverRow;
            updateRow(previousRow);
            updateRow(m_hoverRow);
        }

        return QStyledItemDelegate::eventFilter(watched, event);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (!m_view || !index.isValid()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);

        const bool selected = opt.state.testFlag(QStyle::State_Selected);
        const bool hovered = index.row() == m_hoverRow;
        if (selected || hovered) {
            paintRowSegment(painter, option.rect, index.column(), selected);
        }

        opt.state &= ~QStyle::State_Selected;
        opt.state &= ~QStyle::State_MouseOver;
        opt.backgroundBrush = Qt::NoBrush;
        if (selected) {
            opt.palette.setColor(QPalette::Text, QColor("#ffffff"));
            opt.palette.setColor(QPalette::HighlightedText, QColor("#ffffff"));
        }

        QStyledItemDelegate::paint(painter, opt, index);
    }

private:
    int firstVisibleColumn() const
    {
        if (!m_view || !m_view->model()) return -1;
        QHeaderView *header = m_view->horizontalHeader();
        for (int visual = 0; visual < header->count(); ++visual) {
            const int logical = header->logicalIndex(visual);
            if (!m_view->isColumnHidden(logical) && header->sectionSize(logical) > 0) return logical;
        }
        return -1;
    }

    int lastVisibleColumn() const
    {
        if (!m_view || !m_view->model()) return -1;
        QHeaderView *header = m_view->horizontalHeader();
        for (int visual = header->count() - 1; visual >= 0; --visual) {
            const int logical = header->logicalIndex(visual);
            if (!m_view->isColumnHidden(logical) && header->sectionSize(logical) > 0) return logical;
        }
        return -1;
    }

    void paintRowSegment(QPainter *painter, const QRect &cellRect, int column, bool selected) const
    {
        const int firstColumn = firstVisibleColumn();
        const int lastColumn = lastVisibleColumn();
        if (firstColumn < 0 || lastColumn < 0) return;

        QRect rect = cellRect.adjusted(column == firstColumn ? 2 : 0, 1, column == lastColumn ? -2 : 0, -1);
        if (!rect.isValid()) return;

        painter->save();
        painter->setPen(Qt::NoPen);
        painter->fillRect(rect, selected ? QColor("#3d5677") : QColor(255, 255, 255, 18));
        painter->restore();
    }

    void updateRow(int row)
    {
        if (!m_view || !m_view->model() || row < 0 || row >= m_view->model()->rowCount()) return;
        const QModelIndex first = m_view->model()->index(row, firstVisibleColumn(), m_view->rootIndex());
        const QRect rect = m_view->visualRect(first);
        if (!rect.isValid()) {
            m_view->viewport()->update();
            return;
        }
        m_view->viewport()->update(QRect(0, rect.y(), m_view->viewport()->width(), rect.height()));
    }

    QTableView *m_view = nullptr;
    int m_hoverRow = -1;
};

void applyUnifiedTableRowStyle(QTableView *table)
{
    if (!table) return;
    if (table->property("unifiedRowStyleInstalled").toBool()) return;

    table->setProperty("unifiedRowStyleInstalled", true);
    table->setShowGrid(false);
    table->setMouseTracking(true);
    table->viewport()->setMouseTracking(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setFocusPolicy(Qt::NoFocus);
    table->setItemDelegate(new UnifiedTableRowDelegate(table));
}

void applyUnifiedTableRowStyle(QWidget *root)
{
    if (!root) return;
    const auto tables = root->findChildren<QTableView*>();
    for (QTableView *table : tables) {
        applyUnifiedTableRowStyle(table);
    }
}
