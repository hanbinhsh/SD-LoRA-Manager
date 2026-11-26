#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QListWidgetItem>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QGraphicsDropShadowEffect>
#include <QMap>
#include <QSet>
#include <QHash>
#include <QPointer>
#include <QPushButton>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QLabel>
#include <QTimer>
#include <QCollator>
#include <QFuture>
#include <QQueue>
#include <QButtonGroup>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QColor>
#include <QElapsedTimer>
#include <QCheckBox>
#include <QFrame>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QStackedWidget>
#include <QComboBox>
#include <QTextBrowser>
#include <functional>

#include "pages/downloadmodels.h"
#include "core/modelitemroles.h"
#include "core/modeltypes.h"
#include "pages/settingspage.h"
#include "pages/aboutpage.h"

const QString CURRENT_VERSION = "1.6.2";
const QString GITHUB_REPO_API = "https://api.github.com/repos/hanbinhsh/SD-LoRA-Manager/releases/latest";

const QString DEFAULT_FILTER_TAGS = "BREAK, ADDCOMM, ADDBASE, ADDCOL, ADDROW";
const QString DEFAULT_GALLERY_NSFW_RULES = "nsfw, explicit, nude, naked, nipples, pussy, penis, sex";

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class PromptParserWidget;
class TagBrowserWidget;
class LlmPromptWidget;
class UsageAnalysisWidget;
class PromptTemplateLibraryWidget;
class LauncherWidget;
class ImageViewerDialog;
class DownloadsPage;
class DownloadManager;
class LocalMetaEditorPage;
class UserGalleryPage;
class HomeGalleryPage;
class AppUpdateChecker;
struct LocalMetaEditorContext;
struct HomeFilterTagInfo;
class PreviewDownloadQueue;
class MetadataSyncService;
class ModelUpdateCoordinator;

struct ImageLoadResult {
    QString path;
    QImage originalImg; // 只存原图，模糊交给主线程做
    bool valid = false;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onModelListClicked(QListWidgetItem *item);
    void onCollectionTreeItemClicked(QTreeWidgetItem *item, int column);
    void onModelsTabButtonClicked();
    void onCollectionsTabButtonClicked();
    void onCollectionTreeContextMenu(const QPoint &pos);
    void onForceUpdateClicked();
    void onScanLocalClicked();
    void onOpenUrlClicked();
    void onCopyLoraTagClicked();
    void onApiMetadataReceived(QNetworkReply *reply);
    void onGalleryImageClicked(int index);
    void onHomeButtonClicked(); // 切换到主页
    void onHomeModelClicked(const QString &targetPath); // 主页大图点击跳转
    void onSidebarContextMenu(const QPoint &pos); // 侧边栏右键
    void onCreateCollection(); // 创建新收藏夹按钮点击
    void onCollectionFilterClicked(const QString &collectionName); // 点击收藏夹过滤
    void onIconLoaded(const QString &filePath, const QImage &image);
    void onHashCalculated();
    void onSearchTextChanged(const QString &text);
    void onBtnFavoriteClicked();
    void onSortIndexChanged(int index);
    void onFilterBaseModelChanged(const QString &text);
    void rebuildSortFilterMenu();        // 重建侧栏“排序/筛选”多级菜单（底模/类型项会随扫描变化）
    void updateSortFilterButtonText();   // 刷新侧栏“排序/筛选”按钮文案以反映当前排序与筛选
    void onToggleDetailTab(); // 切换 Tab 的槽函数
    void onOpenModelGraphClicked(); // 跳转到 Tag 与模型图谱并聚焦当前模型
    void onRescanUserClicked();
    void onGalleryButtonClicked();
    // === 菜单槽函数 ===
    void onMenuSwitchToLibrary();
    void onMenuSwitchToSettings();
    // === 设置页槽函数 ===
    void onBrowseLoraPath();
    void onBrowseGalleryPath();
    void onSetSdFolderClicked();
    void onClearUserGalleryCacheClicked();

    void onBrowseTranslationPath();
    void showRawImageMetadataDialog(const QString &filePath);
    void showComfyWorkflowViewer(const QString &filePath);

