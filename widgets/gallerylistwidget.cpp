#include "gallerylistwidget.h"

GalleryListWidget::GalleryListWidget(QWidget *parent)
    : QListWidget(parent)
{
}

void GalleryListWidget::dataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight,
                                  const QList<int> &roles)
{
    if (viewMode() != QListView::IconMode || !topLeft.isValid() || !bottomRight.isValid()) {
        QListWidget::dataChanged(topLeft, bottomRight, roles);
        return;
    }

    // Qt's IconMode can reinsert hidden rows into its hit-test geometry when
    // their thumbnails finish loading. Their data is already in the model;
    // let the next unhide/layout read it instead of updating hidden geometry.
    int firstVisible = -1;
    for (int row = topLeft.row(); row <= bottomRight.row(); ++row) {
        if (!isRowHidden(row)) {
            if (firstVisible < 0) firstVisible = row;
        } else if (firstVisible >= 0) {
            QListWidget::dataChanged(model()->index(firstVisible, topLeft.column()),
                                     model()->index(row - 1, bottomRight.column()), roles);
            firstVisible = -1;
        }
    }
    if (firstVisible >= 0) {
        QListWidget::dataChanged(model()->index(firstVisible, topLeft.column()), bottomRight, roles);
    }
}
