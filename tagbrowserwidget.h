#ifndef TAGBROWSERWIDGET_H
#define TAGBROWSERWIDGET_H

#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QString>
#include <QWidget>
#include <QPair>
#include <QVector>
#include <QFutureWatcher>

namespace Ui {
class TagBrowserWidget;
}

class QShowEvent;
class QTimer;

struct UserTagUsageRow
{
    QString tag;
    QString kind;
    int count = 0;
};

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
    void onTabChanged(int index);
    void onSearchTextChanged(const QString &text);
    void onSortModeChanged(int index);
    void onUserTagSearchTextChanged(const QString &text);
    void onUserTagSortModeChanged(int index);
    void onUserTagScopeChanged(int index);
    void onRefreshUserTagsClicked();
    void onUserTagContextMenu(const QPoint &pos);
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
    QStandardItemModel *m_userTagModel;
    QSortFilterProxyModel *m_userTagProxy;
    QString m_csvPath;
    bool m_csvLoaded = false;
    bool m_dirty = false;
    bool m_loading = false;
    int m_loadGeneration = 0;
    QVector<QPair<QString, QString>> m_pendingRows;
    int m_pendingRowIndex = 0;
    QTimer *m_batchAppendTimer = nullptr;
    QFutureWatcher<QVector<QPair<QString, QString>>> *m_loadWatcher = nullptr;
    QFutureWatcher<QVector<UserTagUsageRow>> *m_userTagWatcher = nullptr;
    bool m_userTagsLoaded = false;
    bool m_userTagsLoading = false;
    int m_userTagLoadGeneration = 0;

    QStringList parseCsvLine(const QString &line) const;
    QString escapeCsvField(const QString &value) const;
    void ensureCsvLoadedForEditing();
    void setLoadingState(bool loading, const QString &message = QString());
    void loadCsv();
    void appendPendingRowsBatch();
    void updateStatusLabel();
    void loadUserTags();
    void updateUserTagTranslations();
    void updateUserTagStatusLabel();
    QHash<QString, QString> currentTranslationMap() const;
    QString translatedTextForTag(const QString &tag, const QHash<QString, QString> &translations) const;
    QString escapeUserTagCsvField(const QString &value) const;
    QVector<UserTagUsageRow> selectedUserTagRows() const;
    QVector<UserTagUsageRow> visibleUserTagRows() const;
    QVector<UserTagUsageRow> allUserTagRows() const;
    void showUserTagExportDialog();
};

#endif // TAGBROWSERWIDGET_H
