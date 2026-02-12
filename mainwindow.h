#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QListWidgetItem>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QGraphicsDropShadowEffect>
#include <QMap>
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

#include "tagflowwidget.h"

// 模型列表相关
const int ROLE_MODEL_NAME           = Qt::UserRole;
const int ROLE_FILE_PATH            = Qt::UserRole + 1;
const int ROLE_PREVIEW_PATH         = Qt::UserRole + 2;
const int ROLE_NSFW_LEVEL           = Qt::UserRole + 5;
const int ROLE_CIVITAI_NAME         = Qt::UserRole + 6;   // 存储从 JSON 读取的真实名称
// 排序与筛选
const int ROLE_SORT_DATE            = Qt::UserRole + 10;  // 存储时间戳 (qint64)
const int ROLE_SORT_DOWNLOADS       = Qt::UserRole + 11;  // 存储下载量 (int)
const int ROLE_SORT_LIKES           = Qt::UserRole + 12;  // 存储点赞量 (int)
const int ROLE_FILTER_BASE          = Qt::UserRole + 13;  // 存储底模名称 (QString)
const int ROLE_SORT_ADDED           = Qt::UserRole + 14;  // 存储本地文件创建时间 (qint64)
// 收藏夹树状图
const int ROLE_IS_COLLECTION_NODE   = Qt::UserRole + 20;  // 标记这是一个收藏夹节点
const int ROLE_COLLECTION_NAME      = Qt::UserRole + 21;  // 存储收藏夹名称
const int ROLE_ITEM_COUNT           = Qt::UserRole + 22;  // 存储该分类下的模型数量
// 用户图库专用
const int ROLE_USER_IMAGE_PATH      = Qt::UserRole + 30;
const int ROLE_USER_IMAGE_PROMPT    = Qt::UserRole + 31;
const int ROLE_USER_IMAGE_NEG       = Qt::UserRole + 32;
const int ROLE_USER_IMAGE_PARAMS    = Qt::UserRole + 33;
const int ROLE_USER_IMAGE_TAGS      = Qt::UserRole + 34;
// 树状图占位符标记
const int ROLE_IS_PLACEHOLDER       = Qt::UserRole + 40;

const QString CURRENT_VERSION = "1.2.4";
const QString GITHUB_REPO_API = "https://api.github.com/repos/hanbinhsh/SD-LoRA-Manager/releases/latest";

const QString DEFAULT_FILTER_TAGS = "BREAK, ADDCOMM, ADDBASE, ADDCOL, ADDROW";

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

struct DownloadTask {
    QString url;
    QString savePath;
    QPointer<QPushButton> button; // 使用 QPointer 防止按钮被销毁后野指针崩溃
};

struct ImageLoadResult {
    QString path;
    QImage originalImg; // 只存原图，模糊交给主线程做
    bool valid = false;
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
    int nsfwLevel;
    int width;
    int height;
    bool nsfw;
};

struct UserImageInfo {
    QString path;
    QString prompt;
    QStringList cleanTags;
    QString negativePrompt;
    QString parameters;
    qint64 lastModified = 0;
};

struct ModelMeta {
    QString fileName;
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
    void onActionOpenFolderTriggered();
    void onForceUpdateClicked();
    void onScanLocalClicked();
    void onOpenUrlClicked();
    void onApiMetadataReceived(QNetworkReply *reply);
    void onImageDownloaded(QNetworkReply *reply);
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
    void onSettingsChanged(); // 通用改变处理
    void onBlurSliderChanged(int value);
    void onSetSdFolderClicked();

    void onBrowseTranslationPath();
    void onUserGalleryContextMenu(const QPoint &pos);

    void onMenuSwitchToAbout();
    void onCheckUpdateClicked();
    void onUpdateApiReceived(QNetworkReply *reply);

private:
    Ui::MainWindow *ui;
    QSettings *settings;
    QNetworkAccessManager *netManager;
    QPixmap currentHeroPixmap;
    QString currentHeroPath;
    ModelMeta currentMeta;

    // === 收藏夹管理 ===
    // Key: 收藏夹名称, Value: 模型文件名列表 (BaseName)
    QMap<QString, QStringList> collections;
    QString currentCollectionFilter; // 当前显示的收藏夹 ("" 代表全部)

    // 收藏夹 JSON 读写
    void loadCollections();
    void saveCollections();
    void refreshHomeCollectionsUI(); // 刷新主页顶部的按钮
    void refreshHomeGallery(); // 刷新主页下方的图库

    void scanModels(const QString &path);
    void updateDetailView(const ModelMeta &meta);
    void clearDetailView();
    QIcon getSquareIcon(const QPixmap &srcPix);

