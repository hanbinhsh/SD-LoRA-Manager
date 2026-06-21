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
#include <functional>

#include "pages/downloadmodels.h"
#include "pages/settingspage.h"
#include "pages/aboutpage.h"
#include "widgets/tagflowwidget.h"

// 模型列表相关
const int ROLE_MODEL_NAME             = Qt::UserRole;
const int ROLE_FILE_PATH              = Qt::UserRole + 1;
const int ROLE_PREVIEW_PATH           = Qt::UserRole + 2;
const int ROLE_NSFW_LEVEL             = Qt::UserRole + 3;
const int ROLE_CIVITAI_NAME           = Qt::UserRole + 4;   // 存储从 JSON 读取的真实名称
// 排序与筛选
const int ROLE_SORT_DATE              = Qt::UserRole + 10;  // 存储时间戳 (qint64)
const int ROLE_SORT_DOWNLOADS         = Qt::UserRole + 11;  // 存储下载量 (int)
const int ROLE_SORT_LIKES             = Qt::UserRole + 12;  // 存储点赞量 (int)
const int ROLE_FILTER_BASE            = Qt::UserRole + 13;  // 存储底模名称 (QString)
const int ROLE_SORT_ADDED             = Qt::UserRole + 14;  // 存储本地文件创建时间 (qint64)
const int ROLE_LOCAL_EDITED           = Qt::UserRole + 15;  // 标记本地/已编辑模型 (bool)
const int ROLE_SORT_USAGE_COUNT       = Qt::UserRole + 16;  // 本地返图使用次数
const int ROLE_SORT_LAST_USED         = Qt::UserRole + 17;  // 本地返图最近使用时间
const int ROLE_MODEL_ROOT_PATH        = Qt::UserRole + 18;  // 模型所属的用户配置根目录
const int ROLE_MODEL_ROOT_NAME        = Qt::UserRole + 19;  // 模型所属根目录显示名
const int ROLE_MODEL_FOLDER_KEY       = Qt::UserRole + 20;  // Models 列表文件夹折叠键
const int ROLE_MODEL_FOLDER_COLLAPSED = Qt::UserRole + 21;  // Models 列表文件夹是否折叠
const int ROLE_MODEL_FILTER_VISIBLE   = Qt::UserRole + 22;  // 搜索/底模/收藏夹过滤后的可见状态
const int ROLE_MODEL_HIGHLIGHT_COLOR  = Qt::UserRole + 23;  // 模型侧边栏高亮色
const int ROLE_USER_RATING            = Qt::UserRole + 24;  // 用户评分
const int ROLE_USER_NOTE              = Qt::UserRole + 25;  // 用户备注
const int ROLE_USER_TAGS              = Qt::UserRole + 26;  // 用户标签
const int ROLE_USER_CUSTOM_TRIGGERS   = Qt::UserRole + 27;  // 用户自定义触发词
const int ROLE_MODEL_CREATOR          = Qt::UserRole + 28;  // Civitai 作者
const int ROLE_MODEL_TAGS             = Qt::UserRole + 29;  // Civitai 模型标签
const int ROLE_MODEL_TYPE             = Qt::UserRole + 30;  // Civitai 模型类型，如 LoRA / Checkpoint
const int ROLE_MODEL_TRAINED_WORDS    = Qt::UserRole + 31;  // Civitai/metadata 触发词
// 用户图库专用
const int ROLE_USER_IMAGE_PATH        = Qt::UserRole + 40;
const int ROLE_USER_IMAGE_PROMPT      = Qt::UserRole + 41;
const int ROLE_USER_IMAGE_NEG         = Qt::UserRole + 42;
const int ROLE_USER_IMAGE_PARAMS      = Qt::UserRole + 43;
const int ROLE_USER_IMAGE_TAGS        = Qt::UserRole + 44;
const int ROLE_USER_IMAGE_NEG_TAGS    = Qt::UserRole + 45;
const int ROLE_EDIT_IMAGE_PATH        = Qt::UserRole + 46;
const int ROLE_IS_FOLDER_HEADER       = Qt::UserRole + 47;
// 树状图占位符标记
const int ROLE_IS_PLACEHOLDER         = Qt::UserRole + 50;
const int ROLE_CIVITAI_MODEL_ID       = Qt::UserRole + 51;
const int ROLE_CIVITAI_VERSION_ID     = Qt::UserRole + 52;
const int ROLE_CIVITAI_SHA256         = Qt::UserRole + 53;
const int ROLE_SYNC_FAILED            = Qt::UserRole + 54;
const int ROLE_SYNC_ERROR             = Qt::UserRole + 55;
// 收藏夹树状图
const int ROLE_IS_COLLECTION_NODE     = Qt::UserRole + 60;  // 标记这是一个收藏夹节点
const int ROLE_COLLECTION_NAME        = Qt::UserRole + 61;  // 存储收藏夹名称
const int ROLE_ITEM_COUNT             = Qt::UserRole + 62;  // 存储该分类下的模型数量
const int ROLE_COLLECTION_EXPAND_KEY  = Qt::UserRole + 63;  // 存储收藏夹树展开状态键

