#include "services/sidebarcollectioncontroller.h"

#include "core/modelitemroles.h"
#include "utils/applanguage.h"
#include "utils/styleconstants.h"

#include <QAction>
#include <QActionGroup>
#include <QCollator>
#include <QColorDialog>
#include <QComboBox>
#include <QFileInfo>
#include <QFont>
#include <QInputDialog>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <algorithm>

namespace {
void copyCommonModelRolesToTree(const QListWidgetItem *source, QTreeWidgetItem *target)
{
    static constexpr int kCopiedRoles[] = {
        ROLE_FILE_PATH,
        ROLE_PREVIEW_PATH,
        ROLE_NSFW_LEVEL,
        ROLE_MODEL_NAME,
        ROLE_CIVITAI_NAME,
        ROLE_LOCAL_EDITED,
        ROLE_CIVITAI_MODEL_ID,
        ROLE_CIVITAI_VERSION_ID,
        ROLE_CIVITAI_SHA256,
        ROLE_MODEL_CREATOR,
        ROLE_MODEL_TAGS,
        ROLE_MODEL_TYPE,
        ROLE_MODEL_TRAINED_WORDS,
        ROLE_USER_RATING,
        ROLE_USER_NOTE,
        ROLE_USER_TAGS,
        ROLE_USER_CUSTOM_TRIGGERS,
        ROLE_MODEL_PREVIEW_STATE,
    };
    for (int role : kCopiedRoles) {
        target->setData(0, role, source->data(role));
    }
}

QListWidgetItem *findListItemForTreeNode(QTreeWidgetItem *treeItem,
                                         QListWidget *modelList,
                                         const QHash<QString, QListWidgetItem*> &modelItemByPath)
{
    if (!treeItem || !modelList) return nullptr;
    const QString filePath = treeItem->data(0, ROLE_FILE_PATH).toString();
    if (!filePath.isEmpty()) {
        if (QListWidgetItem *direct = modelItemByPath.value(filePath, nullptr)) {
            return direct;
        }
        for (int i = 0; i < modelList->count(); ++i) {
            QListWidgetItem *listItem = modelList->item(i);
            if (listItem && listItem->data(ROLE_FILE_PATH).toString() == filePath) {
                return listItem;
            }
        }
    }
    QString baseName = treeItem->data(0, ROLE_MODEL_NAME).toString();
    if (baseName.isEmpty()) baseName = treeItem->text(0);
    if (baseName.isEmpty()) return nullptr;
    for (int i = 0; i < modelList->count(); ++i) {
        QListWidgetItem *listItem = modelList->item(i);
        if (listItem && listItem->data(ROLE_MODEL_NAME).toString() == baseName) {
            return listItem;
        }
    }
    return nullptr;
}
} // namespace