    void onMenuSwitchToAbout();
    void onCheckUpdateClicked();
    void onTestCivitaiApiKeyClicked();
    void onLocalMetaSaved(const QString &filePath, const QString &jsonPath, const QString &previewPath);
    void onLocalMetaResetClicked();
    void onEditMetaTabClicked();

private:
    Ui::MainWindow *ui = nullptr;
    PromptParserWidget *parserWidget = nullptr;
    TagBrowserWidget *tagBrowserWidget = nullptr;
    LlmPromptWidget *llmPromptWidget = nullptr;
    UsageAnalysisWidget *usageAnalysisWidget = nullptr;
    PromptTemplateLibraryWidget *promptTemplateLibraryWidget = nullptr;
    LauncherWidget *launcherWidget = nullptr;
    DownloadsPage *downloadsPage = nullptr;
    SettingsPage *settingsPage = nullptr;
    QPointer<ImageViewerDialog> imageViewer;
    AboutPage *aboutPage = nullptr;
    HomeGalleryPage *homeGalleryPage = nullptr;
    UserGalleryPage *userGalleryPage = nullptr;
    LocalMetaEditorPage *localMetaEditorPage = nullptr;
    QSet<int> pendingToolTabLoads;
    QNetworkAccessManager *netManager = nullptr;
    QPixmap currentHeroPixmap;
    QString currentHeroPath;
    ModelMeta currentMeta;

    // === 收藏夹管理 ===
    // Key: 收藏夹名称, Value: 模型文件名列表 (BaseName)
    QMap<QString, QStringList> collections;
    QHash<QString, QColor> modelHighlightColors;
    QHash<QString, ModelUserNote> modelUserNotes;
    QString currentCollectionFilter; // 当前显示的收藏夹 ("" 代表全部)

    // 收藏夹 JSON 读写
    void loadCollections();
    void saveCollections();
    void loadModelHighlightColors();
    void saveModelHighlightColors();
    void loadModelUserNotes();
    void saveModelUserNotes() const;
    QString modelUserNotesPath() const;
    QStringList normalizeModelUserTags(const QStringList &tags) const;
    QStringList normalizeModelUserTagsText(const QString &text) const;
    QStringList normalizeModelCustomTriggers(const QStringList &triggers) const;
    QString formatModelRating(double rating) const;
    QString formatModelUserNoteTooltip(const QString &filePath, const QString &baseTooltip = QString()) const;
    void applyModelUserNoteData(QListWidgetItem *item);
    void applyModelUserNoteData(QTreeWidgetItem *item);
    void refreshModelUserNoteItems(const QString &filePath);
    void refreshModelUserNotePanel(const QString &filePath = QString());
    void refreshModelAttributionPanel(const ModelMeta &meta);
    void refreshHomeFilterChips();
    QList<HomeFilterTagInfo> collectAvailableHomeFilterTags() const;
    void setHomeAuthorFilter(const QString &author);
    void addHomeTagFilter(const QString &tag);
    void clearHomeFilters();
    void openModelNoteDialog(QListWidgetItem *item);
    void setUserRatingForItems(const QList<QListWidgetItem*> &items, double rating);
    void addUserTagsForItems(const QList<QListWidgetItem*> &items, const QStringList &tags);
    void removeUserTagsForItems(const QList<QListWidgetItem*> &items, const QStringList &tags);
    void refreshHomeCollectionsUI(); // 刷新主页顶部的按钮
    void refreshHomeGallery(); // 刷新主页下方的图库
    void syncHomeGalleryOptions();
    void renameCollectionByName(const QString &name);
    void deleteCollectionByName(const QString &name);

    void scanModels(const QString &path);
    void scanModels(const QStringList &paths, std::function<void()> onComplete = {});
    void updateDetailView(const ModelMeta &meta);
    void fitDetailContentToCurrentPage();
    void refreshTriggerWordsPanel(const ModelMeta &meta);
    void clearDetailView();

