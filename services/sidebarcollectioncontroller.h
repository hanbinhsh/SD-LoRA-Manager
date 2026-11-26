#ifndef SIDEBARCOLLECTIONCONTROLLER_H
#define SIDEBARCOLLECTIONCONTROLLER_H

#include <QColor>
#include <QHash>
#include <QList>
#include <QMap>
#include <QMultiHash>
#include <QPoint>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>

class QComboBox;
class QListWidget;
class QListWidgetItem;
class QObject;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

struct CollectionTreeBuildInput {
    QTreeWidget *tree = nullptr;
    QListWidget *modelList = nullptr;
    const QMap<QString, QStringList> *collections = nullptr;
    QString uncategorizedKey;
    bool showEmptyCollections = false;
    bool folderTopLevel = false;
    bool folderSecondLevel = false;
    bool restoreTreeState = true;
    bool *isFirstTreeRefresh = nullptr;
    QSet<QString> startupExpandedCollections;
    int startupTreeScrollPos = 0;
    QMultiHash<QString, QTreeWidgetItem*> *treeItemsByPath = nullptr;
    std::function<void(QTreeWidgetItem*)> applyHighlight;
    std::function<void(QTreeWidgetItem*)> applyUserNote;
};

struct ModelBatchMenuContext {
    const QHash<QString, QColor> *highlightColors = nullptr;
    QMap<QString, QStringList> *collections = nullptr;
    QString uncategorizedKey;
    QString currentCollectionFilter;

    std::function<void(const QList<QListWidgetItem*>&, const QColor&)> setHighlightColor;
    std::function<void(const QList<QListWidgetItem*>&)> clearHighlightColor;
    std::function<void(QListWidgetItem*)> editUserNote;
    std::function<QString(double)> formatRating;
    std::function<void(const QList<QListWidgetItem*>&, double)> setUserRating;
    std::function<QStringList(const QString&)> normalizeUserTagsText;
    std::function<void(const QList<QListWidgetItem*>&, const QStringList&)> addUserTags;
    std::function<void(const QList<QListWidgetItem*>&, const QStringList&)> removeUserTags;
    std::function<void(const QString&)> showFileInFolder;
    std::function<void(const QStringList&)> syncMetadataForPaths;
    std::function<void()> saveCollections;
    std::function<void()> refreshHomeCollections;
    std::function<void()> refreshHomeGallery;
    std::function<void()> refreshCollectionTree;
    std::function<void(const QString&, int)> statusMessage;
};

namespace SidebarCollectionController {

bool isModelListItem(const QListWidgetItem *item);

QListWidgetItem *createModelFolderHeader(const QString &folderName,
                                         const QString &folderKey,
                                         bool collapsed);

void applyModelFolderVisibility(QListWidget *modelList,
                                bool folderGrouping,
                                const QSet<QString> &collapsedFolders);

void sortModelList(QListWidget *modelList,
                   int sortType,
                   bool folderGrouping,
                   const QSet<QString> &collapsedFolders);

void rebuildSortFilterMenu(QObject *owner,
                           QPushButton *btnSortFilter,
                           QComboBox *comboSort,
                           QComboBox *comboBaseModel,
                           QComboBox *comboModelType);

void updateSortFilterButtonText(QPushButton *btnSortFilter,
                                QComboBox *comboSort,
                                QComboBox *comboBaseModel,
                                QComboBox *comboModelType);

void rebuildCollectionTree(const CollectionTreeBuildInput &in);

void handleCollectionTreeItemClicked(QTreeWidgetItem *item,
                                     QListWidget *modelList,
                                     const QHash<QString, QListWidgetItem*> &modelItemByPath,
                                     const QString &uncategorizedKey,
                                     const std::function<void(QListWidgetItem*)> &onModelActivated);

void handleCollectionTreeContextMenu(QWidget *parent,
                                     QTreeWidget *tree,
                                     QListWidget *modelList,
                                     const QHash<QString, QListWidgetItem*> &modelItemByPath,
                                     const QPoint &pos,
                                     const QString &uncategorizedKey,
                                     const std::function<void(const QString&)> &onRenameCollection,
                                     const std::function<void(const QString&)> &onDeleteCollection,
                                     const std::function<void(const QList<QListWidgetItem*>&, const QPoint&)> &onShowBatchMenu);

void syncTreeSelection(QTreeWidget *collectionTree, const QString &filePath);

void showModelBatchMenu(QWidget *parent,
                        const QList<QListWidgetItem*> &items,
                        const QPoint &globalPos,
                        const ModelBatchMenuContext &ctx);

} // namespace SidebarCollectionController

#endif // SIDEBARCOLLECTIONCONTROLLER_H
