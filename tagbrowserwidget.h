#ifndef TAGBROWSERWIDGET_H
#define TAGBROWSERWIDGET_H

#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QWidget>
#include <QPair>
#include <QVector>

namespace Ui {
class TagBrowserWidget;
}

class QShowEvent;

class TagBrowserWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TagBrowserWidget(QWidget *parent = nullptr);
    ~TagBrowserWidget();

    void setCsvPath(const QString &path);
    QString csvPath() const;

protected:
    void showEvent(QShowEvent *event) override;

signals:
    void csvSaved(const QString &path);

private slots:
    void onSearchTextChanged(const QString &text);
    void onSortModeChanged(int index);
    void onResetSortClicked();
    void onAddRowClicked();
    void onDeleteRowsClicked();
    void onReloadClicked();
    void onSaveClicked();
    void onModelChanged();

private:
    Ui::TagBrowserWidget *ui;
    QStandardItemModel *m_model;
    QSortFilterProxyModel *m_proxy;
    QString m_csvPath;
    bool m_csvLoaded = false;
    bool m_dirty = false;
    bool m_loading = false;
    int m_loadGeneration = 0;

    QStringList parseCsvLine(const QString &line) const;
    QString escapeCsvField(const QString &value) const;
    void ensureCsvLoadedForEditing();
    void setLoadingState(bool loading, const QString &message = QString());
    void loadCsv();
    void updateStatusLabel();
};

#endif // TAGBROWSERWIDGET_H
