#ifndef TAGBROWSERWIDGET_H
#define TAGBROWSERWIDGET_H

#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QString>
#include <QWidget>
#include <QPair>
#include <QVector>
#include <QFutureWatcher>
#include <QHash>

namespace Ui {
class TagBrowserWidget;
}

class QShowEvent;
class QTimer;

struct UserTagUsageRow
{
    QString tag;
    QString kind;        // 正面 / 负面
    QString category;    // 来自翻译表
    QString translation; // 来自翻译表
    QString priority;    // 来自翻译表
    int count = 0;       // 用户图库实际使用次数
};
struct TagTranslationRow
{
    QString tag;
    QString category;
    QString translation;
    QString count; // priority
};

struct TagTranslationInfo
{
    QString category;
    QString translation;
    QString priority;
};

class TagSearchProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    enum MatchMode {
        ContainsMatch = 0,
        WordMatch = 1,
        ExactMatch = 2
    };

    explicit TagSearchProxyModel(QObject *parent = nullptr);

    void setSearchText(const QString &text);
    void setMatchMode(int mode);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    QString m_searchText;
    QString m_normalizedSearchText;
    QString m_wordSearchText;
    MatchMode m_matchMode = ContainsMatch;

    static QString normalizedSearchText(const QString &text);
    bool matchesText(const QString &value) const;
};

class TagBrowserWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TagBrowserWidget(QWidget *parent = nullptr);
    ~TagBrowserWidget();

    void setCsvPath(const QString &path);
    QString csvPath() const;
    void setMergedTranslationMap(const QHash<QString, QString> *map);

protected:
    void showEvent(QShowEvent *event) override;

signals:
    void csvSaved(const QString &path);

private slots:
    void onTabChanged(int index);
    void onSearchTextChanged(const QString &text);
    void onUserTagSearchTextChanged(const QString &text);
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
    TagSearchProxyModel *m_proxy;
    QStandardItemModel *m_userTagModel;
    TagSearchProxyModel *m_userTagProxy;
    QString m_csvPath;
    bool m_csvLoaded = false;
    bool m_dirty = false;
    bool m_loading = false;
    int m_loadGeneration = 0;
    QVector<TagTranslationRow> m_pendingRows;
    int m_pendingRowIndex = 0;
    QTimer *m_batchAppendTimer = nullptr;
    QFutureWatcher<QVector<TagTranslationRow>> *m_loadWatcher = nullptr;
    QFutureWatcher<QVector<UserTagUsageRow>> *m_userTagWatcher = nullptr;
    bool m_userTagsLoaded = false;
    bool m_userTagsLoading = false;
    int m_userTagLoadGeneration = 0;
    const QHash<QString, QString> *m_mergedTranslationMap = nullptr;

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
    QHash<QString, TagTranslationInfo> currentTranslationInfoMap() const;
    QHash<QString, QString> currentTranslationMap() const;
    QString translatedTextForTag(const QString &tag, const QHash<QString, QString> &translations) const;
    QString escapeUserTagCsvField(const QString &value) const;
    QVector<UserTagUsageRow> selectedUserTagRows() const;
    QVector<UserTagUsageRow> visibleUserTagRows() const;
    QVector<UserTagUsageRow> allUserTagRows() const;
    void showUserTagExportDialog();

    int m_tagSortSection = -1;
    Qt::SortOrder m_tagSortOrder = Qt::AscendingOrder;

    void resetTagSort();
};

#endif // TAGBROWSERWIDGET_H