const QString CURRENT_VERSION = "1.5.5";
const QString GITHUB_REPO_API = "https://api.github.com/repos/hanbinhsh/SD-LoRA-Manager/releases/latest";

const QString DEFAULT_FILTER_TAGS = "BREAK, ADDCOMM, ADDBASE, ADDCOL, ADDROW";

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE
struct ManagedPathEntry;

class PromptParserWidget;
class TagBrowserWidget;
class LlmPromptWidget;
class UsageAnalysisWidget;
class PromptTemplateLibraryWidget;
class LauncherWidget;
class DownloadsPage;
class DownloadManager;

struct ModelUserNote {
    double rating = 0.0;
    QString note;
    QStringList tags;
    QStringList customTriggers;
    QString updatedAt;
};

struct UpdateCheckSnapshot {
    QString filePath;
    QString modelDir;
    QString baseName;
    QString displayName;
    QString currentSha256;
    int modelId = 0;
    int currentVersionId = 0;
    bool localEdited = false;
};

struct ImageLoadResult {
    QString path;
    QImage originalImg; // 只存原图，模糊交给主线程做
    bool valid = false;
};

struct MetadataSyncJob {
    UpdateCheckSnapshot snapshot;
    bool updateExisting = false;
    bool civArchiveOnly = false;
    bool detailFallback = false;
};

struct PreviewMetadataPayload {
    QString prompt;
    QString negativePrompt;
    QString sampler;
    QString cfgScale;
    QString steps;
    QString seed;
    int width = 0;
    int height = 0;
    int nsfwLevel = 0;
};

struct DownloadTask {
    QString url;
    QString savePath;
    QString localBaseName;
    QPointer<QPushButton> button; // 使用 QPointer 防止按钮被销毁后野指针崩溃
    int imageIndex = -1;
    PreviewMetadataPayload previewMeta;
    bool allowNoButton = false;
    bool metadataOnly = false;
    bool countForMetadataSync = false;
};

struct ImageInfo {
    QString url;
    QString hash;
    QString prompt;
    QString negativePrompt;
    QString sampler;
    QString cfgScale;
    QString steps;
    QString seed;
    QString model;
    int nsfwLevel = 0;
    int width = 0;
    int height = 0;
    bool nsfw = false;
};

struct UserImageInfo {
    QString path;
    QString prompt;
    QStringList cleanTags;
    QStringList negativeCleanTags;
    QString negativePrompt;
    QString parameters;
    qint64 lastModified = 0;
    int parserVersion = 0;
};

