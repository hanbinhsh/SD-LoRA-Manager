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

const int ROLE_SORT_DATE = Qt::UserRole + 10;      // 存储时间戳 (qint64)
const int ROLE_SORT_DOWNLOADS = Qt::UserRole + 11; // 存储下载量 (int)
const int ROLE_SORT_LIKES = Qt::UserRole + 12;     // 存储点赞量 (int)
const int ROLE_FILTER_BASE = Qt::UserRole + 13;    // 存储底模名称 (QString)

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

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
    int width;
    int height;
    bool nsfw;
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

    void initMenu();
    void loadSettings();

    // 收藏夹 JSON 读写
    void loadCollections();
    void saveCollections();
    void refreshHomeCollectionsUI(); // 刷新主页顶部的按钮
    void refreshHomeGallery(); // 刷新主页下方的图库

    void scanModels(const QString &path);
    void updateDetailView(const ModelMeta &meta);
    void clearDetailView();
    QIcon getSquareIcon(const QString &path);

    // 生成一个适合主页大图的 Icon (2:3比例)
    QIcon getRoundedSquareIcon(const QString &path, int size, int radius);

    QString findLocalPreviewPath(const QString &dirPath, const QString &currentBaseName, const QString &serverFileName, int imgIndex);

    QString calculateFileHash(const QString &filePath);
    void fetchModelInfoFromCivitai(const QString &hash);
    void saveLocalMetadata(const QString &baseName, const QJsonObject &data);
    bool readLocalJson(const QString &baseName, ModelMeta &meta);

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

    // 辅助函数：执行排序
    void executeSort();
    // 辅助函数：执行筛选
    void executeFilter();

    QFutureWatcher<QString> *hashWatcher;
    QString currentProcessingPath;

    QThreadPool *threadPool;
    QIcon placeholderIcon;

    QTimer *bgResizeTimer;
};

#endif // MAINWINDOW_H