    void fetchModelInfoFromCivitai(const QString &hash);
    void startModelHashSync(const QString &filePath, const QString &baseName, bool forceRefresh);
    void showPendingLocalModelDetail(const ModelMeta &meta, const QString &message);
    void setModelTitleNormal();
    void setModelTitleError(const QString &message);
    QString modelSyncFailurePath() const;
    void loadModelSyncFailures();
    void saveModelSyncFailures() const;
    void recordModelSyncFailure(const QString &filePath, const QString &baseName, const QString &error);
    void clearModelSyncFailure(const QString &filePath);
    QString modelSyncFailureMessage(const QString &filePath) const;
    QNetworkRequest makeNetworkRequest(const QUrl &url, bool allowCivitaiAuth = true) const;
    QString civitaiApiKey() const;
    bool isCivitaiUrl(const QUrl &url) const;
    bool shouldUseCivitaiBearerAuth(const QUrl &url) const;
    QUrl civitaiUrlWithToken(const QUrl &url) const;
    QString civitaiNetworkErrorMessage(QNetworkReply *reply) const;
    void applyCivitaiAttributionToItem(QListWidgetItem *item, const QString &creator, const QStringList &tags);
    void saveLocalMetadata(const QString &modelDir, const QString &baseName, const QJsonObject &data);
    void clearLayout(QLayout *layout);
    void addBadge(QString text, bool isRed = false);
    void downloadThumbnail(const QString &url, const QString &savePath, QPushButton *button);
    void openImageViewer(const QStringList &paths, const QString &currentPath);
    void openImageViewerForPath(const QString &currentPath);
    QStringList currentModelPreviewPaths() const;
    QStringList visibleUserGalleryImagePaths() const;
    void applyDownloadedPreviewToUi(const QString &localBaseName, const QString &savePath);
    void updateBackgroundImage();
    void updateLocalEditorFromMeta(const ModelMeta &meta);
    void setLocalMetaStatus(const ModelMeta &meta);
    LocalMetaEditorContext buildLocalMetaEditorContext(const ModelMeta &meta) const;
    void refreshCurrentDetailCacheStatus();
    void refreshUsageAnalysisWidget();
    void refreshPromptTemplateModelTriggerRows();
    int countLocalEditedModels() const;
    bool confirmLocalEditOverwrite(QListWidgetItem *item);
    void showCollectionMenu(const QList<QListWidgetItem*> &items, const QPoint &globalPos);
    void applyModelHighlightColor(QListWidgetItem *item);
    void applyModelHighlightColor(QTreeWidgetItem *item);
    void setHighlightColorForItems(const QList<QListWidgetItem*> &items, const QColor &color);
    void clearHighlightColorForItems(const QList<QListWidgetItem*> &items);
    // 快速读取单个 JSON 的元数据用于列表显示
    void preloadItemMetadata(QListWidgetItem *item, const QString &jsonPath);
    void refreshModelUsageStatsAsync();
    QFutureWatcher<ImageLoadResult> *imageLoadWatcher = nullptr;
    QPixmap applyBlurToImage(const QImage &srcImg, const QSize &bgSize, const QSize &heroSize);
    static ImageLoadResult processImageTask(const QString &path);
    QPixmap nextHeroPixmap;         // 即将显示的新 Hero 图
    QPixmap currentBlurredBgPix;    // 当前背景的模糊完成图 (用于过渡缓存)
    QPixmap nextBlurredBgPix;       // 下一张背景的模糊完成图 (用于过渡缓存)
    float transitionOpacity = 0.0;  // 过渡透明度 (0.0 - 1.0)
    QVariantAnimation *transitionAnim = nullptr; // 动画控制器
    void transitionToImage(const QString &path);
    void updateBackgroundDuringTransition();
    void initServices();
    void enqueueDownload(const QString &url,
                         const QString &savePath,
                         QPushButton *btn,
                         const QString &localBaseName,
                         int imageIndex = -1,
                         const PreviewMetadataPayload &previewMeta = PreviewMetadataPayload(),
                         bool allowNoButton = false,
                         bool metadataOnly = false,
                         bool countForMetadataSync = false);
    void syncPreviewImagesFromMetadata(const QString &modelDir, const QString &baseName, const QVector<ImageInfo> &images, bool forceNonCoverDownload, bool countForMetadataSync = false);
    void initDownloadsPage();
    void initSettingsPage();
    void onMenuSwitchToDownloads();
    void checkUpdatesForItems(const QList<QListWidgetItem*> &items, bool switchToDownloads = true, bool detailPrompt = false);
    void handleModelUpdateRootReceived(const QString &filePath,
                                       int matchedVersionId,
                                       const QJsonObject &modelRoot,
                                       const ModelUpdateInfo &fallbackInfo);
    void handleAllModelUpdateChecksCompleted(int totalCount, bool detailPrompt, const QString &detailFilePath);
    ModelUpdateInfo parseModelUpdateInfo(QListWidgetItem *item, const QJsonObject &modelRoot) const;
    void addOrUpdateDownloadCard(const ModelUpdateInfo &info, const QString &status);
    QString chooseModelDownloadTarget(const ModelUpdateInfo &info, bool *overwrite);
    void finishModelDownload(const ModelFileDownloadTask &task);
    void jumpToDownloadSource(const QString &filePath);
    void openDownloadCivitaiPage(const QString &filePath);
    void showFileInFolder(const QString &filePath);
    QVector<MetadataScanItem> collectMetadataScanSeeds() const;
    void startMetadataScan();
    void runMetadataHealthCheck();
    void startMetadataSyncForPaths(const QStringList &filePaths, bool updateExisting, bool archiveOnly = false);
    bool saveMetadataFromModelRoot(const MetadataSyncJob &job,
                                   const QJsonObject &modelRoot,
                                   const QJsonObject &versionHint,
                                   bool syncPreviewImages,
                                   bool batchRunning);
    bool startDetailCivArchiveFallback(const QString &filePath, const QString &baseName, const QString &modelDir, const QString &reason, const QString &currentSha256 = QString());
    void handleMetadataSyncJobFailed(const MetadataSyncJob &job, const QString &message, const QString &category);
    UpdateCheckSnapshot snapshotForModelItem(QListWidgetItem *item) const;
    QListWidgetItem *findModelItemByFilePath(const QString &filePath) const;
    QString resolveDownloadPreviewPath(const ModelUpdateInfo &info) const;
    void applySettingsState(SettingsState state);
    void resetFilterTagsToDefault();
    void resetGalleryNsfwRulesToDefault();
    void applyRandomUserAgent();
    void downloadAiTagTranslateDictionary();
    void applyApplicationTheme(const QString &themeId, const QString &customPath, bool updateStatus);
    void applyActivePaletteToUi(); // 用当前活动调色板重建 QSS 并刷新（主题编辑器实时预览用）
    void applyToolPageTheme(QWidget *page);
    void refreshLoadedToolPageThemes();
    void onEditThemeRequested(const QString &baseThemeId); // 打开主题编辑器
    void onThemeSaved(const QString &themeId);             // 编辑器保存 -> 应用并持久化
    void onThemeDeleteRequested(const QString &themeId);   // 删除用户主题
    void showModelDescriptionDialog();
    void beginGalleryBuild(const ModelMeta &meta);
    void buildGalleryBatch();
    void addGalleryThumbButton(const ModelMeta &meta, int index, const QString &modelDir, const QString &baseName);
    void cancelGalleryBuild();