struct ModelMeta {
    QString fileName;
    QString modelName;
    QString versionName;
    QString name;
    QString filePath;
    QString previewPath;
    QStringList trainedWordsGroups;
    QString modelUrl;
    QString baseModel;
    QString type;
    QString description;
    QString createdAt;
    bool nsfw;
    int downloadCount;
    int thumbsUpCount;
    double fileSizeMB;
    QString sha256;
    QString fileNameServer;
    QString creatorName;
    QString creatorAvatarUrl;
    QStringList modelTags;
    int modelId = 0;
    int versionId = 0;
    bool isLocalEdited = false;
    bool isLocalOnly = false;
    QList<ImageInfo> images;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

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
    void onHomeGalleryClicked(QListWidgetItem *item); // 主页大图点击跳转
    void onSidebarContextMenu(const QPoint &pos); // 侧边栏右键
    void onCreateCollection(); // 创建新收藏夹按钮点击
    void onCollectionFilterClicked(const QString &collectionName); // 点击收藏夹过滤
    void onIconLoaded(const QString &filePath, const QImage &image);
    void onHashCalculated();
    void onSearchTextChanged(const QString &text);
    void onBtnFavoriteClicked();
    void onHomeGalleryContextMenu(const QPoint &pos);
    void onSortIndexChanged(int index);
    void onFilterBaseModelChanged(const QString &text);
    void rebuildSortFilterMenu();        // 重建侧栏“排序/筛选”多级菜单（底模/类型项会随扫描变化）
    void updateSortFilterButtonText();   // 刷新侧栏“排序/筛选”按钮文案以反映当前排序与筛选
    void onUserImageClicked(QListWidgetItem *item);
    void onTagFilterChanged(const QSet<QString> &selectedTags);
    void onToggleDetailTab(); // 切换 Tab 的槽函数
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
    void onUserGalleryContextMenu(const QPoint &pos);
    void showRawImageMetadataDialog(const QString &filePath);
    void showComfyWorkflowViewer(const QString &filePath);

    void onMenuSwitchToAbout();
    void onCheckUpdateClicked();
    void onUpdateApiReceived(QNetworkReply *reply);
    void onTestCivitaiApiKeyClicked();
    void onLocalMetaSaveClicked();
    void onLocalMetaResetClicked();
    void onEditMetaTabClicked();
    void onEditImageSelectionChanged(int row);
    void onEditAddImageClicked();
    void onEditReplaceImageClicked();
    void onEditRemoveImageClicked();
    void onEditSetCoverClicked();

private:
    Ui::MainWindow *ui = nullptr;
    QTabWidget *toolsTabWidget = nullptr;
    PromptParserWidget *parserWidget = nullptr;
    TagBrowserWidget *tagBrowserWidget = nullptr;
    LlmPromptWidget *llmPromptWidget = nullptr;
    UsageAnalysisWidget *usageAnalysisWidget = nullptr;
    PromptTemplateLibraryWidget *promptTemplateLibraryWidget = nullptr;
    LauncherWidget *launcherWidget = nullptr;
    DownloadsPage *downloadsPage = nullptr;
    SettingsPage *settingsPage = nullptr;
    AboutPage *aboutPage = nullptr;
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
    QString currentHomeAuthorFilter;
    QSet<QString> currentHomeTagFilters;
    bool homeFilterExpanded = false;
    bool homeFilterTagsSortByCount = true;

    struct HomeFilterTagInfo {
        QString tag;
        int count = 0;
        bool fromModel = false;
        bool fromUser = false;

        QString sourceName() const
        {
            if (fromModel && fromUser) return QStringLiteral("mixed");
            if (fromUser) return QStringLiteral("user");
            return QStringLiteral("model");
        }
    };

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

    void scanModels(const QString &path);
    void scanModels(const QStringList &paths, std::function<void()> onComplete = {});
    void updateDetailView(const ModelMeta &meta);
    void fitDetailContentToCurrentPage();
    void refreshTriggerWordsPanel(const ModelMeta &meta);
    void clearDetailView();
    QIcon getSquareIcon(const QPixmap &srcPix);

