#ifndef HIGHLIGHTITEMDELEGATE_H
#define HIGHLIGHTITEMDELEGATE_H

#include <QStyledItemDelegate>

class HighlightItemDelegate : public QStyledItemDelegate
{
public:
    explicit HighlightItemDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

#endif // HIGHLIGHTITEMDELEGATE_H