    // 执行排序
    void executeSort();
    // 执行筛选
    void executeFilter();

    int modelUsageStatsToken = 0;
    int modelScanToken = 0;
    bool modelScanRunning = false;
    int translationLoadToken = 0;
    bool m_forceResyncPreview = false;
    bool m_skipPreviewSync = false;

    QFutureWatcher<QString> *hashWatcher = nullptr;
    QFutureWatcher<QVector<MetadataScanItem>> *metadataScanWatcher = nullptr;
    QFutureWatcher<QVector<MetadataHealthIssue>> *metadataHealthWatcher = nullptr;
    QString currentProcessingPath;
    QString currentProcessingBaseName;
    QString pendingHashSyncPath;
    QString pendingHashSyncBaseName;
    bool pendingHashSyncForceRefresh = false;

    QThreadPool *threadPool = nullptr;           //用于详情页、大图 (可被 cancel)
    QThreadPool *backgroundThreadPool = nullptr; // 【新增】用于侧边栏、主页列表 (不可被 cancel)
    QIcon placeholderIcon;       // 主页大图：铺满整框的占位X
    QIcon smallPlaceholderIcon;  // 侧边栏/收藏树：带内边距的小占位X（与 getSquareIcon 视觉一致）
    QIcon noPreviewIcon;         // 已同步/本地模型明确无预览：空心圆
    QIcon smallNoPreviewIcon;    // 侧边栏/收藏树用空心圆

    QTimer *bgResizeTimer = nullptr;
    QTimer *detailGalleryBuildTimer = nullptr;
    ModelMeta pendingGalleryMeta;
    QString pendingGalleryModelDir;
    QString pendingGalleryBaseName;
    QList<int> pendingGalleryIndices;
    int galleryBuildToken = 0;
    QHash<QString, QJsonObject> modelSyncFailures;
    bool currentHashSyncForceRefresh = false;

    AppUpdateChecker *appUpdateChecker = nullptr;
    PreviewDownloadQueue *previewDownloadQueue = nullptr;
    MetadataSyncService *metadataSyncService = nullptr;
    ModelUpdateCoordinator *modelUpdateCoordinator = nullptr;
    DownloadManager *downloadManager = nullptr;
    QMetaObject::Connection pendingDownloadChecksAfterRestore;
    bool isShuttingDown = false;
    bool settingsPageConnectionsInitialized = false;