    // 生成一个适合主页大图的 Icon (2:3比例)
    QString findLocalPreviewPath(const QString &dirPath, const QString &currentBaseName, const QString &serverFileName, int imgIndex) const;
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
    QJsonObject mergeCivitaiModelIntoVersion(const QJsonObject &versionRoot, const QJsonObject &modelRoot) const;
    QStringList readModelTagsFromJson(const QJsonObject &root) const;
    QString readModelCreatorFromJson(const QJsonObject &root) const;
    QString readModelCreatorAvatarFromJson(const QJsonObject &root) const;
    void applyCivitaiAttributionToItem(QListWidgetItem *item, const QString &creator, const QStringList &tags);
    void saveLocalMetadata(const QString &modelDir, const QString &baseName, const QJsonObject &data);
    bool readLocalJson(const QString &dirPath, const QString &baseName, ModelMeta &meta);
    void clearLayout(QLayout *layout);
    void addBadge(QString text, bool isRed = false);
    void downloadThumbnail(const QString &url, const QString &savePath, QPushButton *button);
    void showFullImageDialog(const QString &imagePath);
    QIcon getFitIcon(const QString &path);
    void applyDownloadedPreviewToUi(const QString &localBaseName, const QString &savePath);
    void updateBackgroundImage();
    void updateLocalEditorFromMeta(const ModelMeta &meta);
    void setLocalMetaStatus(const ModelMeta &meta);
    void refreshCurrentDetailCacheStatus();
    void refreshUsageAnalysisWidget();
    void refreshPromptTemplateModelTriggerRows();
    int countLocalEditedModels() const;
    bool confirmLocalEditOverwrite(QListWidgetItem *item);
    void refreshEditImages(const ModelMeta &meta);
    void loadEditImageFields(int index);
    void commitEditImageFields();
    QString currentEditBaseName() const;
    QString currentEditModelDir() const;
    QString editPreviewPathForIndex(int index) const;
    bool saveImageToPreviewPath(const QString &srcPath, const QString &destPath, int &outW, int &outH);
    void applyImageMetadataFromFile(const QString &srcPath, ImageInfo &img);
    void applyParametersToImage(const QString &params, ImageInfo &img);
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
    void enqueueDownload(const QString &url,
                         const QString &savePath,
                         QPushButton *btn,
                         const QString &localBaseName,
                         int imageIndex = -1,
                         const PreviewMetadataPayload &previewMeta = PreviewMetadataPayload(),
                         bool allowNoButton = false,
                         bool metadataOnly = false,
                         bool countForMetadataSync = false);
    void processNextDownload();
    void markMetadataPreviewTaskFinished();
    void finishMetadataSyncBatch();
    QString buildPreviewParametersText(const PreviewMetadataPayload &payload) const;
    bool savePreviewImageWithMetadata(const QByteArray &data, const QString &savePath, const PreviewMetadataPayload &payload) const;
    bool ensurePreviewImageMetadata(const QString &path, const PreviewMetadataPayload &payload) const;
    void syncPreviewImagesFromMetadata(const QString &modelDir, const QString &baseName, const QVector<ImageInfo> &images, bool forceNonCoverDownload, bool countForMetadataSync = false);
    void initDownloadsPage();
    void initSettingsPage();
    void onMenuSwitchToDownloads();
    void checkUpdatesForItems(const QList<QListWidgetItem*> &items, bool switchToDownloads = true, bool detailPrompt = false);
    void checkUpdateForSnapshot(const UpdateCheckSnapshot &snapshot);
    void dispatchQueuedUpdateChecks();
    void enqueueUpdateHashCheck(const UpdateCheckSnapshot &snapshot);
    void dispatchUpdateHashChecks();
    void markUpdateCheckFinished();
    void handleModelUpdateReply(QNetworkReply *reply);
    ModelUpdateInfo parseModelUpdateInfo(QListWidgetItem *item, const QJsonObject &modelRoot) const;
    void addOrUpdateDownloadCard(const ModelUpdateInfo &info, const QString &status);
    QString chooseModelDownloadTarget(const ModelUpdateInfo &info, bool *overwrite);
    QString uniqueFilePath(const QString &dirPath, const QString &fileName) const;
    void finishModelDownload(const ModelFileDownloadTask &task);
    void updateDownloadSelectionSummary();
    void updateDownloadModelActionButtons();
    void jumpToDownloadSource(const QString &filePath);
    void openDownloadCivitaiPage(const QString &filePath);
    void showFileInFolder(const QString &filePath);
    QVector<MetadataScanItem> collectMetadataScanSeeds() const;
    void startMetadataScan();
    void runMetadataHealthCheck();
    void startMetadataSyncForPaths(const QStringList &filePaths, bool updateExisting);
    void processNextMetadataSyncJob();
    void fetchMetadataForSyncJob(const MetadataSyncJob &job);
    void handleMetadataSyncModelReply(QNetworkReply *reply);
    void handleMetadataSyncVersionReply(QNetworkReply *reply);
    void handleMetadataSyncHashReply(QNetworkReply *reply);
    void handleMetadataSyncCivArchiveReply(QNetworkReply *reply);
    bool saveMetadataFromModelRoot(const MetadataSyncJob &job, const QJsonObject &modelRoot, const QJsonObject &versionHint = QJsonObject());
    void fetchMetadataFromCivArchive(const MetadataSyncJob &job, const QString &reason, bool directOnly = false);
    bool startDetailCivArchiveFallback(const QString &filePath, const QString &baseName, const QString &modelDir, const QString &reason, const QString &currentSha256 = QString());
    void finishMetadataSyncJobWithFailure(const MetadataSyncJob &job, const QString &message, const QString &category = "failed");
    bool tryStartCivArchiveHashCalculation(const MetadataSyncJob &job, const QString &reason);
    UpdateCheckSnapshot snapshotForModelItem(QListWidgetItem *item) const;
    QListWidgetItem *findModelItemByFilePath(const QString &filePath) const;
    QString resolveDownloadPreviewPath(const ModelUpdateInfo &info) const;
    void applySettingsState(SettingsState state);
    void resetFilterTagsToDefault();
    void applyRandomUserAgent();
    void applyApplicationTheme(const QString &themeId, const QString &customPath, bool updateStatus);
    void applyToolPageTheme(QWidget *page);
    void refreshLoadedToolPageThemes();
    void showModelDescriptionDialog();
    void beginGalleryBuild(const ModelMeta &meta);
    void buildGalleryBatch();
    void addGalleryThumbButton(const ModelMeta &meta, int index, const QString &modelDir, const QString &baseName);
    void cancelGalleryBuild();

