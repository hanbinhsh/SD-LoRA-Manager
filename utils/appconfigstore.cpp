#include "utils/appconfigstore.h"
#include "utils/pathutils.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <utility>

namespace {
QStringList readPathList(const QJsonObject &obj, const QString &arrayKey, const QStringList &fallbackKeys)
{
    QStringList out;
    if (obj.contains(arrayKey) && obj[arrayKey].isArray()) {
        const QJsonArray arr = obj[arrayKey].toArray();
        for (const QJsonValue &val : arr) {
            const QString path = val.toString().trimmed();
            if (!path.isEmpty()) out.append(path);
        }
        return out;
    }
    for (const QString &key : fallbackKeys) {
        const QString val = obj.value(key).toString().trimmed();
        if (!val.isEmpty()) {
            out.append(val);
            break;
        }
    }
    return out;
}

QSet<QString> readPathSet(const QJsonObject &obj, const QString &arrayKey)
{
    QSet<QString> out;
    if (!obj.contains(arrayKey) || !obj[arrayKey].isArray()) return out;
    const QJsonArray arr = obj[arrayKey].toArray();
    for (const QJsonValue &val : arr) {
        const QString path = val.toString().trimmed();
        if (!path.isEmpty()) out.insert(path);
    }
    return out;
}

QJsonArray toJsonArray(const QStringList &list)
{
    QJsonArray arr;
    for (const QString &item : list) arr.append(item);
    return arr;
}

QJsonArray toJsonArray(const QSet<QString> &set)
{
    QJsonArray arr;
    for (const QString &item : set) arr.append(item);
    return arr;
}
} // namespace

void AppPathsConfig::pruneSetToPaths(QSet<QString> &target, const QStringList &allowedPaths)
{
    QSet<QString> filtered;
    for (const QString &path : allowedPaths) {
        if (target.contains(path)) filtered.insert(path);
    }
    target = std::move(filtered);
}

void AppPathsConfig::normalize()
{
    loraPaths = PathUtils::normalizePathList(loraPaths);
    galleryPaths = PathUtils::normalizePathList(galleryPaths);
    translationCsvPaths = PathUtils::normalizePathList(translationCsvPaths);
    disabledLoraPaths = PathUtils::normalizePathSet(disabledLoraPaths);
    disabledGalleryPaths = PathUtils::normalizePathSet(disabledGalleryPaths);
    disabledTranslationCsvPaths = PathUtils::normalizePathSet(disabledTranslationCsvPaths);
    collapsedModelFolders = PathUtils::normalizePathSet(collapsedModelFolders);

    pruneSetToPaths(disabledLoraPaths, loraPaths);
    pruneSetToPaths(disabledGalleryPaths, galleryPaths);
    pruneSetToPaths(disabledTranslationCsvPaths, translationCsvPaths);

    const QStringList enabledLoraPaths = PathUtils::collectEnabledPaths(loraPaths, disabledLoraPaths);
    pruneSetToPaths(collapsedModelFolders, enabledLoraPaths);
}

namespace AppConfigStore {

AppConfigData load(const QString &configPath)
{
    AppConfigData data;
    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return data;
    }
    data.fileOpened = true;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
        data.rawRoot = doc.object();
    } else {
        qWarning() << "Invalid global settings JSON:" << configPath << parseError.errorString();
    }

    const QJsonObject &root = data.rawRoot;
    data.paths.loraPaths = readPathList(root, QStringLiteral("lora_paths"), {QStringLiteral("lora_path")});
    data.paths.galleryPaths = readPathList(
        root, QStringLiteral("gallery_paths"), {QStringLiteral("gallery_path"), QStringLiteral("sd_folder")});
    data.paths.translationCsvPaths = readPathList(
        root, QStringLiteral("translation_paths"), {QStringLiteral("translation_path")});
    data.paths.disabledLoraPaths = readPathSet(root, QStringLiteral("lora_paths_disabled"));
    data.paths.disabledGalleryPaths = readPathSet(root, QStringLiteral("gallery_paths_disabled"));
    data.paths.disabledTranslationCsvPaths = readPathSet(root, QStringLiteral("translation_paths_disabled"));
    data.paths.collapsedModelFolders = readPathSet(root, QStringLiteral("model_list_collapsed_folders"));
    data.paths.normalize();

    if (root.contains(QStringLiteral("tree_state"))) {
        data.hasTreeState = true;
        const QJsonObject treeState = root.value(QStringLiteral("tree_state")).toObject();
        data.treeState.scrollPos = treeState.value(QStringLiteral("scroll_pos")).toInt(0);
        const QJsonArray arr = treeState.value(QStringLiteral("expanded_items")).toArray();
        for (const QJsonValue &val : arr) {
            data.treeState.expandedItems.insert(val.toString());
        }
    }

    if (root.contains(QStringLiteral("filter_sort_state"))) {
        data.hasFilterSortState = true;
        const QJsonObject fs = root.value(QStringLiteral("filter_sort_state")).toObject();
        StartupFilterSortState &s = data.filterSortState;
        s.sortIndex = fs.value(QStringLiteral("sort_index")).toInt(0);
        s.filterBaseModel = fs.value(QStringLiteral("base_model")).toString(QStringLiteral("All"));
        s.filterModelType = fs.value(QStringLiteral("model_type")).toString(QStringLiteral("All"));
        s.searchText = fs.value(QStringLiteral("search_text")).toString();
        s.sidebarTab = fs.value(QStringLiteral("sidebar_tab")).toInt(1);
        s.collectionFilter = fs.value(QStringLiteral("collection_filter")).toString();
        s.homeAuthorFilter = fs.value(QStringLiteral("home_author_filter")).toString();
        s.homeFilterExpanded = fs.value(QStringLiteral("home_filter_expanded")).toBool(false);
        s.homeFilterTagsSortByCount = fs.value(QStringLiteral("home_tag_sort_by_count")).toBool(true);
        s.homeTagFilters.clear();
        const QJsonArray tagArr = fs.value(QStringLiteral("home_tag_filters")).toArray();
        for (const QJsonValue &v : tagArr) {
            const QString clean = v.toString().trimmed();
            if (!clean.isEmpty()) s.homeTagFilters.insert(clean);
        }
        s.userTagSortMode = fs.value(QStringLiteral("user_tag_sort_mode")).toInt(0);
        s.userTagLoraOnly = fs.value(QStringLiteral("user_tag_lora_only")).toBool(false);
        s.userTagIncludeNegative = fs.value(QStringLiteral("user_tag_include_negative")).toBool(false);
        s.galleryPromptSortMode = fs.value(QStringLiteral("gallery_prompt_sort_mode")).toInt(0);
        s.galleryTagViewActive = fs.value(QStringLiteral("gallery_tag_view_active")).toBool(false);
    }

    return data;
}

