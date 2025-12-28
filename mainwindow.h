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

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

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

    // === 新增槽函数 ===
    void onHomeButtonClicked(); // 切换到主页
    void onHomeGalleryClicked(QListWidgetItem *item); // 主页大图点击跳转
    void onSidebarContextMenu(const QPoint &pos); // 侧边栏右键
    void onCreateCollection(); // 创建新收藏夹按钮点击
    void onCollectionFilterClicked(const QString &collectionName); // 点击收藏夹过滤
    void onIconLoaded(const QString &filePath, const QImage &image);
    void onHashCalculated();

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
    void setHeroImage(const QString &path);
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

    QFutureWatcher<QString> *hashWatcher;
    QString currentProcessingPath;

    QThreadPool *threadPool;
    QIcon placeholderIcon;

    QTimer *bgResizeTimer;
};

#endif // MAINWINDOW_H