    // 执行排序
    void executeSort();
    // 执行筛选
    void executeFilter();

    int currentEditImageIndex = -1;
    int editImageLoadToken = 0;
    int modelUsageStatsToken = 0;
    int modelScanToken = 0;
    bool editImagesNeedRefresh = false;
    bool m_forceResyncPreview = false;
    bool m_skipPreviewSync = false;

    QFutureWatcher<QString> *hashWatcher = nullptr;
    QFutureWatcher<QVector<MetadataScanItem>> *metadataScanWatcher = nullptr;
    QFutureWatcher<QVector<MetadataHealthIssue>> *metadataHealthWatcher = nullptr;
    QQueue<MetadataSyncJob> pendingMetadataSyncJobs;
    int metadataSyncTotal = 0;
    int metadataSyncDone = 0;
    bool metadataSyncRunning = false;
    bool metadataSyncPreviewImages = false;
    bool metadataSyncWaitingForPreviews = false;
    int metadataPreviewTasksPending = 0;
    QString currentProcessingPath;

    QThreadPool *threadPool = nullptr;           //用于详情页、大图 (可被 cancel)
    QThreadPool *backgroundThreadPool = nullptr; // 【新增】用于侧边栏、主页列表 (不可被 cancel)
    QIcon placeholderIcon;

    QTimer *bgResizeTimer = nullptr;
    QTimer *detailGalleryBuildTimer = nullptr;
    ModelMeta pendingGalleryMeta;
    QString pendingGalleryModelDir;
    QString pendingGalleryBaseName;
    QList<int> pendingGalleryIndices;
    int galleryBuildToken = 0;
    QHash<QString, QJsonObject> modelSyncFailures;
    bool currentHashSyncForceRefresh = false;

    QQueue<DownloadTask> downloadQueue; // 任务队列
    bool isDownloading = false;         // 当前是否有任务在运行
    DownloadManager *downloadManager = nullptr;
    bool isShuttingDown = false;
    bool settingsPageConnectionsInitialized = false;
    QQueue<UpdateCheckSnapshot> pendingUpdateChecksQueue;
    QQueue<UpdateCheckSnapshot> pendingUpdateHashChecks;
    int activeUpdateNetworkChecks = 0;
    int activeUpdateHashChecks = 0;
    int pendingUpdateChecks = 0;
    int completedUpdateChecks = 0;
    int updateCheckToken = 0;
    QString detailUpdateCheckFilePath;
    bool detailUpdateCheckPending = false;


    TagFlowWidget *tagFlowWidget = nullptr;

    void scanForUserImages(const QString &loraBaseName);
    void parsePngInfo(const QString &path, UserImageInfo &info);
    void refreshUserTagFlowStats();
    void resetUserImageThumbLoading();
    void scheduleVisibleUserImageThumbLoad();
    void dispatchVisibleUserImageThumbLoad();

