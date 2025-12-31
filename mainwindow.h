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

#include "tagflowwidget.h"

const int ROLE_SORT_DATE = Qt::UserRole + 10;      // 存储时间戳 (qint64)
const int ROLE_SORT_DOWNLOADS = Qt::UserRole + 11; // 存储下载量 (int)
const int ROLE_SORT_LIKES = Qt::UserRole + 12;     // 存储点赞量 (int)
const int ROLE_FILTER_BASE = Qt::UserRole + 13;    // 存储底模名称 (QString)

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

private:
    Ui::MainWindow *ui;
    QSettings *settings;
    QString currentLoraPath;
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

    void showCollectionMenu(const QString &modelName, const QPoint &globalPos);

    // 辅助函数：快速读取单个 JSON 的元数据用于列表显示
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

    // 辅助函数：执行排序
    void executeSort();
    // 辅助函数：执行筛选
    void executeFilter();

    QFutureWatcher<QString> *hashWatcher;
    QString currentProcessingPath;

    QThreadPool *threadPool;
    QIcon placeholderIcon;

    QTimer *bgResizeTimer;

    QQueue<DownloadTask> downloadQueue; // 任务队列
    bool isDownloading = false;         // 当前是否有任务在运行


    TagFlowWidget *tagFlowWidget;
    QString sdOutputFolder;
    void scanForUserImages(const QString &loraBaseName);
    void parsePngInfo(const QString &path, UserImageInfo &info);
    void updateUserStats(const QList<UserImageInfo> &images);

    QString getSafetensorsInternalName(const QString &path);

    QStringList parsePromptsToTags(const QString &rawPrompt);
    QString cleanTagText(QString t);

    QIcon generatePlaceholderIcon();

    // === 配置变量 ===
    bool optLoraRecursive = false;    // 默认关闭
    bool optGalleryRecursive = false; // 默认关闭
    int optBlurRadius = 30;           // 默认 30
    bool optDownscaleBlur = true;     // 默认开启缩小
    int optBlurProcessWidth = 500;    // 默认缩小到 500px
    // === 新增函数 ===
    void initMenuBar();       // 重写菜单初始化
    void loadGlobalConfig();  // 加载配置
    void saveGlobalConfig();  // 保存配置

    // 辅助：从注册表加载路径
    void loadPathSettings();
    void savePathSettings();


    // 定义一个特殊的字符串标识“未分类”
    const QString FILTER_UNCATEGORIZED = "__UNCATEGORIZED__";

    bool optFilterNSFW = false;
    int optNSFWMode = 1; // 0: 完全隐藏, 1: 高斯模糊
    int optNSFWLevel = 1;
    QPixmap applyNSFWBlur(const QPixmap &pix);

    QPixmap applyRoundedMask(const QPixmap &src, int radius);

    QHash<QString, QString> translationMap; // 存储翻译数据
    QString translationCsvPath;             // 翻译文件路径
    void loadTranslationCSV(const QString &path); // 加载函数
};

#endif // MAINWINDOW_H