bool save(const QString &configPath, const AppConfigSaveInput &input)
{
    QDir().mkpath(QFileInfo(configPath).absolutePath());

    QJsonObject root;
    QFile readFile(configPath);
    if (readFile.open(QIODevice::ReadOnly)) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(readFile.readAll(), &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()) {
            root = document.object();
        } else {
            qWarning() << "Refusing to overwrite malformed settings JSON:" << configPath
                       << parseError.errorString();
            return false;
        }
        readFile.close();
    }

    root[QStringLiteral("lora_paths")] = toJsonArray(input.paths.loraPaths);
    root[QStringLiteral("gallery_paths")] = toJsonArray(input.paths.galleryPaths);
    root[QStringLiteral("translation_paths")] = toJsonArray(input.paths.translationCsvPaths);
    root[QStringLiteral("lora_paths_disabled")] = toJsonArray(input.paths.disabledLoraPaths);
    root[QStringLiteral("gallery_paths_disabled")] = toJsonArray(input.paths.disabledGalleryPaths);
    root[QStringLiteral("translation_paths_disabled")] = toJsonArray(input.paths.disabledTranslationCsvPaths);
    root[QStringLiteral("model_list_collapsed_folders")] = toJsonArray(input.paths.collapsedModelFolders);
    root[QStringLiteral("translation_path")] =
        PathUtils::collectEnabledPaths(input.paths.translationCsvPaths, input.paths.disabledTranslationCsvPaths).value(0);

    if (input.writeSettings) {
        input.writeSettings(root);
    }
    root.remove(QStringLiteral("model_switch_delay_ms"));

    if (input.windowSize.isValid()) {
        root[QStringLiteral("window_width")] = input.windowSize.width();
        root[QStringLiteral("window_height")] = input.windowSize.height();
    }

    if (input.saveTreeState) {
        QJsonObject treeState;
        treeState[QStringLiteral("expanded_items")] = toJsonArray(input.treeState.expandedItems);
        treeState[QStringLiteral("scroll_pos")] = input.treeState.scrollPos;
        root[QStringLiteral("tree_state")] = treeState;
    }

    if (input.saveFilterSortState) {
        const StartupFilterSortState &s = input.filterSortState;
        QJsonObject fs;
        fs[QStringLiteral("sort_index")] = s.sortIndex;
        fs[QStringLiteral("base_model")] = s.filterBaseModel;
        fs[QStringLiteral("model_type")] = s.filterModelType;
        fs[QStringLiteral("search_text")] = s.searchText;
        fs[QStringLiteral("sidebar_tab")] = s.sidebarTab;
        fs[QStringLiteral("collection_filter")] = s.collectionFilter;
        fs[QStringLiteral("home_author_filter")] = s.homeAuthorFilter;
        fs[QStringLiteral("home_filter_expanded")] = s.homeFilterExpanded;
        fs[QStringLiteral("home_tag_sort_by_count")] = s.homeFilterTagsSortByCount;
        QJsonArray homeTagsArr;
        for (const QString &t : s.homeTagFilters) {
            if (!t.trimmed().isEmpty()) homeTagsArr.append(t.trimmed());
        }
        fs[QStringLiteral("home_tag_filters")] = homeTagsArr;
        fs[QStringLiteral("user_tag_sort_mode")] = s.userTagSortMode;
        fs[QStringLiteral("user_tag_lora_only")] = s.userTagLoraOnly;
        fs[QStringLiteral("user_tag_include_negative")] = s.userTagIncludeNegative;
        fs[QStringLiteral("gallery_prompt_sort_mode")] = s.galleryPromptSortMode;
        fs[QStringLiteral("gallery_tag_view_active")] = s.galleryTagViewActive;
        root[QStringLiteral("filter_sort_state")] = fs;
    } else {
        root.remove(QStringLiteral("filter_sort_state"));
    }

    QSaveFile file(configPath);
    const QByteArray payload = QJsonDocument(root).toJson();
    if (!file.open(QIODevice::WriteOnly)
        || file.write(payload) != payload.size()
        || !file.commit()) {
        qWarning() << "Unable to save global settings atomically:" << file.errorString();
        return false;
    }
    return true;
}

} // namespace AppConfigStore