    QString getSafetensorsInternalName(const QString &path);
    QString currentModelLoraTagName() const;

    QStringList parsePromptsToTags(const QString &rawPrompt);

    QIcon generatePlaceholderIcon();

    // 定义一个特殊的字符串标识“未分类”
    const QString FILTER_UNCATEGORIZED = "__UNCATEGORIZED__";

    QPixmap applyNSFWBlur(const QPixmap &pix);
    QPixmap applyRoundedMask(const QPixmap &src, int radius);
    QHash<QString, QString> translationMap; // 存储翻译数据

    void refreshCollectionTreeView();
    void cancelPendingTasks();
    void syncTreeSelection(const QString &filePath);
    void initMenuBar();       // 菜单初始化
    void ensureToolTabLoaded(int index);

    QString currentUserAgent;                           // 当前UA

    // Key: 文件绝对路径, Value: 缓存的图片信息
    QMap<QString, UserImageInfo> imageCache;
    QSet<QString> queuedUserImageThumbPaths;
    QSet<QString> loadedUserImageThumbPaths;
    QTimer *userImageThumbLoadTimer = nullptr;
    void loadUserGalleryCache();
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
    bool          optRestoreTreeState                         = true;             // 保存菜单状态
    bool          optSplitOnNewline                           = true;             // 换行符分割
    bool          optFilterNSFW                               = false;            // NSFW过滤
    int           optNSFWMode                                 = 1;                // 0: 完全隐藏, 1: 高斯模糊
    int           optNSFWLevel                                = 1;                // NSFW筛选等级
    bool          optShowEmptyCollections                     = false;            // 显示空收藏夹
    bool          optCollectionFolderTopLevel                 = false;            // 收藏夹树按 LoRA 根目录作为顶层分类
    bool          optCollectionFolderSecondLevel              = false;            // 收藏夹树按 收藏夹/LoRA 根目录/模型 分组
    bool          optModelListFolderGrouping                  = false;            // Models 列表按 LoRA 根目录分组
    bool          optUseArrangedUA                            = false;            // 使用自定义UA
    QStringList   optFilterTags                               = DEFAULT_FILTER_TAGS.split(',', Qt::SkipEmptyParts);    // 过滤词列表 (存储清洗后的列表)
    bool          optUseCivitaiName                           = false;            // 使用json中的模型名称
    bool          optSuppressLocalWarnings                    = false;            // 隐藏本地模型总量提醒
    int           optUserGalleryMatchMode                     = 0;                // 0: 当前逻辑匹配, 1: 摘要值匹配(可回退), 2: 严格摘要值匹配(不回退)
    bool          optRecalculateKnownMetadataHash             = false;            // 元信息同步时是否重新计算已有 Hash
    bool          optTryCivArchiveOnMetadataFail              = true;             // Civitai 元信息失败时尝试 CivArchive
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
    QSet<QString> collapsedModelFolders;      // Models 列表折叠的文件夹
    // 设置辅助函数
    QString getRandomUserAgent();             // 获取随机 UA
    void updateModelListNames();              // 刷新列表显示名称的辅助函数
    bool isModelListItem(const QListWidgetItem *item) const;
    QListWidgetItem *createModelFolderHeader(const QString &folderName, const QString &folderKey) const;
    void applyModelFolderVisibility();
    void toggleModelFolderCollapsed(const QString &folderKey);

    QStringList normalizePathList(const QStringList &paths) const;
    QSet<QString> normalizePathSet(const QSet<QString> &paths) const;
    QString formatPathListForEdit(const QStringList &paths) const;
    QStringList collectValidPaths(const QStringList &paths) const;
    QStringList collectEnabledPaths(const QStringList &paths, const QSet<QString> &disabledPaths) const;
    QList<ManagedPathEntry> buildPathEntries(const QStringList &paths, const QSet<QString> &disabledPaths) const;
    void applyPathEntries(const QList<ManagedPathEntry> &entries, QStringList &paths, QSet<QString> &disabledPaths);
    void applyPathListsToUi();
    bool editLoraPaths(bool rescanAfter);
    bool editGalleryPaths(bool rescanAfter);
    bool editTranslationCsvPaths();
    void reloadTranslationMaps(bool notifyWidgets = true);
};

#endif // MAINWINDOW_H
