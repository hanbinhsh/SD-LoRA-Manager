#pragma once

#include <QListWidget>

class GalleryListWidget : public QListWidget
{
    Q_OBJECT
public:
    explicit GalleryListWidget(QWidget *parent = nullptr);

protected:
    void dataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight,
                     const QList<int> &roles = {}) override;
};