    // 生成一个适合主页大图的 Icon (2:3比例)
    QIcon getRoundedSquareIcon(const QString &path, int size, int radius);
    QString findLocalPreviewPath(const QString &dirPath, const QString &currentBaseName, const QString &serverFileName, int imgIndex);
    QString calculateFileHash(const QString &filePath);
    void fetchModelInfoFromCivitai(const QString &hash);
    void saveLocalMetadata(const QString &modelDir, const QString &baseName, const QJsonObject &data);
    bool readLocalJson(const QString &dirPath, const QString &baseName, ModelMeta &meta);
    void clearLayout(QLayout *layout);
    void addBadge(QString text, bool isRed = false);
    void downloadThumbnail(const QString &url, const QString &savePath, QPushButton *button);
    void showFullImageDialog(const QString &imagePath);
    QIcon getFitIcon(const QString &path);
    void updateBackgroundImage();
    void showCollectionMenu(const QList<QListWidgetItem*> &items, const QPoint &globalPos);
    // 快速读取单个 JSON 的元数据用于列表显示
    void preloadItemMetadata(QListWidgetItem *item, const QString &jsonPath);
    QFutureWatcher<ImageLoadResult> *imageLoadWatcher;
    QPixmap applyBlurToImage(const QImage &srcImg, const QSize &bgSize, const QSize &heroSize);
    static ImageLoadResult processImageTask(const QString &path);
    QPixmap nextHeroPixmap;         // 即将显示的新 Hero 图
    QPixmap currentBlurredBgPix;    // 当前背景的模糊完成图 (用于过渡缓存)
    QPixmap nextBlurredBgPix;       // 下一张背景的模糊完成图 (用于过渡缓存)
    float transitionOpacity = 0.0;  // 过渡透明度 (0.0 - 1.0)
    QVariantAnimation *transitionAnim = nullptr; // 动画控制器
    void transitionToImage(const QString &path);
    void updateBackgroundDuringTransition();
    void enqueueDownload(const QString &url, const QString &savePath, QPushButton *btn);
    void processNextDownload();

    // 执行排序
    void executeSort();
    // 执行筛选
    void executeFilter();

    QFutureWatcher<QString> *hashWatcher;
    QString currentProcessingPath;

    QThreadPool *threadPool;           //用于详情页、大图 (可被 cancel)
    QThreadPool *backgroundThreadPool; // 【新增】用于侧边栏、主页列表 (不可被 cancel)
    QIcon placeholderIcon;

    QTimer *bgResizeTimer;

    QQueue<DownloadTask> downloadQueue; // 任务队列
    bool isDownloading = false;         // 当前是否有任务在运行


    TagFlowWidget *tagFlowWidget;

    void scanForUserImages(const QString &loraBaseName);
    void parsePngInfo(const QString &path, UserImageInfo &info);
    void updateUserStats(const QList<UserImageInfo> &images);

    QString getSafetensorsInternalName(const QString &path);

    QStringList parsePromptsToTags(const QString &rawPrompt);
    QString cleanTagText(QString t);

    QIcon generatePlaceholderIcon();

    // 定义一个特殊的字符串标识“未分类”
    const QString FILTER_UNCATEGORIZED = "__UNCATEGORIZED__";

    QPixmap applyNSFWBlur(const QPixmap &pix);
    QPixmap applyRoundedMask(const QPixmap &src, int radius);
    QHash<QString, QString> translationMap; // 存储翻译数据
    void loadTranslationCSV(const QString &path); // 加载函数

    QString extractPngParameters(const QString &filePath);

    void refreshCollectionTreeView();
    void filterModelsByCollection(const QString &collectionName);
    void addPlaceholderChild(QTreeWidgetItem *parent);
    void cancelPendingTasks();
    void syncTreeSelection(const QString &filePath);
    void initMenuBar();       // 菜单初始化

    QString currentUserAgent;                           // 当前UA

    // Key: 文件绝对路径, Value: 缓存的图片信息
    QMap<QString, UserImageInfo> imageCache;
    void loadUserGalleryCache();
    void saveUserGalleryCache();

    // === 配置变量 ===
    QString currentLoraPath;                            // LoRA文件夹
    QString translationCsvPath;                         // 翻译文件路径
    QString sdOutputFolder;                             // 图库路径
    QString optSavedUAString        = "";               // 设置的UA
    bool    optLoraRecursive        = false;            // 递归搜索Lora文件夹
    bool    optGalleryRecursive     = false;            // 递归搜索图库文件夹
    int     optBlurRadius           = 30;               // 模糊半径
    bool    optDownscaleBlur        = true;             // 模糊前缩放
    int     optBlurProcessWidth     = 500;              // 默认缩小到 500px
    int     optRenderThreadCount    = 4;                // 图片处理线程数
    bool    optRestoreTreeState     = true;             // 保存菜单状态
    bool    optSplitOnNewline       = true;             // 换行符分割
    bool    optFilterNSFW           = false;            // NSFW过滤
    int     optNSFWMode             = 1;                // 0: 完全隐藏, 1: 高斯模糊
    int     optNSFWLevel            = 1;                // NSFW筛选等级
    bool    optShowEmptyCollections = false;            // 显示空收藏夹
    bool    optUseArrangedUA        = false;            // 使用自定义UA
    QStringList optFilterTags       = DEFAULT_FILTER_TAGS.split(',', Qt::SkipEmptyParts);    // 过滤词列表 (存储清洗后的列表)
    bool    optUseCivitaiName = false;                  // 使用json中的模型名称
    // 保存与加载
    void loadGlobalConfig();        // 加载配置
    void saveGlobalConfig();        // 保存配置
    // 从注册表加载路径
    void loadPathSettings();
    void savePathSettings();
    // 设置辅助变量
    QSet<QString> startupExpandedCollections; // 启动时从文件读出的展开项
    int startupTreeScrollPos;                 // 启动时从文件读出的滚动位置
    bool isFirstTreeRefresh;                  // 标记是否是第一次刷新树（用于判断是否使用缓存）
    // 设置辅助函数
    QString getRandomUserAgent();             // 获取随机 UA
    void updateModelListNames();              // 刷新列表显示名称的辅助函数
};

#endif // MAINWINDOW_H
