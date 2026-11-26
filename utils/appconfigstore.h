#ifndef APPCONFIGSTORE_H
#define APPCONFIGSTORE_H

#include <QJsonObject>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>
#include <functional>

struct AppPathsConfig {
    QStringList loraPaths;
    QStringList galleryPaths;
    QStringList translationCsvPaths;
    QSet<QString> disabledLoraPaths;
    QSet<QString> disabledGalleryPaths;
    QSet<QString> disabledTranslationCsvPaths;
    QSet<QString> collapsedModelFolders;

    void normalize();
    static void pruneSetToPaths(QSet<QString> &target, const QStringList &allowedPaths);
};

struct AppTreeState {
    int scrollPos = 0;
    QSet<QString> expandedItems;
};

struct StartupFilterSortState {
    int sortIndex = 0;
    QString filterBaseModel = QStringLiteral("All");
    QString filterModelType = QStringLiteral("All");
    QString searchText;
    int sidebarTab = 1;
    QString collectionFilter;
    QString homeAuthorFilter;
    QSet<QString> homeTagFilters;
    bool homeFilterExpanded = false;
    bool homeFilterTagsSortByCount = true;
    int userTagSortMode = 0;
    bool userTagLoraOnly = false;
    bool userTagIncludeNegative = false;
    int galleryPromptSortMode = 0;
    bool galleryTagViewActive = false;
};

struct AppConfigData {
    bool fileOpened = false;
    QJsonObject rawRoot;
    AppPathsConfig paths;
    bool hasTreeState = false;
    AppTreeState treeState;
    bool hasFilterSortState = false;
    StartupFilterSortState filterSortState;
};

struct AppConfigSaveInput {
    AppPathsConfig paths;
    bool saveTreeState = false;
    AppTreeState treeState;
    bool saveFilterSortState = false;
    StartupFilterSortState filterSortState;
    QSize windowSize;
    std::function<void(QJsonObject &)> writeSettings;
};

namespace AppConfigStore {
AppConfigData load(const QString &configPath);
bool save(const QString &configPath, const AppConfigSaveInput &input);
}

#endif // APPCONFIGSTORE_H