    void jumpToModelOrLora(const QString &name, const QString &hash = QString()); // 点击模型/LoRA chip 跳转
    QListWidgetItem *findModelItemByNameOrHash(const QString &name, const QString &hash = QString()) const;

    bool isGalleryImageNsfw(const QString &prompt, const QStringList &cleanTags) const;
    bool isGalleryImageNsfwByPath(const QString &imagePath) const;
    void syncUserGalleryOptions();
    void refreshGalleryNsfwDisplay();

    void scanForUserImages(const QString &loraBaseName);

    QString currentModelLoraTagName() const;

    QIcon generatePlaceholderIcon();
    QIcon generateSmallPlaceholderIcon(); // 侧边栏/收藏树用：64px + 8px 内边距，匹配 getSquareIcon
    QIcon generateNoPreviewIcon(bool small) const;
    void recolorPlaceholderItems(); // 切主题时重染模型缺图状态和普通损坏占位
    void syncModelPreviewStateToViews(QListWidgetItem *sourceItem);

    // 定义一个特殊的字符串标识“未分类”
    const QString FILTER_UNCATEGORIZED = "__UNCATEGORIZED__";

    QHash<QString, QString> translationMap; // 存储翻译数据

    void refreshCollectionTreeView();
    void cancelPendingTasks();
    void syncTreeSelection(const QString &filePath);
    void initMenuBar();       // 菜单初始化
    void ensureToolTabLoaded(int index);
    void openTagModelGraph(int scope, const QString &focusQuery);

    QString currentUserAgent;                           // 当前UA

    // Key: 文件绝对路径, Value: 缓存的图片信息
    QMap<QString, UserImageInfo> imageCache;
    bool userGalleryCacheLoading = false;
    int userGalleryCacheLoadToken = 0;
    bool pendingUserGalleryScan = false;
    QString pendingUserGalleryModel;
    QString pendingUserGalleryModelPath;
    bool userGalleryGlobalMode = false;
    void loadUserGalleryCache();
    void resumePendingUserGalleryScan();
    void saveUserGalleryCache();

    // === 配置变量 ===
    QStringList   loraPaths;                                                      // LoRA文件夹列表
    QStringList   galleryPaths;                                                   // 图库路径列表
    QSet<QString> disabledLoraPaths;                                              // 关闭的 LoRA 路径
    QSet<QString> disabledGalleryPaths;                                           // 关闭的图库路径
    QStringList   translationCsvPaths;                                            // Tag 翻译表路径列表
    QSet<QString> disabledTranslationCsvPaths;                                    // 关闭的 Tag 翻译表路径
    QString       currentLoraPath;                                                // LoRA主路径 (兼容)
    QString       translationCsvPath;                                             // 翻译文件路径
    QString       sdOutputFolder;                                                 // 图库主路径 (兼容)
    QString       optSavedUAString                            = "";               // 设置的UA
    QString       optCivitaiApiKey                            = "";               // Civitai API Key
    bool          optLoraRecursive                            = false;            // 递归搜索Lora文件夹
    bool          optGalleryRecursive                         = false;            // 递归搜索图库文件夹
    int           optBlurRadius                               = 30;               // 模糊半径
    bool          optDownscaleBlur                            = true;             // 模糊前缩放
    int           optBlurProcessWidth                         = 500;              // 默认缩小到 500px
    int           optRenderThreadCount                        = 4;                // 图片处理线程数
    QHash<QString, QIcon> sidebarIconCache;
    QHash<QString, QListWidgetItem*> modelItemByPath;
    QMultiHash<QString, QTreeWidgetItem*> collectionTreeItemsByPath;
    QTimer *searchDebounceTimer = nullptr;
    int usageAnalysisRefreshToken = 0;

    void invalidateIconCacheForPath(const QString &previewPath);