namespace SidebarCollectionController {

bool isModelListItem(const QListWidgetItem *item)
{
    return item
           && !item->data(ROLE_IS_FOLDER_HEADER).toBool()
           && !item->data(ROLE_FILE_PATH).toString().isEmpty();
}

QListWidgetItem *createModelFolderHeader(const QString &folderName,
                                         const QString &folderKey,
                                         bool collapsed)
{
    auto *header = new QListWidgetItem(folderName);
    header->setData(ROLE_IS_FOLDER_HEADER, true);
    header->setData(ROLE_MODEL_FOLDER_KEY, folderKey);
    header->setData(ROLE_MODEL_FOLDER_COLLAPSED, collapsed);
    header->setData(ROLE_MODEL_ROOT_NAME, folderName);
    header->setFlags(Qt::ItemIsEnabled);
    QFont font = header->font();
    font.setBold(true);
    header->setFont(font);
    header->setForeground(QColor(AppStyle::AccentBlue()));
    header->setBackground(QColor(AppStyle::HeaderBackground()));
    return header;
}

void applyModelFolderVisibility(QListWidget *modelList,
                                bool folderGrouping,
                                const QSet<QString> &collapsedFolders)
{
    if (!modelList) return;
    for (int i = 0; i < modelList->count(); ++i) {
        QListWidgetItem *item = modelList->item(i);
        if (!item) continue;
        if (!item->data(ROLE_IS_FOLDER_HEADER).toBool()) {
            if (!folderGrouping && isModelListItem(item)) {
                item->setHidden(!item->data(ROLE_MODEL_FILTER_VISIBLE).toBool());
            }
            continue;
        }

        const QString folderKey = item->data(ROLE_MODEL_FOLDER_KEY).toString();
        const bool collapsed = collapsedFolders.contains(folderKey);
        int visibleChildCount = 0;
        for (int j = i + 1; j < modelList->count(); ++j) {
            QListWidgetItem *child = modelList->item(j);
            if (!child) continue;
            if (child->data(ROLE_IS_FOLDER_HEADER).toBool()) break;
            if (!isModelListItem(child)) continue;

            const bool filterVisible = child->data(ROLE_MODEL_FILTER_VISIBLE).toBool();
            if (filterVisible) visibleChildCount++;
            child->setHidden(!filterVisible || collapsed);
        }

        const QString folderName = item->data(ROLE_MODEL_ROOT_NAME).toString().isEmpty()
                                       ? item->text().mid(2).section(" (", 0, 0).trimmed()
                                       : item->data(ROLE_MODEL_ROOT_NAME).toString();
        item->setData(ROLE_MODEL_FOLDER_COLLAPSED, collapsed);
        item->setText(QString("%1 %2 (%3)").arg(collapsed ? " + " : " - ", folderName).arg(visibleChildCount));
        item->setHidden(!folderGrouping || visibleChildCount <= 0);
    }
}

void sortModelList(QListWidget *modelList,
                   int sortType,
                   bool folderGrouping,
                   const QSet<QString> &collapsedFolders)
{
    if (!modelList) return;

    QList<QListWidgetItem*> items;
    while (modelList->count() > 0) {
        QListWidgetItem *item = modelList->takeItem(0);
        if (item && item->data(ROLE_IS_FOLDER_HEADER).toBool()) {
            delete item;
        } else if (item) {
            items.append(item);
        }
    }

    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setIgnorePunctuation(false);

    std::sort(items.begin(), items.end(),
        [sortType, &collator](QListWidgetItem *a, QListWidgetItem *b) {
            auto compareByName = [&collator](QListWidgetItem *left, QListWidgetItem *right) {
                return collator.compare(left->text(), right->text()) < 0;
            };

            switch (sortType) {
            case 0:
                return compareByName(a, b);
            case 1:
                return a->data(ROLE_SORT_DATE).toLongLong() > b->data(ROLE_SORT_DATE).toLongLong();
            case 2:
                return a->data(ROLE_SORT_DOWNLOADS).toInt() > b->data(ROLE_SORT_DOWNLOADS).toInt();
            case 3:
                return a->data(ROLE_SORT_LIKES).toInt() > b->data(ROLE_SORT_LIKES).toInt();
            case 4:
                return a->data(ROLE_SORT_ADDED).toLongLong() > b->data(ROLE_SORT_ADDED).toLongLong();
            case 5: {
                const int usageA = a->data(ROLE_SORT_USAGE_COUNT).toInt();
                const int usageB = b->data(ROLE_SORT_USAGE_COUNT).toInt();
                if (usageA != usageB) return usageA > usageB;
                return compareByName(a, b);
            }
            case 6: {
                const qint64 usedA = a->data(ROLE_SORT_LAST_USED).toLongLong();
                const qint64 usedB = b->data(ROLE_SORT_LAST_USED).toLongLong();
                if (usedA != usedB) return usedA > usedB;
                return compareByName(a, b);
            }
            case 7: {
                const double ratingA = a->data(ROLE_USER_RATING).toDouble();
                const double ratingB = b->data(ROLE_USER_RATING).toDouble();
                if (!qFuzzyCompare(ratingA + 1.0, ratingB + 1.0)) return ratingA > ratingB;
                return compareByName(a, b);
            }
            default:
                return compareByName(a, b);
            }
        });

    if (folderGrouping) {
        QMap<QString, QList<QListWidgetItem*>> grouped;
        QMap<QString, QString> folderLabels;
        QStringList folderOrder;
        for (QListWidgetItem *item : items) {
            const QString rootPath = item->data(ROLE_MODEL_ROOT_PATH).toString();
            const QString rootKey = rootPath.isEmpty() ? QStringLiteral("__unknown__")
                                                       : QFileInfo(rootPath).absoluteFilePath();
            QString rootName = item->data(ROLE_MODEL_ROOT_NAME).toString();
            if (rootName.isEmpty()) rootName = QStringLiteral("未指定文件夹");
            if (!grouped.contains(rootKey)) {
                folderOrder.append(rootKey);
                folderLabels.insert(rootKey, rootName);
            }
            grouped[rootKey].append(item);
        }

        QCollator folderCollator;
        folderCollator.setNumericMode(true);
        folderCollator.setCaseSensitivity(Qt::CaseInsensitive);
        std::sort(folderOrder.begin(), folderOrder.end(), [&](const QString &a, const QString &b) {
            return folderCollator.compare(folderLabels.value(a), folderLabels.value(b)) < 0;
        });

        for (const QString &folderKey : folderOrder) {
            modelList->addItem(createModelFolderHeader(
                folderLabels.value(folderKey), folderKey, collapsedFolders.contains(folderKey)));
            for (QListWidgetItem *item : grouped.value(folderKey)) {
                modelList->addItem(item);
            }
        }
    } else {
        for (QListWidgetItem *item : items) {
            modelList->addItem(item);
        }
    }
}

void rebuildSortFilterMenu(QObject *owner,
                           QPushButton *btnSortFilter,
                           QComboBox *comboSort,
                           QComboBox *comboBaseModel,
                           QComboBox *comboModelType)
{
    if (!btnSortFilter || !comboSort || !comboBaseModel || !comboModelType) return;
    QMenu *menu = btnSortFilter->menu();
    if (!menu) return;
    menu->clear();

    auto buildSubmenu = [owner, menu](const QString &title, QComboBox *combo, bool byIndex) {
        QMenu *sub = menu->addMenu(title);
        auto *group = new QActionGroup(sub);
        group->setExclusive(true);
        for (int i = 0; i < combo->count(); ++i) {
            QAction *act = sub->addAction(combo->itemText(i));
            act->setCheckable(true);
            act->setChecked(i == combo->currentIndex());
            group->addAction(act);
            if (byIndex) {
                QObject::connect(act, &QAction::triggered, owner, [combo, i]() { combo->setCurrentIndex(i); });
            } else {
                const QString text = combo->itemText(i);
                QObject::connect(act, &QAction::triggered, owner, [combo, text]() { combo->setCurrentText(text); });
            }
        }
    };

    buildSubmenu(AppText::tr("排序 / Sort"), comboSort, true);
    menu->addSeparator();
    buildSubmenu(AppText::tr("底模 / Base"), comboBaseModel, false);
    buildSubmenu(AppText::tr("类型 / Type"), comboModelType, false);
}

void updateSortFilterButtonText(QPushButton *btnSortFilter,
                                QComboBox *comboSort,
                                QComboBox *comboBaseModel,
                                QComboBox *comboModelType)
{
    if (!btnSortFilter || !comboSort || !comboBaseModel || !comboModelType) return;
    QString text = "⚙ " + comboSort->currentText();
    QStringList active;
    if (comboBaseModel->currentIndex() > 0) active << comboBaseModel->currentText();
    if (comboModelType->currentIndex() > 0) active << comboModelType->currentText();
    if (!active.isEmpty()) text += " · " + active.join(" · ");
    btnSortFilter->setText(text);
}

void rebuildCollectionTree(const CollectionTreeBuildInput &in)
{
    if (!in.tree || !in.modelList || !in.collections) return;

    QSet<QString> expandedCollections;
    int scrollPos = 0;

    if (in.isFirstTreeRefresh && *in.isFirstTreeRefresh && in.restoreTreeState) {
        expandedCollections = in.startupExpandedCollections;
        scrollPos = in.startupTreeScrollPos;
        *in.isFirstTreeRefresh = false;
    } else {
        std::function<void(QTreeWidgetItem*)> collectExpanded = [&](QTreeWidgetItem *item) {
            if (!item) return;
            if (item->isExpanded()) {
                const QString key = item->data(0, ROLE_COLLECTION_EXPAND_KEY).toString();
                expandedCollections.insert(key.isEmpty() ? item->data(0, ROLE_COLLECTION_NAME).toString() : key);
            }
            for (int i = 0; i < item->childCount(); ++i) collectExpanded(item->child(i));
        };
        for (int i = 0; i < in.tree->topLevelItemCount(); ++i) {
            collectExpanded(in.tree->topLevelItem(i));
        }
        scrollPos = in.tree->verticalScrollBar()->value();
    }

    in.tree->setUpdatesEnabled(false);
    in.tree->setAnimated(false);
    if (in.treeItemsByPath) in.treeItemsByPath->clear();
    in.tree->clear();
    in.tree->setIconSize(QSize(32, 32));
    in.tree->setRootIsDecorated(false);
    in.tree->setIndentation(10);
    in.tree->setExpandsOnDoubleClick(false);

    QFont categoryFont = in.tree->font();
    categoryFont.setBold(true);
    categoryFont.setPointSize(10);

    const QString PRE_OPEN = QStringLiteral(" - ");
    const QString PRE_CLOSED = QStringLiteral(" + ");

    QHash<QString, QListWidgetItem*> visibleItemMap;
    QHash<QString, int> visibleItemRank;
    visibleItemMap.reserve(in.modelList->count());
    visibleItemRank.reserve(in.modelList->count());

    for (int i = 0; i < in.modelList->count(); ++i) {
        QListWidgetItem *item = in.modelList->item(i);
        if (!isModelListItem(item)) continue;
        if (!item->data(ROLE_MODEL_FILTER_VISIBLE).toBool()) continue;

        const QString baseName = item->data(ROLE_MODEL_NAME).toString();
        visibleItemMap.insert(baseName, item);
        visibleItemRank.insert(baseName, i);
    }

    auto rankSort = [&](const QString &s1, const QString &s2) {
        return visibleItemRank.value(s1) < visibleItemRank.value(s2);
    };

    auto addModelChildren = [&](QTreeWidgetItem *parent, QStringList models) {
        QMutableStringListIterator it(models);
        while (it.hasNext()) {
            if (!visibleItemMap.contains(it.next())) it.remove();
        }
        std::sort(models.begin(), models.end(), rankSort);

        for (const QString &baseName : models) {
            if (QListWidgetItem *sourceItem = visibleItemMap.value(baseName, nullptr)) {
                auto *child = new QTreeWidgetItem(parent);
                child->setText(0, sourceItem->text());
                copyCommonModelRolesToTree(sourceItem, child);
                child->setData(0, ROLE_PREVIEW_PLACEHOLDER, sourceItem->data(ROLE_PREVIEW_PLACEHOLDER));
                child->setIcon(0, sourceItem->icon());
                if (in.applyHighlight) in.applyHighlight(child);
                if (in.applyUserNote) in.applyUserNote(child);
                const QString fp = sourceItem->data(ROLE_FILE_PATH).toString();
                if (!fp.isEmpty() && in.treeItemsByPath) in.treeItemsByPath->insert(fp, child);
            }
        }
    };

    QSet<QString> categorizedSet;
    for (auto it = in.collections->cbegin(); it != in.collections->cend(); ++it) {
        for (const QString &m : it.value()) categorizedSet.insert(m);
    }

    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setIgnorePunctuation(false);

    QList<QString> collectionNames = in.collections->keys();
    collectionNames.removeAll(in.uncategorizedKey);
    std::sort(collectionNames.begin(), collectionNames.end(), [&](const QString &s1, const QString &s2) {
        return collator.compare(s1, s2) < 0;
    });

    auto makeTreeParent = [&](QTreeWidgetItem *parent,
                              const QString &expandKey,
                              const QString &collectionName,
                              const QString &displayName,
                              int count) {
        QTreeWidgetItem *node = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(in.tree);
        node->setData(0, ROLE_IS_COLLECTION_NODE, true);
        node->setData(0, ROLE_COLLECTION_NAME, collectionName);
        node->setData(0, ROLE_COLLECTION_EXPAND_KEY, expandKey);
        node->setData(0, ROLE_ITEM_COUNT, count);
        node->setFont(0, categoryFont);

        const bool expanded = expandedCollections.contains(expandKey);
        node->setExpanded(expanded);
        node->setText(0, (expanded ? PRE_OPEN : PRE_CLOSED) + displayName + " (" + QString::number(count) + ")");
        return node;
    };

    auto modelBelongsToFolder = [&](const QString &baseName, const QString &folderPath) {
        QListWidgetItem *item = visibleItemMap.value(baseName, nullptr);
        if (!item) return false;
        return item->data(ROLE_MODEL_ROOT_PATH).toString() == folderPath;
    };

    auto addFolderChildrenForCollection = [&](QTreeWidgetItem *collectionNode,
                                              const QString &collectionKeyPrefix,
                                              const QStringList &models) {
        QHash<QString, QStringList> modelsByFolder;
        QHash<QString, QString> folderNames;
        for (const QString &baseName : models) {
            QListWidgetItem *item = visibleItemMap.value(baseName, nullptr);
            if (!item) continue;
            QString folderPath = item->data(ROLE_MODEL_ROOT_PATH).toString();
            if (folderPath.isEmpty()) folderPath = QStringLiteral("__unknown__");
            modelsByFolder[folderPath].append(baseName);
            if (!folderNames.contains(folderPath)) {
                QString folderName = item->data(ROLE_MODEL_ROOT_NAME).toString();
                if (folderName.isEmpty()) {
                    folderName = (folderPath == QStringLiteral("__unknown__"))
                                     ? QStringLiteral("未指定文件夹")
                                     : folderPath;
                }
                folderNames.insert(folderPath, folderName);
            }
        }

        QStringList folderPaths = modelsByFolder.keys();
        std::sort(folderPaths.begin(), folderPaths.end(), [&](const QString &a, const QString &b) {
            return collator.compare(folderNames.value(a), folderNames.value(b)) < 0;
        });

        for (const QString &folderPath : folderPaths) {
            const QStringList folderModels = modelsByFolder.value(folderPath);
            const int visibleCount = folderModels.count();
            if (visibleCount <= 0 && !in.showEmptyCollections) continue;
            const QString key = QString("%1:folder:%2").arg(collectionKeyPrefix, folderPath);
            QTreeWidgetItem *folderNode = makeTreeParent(collectionNode, key, key, folderNames.value(folderPath), visibleCount);
            folderNode->setData(0, ROLE_IS_FOLDER_HEADER, true);
            folderNode->setData(0, ROLE_MODEL_ROOT_NAME, folderNames.value(folderPath));
            addModelChildren(folderNode, folderModels);
        }
    };

    auto addCollectionGroup = [&](QTreeWidgetItem *parent, const QString &folderPath) {
        QStringList uncatModels;
        for (auto it = visibleItemMap.cbegin(); it != visibleItemMap.cend(); ++it) {
            if (in.folderTopLevel && !modelBelongsToFolder(it.key(), folderPath)) continue;
            if (!categorizedSet.contains(it.key())) uncatModels.append(it.key());
        }

        const int uncatCount = uncatModels.count();
        if (uncatCount > 0 || in.showEmptyCollections) {
            const QString key = in.folderTopLevel
                                    ? QString("folder:%1:%2").arg(folderPath, in.uncategorizedKey)
                                    : in.uncategorizedKey;
            QTreeWidgetItem *uncategorizedNode = makeTreeParent(
                parent, key, in.uncategorizedKey, AppText::tr("未分类 / Uncategorized"), uncatCount);
            std::sort(uncatModels.begin(), uncatModels.end(), rankSort);
            if (in.folderSecondLevel && !in.folderTopLevel) {
                addFolderChildrenForCollection(uncategorizedNode, key, uncatModels);
            } else {
                addModelChildren(uncategorizedNode, uncatModels);
            }
        }

        for (const QString &colName : collectionNames) {
            QStringList models = in.collections->value(colName);
            if (in.folderTopLevel) {
                QMutableStringListIterator it(models);
                while (it.hasNext()) {
                    if (!modelBelongsToFolder(it.next(), folderPath)) it.remove();
                }
            }

            int visibleCount = 0;
            for (const QString &m : models) {
                if (visibleItemMap.contains(m)) visibleCount++;
            }

            if (visibleCount > 0 || in.showEmptyCollections) {
                const QString key = in.folderTopLevel ? QString("folder:%1:%2").arg(folderPath, colName) : colName;
                QTreeWidgetItem *collectionNode = makeTreeParent(parent, key, colName, colName, visibleCount);
                if (in.folderSecondLevel && !in.folderTopLevel) {
                    addFolderChildrenForCollection(collectionNode, key, models);
                } else {
                    addModelChildren(collectionNode, models);
                }
            }
        }
    };

    if (in.folderTopLevel) {
        QMap<QString, QString> folderNames;
        QHash<QString, int> folderCounts;
        for (auto it = visibleItemMap.cbegin(); it != visibleItemMap.cend(); ++it) {
            QListWidgetItem *item = it.value();
            const QString folderPath = item->data(ROLE_MODEL_ROOT_PATH).toString();
            if (folderPath.isEmpty()) continue;
            folderCounts[folderPath]++;
            if (!folderNames.contains(folderPath)) {
                QString folderName = item->data(ROLE_MODEL_ROOT_NAME).toString();
                if (folderName.isEmpty()) folderName = folderPath;
                folderNames.insert(folderPath, folderName);
            }
        }

        QStringList folderPaths = folderNames.keys();
        std::sort(folderPaths.begin(), folderPaths.end(), [&](const QString &a, const QString &b) {
            return collator.compare(folderNames.value(a), folderNames.value(b)) < 0;
        });

        for (const QString &folderPath : folderPaths) {
            const int folderVisibleCount = folderCounts.value(folderPath, 0);
            if (folderVisibleCount <= 0 && !in.showEmptyCollections) continue;

            const QString folderKey = "folder:" + folderPath;
            QTreeWidgetItem *folderNode = makeTreeParent(
                nullptr, folderKey, folderKey, folderNames.value(folderPath), folderVisibleCount);
            folderNode->setData(0, ROLE_IS_FOLDER_HEADER, true);
            folderNode->setData(0, ROLE_MODEL_ROOT_NAME, folderNames.value(folderPath));
            addCollectionGroup(folderNode, folderPath);
        }
    } else {
        addCollectionGroup(nullptr, QString());
    }

    in.tree->setAnimated(true);
    in.tree->setUpdatesEnabled(true);

    if (scrollPos > 0) {
        QTreeWidget *tree = in.tree;
        QTimer::singleShot(0, tree, [tree, scrollPos]() {
            if (tree->verticalScrollBar()) tree->verticalScrollBar()->setValue(scrollPos);
        });
    }
}

void handleCollectionTreeItemClicked(QTreeWidgetItem *item,
                                     QListWidget *modelList,
                                     const QHash<QString, QListWidgetItem*> &modelItemByPath,
                                     const QString &uncategorizedKey,
                                     const std::function<void(QListWidgetItem*)> &onModelActivated)
{
    if (!item || !modelList) return;

    if (item->data(0, ROLE_IS_COLLECTION_NODE).toBool()) {
        const QString collectionName = item->data(0, ROLE_COLLECTION_NAME).toString();
        const int count = item->data(0, ROLE_ITEM_COUNT).toInt();
        const bool wasExpanded = item->isExpanded();
        item->setExpanded(!wasExpanded);

        QString displayName;
        if (item->data(0, ROLE_IS_FOLDER_HEADER).toBool()) {
            displayName = item->data(0, ROLE_MODEL_ROOT_NAME).toString();
            if (displayName.isEmpty()) displayName = collectionName;
        } else {
            displayName = (collectionName == uncategorizedKey)
                              ? AppText::tr("未分类 / Uncategorized")
                              : collectionName;
        }
        const QString prefix = (!wasExpanded) ? QStringLiteral(" - ") : QStringLiteral(" + ");
        item->setText(0, QString("%1%2 (%3)").arg(prefix, displayName).arg(count));
    } else {
        if (QListWidgetItem *matchItem = findListItemForTreeNode(item, modelList, modelItemByPath)) {
            modelList->setCurrentItem(matchItem);
            if (onModelActivated) onModelActivated(matchItem);
        }
    }
}

void handleCollectionTreeContextMenu(QWidget *parent,
                                     QTreeWidget *tree,
                                     QListWidget *modelList,
                                     const QHash<QString, QListWidgetItem*> &modelItemByPath,
                                     const QPoint &pos,
                                     const QString &uncategorizedKey,
                                     const std::function<void(const QString&)> &onRenameCollection,
                                     const std::function<void(const QString&)> &onDeleteCollection,
                                     const std::function<void(const QList<QListWidgetItem*>&, const QPoint&)> &onShowBatchMenu)
{
    if (!tree || !modelList) return;
    QTreeWidgetItem *clickedItem = tree->itemAt(pos);
    if (!clickedItem) return;

    QList<QTreeWidgetItem*> selectedTreeItems = tree->selectedItems();
    if (selectedTreeItems.isEmpty()) {
        selectedTreeItems.append(clickedItem);
    }

    if (clickedItem->data(0, ROLE_IS_COLLECTION_NODE).toBool()) {
        if (clickedItem->data(0, ROLE_IS_FOLDER_HEADER).toBool()) {
            QList<QListWidgetItem*> targetListItems;
            std::function<void(QTreeWidgetItem*)> collectModelItems = [&](QTreeWidgetItem *treeItem) {
                if (!treeItem) return;
                if (!treeItem->data(0, ROLE_IS_COLLECTION_NODE).toBool()) {
                    if (QListWidgetItem *listItem = findListItemForTreeNode(treeItem, modelList, modelItemByPath)) {
                        targetListItems.append(listItem);
                    }
                }
                for (int i = 0; i < treeItem->childCount(); ++i) collectModelItems(treeItem->child(i));
            };
            collectModelItems(clickedItem);
            if (!targetListItems.isEmpty() && onShowBatchMenu) {
                onShowBatchMenu(targetListItems, tree->mapToGlobal(pos));
            }
            return;
        }

        const QString collectionName = clickedItem->data(0, ROLE_COLLECTION_NAME).toString();
        QMenu menu(parent);
        QAction *title = menu.addAction(AppText::tr("管理收藏夹: %1").arg(collectionName));
        title->setEnabled(false);
        menu.addSeparator();

        if (collectionName == uncategorizedKey) {
            QAction *dummy = menu.addAction(AppText::tr("无法操作此项"));
            dummy->setEnabled(false);
            menu.exec(tree->mapToGlobal(pos));
        } else {
            QAction *actRename = menu.addAction(AppText::tr("重命名 / Rename Collection"));
            QAction *actDelete = menu.addAction(AppText::tr("删除 / Delete Collection"));
            QAction *selected = menu.exec(tree->mapToGlobal(pos));
            if (selected == actRename && onRenameCollection) {
                onRenameCollection(collectionName);
            } else if (selected == actDelete && onDeleteCollection) {
                onDeleteCollection(collectionName);
            }
        }
    } else {
        QList<QListWidgetItem*> targetListItems;
        for (QTreeWidgetItem *tItem : selectedTreeItems) {
            if (tItem->data(0, ROLE_IS_COLLECTION_NODE).toBool()) continue;
            if (QListWidgetItem *listItem = findListItemForTreeNode(tItem, modelList, modelItemByPath)) {
                targetListItems.append(listItem);
            }
        }
        if (!targetListItems.isEmpty() && onShowBatchMenu) {
            onShowBatchMenu(targetListItems, tree->mapToGlobal(pos));
        }
    }
}

void syncTreeSelection(QTreeWidget *collectionTree, const QString &filePath)
{
    if (!collectionTree || filePath.isEmpty()) return;

    std::function<bool(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem *node) {
        if (!node) return false;
        if (node->data(0, ROLE_FILE_PATH).toString() == filePath) {
            QTreeWidgetItem *parent = node->parent();
            while (parent) {
                parent->setExpanded(true);
                parent = parent->parent();
            }
            collectionTree->setCurrentItem(node);
            node->setSelected(true);
            collectionTree->scrollToItem(node, QAbstractItemView::PositionAtCenter);
            return true;
        }
        for (int i = 0; i < node->childCount(); ++i) {
            if (visit(node->child(i))) return true;
        }
        return false;
    };

    for (int i = 0; i < collectionTree->topLevelItemCount(); ++i) {
        if (visit(collectionTree->topLevelItem(i))) break;
    }
}

void showModelBatchMenu(QWidget *parent,
                        const QList<QListWidgetItem*> &items,
                        const QPoint &globalPos,
                        const ModelBatchMenuContext &ctx)
{
    QList<QListWidgetItem*> modelItems;
    for (QListWidgetItem *item : items) {
        if (isModelListItem(item)) modelItems.append(item);
    }
    if (modelItems.isEmpty() || !ctx.collections) return;

    QMenu menu(parent);

    // 1. 标题逻辑
    if (modelItems.count() == 1) {
        QListWidgetItem *first = modelItems.first();
        QString name = first->text();
        if (name.isEmpty()) {
            name = first->data(ROLE_CIVITAI_NAME).toString();
            if (name.isEmpty()) name = first->data(ROLE_MODEL_NAME).toString();
        }
        if (name.length() > 20) name = name.left(18) + "..";
        QAction *titleAct = menu.addAction(name);
        titleAct->setEnabled(false);
    } else {
        QAction *titleAct = menu.addAction(AppText::tr("已选中 %1 个模型").arg(modelItems.count()));
        titleAct->setEnabled(false);
    }

    menu.addSeparator();

    QColor defaultColor(102, 192, 244, 96);
    if (modelItems.count() == 1 && ctx.highlightColors) {
        const QString filePath = QFileInfo(modelItems.first()->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
        if (ctx.highlightColors->contains(filePath)) {
            defaultColor = ctx.highlightColors->value(filePath);
        }
    }

    QAction *actSetHighlightColor = menu.addAction(AppText::tr("设置高亮颜色... / Set Highlight Color..."));
    QObject::connect(actSetHighlightColor, &QAction::triggered, parent, [parent, modelItems, defaultColor, cb = ctx.setHighlightColor]() {
        QColorDialog dialog(defaultColor, parent);
        dialog.setWindowTitle(AppText::tr("设置模型高亮颜色"));
        dialog.setOption(QColorDialog::ShowAlphaChannel, true);
        if (dialog.exec() != QDialog::Accepted) return;
        const QColor color = dialog.selectedColor();
        if (color.isValid() && cb) cb(modelItems, color);
    });

    QAction *actClearHighlightColor = menu.addAction(AppText::tr("清除高亮颜色 / Clear Highlight Color"));
    bool hasHighlightColor = false;
    if (ctx.highlightColors) {
        for (QListWidgetItem *item : modelItems) {
            const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
            if (ctx.highlightColors->contains(filePath)) {
                hasHighlightColor = true;
                break;
            }
        }
    }
    actClearHighlightColor->setEnabled(hasHighlightColor);
    QObject::connect(actClearHighlightColor, &QAction::triggered, parent, [modelItems, cb = ctx.clearHighlightColor]() {
        if (cb) cb(modelItems);
    });

    menu.addSeparator();

    QAction *actEditUserNote = menu.addAction(AppText::tr("编辑评分、备注与触发词... / Edit Rating, Note && Triggers..."));
    actEditUserNote->setEnabled(modelItems.count() == 1);
    QObject::connect(actEditUserNote, &QAction::triggered, parent, [modelItems, cb = ctx.editUserNote]() {
        if (modelItems.count() == 1 && cb) cb(modelItems.first());
    });

    QMenu *ratingMenu = menu.addMenu(AppText::tr("设置评分 / Set Rating"));
    QAction *clearRatingAct = ratingMenu->addAction(AppText::tr("未评分 / Clear Rating"));
    QObject::connect(clearRatingAct, &QAction::triggered, parent, [modelItems, cb = ctx.setUserRating]() {
        if (cb) cb(modelItems, 0.0);
    });
    ratingMenu->addSeparator();
    for (int half = 2; half <= 10; ++half) {
        const double rating = half / 2.0;
        const QString label = ctx.formatRating ? ctx.formatRating(rating) : QString::number(rating, 'f', 1);
        QAction *act = ratingMenu->addAction(label);
        QObject::connect(act, &QAction::triggered, parent, [modelItems, rating, cb = ctx.setUserRating]() {
            if (cb) cb(modelItems, rating);
        });
    }

    QAction *actAddUserTags = menu.addAction(AppText::tr("添加用户标签... / Add User Tags..."));
    QObject::connect(actAddUserTags, &QAction::triggered, parent,
                     [parent, modelItems, norm = ctx.normalizeUserTagsText, cb = ctx.addUserTags]() {
        bool ok = false;
        const QString text = QInputDialog::getMultiLineText(
            parent, AppText::tr("添加用户标签"), AppText::tr("输入标签，逗号或换行分隔："), QString(), &ok);
        if (ok && cb && norm) cb(modelItems, norm(text));
    });

    QAction *actRemoveUserTags = menu.addAction(AppText::tr("移除用户标签... / Remove User Tags..."));
    QObject::connect(actRemoveUserTags, &QAction::triggered, parent,
                     [parent, modelItems, norm = ctx.normalizeUserTagsText, cb = ctx.removeUserTags]() {
        bool ok = false;
        const QString text = QInputDialog::getMultiLineText(
            parent, AppText::tr("移除用户标签"), AppText::tr("输入要移除的标签，逗号或换行分隔："), QString(), &ok);
        if (ok && cb && norm) cb(modelItems, norm(text));
    });

    menu.addSeparator();

    QStringList targetFilePaths;
    for (QListWidgetItem *item : modelItems) {
        if (!item) continue;
        const QString path = item->data(ROLE_FILE_PATH).toString().trimmed();
        if (!path.isEmpty() && !targetFilePaths.contains(path)) targetFilePaths.append(path);
    }

    QAction *actOpenModelLocation = menu.addAction(AppText::tr("打开模型位置 / Show Model in Folder"));
    actOpenModelLocation->setEnabled(!targetFilePaths.isEmpty());
    QObject::connect(actOpenModelLocation, &QAction::triggered, parent,
                     [targetFilePaths, showFn = ctx.showFileInFolder, statusFn = ctx.statusMessage]() {
        if (targetFilePaths.isEmpty()) return;
        if (showFn) showFn(targetFilePaths.first());
        if (targetFilePaths.size() > 1 && statusFn) {
            statusFn(AppText::tr("已打开首个模型位置（共选中 %1 个）").arg(targetFilePaths.size()), 2500);
        }
    });

    QAction *actSyncMetadata = menu.addAction(AppText::tr("🔄 批量同步元数据与预览图 / Sync Metadata..."));
    actSyncMetadata->setEnabled(!targetFilePaths.isEmpty());
    QObject::connect(actSyncMetadata, &QAction::triggered, parent, [targetFilePaths, cb = ctx.syncMetadataForPaths]() {
        if (!targetFilePaths.isEmpty() && cb) cb(targetFilePaths);
    });

    menu.addSeparator();

    QStringList targetBaseNames;
    for (QListWidgetItem *item : modelItems) {
        targetBaseNames.append(item->data(ROLE_MODEL_NAME).toString());
    }

    // 2. "从收藏夹移除..."
    QMenu *removeMenu = menu.addMenu(AppText::tr("从指定收藏夹移除..."));
    bool canRemoveAny = false;
    for (auto it = ctx.collections->begin(); it != ctx.collections->end(); ++it) {
        const QString colName = it.key();
        if (colName == ctx.uncategorizedKey) continue;

        int countInCol = 0;
        for (const QString &bn : targetBaseNames) {
            if (it.value().contains(bn)) countInCol++;
        }
        if (countInCol > 0) {
            canRemoveAny = true;
            QAction *actRemove = removeMenu->addAction(QString("%1 (%2)").arg(colName).arg(countInCol));
            QObject::connect(actRemove, &QAction::triggered, parent, [ctx, colName, targetBaseNames]() {
                int removedCount = 0;
                for (const QString &bn : targetBaseNames) {
                    if ((*ctx.collections)[colName].contains(bn)) {
                        (*ctx.collections)[colName].removeAll(bn);
                        removedCount++;
                    }
                }
                if (ctx.saveCollections) ctx.saveCollections();
                if (ctx.refreshHomeGallery) ctx.refreshHomeGallery();
                if (ctx.refreshCollectionTree) ctx.refreshCollectionTree();
                if (ctx.statusMessage) {
                    ctx.statusMessage(AppText::tr("已从 %1 移除 %2 个模型").arg(colName).arg(removedCount), 2000);
                }
            });
        }
    }
    if (!canRemoveAny) removeMenu->setEnabled(false);

    // 3. "添加至收藏夹..."
    QMenu *addMenu = menu.addMenu(AppText::tr("添加至收藏夹..."));
    for (auto it = ctx.collections->begin(); it != ctx.collections->end(); ++it) {
        const QString colName = it.key();
        if (colName == ctx.uncategorizedKey) continue;

        QAction *action = addMenu->addAction(colName);
        action->setCheckable(true);
        bool allIn = true;
        for (const QString &bn : targetBaseNames) {
            if (!it.value().contains(bn)) {
                allIn = false;
                break;
            }
        }
        action->setChecked(allIn);

        QObject::connect(action, &QAction::triggered, parent, [ctx, colName, targetBaseNames, action]() {
            const bool isAdding = action->isChecked();
            int count = 0;
            if (isAdding) {
                for (const QString &bn : targetBaseNames) {
                    if (!(*ctx.collections)[colName].contains(bn)) {
                        (*ctx.collections)[colName].append(bn);
                        count++;
                    }
                }
                if (ctx.statusMessage) {
                    ctx.statusMessage(AppText::tr("已将 %1 个模型加入 %2").arg(count).arg(colName), 2000);
                }
            } else {
                for (const QString &bn : targetBaseNames) {
                    if ((*ctx.collections)[colName].contains(bn)) {
                        (*ctx.collections)[colName].removeAll(bn);
                        count++;
                    }
                }
                if (ctx.statusMessage) {
                    ctx.statusMessage(AppText::tr("已从 %1 移除 %2 个模型").arg(colName).arg(count), 2000);
                }
            }
            if (ctx.saveCollections) ctx.saveCollections();
            if (ctx.currentCollectionFilter == colName && ctx.refreshHomeGallery) ctx.refreshHomeGallery();
            if (ctx.refreshCollectionTree) ctx.refreshCollectionTree();
        });
    }

    addMenu->addSeparator();
    QAction *newAction = addMenu->addAction(AppText::tr("新建收藏夹..."));
    QObject::connect(newAction, &QAction::triggered, parent, [parent, ctx, targetBaseNames]() {
        bool ok = false;
        const QString text = QInputDialog::getText(
            parent, AppText::tr("新建"), AppText::tr("名称:"), QLineEdit::Normal, QString(), &ok);
        if (ok && !text.isEmpty() && !ctx.collections->contains(text)) {
            (*ctx.collections)[text] = targetBaseNames;
            if (ctx.saveCollections) ctx.saveCollections();
            if (ctx.refreshHomeCollections) ctx.refreshHomeCollections();
            if (ctx.refreshCollectionTree) ctx.refreshCollectionTree();
            if (ctx.statusMessage) {
                ctx.statusMessage(AppText::tr("新建收藏夹并加入 %1 个模型").arg(targetBaseNames.count()), 2000);
            }
        }
    });

    menu.exec(globalPos);
}

} // namespace SidebarCollectionController