    bool          optRestoreTreeState                         = true;             // 保存菜单状态
    bool          optRestoreFilterSortState                   = true;             // 启动时恢复排序与筛选条件
    bool          optSplitOnNewline                           = true;             // 换行符分割
    bool          optFilterNSFW                               = false;            // NSFW过滤
    int           optNSFWMode                                 = 1;                // 0: 完全隐藏, 1: 高斯模糊
    int           optNSFWLevel                                = 1;                // NSFW筛选等级
    bool          optFilterGalleryNSFW                        = false;            // 返图 NSFW 过滤
    int           optGalleryNSFWMode                          = 1;                // 0: 完全隐藏, 1: 高斯模糊
    QStringList   optGalleryNSFWRules                         = DEFAULT_GALLERY_NSFW_RULES.split(',', Qt::SkipEmptyParts);
    bool          optShowEmptyCollections                     = false;            // 显示空收藏夹
    bool          optCollectionFolderTopLevel                 = false;            // 收藏夹树按 LoRA 根目录作为顶层分类
    bool          optCollectionFolderSecondLevel              = false;            // 收藏夹树按 收藏夹/LoRA 根目录/模型 分组
    bool          optModelListFolderGrouping                  = false;            // Models 列表按 LoRA 根目录分组
    bool          optUseArrangedUA                            = false;            // 使用自定义UA
    QStringList   optFilterTags                               = DEFAULT_FILTER_TAGS.split(',', Qt::SkipEmptyParts);    // 过滤词列表 (存储清洗后的列表)
    bool          optUseCivitaiName                           = false;            // 使用json中的模型名称
    bool          optSuppressLocalWarnings                    = false;            // 隐藏本地模型总量提醒
    int           optUserGalleryMatchMode                     = 0;                // 0: 当前逻辑匹配, 1: 摘要值匹配(可回退), 2: 严格摘要值匹配(不回退)
    bool          optComfyModelNameFallback                   = true;             // ComfyUI 图片缺少摘要时允许按名称匹配模型
    bool          optRecalculateKnownMetadataHash             = false;            // 元信息同步时是否重新计算已有 Hash
    bool          optTryCivArchiveOnMetadataFail              = true;             // Civitai 元信息失败时尝试 CivArchive
    bool          optConfirmCloseWhileLauncherRunning         = true;             // 启动器有进程运行时关闭软件是否弹窗确认
    int           optModelUpdateDownloadPolicy                = 0;                // 0: 每次询问, 1: 保留旧版, 2: 覆盖当前文件
    bool          optAutoCheckUpdatesOnStartup                = true;             // 启动时自动检查软件更新
    double        optUiScale                                  = 1.0;              // 缩放比率
    QString       optThemeId                                  = "steam_dark";    // 当前程序主题
    QString       optCustomThemePath                          = "";              // 外部 QSS 主题路径
    QString       currentToolPageQss;                                             // 当前工具页样式
    // 保存与加载
    void loadGlobalConfig();        // 加载配置
    void saveGlobalConfig();        // 保存配置
    void startAppUpdateCheck(bool silentIfLatest);
    // 设置辅助变量
    QSet<QString> startupExpandedCollections; // 启动时从文件读出的展开项
    int startupTreeScrollPos;                 // 启动时从文件读出的滚动位置
    bool isFirstTreeRefresh;                  // 标记是否是第一次刷新树（用于判断是否使用缓存）
    bool isFirstFilterSortRestore = true;     // 标记是否是第一次模型扫描后恢复排序与筛选
    int startupSortIndex = 0;
    QString startupFilterBaseModel = QStringLiteral("All");
    QString startupFilterModelType = QStringLiteral("All");
    QString startupSearchText;
    int startupSidebarTab = 1;
    int startupUserTagSortMode = 0;
    bool startupUserTagLoraOnly = false;
    bool startupUserTagIncludeNegative = false;
    int startupGalleryPromptSortMode = 0;
    bool startupGalleryTagViewActive = false;
    bool startupHomeFilterExpanded = false;
    bool startupHomeFilterTagsSortByCount = true;
    QString startupCollectionFilter;
    QString startupHomeAuthorFilter;
    QSet<QString> startupHomeTagFilters;
    QSet<QString> collapsedModelFolders;      // Models 列表折叠的文件夹
    // 设置辅助函数
    QString getRandomUserAgent();             // 获取随机 UA
    void updateModelListNames();              // 刷新列表显示名称的辅助函数
    bool isModelListItem(const QListWidgetItem *item) const;
    QListWidgetItem *createModelFolderHeader(const QString &folderName, const QString &folderKey) const;
    void applyModelFolderVisibility();
    void toggleModelFolderCollapsed(const QString &folderKey);

    void applyPathListsToUi();
    bool editLoraPaths(bool rescanAfter);
    bool editGalleryPaths(bool rescanAfter);
    bool editTranslationCsvPaths();
    void reloadTranslationMaps(bool notifyWidgets = true, bool asynchronous = false);
};

#endif // MAINWINDOW_H
