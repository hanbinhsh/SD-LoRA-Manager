#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QDebug>
#include <QClipboard>
#include <QScreen>
#include <QMouseEvent>
#include <QDesktopServices>
#include <QPainter>
#include <QPushButton>
#include <QLabel>
#include <QScrollBar>
#include <QMenu>
#include <QInputDialog>
#include <QJsonArray>
#include <QPainterPath>
#include <QImageReader>
#include <QTimer>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QGraphicsBlurEffect>

#include "imageloader.h"

// 自定义数据 Role
const int ROLE_FILE_PATH = Qt::UserRole + 1;
const int ROLE_PREVIEW_PATH = Qt::UserRole + 2;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    threadPool = new QThreadPool(this);
    threadPool->setMaxThreadCount(4);
    hashWatcher = new QFutureWatcher<QString>(this);
    connect(hashWatcher, &QFutureWatcherBase::finished, this, &MainWindow::onHashCalculated);

    QPixmap pix(180, 180);
    pix.fill(QColor("#25282f"));
    // 可以简单画个圆角
    QPixmap rounded(180, 180);
    rounded.fill(Qt::transparent);
    QPainter p(&rounded);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(0,0,180,180,12,12);
    p.setClipPath(path);
    p.drawPixmap(0,0,pix);
    placeholderIcon = QIcon(rounded);

    settings = new QSettings("MyAiTools", "LoraManager", this);
    netManager = new QNetworkAccessManager(this);

    // 样式设置
    QGraphicsDropShadowEffect *shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(20);
    shadow->setColor(Qt::black);
    shadow->setOffset(0, 0);
    ui->lblModelName->setGraphicsEffect(shadow);

    ui->heroFrame->installEventFilter(this);
    ui->heroFrame->setCursor(Qt::PointingHandCursor);

    initMenu();

    // === 信号连接 ===
    connect(ui->modelList, &QListWidget::itemClicked, this, &MainWindow::onModelListClicked);

    // 侧边栏右键菜单
    ui->modelList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->modelList, &QListWidget::customContextMenuRequested, this, &MainWindow::onSidebarContextMenu);

    connect(ui->btnOpenUrl, &QPushButton::clicked, this, &MainWindow::onOpenUrlClicked);
    connect(ui->btnScanLocal, &QPushButton::clicked, this, &MainWindow::onScanLocalClicked);
    connect(ui->btnForceUpdate, &QPushButton::clicked, this, &MainWindow::onForceUpdateClicked);

    // 主页相关
    connect(ui->btnHome, &QPushButton::clicked, this, &MainWindow::onHomeButtonClicked);
    connect(ui->homeGalleryList, &QListWidget::itemClicked, this, &MainWindow::onHomeGalleryClicked);
    connect(ui->btnAddCollection, &QPushButton::clicked, this, &MainWindow::onCreateCollection);

    // 设置 Splitter
    ui->splitter->setSizes(QList<int>() << 260 << 1000);

    // 默认显示主页 (Page 0)
    ui->mainStack->setCurrentIndex(0);

    bgResizeTimer = new QTimer(this);
    bgResizeTimer->setSingleShot(true); // 只触发一次
    // 当定时器时间到，执行更新背景函数
    connect(bgResizeTimer, &QTimer::timeout, this, &MainWindow::updateBackgroundImage);

    if (ui->backgroundLabel && ui->scrollAreaWidgetContents) {

        ui->scrollAreaWidgetContents->installEventFilter(this);
        ui->backgroundLabel->setScaledContents(true);
        ui->backgroundLabel->setGeometry(ui->scrollAreaWidgetContents->rect());
    }

    clearDetailView();
    loadCollections(); // 加载收藏夹配置
    loadSettings();    // 扫描模型
}

MainWindow::~MainWindow()
{
    threadPool->clear();
    threadPool->waitForDone(500);
    delete ui;
}

// ---------------------------------------------------------
// 主页与收藏夹逻辑
// ---------------------------------------------------------
void MainWindow::onCollectionFilterClicked(const QString &collectionName)
{
    currentCollectionFilter = collectionName;
    refreshHomeGallery();
    refreshHomeCollectionsUI();
}

void MainWindow::onHomeButtonClicked()
{
    ui->mainStack->setCurrentIndex(0); // 切换到主页
    ui->modelList->clearSelection();   // 清除侧边栏选中
    currentCollectionFilter = "";      // 重置过滤，显示全部
    refreshHomeGallery();
    refreshHomeCollectionsUI();
}

void MainWindow::loadCollections()
{
    collections.clear();
    QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);

    QFile file(configDir + "/collections.json");
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        QJsonObject root = doc.object();
        for (auto it = root.begin(); it != root.end(); ++it) {
            QString name = it.key();
            QStringList files;
            for (auto v : it.value().toArray()) files << v.toString();
            collections.insert(name, files);
        }
    }
    refreshHomeCollectionsUI();
}

void MainWindow::saveCollections()
{
    QJsonObject root;
    for (auto it = collections.begin(); it != collections.end(); ++it) {
        QJsonArray arr;
        for (const QString &f : it.value()) arr.append(f);
        root.insert(it.key(), arr);
    }

    QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);
    QFile file(configDir + "/collections.json");
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson());
    }
    refreshHomeCollectionsUI();
}

void MainWindow::onCreateCollection()
{
    bool ok;
    QString text = QInputDialog::getText(this, "新建收藏夹", "收藏夹名称:", QLineEdit::Normal, "", &ok);
    if (ok && !text.trimmed().isEmpty()) {
        if (!collections.contains(text)) {
            collections.insert(text, QStringList());
            saveCollections();
        }
    }
}

void MainWindow::refreshHomeCollectionsUI()
{
    // 清除旧按钮 (保留第一个新建按钮)
    QLayout *layout = ui->scrollAreaCollections->widget()->layout();
    QLayoutItem *item;
    while (layout->count() > 1) {
        item = layout->takeAt(1);
        if (item->widget()) delete item->widget();
        delete item;
    }

    // === 1. 修改新建按钮样式 ===
    ui->btnAddCollection->setFixedSize(90, 90);
    ui->btnAddCollection->setProperty("class", "collectionBtn");
    ui->btnAddCollection->setText("+\nNew");

    // === 2. 添加 "全部" 按钮 ===
    QPushButton *btnAll = new QPushButton("ALL\n全部");
    btnAll->setFixedSize(90, 90);
    btnAll->setProperty("class", "collectionBtn");
    btnAll->setCheckable(true);
    btnAll->setChecked(currentCollectionFilter.isEmpty());
    btnAll->setCursor(Qt::PointingHandCursor);

    connect(btnAll, &QPushButton::clicked, this, [this](){
        onCollectionFilterClicked("");
    });
    layout->addWidget(btnAll);

    // === 3. 添加收藏夹按钮 (带右键功能) ===
    for (auto it = collections.begin(); it != collections.end(); ++it) {
        QString name = it.key();

        // 名字截断
        QString displayName = name;
        if (displayName.length() > 8) displayName = displayName.left(6) + "..";

        QPushButton *btn = new QPushButton(displayName);
        btn->setFixedSize(90, 90);
        btn->setProperty("class", "collectionBtn");
        btn->setCheckable(true);
        btn->setChecked(currentCollectionFilter == name);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setToolTip(name);

        // 左键点击：筛选
        connect(btn, &QPushButton::clicked, this, [this, name](){
            onCollectionFilterClicked(name);
        });

        // === 右键菜单逻辑 ===
        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(btn, &QPushButton::customContextMenuRequested, this, [this, btn, name](const QPoint &pos){
            QMenu menu;

            QAction *actRename = menu.addAction("重命名 / Rename");
            QAction *actDelete = menu.addAction("删除 / Delete");

            QAction *selected = menu.exec(btn->mapToGlobal(pos));

            if (selected == actRename) {
                bool ok;
                QString newName = QInputDialog::getText(this, "重命名收藏夹", "新名称:", QLineEdit::Normal, name, &ok);
                if (ok && !newName.trimmed().isEmpty() && newName != name) {
                    if (collections.contains(newName)) {
                        QMessageBox::warning(this, "错误", "该名称已存在！");
                        return;
                    }
                    // 执行重命名：取出旧值，插入新键，删除旧键
                    QStringList files = collections.value(name);
                    collections.insert(newName, files);
                    collections.remove(name);

                    // 如果当前正选着这个收藏夹，更新过滤名
                    if (currentCollectionFilter == name) currentCollectionFilter = newName;

                    saveCollections(); // 保存并刷新UI
                }
            }
            else if (selected == actDelete) {
                auto reply = QMessageBox::question(this, "确认删除",
                                                   QString("确定要删除收藏夹 \"%1\" 吗？\n(模型文件本身不会被删除)").arg(name),
                                                   QMessageBox::Yes | QMessageBox::No);
                if (reply == QMessageBox::Yes) {
                    collections.remove(name);
                    if (currentCollectionFilter == name) currentCollectionFilter = ""; // 重置为全部
                    saveCollections(); // 保存并刷新UI
                }
            }
        });

        layout->addWidget(btn);
    }

    ((QHBoxLayout*)layout)->addStretch();
}

void MainWindow::refreshHomeGallery()
{
    ui->homeGalleryList->clear();

    // 1. 设置图标大小 (正方形)
    int iconSize = 180;
    ui->homeGalleryList->setIconSize(QSize(iconSize, iconSize));

    // 2. 设置网格大小 (正方形)
    // 既然没有文字了，高度不需要留空，设为 200x200 足够容纳 180 的图标加一点边距
    ui->homeGalleryList->setGridSize(QSize(200, 200));

    // 3. 布局模式
    ui->homeGalleryList->setViewMode(QListWidget::IconMode);
    ui->homeGalleryList->setResizeMode(QListWidget::Adjust);
    ui->homeGalleryList->setSpacing(10);
    // 禁用拖拽，防止意外移动
    ui->homeGalleryList->setMovement(QListView::Static);

    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *sideItem = ui->modelList->item(i);
        QString baseName = sideItem->text();
        QString previewPath = sideItem->data(ROLE_PREVIEW_PATH).toString();
        QString filePath = sideItem->data(ROLE_FILE_PATH).toString();

        if (!currentCollectionFilter.isEmpty()) {
            QStringList list = collections.value(currentCollectionFilter);
            if (!list.contains(baseName)) continue;
        }

        QListWidgetItem *item = new QListWidgetItem();
        item->setToolTip(baseName);
        item->setData(ROLE_FILE_PATH, filePath);
        item->setData(ROLE_PREVIEW_PATH, previewPath);

        item->setIcon(placeholderIcon);
        ui->homeGalleryList->addItem(item);

        // === 优化点：如果有图，启动后台加载 ===
        if (!filePath.isEmpty()) {
            QString pathToSend = previewPath.isEmpty() ? "invalid_path" : previewPath;

            IconLoaderTask *task = new IconLoaderTask(pathToSend, iconSize, 12, this, filePath);
            task->setAutoDelete(true);
            threadPool->start(task);
        }
    }
}

// 点击主页的大图 -> 跳转详情页
void MainWindow::onHomeGalleryClicked(QListWidgetItem *item)
{
    if (!item) return;

    // 1. 获取点击项的文件路径 (这是最可靠的唯一标识)
    QString targetPath = item->data(ROLE_FILE_PATH).toString();
    if (targetPath.isEmpty()) return;

    // 2. 在侧边栏 (modelList) 中寻找匹配该路径的项
    QListWidgetItem* matchItem = nullptr;
    for(int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem* sideItem = ui->modelList->item(i);
        if (sideItem->data(ROLE_FILE_PATH).toString() == targetPath) {
            matchItem = sideItem;
            break;
        }
    }

    // 3. 如果找到了，选中它并触发加载逻辑
    if (matchItem) {
        ui->modelList->setCurrentItem(matchItem);
        // 手动调用点击事件，让详情页加载数据
        onModelListClicked(matchItem);

        // 4. 切换到详情页 (Page 2)
        ui->mainStack->setCurrentIndex(1);
    } else {
        // 理论上不会发生，除非侧边栏被清空了
        qDebug() << "Error: Model not found in sidebar list.";
    }
}

// 侧边栏右键菜单
void MainWindow::onSidebarContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = ui->modelList->itemAt(pos);
    if (!item) return;

    QString baseName = item->text();

    QMenu menu(this);
    QMenu * subMenu = menu.addMenu("添加至收藏夹...");

    // 列出所有收藏夹
    for (auto it = collections.begin(); it != collections.end(); ++it) {
        QString colName = it.key();
        QAction *action = subMenu->addAction(colName);
        action->setCheckable(true);
        action->setChecked(it.value().contains(baseName));

        connect(action, &QAction::triggered, this, [this, colName, baseName, action](){
            if (action->isChecked()) {
                if (!collections[colName].contains(baseName))
                    collections[colName].append(baseName);
            } else {
                collections[colName].removeAll(baseName);
            }
            saveCollections();
        });
    }

    subMenu->addSeparator();
    QAction *newAction = subMenu->addAction("新建收藏夹...");
    connect(newAction, &QAction::triggered, this, [this, baseName](){
        bool ok;
        QString text = QInputDialog::getText(this, "新建", "名称:", QLineEdit::Normal, "", &ok);
        if(ok && !text.isEmpty()) {
            if(!collections.contains(text)) {
                collections[text] = QStringList() << baseName;
                saveCollections();
            }
        }
    });

    menu.exec(ui->modelList->mapToGlobal(pos));
}

// 生成竖版封面图标 (2:3)
QIcon MainWindow::getRoundedSquareIcon(const QString &path, int size, int radius)
{
    // 1. 创建高分屏画布 (size x size)
    QPixmap finalPix(size, size);
    finalPix.fill(Qt::transparent); // 必须透明底，否则四个角是黑/白的

    QPainter painter(&finalPix);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // 2. 定义圆角路径
    QPainterPath pathObj;
    pathObj.addRoundedRect(0, 0, size, size, radius, radius);

    // 3. 设置裁剪 (所有后续绘制都会限制在这个圆角框内)
    painter.setClipPath(pathObj);

    QPixmap srcPix(path);

    // === 情况 A: 没有图片 (绘制占位符) ===
    if (srcPix.isNull()) {
        // 填充背景色 (深灰)
        painter.fillRect(QRect(0, 0, size, size), QColor("#25282f"));

        // 画边框 (可选，增加质感)
        QPen pen(QColor("#3d4450"));
        pen.setWidth(2);
        painter.setPen(pen);
        painter.drawRoundedRect(1, 1, size-2, size-2, radius, radius);

        // 画文字 "No Image"
        painter.setPen(QColor("#5a6f8a"));
        QFont f = painter.font();
        f.setPixelSize(size / 5); // 动态字体大小
        f.setBold(true);
        painter.setFont(f);
        painter.drawText(QRect(0, 0, size, size), Qt::AlignCenter, "No\nImage");
    }
    // === 情况 B: 有图片 (裁剪+缩放) ===
    else {
        // 计算短边裁剪 (Smart Crop: 居中 + 顶端对齐)
        int side = qMin(srcPix.width(), srcPix.height());
        int x = (srcPix.width() - side) / 2;
        int y = 0; // 顶端对齐

        // 裁剪出正方形
        QPixmap square = srcPix.copy(x, y, side, side);

        // 缩放到目标大小
        QPixmap scaled = square.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);

        // 绘制 (会被限制在 setClipPath 定义的圆角内)
        painter.drawPixmap(0, 0, scaled);

        // (可选) 可以在图片上画一圈细边框，防止图片和背景融为一体
        QPen pen(QColor(255,255,255, 30));
        pen.setWidth(2);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(1, 1, size-2, size-2, radius, radius);
    }

    return QIcon(finalPix);
}

// ---------------------------------------------------------
// 主页与收藏夹逻辑结束
// ---------------------------------------------------------

// === 辅助：生成正方形图标 ===
QIcon MainWindow::getSquareIcon(const QString &path)
{
    QPixmap pix(path);
    if (pix.isNull()) return QIcon();

    // 1. 计算裁剪区域 (短边裁剪)
    int side = qMin(pix.width(), pix.height());
    // X轴居中，Y轴顶端对齐 (适合人物)
    int x = (pix.width() - side) / 2;
    int y = 0;

    // 获取原始的正方形裁剪图
    QPixmap square = pix.copy(x, y, side, side);

    // 2. === 核心修改：增加透明内边距 ===
    // 设定输出图标的基础分辨率 (越高越清晰，64x64 对侧边栏足够)
    int fullSize = 64;

    // 设定内边距 (比如 8px，意味着图片四周都有 8px 的透明区域)
    // 这样图片实际显示大小就是 48x48，视觉上就分开了
    int padding = 8;
    int contentSize = fullSize - (padding * 2);

    // 创建透明底图
    QPixmap finalPix(fullSize, fullSize);
    finalPix.fill(Qt::transparent);

    QPainter painter(&finalPix);
    // 开启高质量抗锯齿
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // 将裁剪好的图缩放并画在中间
    painter.drawPixmap(padding, padding,
                       square.scaled(contentSize, contentSize,
                                     Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation));

    return QIcon(finalPix);
}

// === 核心：设置 Hero 图片 (只加载数据) ===
void MainWindow::setHeroImage(const QString &path)
{
    currentHeroPath = path;

    if (path.isEmpty() || !QFile::exists(path)) {
        currentHeroPixmap = QPixmap();
    } else {
        currentHeroPixmap.load(path);
    }

    ui->heroFrame->update();

    // === 更新背景 ===
    updateBackgroundImage();
}

// === 核心：事件过滤器 (绘图 + 点击) ===
bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->heroFrame) {

        // --- 处理绘图 (Paint) ---
        if (event->type() == QEvent::Paint) {
            QPainter painter(ui->heroFrame);

            if (currentHeroPixmap.isNull()) {
                painter.fillRect(ui->heroFrame->rect(), Qt::black);
                return true;
            }

            // 智能裁剪算法 (Cover 模式)
            QSize widgetSize = ui->heroFrame->size();
            QSize imgSize = currentHeroPixmap.size();

            if (imgSize.isEmpty()) return true;

            // 计算缩放比例
            double scaleW = (double)widgetSize.width() / imgSize.width();
            double scaleH = (double)widgetSize.height() / imgSize.height();
            double scale = qMax(scaleW, scaleH);

            double newW = imgSize.width() * scale;
            double newH = imgSize.height() * scale;

            // 居中/顶端对齐
            double offsetX = (widgetSize.width() - newW) / 2.0;
            double offsetY = (widgetSize.height() - newH) / 4.0;

            // 绘制
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.drawPixmap(QRectF(offsetX, offsetY, newW, newH), currentHeroPixmap, currentHeroPixmap.rect());

            return true;
        }

        // --- 处理点击 (查看大图) ---
        if (event->type() == QEvent::MouseButtonPress) {
            QMouseEvent *mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                if (!currentHeroPath.isEmpty() && QFile::exists(currentHeroPath)) {
                    showFullImageDialog(currentHeroPath); // 使用新封装的函数
                    return true;
                }
            }
        }
    }

    if (watched == ui->scrollAreaWidgetContents && event->type() == QEvent::Resize) {
        if (ui->backgroundLabel) {
            // 让背景铺满整个滚动内容区域
            ui->backgroundLabel->setGeometry(ui->scrollAreaWidgetContents->rect());

            // 启动防抖
            bgResizeTimer->start(150);
        }
    }

    if (event->type() == QEvent::MouseButtonDblClick) {
        // 尝试将 watched 对象转换为 QPushButton
        QPushButton *btn = qobject_cast<QPushButton*>(watched);
        if (btn) {
            // 获取我们之前绑定的 fullImagePath 属性
            QString path = btn->property("fullImagePath").toString();
            if (!path.isEmpty() && QFile::exists(path)) {
                showFullImageDialog(path); // 打开大图
                return true; // 消费事件
            }
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

// ---------------------------------------------------------
// 业务逻辑
// ---------------------------------------------------------

void MainWindow::scanModels(const QString &path)
{
    ui->modelList->clear();
    QDir dir(path);
    QStringList filters;
    filters << "*.safetensors" << "*.pt";
    dir.setNameFilters(filters);

    QFileInfoList fileList = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    ui->statusbar->showMessage(QString("扫描完成，共 %1 个模型").arg(fileList.count()));

    for (const QFileInfo &fileInfo : fileList) {
        QString baseName = fileInfo.completeBaseName();
        QString fullPath = fileInfo.absoluteFilePath();
        QString previewPath = "";

        QStringList imgExts = {".preview.png", ".png", ".jpg", ".jpeg"};
        for (const QString &ext : imgExts) {
            QString tryPath = dir.absoluteFilePath(baseName + ext);
            if (QFile::exists(tryPath)) {
                previewPath = tryPath;
                break;
            }
        }

        QListWidgetItem *item = new QListWidgetItem(baseName);
        item->setToolTip(fullPath);
        item->setData(ROLE_FILE_PATH, fullPath);
        item->setData(ROLE_PREVIEW_PATH, previewPath);

        // 设置图标
        if (!previewPath.isEmpty()) {
            item->setIcon(getSquareIcon(previewPath));
        }

        ui->modelList->addItem(item);
    }
    refreshHomeGallery();
}

// 更新界面显示
void MainWindow::updateDetailView(const ModelMeta &meta)
{
    // 1. 基础信息
    ui->lblModelName->setText(meta.name);
    ui->heroFrame->setProperty("fullImagePath", meta.previewPath);

    if (!meta.modelUrl.isEmpty()) {
        ui->btnOpenUrl->setVisible(true);
        ui->btnOpenUrl->setProperty("url", meta.modelUrl);
    } else { ui->btnOpenUrl->setVisible(false); }

    // 2. 标签栏 (Badges)
    clearLayout(ui->badgesFrame->layout());

    if (meta.nsfw) addBadge("NSFW", true);
    if (!meta.baseModel.isEmpty()) addBadge(meta.baseModel);
    if (!meta.type.isEmpty()) addBadge(meta.type);
    if (meta.fileSizeMB > 0) addBadge(QString("%1 MB").arg(meta.fileSizeMB, 0, 'f', 1));

    if (!meta.createdAt.isEmpty()) {
        QDateTime dt = QDateTime::fromString(meta.createdAt, Qt::ISODate);
        if (dt.isValid()) {
            addBadge("📅 " + dt.toString("yyyy-MM-dd"));
        }
    }

    if (meta.downloadCount > 0) {
        QString dlStr = (meta.downloadCount > 1000) ? QString::number(meta.downloadCount/1000.0, 'f', 1)+"k" : QString::number(meta.downloadCount);
        addBadge(QString("⇩ %1").arg(dlStr));
    }
    if (meta.thumbsUpCount > 0) addBadge(QString("👍 %1").arg(meta.thumbsUpCount));

    ((QHBoxLayout*)ui->badgesFrame->layout())->addStretch(); // 左对齐

    // 3. 动态生成触发词框 (Trigger Words)
    clearLayout(ui->layoutTriggerStack);

    if (meta.trainedWordsGroups.isEmpty()) {
        QLabel *lbl = new QLabel("No trigger words provided.");
        lbl->setStyleSheet("color: #666; font-style: italic; margin-left: 10px;");
        ui->layoutTriggerStack->addWidget(lbl);
    } else {
        for (const QString &words : meta.trainedWordsGroups) {
            // 创建容器：[文本框] [复制按钮]
            QWidget *rowWidget = new QWidget();
            QHBoxLayout *rowLayout = new QHBoxLayout(rowWidget);
            rowLayout->setContentsMargins(0,0,0,10);
            rowLayout->setSpacing(5);

            QTextBrowser *tb = new QTextBrowser();
            tb->setText(words);
            tb->setFixedHeight(90);

            QPushButton *btnCopy = new QPushButton("Copy");
            btnCopy->setFixedSize(60, 90);
            btnCopy->setCursor(Qt::PointingHandCursor);
            btnCopy->setProperty("class", "copyBtn"); // 应用 XML 里的 QSS

            connect(btnCopy, &QPushButton::clicked, this, [words, this](){
                QClipboard *clip = QGuiApplication::clipboard();
                clip->setText(words);
                ui->statusbar->showMessage("Copied trigger words!", 1500);
            });

            rowLayout->addWidget(tb);
            rowLayout->addWidget(btnCopy);

            ui->layoutTriggerStack->addWidget(rowWidget);
        }
    }

    // 4. 图库 (Gallery)
    clearLayout(ui->layoutGallery);

    if (meta.images.isEmpty()) {
        ui->layoutGallery->addWidget(new QLabel("No preview images."));
    } else {
        QString currentBaseName = ui->modelList->currentItem() ? ui->modelList->currentItem()->text() : meta.name;
        // 获取当前模型的基础文件名 (用于拼接图片路径)
        QString baseName = ui->modelList->currentItem() ? ui->modelList->currentItem()->text() : meta.name;
        QFileInfo fi(meta.filePath);
        QString safeBaseName = fi.completeBaseName();
        if (safeBaseName.isEmpty()) safeBaseName = meta.name;

        for (int i = 0; i < meta.images.count(); ++i) {
            const ImageInfo &img = meta.images[i];

            QPushButton *thumbBtn = new QPushButton();
            thumbBtn->setFixedSize(100, 150);
            thumbBtn->setCheckable(true);
            thumbBtn->setAutoExclusive(true);
            thumbBtn->setCursor(Qt::PointingHandCursor);

            // 样式优化：加上 padding 让图片看起来不像贴在边上
            thumbBtn->setProperty("class", "galleryThumb");

            // 计算路径
            QString imgFileName;
            if (i == 0) imgFileName = safeBaseName + ".preview.png";
            else imgFileName = safeBaseName + QString(".preview.%1.png").arg(i);

            QString localPath = findLocalPreviewPath(currentLoraPath, currentBaseName, meta.fileNameServer, i);

            // === 关键修改 1: 存储全路径到 Property (供双击事件使用) ===
            thumbBtn->setProperty("fullImagePath", localPath);

            // === 关键修改 2: 安装事件过滤器 (监听双击) ===
            thumbBtn->installEventFilter(this);

            if (QFile::exists(localPath)) {
                // 1. 先设置一个空的或者占位图标 (避免界面跳动)
                // 这里可以直接用你的 placeholderIcon (如果是正方形可能会拉伸，最好搞个长方形的占位)
                // 或者暂时留空，等待回调
                thumbBtn->setText("Loading...");

                // 2. 启动异步加载 (Fit模式)
                // 参数: 路径, 尺寸(虽然Fit模式内部定死100x150, 但传个占位), 圆角, 接收者, ID, isFitMode=true
                IconLoaderTask *task = new IconLoaderTask(localPath, 100, 0, this, localPath, true);
                task->setAutoDelete(true);
                threadPool->start(task);

            } else {
                thumbBtn->setText("Downloading...");
                downloadThumbnail(img.url, localPath, thumbBtn);
            }

            // 单击事件 (查看参数 & 预览)
            connect(thumbBtn, &QPushButton::clicked, this, [this, i](){
                onGalleryImageClicked(i);
            });

            ui->layoutGallery->addWidget(thumbBtn);
        }
        ui->layoutGallery->addStretch();

        // 默认选中第一张
        if (ui->layoutGallery->count() > 0) {
            QPushButton *firstBtn = qobject_cast<QPushButton*>(ui->layoutGallery->itemAt(0)->widget());
            if (firstBtn) {
                firstBtn->setChecked(true);
                onGalleryImageClicked(0);
            }
        }
    }

    // 5. 右侧信息
    ui->textDescription->setHtml(meta.description);
    ui->lblFileInfo->setText(QString("Filename: %1\nSize: %2 MB\nSHA256: %3")
                                 .arg(meta.fileNameServer.isEmpty() ? meta.fileName : meta.fileNameServer)
                                 .arg(meta.fileSizeMB, 0, 'f', 1)
                                 .arg(meta.sha256.left(10) + "..."));

    QTimer::singleShot(10, this, [this, meta](){
        ui->scrollAreaWidgetContents->adjustSize();
        setHeroImage(meta.previewPath);
    });
}

void MainWindow::onGalleryImageClicked(int index)
{
    if (index < 0 || index >= currentMeta.images.count()) return;

    const ImageInfo &img = currentMeta.images[index];

    // 更新 Prompt 显示
    ui->textImgPrompt->setText(img.prompt.isEmpty() ? "No positive prompt." : img.prompt);
    ui->textImgNegPrompt->setText(img.negativePrompt.isEmpty() ? "No negative prompt." : img.negativePrompt);

    // 更新参数行
    QString params = QString("Sampler: <span style='color:white'>%1</span> | Steps: <span style='color:white'>%2</span> | CFG: <span style='color:white'>%3</span> | Seed: <span style='color:white'>%4</span>")
                         .arg(img.sampler)
                         .arg(img.steps)
                         .arg(img.cfgScale)
                         .arg(img.seed);
    ui->lblImgParams->setText(params);

    // 如果选中的是封面(第0张)，且本地有图，同步更新大图背景
    if (index == 0 && !currentMeta.previewPath.isEmpty()) {
        setHeroImage(currentMeta.previewPath);
    }
}

// 辅助函数
void MainWindow::addBadge(QString text, bool isRed)
{
    QLabel *lbl = new QLabel(text);
    lbl->setProperty("class", isRed ? "tagRed" : "tag");
    ui->badgesFrame->layout()->addWidget(lbl);
}

void MainWindow::clearLayout(QLayout *layout)
{
    if (!layout) return;
    QLayoutItem *item;
    while ((item = layout->takeAt(0)) != nullptr) {
        if (item->widget()) delete item->widget();
        if (item->layout()) clearLayout(item->layout());
        delete item;
    }
}

void MainWindow::clearDetailView()
{
    ui->lblModelName->setText("请选择一个模型 / Select a Model");
    ui->textDescription->clear();
    ui->textDescription->setPlaceholderText("暂无简介 / No description.");
    ui->lblFileInfo->setText("Filename: --\nSize: --\nHash: --");

    ui->textImgPrompt->clear();
    ui->textImgNegPrompt->clear();
    ui->lblImgParams->setText("Sampler: -- | Steps: -- | CFG: -- | Seed: --");

    ui->btnOpenUrl->setVisible(false);

    clearLayout(ui->badgesFrame->layout());
    clearLayout(ui->layoutTriggerStack);
    clearLayout(ui->layoutGallery);

    setHeroImage("");
    ui->heroFrame->setProperty("fullImagePath", "");
}

// ---------------------------------------------------------
// 文件与网络部分
// ---------------------------------------------------------

void MainWindow::initMenu() {
    QMenu *fileMenu = menuBar()->addMenu("文件(&F)");
    QAction *openAction = new QAction("选择模型文件夹...", this);
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::onActionOpenFolderTriggered);
    fileMenu->addAction(openAction);
}

void MainWindow::loadSettings() {
    QString lastPath = settings->value("lora_path").toString();
    if (!lastPath.isEmpty() && QDir(lastPath).exists()) {
        currentLoraPath = lastPath;
        scanModels(currentLoraPath);
    }
}

void MainWindow::onActionOpenFolderTriggered() {
    QString dir = QFileDialog::getExistingDirectory(this, "选择 LoRA 文件夹", currentLoraPath);
    if (!dir.isEmpty()) {
        currentLoraPath = dir;
        settings->setValue("lora_path", currentLoraPath);
        scanModels(currentLoraPath);
    }
}

void MainWindow::onScanLocalClicked() {
    if (!currentLoraPath.isEmpty()) scanModels(currentLoraPath);
}

// 点击列表项
void MainWindow::onModelListClicked(QListWidgetItem *item) {
    if (!item) return;

    // 1. 如果正在计算上一个，先取消或忽略
    if (hashWatcher->isRunning()) {
        // 简单处理：提示用户稍等，或者强制让 UI 变动
        // 更好的做法是 cancel，但 SHA 计算很难中途 cancel，所以我们用标志位判断
    }

    ui->mainStack->setCurrentIndex(1); // 进详情页
    clearDetailView(); // 清空旧数据

    QString filePath = item->data(ROLE_FILE_PATH).toString();
    QString previewPath = item->data(ROLE_PREVIEW_PATH).toString();
    QString baseName = item->text();

    ModelMeta meta;
    meta.name = baseName;
    meta.filePath = filePath;
    meta.previewPath = previewPath;

    // 2. 尝试读取本地 JSON
    bool hasLocalData = readLocalJson(baseName, meta);

    if (hasLocalData) {
        // === 情况 A: 有本地数据，直接显示 (秒开) ===
        currentMeta = meta;
        updateDetailView(meta);
    } else {
        // === 情况 B: 无本地数据，需要计算 Hash 然后联网 ===

        // UI 状态反馈：显示“正在分析模型...”
        ui->lblModelName->setText("正在分析模型文件 (计算 Hash)...");
        ui->btnForceUpdate->setEnabled(false);

        // 记录当前正在处理的文件，防止回调时错位
        currentProcessingPath = filePath;
        ui->modelList->setProperty("current_processing_file", baseName);

        // === 启动后台线程计算 Hash ===
        // 使用 QtConcurrent::run 把耗时函数丢到后台
        QFuture<QString> future = QtConcurrent::run([this, filePath]() {
            return calculateFileHash(filePath); // 这里是你原来的耗时函数
        });
        hashWatcher->setFuture(future);
    }
}

// 强制联网
void MainWindow::onForceUpdateClicked() {
    QListWidgetItem *item = ui->modelList->currentItem();
    if (!item) return;

    ui->statusbar->showMessage("正在连接 Civitai 获取元数据...");
    ui->btnForceUpdate->setEnabled(false);

    QString baseName = item->text();
    QString filePath = item->data(ROLE_FILE_PATH).toString();

    QString hash = calculateFileHash(filePath);
    if (hash.isEmpty()) {
        ui->statusbar->showMessage("错误: 无法计算文件哈希");
        ui->btnForceUpdate->setEnabled(true);
        return;
    }
    ui->modelList->setProperty("current_processing_file", baseName);
    fetchModelInfoFromCivitai(hash);
}

void MainWindow::fetchModelInfoFromCivitai(const QString &hash) {
    QString urlStr = QString("https://civitai.com/api/v1/model-versions/by-hash/%1").arg(hash);
    QNetworkRequest request((QUrl(urlStr)));
    request.setHeader(QNetworkRequest::UserAgentHeader, "MyLoraManager/1.0");
    QNetworkReply *reply = netManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply](){
        this->onApiMetadataReceived(reply);
    });
}

// 解析 JSON
bool MainWindow::readLocalJson(const QString &baseName, ModelMeta &meta)
{
    if (currentLoraPath.isEmpty()) return false;
    QString jsonPath = QDir(currentLoraPath).filePath(baseName + ".json");
    QFile file(jsonPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) return false;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    QJsonObject root = doc.object();

    // 1. 基础名称
    QString modelName = root["model"].toObject()["name"].toString();
    QString versionName = root["name"].toString();
    if (!modelName.isEmpty()) meta.name = modelName + " [" + versionName + "]";

    // ID (用于打开网页)
    int modelId = root["modelId"].toInt();
    if (modelId > 0) {
        meta.modelUrl = QString("https://civitai.com/models/%1").arg(modelId);
    }

    // 2. 解析触发词组
    QJsonArray twArray = root["trainedWords"].toArray();
    for(auto val : twArray) {
        QString w = val.toString().trimmed();
        if(w.endsWith(",")) w.chop(1);
        if(!w.isEmpty()) meta.trainedWordsGroups.append(w);
    }

    // 3. 解析图片 (补全了 width, height, nsfw 的读取)
    QJsonArray images = root["images"].toArray();
    for (auto val : images) {
        QJsonObject imgObj = val.toObject();
        QString type = imgObj["type"].toString();
        QString url = imgObj["url"].toString();
        if (type == "video" || url.endsWith(".mp4", Qt::CaseInsensitive) || url.endsWith(".webm", Qt::CaseInsensitive)) {
            continue; // 跳过，不加入列表
        }
        ImageInfo imgInfo;
        imgInfo.url = imgObj["url"].toString();
        imgInfo.hash = imgObj["hash"].toString();
        imgInfo.width = imgObj["width"].toInt();       // 补全
        imgInfo.height = imgObj["height"].toInt();     // 补全
        imgInfo.nsfw = (imgObj["nsfwLevel"].toInt() > 1); // 补全

        QJsonObject imgMeta = imgObj["meta"].toObject();
        if(!imgMeta.isEmpty()) {
            imgInfo.prompt = imgMeta["prompt"].toString();
            imgInfo.negativePrompt = imgMeta["negativePrompt"].toString();
            imgInfo.sampler = imgMeta["sampler"].toString();
            imgInfo.steps = QString::number(imgMeta["steps"].toInt());
            imgInfo.cfgScale = QString::number(imgMeta["cfgScale"].toDouble());
            imgInfo.seed = QString::number(imgMeta["seed"].toVariant().toLongLong());
        }
        meta.images.append(imgInfo);
    }

    // 4. 其他信息 (之前漏掉了 createdAt)
    meta.description = root["description"].toString();
    meta.baseModel = root["baseModel"].toString();
    meta.type = root["model"].toObject()["type"].toString();
    meta.nsfw = root["model"].toObject()["nsfw"].toBool();

    // === 关键修复：补上日期读取 ===
    meta.createdAt = root["createdAt"].toString();
    // ===========================

    QJsonObject stats = root["stats"].toObject();
    meta.downloadCount = stats["downloadCount"].toInt();
    meta.thumbsUpCount = stats["thumbsUpCount"].toInt();

    QJsonArray files = root["files"].toArray();
    if(!files.isEmpty()) {
        // 通常取第一个文件信息
        QJsonObject f = files[0].toObject();
        meta.fileSizeMB = f["sizeKB"].toDouble() / 1024.0;
        meta.fileNameServer = f["name"].toString();
        meta.sha256 = f["hashes"].toObject()["SHA256"].toString();
    }

    QString bestPreviewPath = findLocalPreviewPath(currentLoraPath, baseName, meta.fileNameServer, 0);

    if (QFile::exists(bestPreviewPath)) {
        QImageReader reader(bestPreviewPath);
        if (reader.canRead()) {
            meta.previewPath = bestPreviewPath;
        } else {
            meta.previewPath = ""; // 文件坏了或不是图片
        }
    } else {
        meta.previewPath = ""; // 没找到文件
    }

    currentMeta = meta;
    return true;
}

// 联网回调
void MainWindow::onApiMetadataReceived(QNetworkReply *reply)
{
    reply->deleteLater();
    ui->btnForceUpdate->setEnabled(true);
    if (reply->error() != QNetworkReply::NoError) {
        // 使用动态添加的Label显示错误，而非 textTriggerWords
        clearLayout(ui->layoutTriggerStack);
        QLabel* err = new QLabel("查询失败: " + reply->errorString());
        err->setStyleSheet("color: red;");
        ui->layoutTriggerStack->addWidget(err);
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonObject root = doc.object();
    ModelMeta meta;

    // 1. 基础信息
    QString modelRealName = root["model"].toObject()["name"].toString();
    QString versionName = root["name"].toString();
    meta.name = modelRealName + " [" + versionName + "]";

    // 2. 触发词 (保存为列表)
    meta.trainedWordsGroups.clear();
    QJsonArray twArray = root["trainedWords"].toArray();
    for(auto val : twArray) {
        QString w = val.toString().trimmed();
        if(w.endsWith(",")) w.chop(1);
        if(!w.isEmpty()) meta.trainedWordsGroups.append(w);
    }

    int modelId = root["modelId"].toInt();
    if (modelId > 0) meta.modelUrl = QString("https://civitai.com/models/%1").arg(modelId);

    meta.baseModel = root["baseModel"].toString();
    meta.type = root["model"].toObject()["type"].toString();
    meta.nsfw = root["model"].toObject()["nsfw"].toBool();
    meta.description = root["description"].toString();
    meta.createdAt = root["createdAt"].toString();

    QJsonObject stats = root["stats"].toObject();
    meta.downloadCount = stats["downloadCount"].toInt();
    meta.thumbsUpCount = stats["thumbsUpCount"].toInt();

    // 3. 文件信息 (计算大小, Hash)
    QJsonArray files = root["files"].toArray();
    if (!files.isEmpty()) {
        QJsonObject f = files[0].toObject(); // 默认取第一个
        meta.fileSizeMB = f["sizeKB"].toDouble() / 1024.0;
        meta.sha256 = f["hashes"].toObject()["SHA256"].toString();
        meta.fileNameServer = f["name"].toString();
    }

    // 4. 图片信息 (非常重要)
    QJsonArray images = root["images"].toArray();
    for (auto val : images) {
        QJsonObject imgObj = val.toObject();
        QString type = imgObj["type"].toString();
        QString url = imgObj["url"].toString();
        if (type == "video" || url.endsWith(".mp4", Qt::CaseInsensitive) || url.endsWith(".webm", Qt::CaseInsensitive)) {
            continue; // 跳过视频，不加入列表
        }

        ImageInfo imgInfo;
        imgInfo.url = imgObj["url"].toString();
        imgInfo.hash = imgObj["hash"].toString(); // blurhash
        imgInfo.width = imgObj["width"].toInt();
        imgInfo.height = imgObj["height"].toInt();
        imgInfo.nsfw = (imgObj["nsfwLevel"].toInt() > 1); // 简单判断

        QJsonObject imgMeta = imgObj["meta"].toObject();
        if (!imgMeta.isEmpty()) {
            imgInfo.prompt = imgMeta["prompt"].toString();
            imgInfo.negativePrompt = imgMeta["negativePrompt"].toString();
            imgInfo.sampler = imgMeta["sampler"].toString();
            imgInfo.steps = QString::number(imgMeta["steps"].toInt());
            imgInfo.cfgScale = QString::number(imgMeta["cfgScale"].toDouble());
            imgInfo.seed = QString::number(imgMeta["seed"].toVariant().toLongLong());
        }
        meta.images.append(imgInfo);
    }

    // 优先用第一张图做封面，如果本地没有下载，就下载
    if (!meta.images.isEmpty()) {
        QString baseName = ui->modelList->property("current_processing_file").toString();
        QString savePath = QDir(currentLoraPath).filePath(baseName + ".preview.png");

        // 如果本地已存在，直接用；否则下载第一张
        if (!QFile::exists(savePath)) {
            QNetworkRequest req((QUrl(meta.images[0].url)));
            QNetworkReply *imgReply = netManager->get(req);
            connect(imgReply, &QNetworkReply::finished, this, [this, imgReply](){
                this->onImageDownloaded(imgReply);
            });
        } else {
            meta.previewPath = savePath;
        }
    }

    // 保存并更新UI
    QString baseName = ui->modelList->property("current_processing_file").toString();
    saveLocalMetadata(baseName, root);

    currentMeta = meta; // 缓存到成员变量
    updateDetailView(meta);
}

void MainWindow::onImageDownloaded(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) return;

    QByteArray imgData = reply->readAll();
    QString baseName = ui->modelList->property("current_processing_file").toString();

    if (!currentLoraPath.isEmpty() && !baseName.isEmpty()) {
        QString savePath = QDir(currentLoraPath).filePath(baseName + ".preview.png");
        QFile file(savePath);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(imgData);
            file.close();

            QList<QListWidgetItem*> items = ui->modelList->findItems(baseName, Qt::MatchExactly);
            if(!items.isEmpty()) {
                items.first()->setData(ROLE_PREVIEW_PATH, savePath);
                items.first()->setIcon(getSquareIcon(savePath));

                // 如果当前正好选中该模型，刷新背景
                if (ui->modelList->currentItem() && ui->modelList->currentItem()->text() == baseName) {
                    setHeroImage(savePath);
                }
            }
        }
    }
}

void MainWindow::saveLocalMetadata(const QString &baseName, const QJsonObject &data) {
    if (currentLoraPath.isEmpty()) return;
    QString savePath = QDir(currentLoraPath).filePath(baseName + ".json");
    QFile file(savePath);
    if (file.open(QIODevice::WriteOnly)) {
        QJsonDocument doc(data);
        file.write(doc.toJson());
        file.close();
    }
}

QString MainWindow::calculateFileHash(const QString &filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const int bufferSize = 65536;
    char buffer[bufferSize];
    while (!file.atEnd()) {
        qint64 size = file.read(buffer, bufferSize);
        hash.addData(buffer, size);
    }
    return hash.result().toHex().toUpper();
}

void MainWindow::onOpenUrlClicked() {
    QString url = ui->btnOpenUrl->property("url").toString();
    if (!url.isEmpty()) QDesktopServices::openUrl(QUrl(url));
}

void MainWindow::downloadThumbnail(const QString &url, const QString &savePath, QPushButton *button)
{
    QNetworkRequest req((QUrl(url)));
    QNetworkReply *reply = netManager->get(req);
    QPointer<QPushButton> safeBtn = button;

    connect(reply, &QNetworkReply::finished, this, [this, reply, savePath, safeBtn]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (safeBtn) safeBtn->setText("Error");
            return;
        }

        QByteArray data = reply->readAll();
        QFile file(savePath);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(data);
            file.close();
            IconLoaderTask *task = new IconLoaderTask(savePath, 100, 0, this, savePath, true);
            task->setAutoDelete(true);
            threadPool->start(task);
            if (safeBtn) {
                safeBtn->setText("Processing...");
            }
        }
    });
}

void MainWindow::showFullImageDialog(const QString &imagePath)
{
    if (imagePath.isEmpty() || !QFile::exists(imagePath)) return;

    QDialog *dlg = new QDialog(this);
    dlg->setWindowTitle("Preview (Esc to close)");
    dlg->resize(1200, 900);

    // 使用黑色背景
    QVBoxLayout *layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(0,0,0,0);

    QLabel *imgLabel = new QLabel;
    imgLabel->setStyleSheet("background-color: black;");
    imgLabel->setAlignment(Qt::AlignCenter);

    QPixmap pix(imagePath);
    // 缩放以适应屏幕/窗口
    imgLabel->setPixmap(pix.scaled(dlg->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));

    layout->addWidget(imgLabel);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->exec();
}

// 新增：适应比例图标 (Fit 模式)
QIcon MainWindow::getFitIcon(const QString &path)
{
    QPixmap pix(path);
    if (pix.isNull()) return QIcon();

    // 目标尺寸 (根据你的图库按钮大小设定，这里是 100x150)
    QSize targetSize(100, 150);

    // 创建一个透明底的容器
    QPixmap base(targetSize);
    base.fill(Qt::transparent); // 或者使用 Qt::black

    QPainter painter(&base);
    // 开启抗锯齿
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);

    // 计算适应比例 (KeepAspectRatio)
    QPixmap scaled = pix.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    // 计算居中位置
    int x = (targetSize.width() - scaled.width()) / 2;
    int y = (targetSize.height() - scaled.height()) / 2;

    // 绘制图片
    painter.drawPixmap(x, y, scaled);

    return QIcon(base);
}

void MainWindow::onIconLoaded(const QString &filePath, const QImage &image)
{
    QPixmap pix = QPixmap::fromImage(image);
    QIcon icon(pix);
    // 1. 更新主页列表 (Home Gallery)
    for(int i = 0; i < ui->homeGalleryList->count(); ++i) {
        QListWidgetItem *item = ui->homeGalleryList->item(i);
        if (item->data(ROLE_FILE_PATH).toString() == filePath) {
            item->setIcon(icon);
            // 只要匹配到了，通常不需要继续找了(除非同一个文件用了多次)
        }
    }

    // 2. 更新详情页预览列表 (Detail Gallery) - 新增逻辑
    // 只有当当前在详情页时才需要更新，或者直接遍历layout
    QLayout *layout = ui->layoutGallery;
    if (layout) {
        for (int i = 0; i < layout->count(); ++i) {
            QLayoutItem *item = layout->itemAt(i);
            if (item->widget()) {
                QPushButton *btn = qobject_cast<QPushButton*>(item->widget());
                if (btn) {
                    // 检查我们在 updateDetailView 里绑定的全路径属性
                    if (btn->property("fullImagePath").toString() == filePath) {
                        btn->setIcon(icon);
                        btn->setIconSize(QSize(90, 135)); // 确保图标大小正确
                        btn->setText(""); // 清除 Loading 文字
                    }
                }
            }
        }
    }

    if (filePath == currentMeta.previewPath) {
        setHeroImage(filePath);
    }
}

QString MainWindow::findLocalPreviewPath(const QString &dirPath, const QString &currentBaseName, const QString &serverFileName, int imgIndex)
{
    QDir dir(dirPath);
    QString suffix = (imgIndex == 0) ? ".preview.png" : QString(".preview.%1.png").arg(imgIndex);

    // 1. 策略 A: 优先使用当前本地模型的文件名 (最准确)
    // 例如: [ALICESOFT]_Dohna.preview.png
    QString pathA = dir.filePath(currentBaseName + suffix);
    if (QFile::exists(pathA)) return pathA;

    // 2. 策略 B: 尝试服务器原始文件名
    // 例如: [ALICESOFT] Dohna.preview.png
    if (!serverFileName.isEmpty()) {
        QFileInfo serverFi(serverFileName);
        QString serverBase = serverFi.completeBaseName();
        QString pathB = dir.filePath(serverBase + suffix);
        if (QFile::exists(pathB)) return pathB;

        // === 【新增】策略 C: 尝试将服务器文件名中的空格替换为下划线 ===
        // 很多下载工具会自动把 "[A] B" 改成 "[A]_B"
        QString serverBaseUnderscore = serverBase;
        serverBaseUnderscore.replace(" ", "_");
        QString pathC = dir.filePath(serverBaseUnderscore + suffix);
        if (QFile::exists(pathC)) return pathC;

        // === 【新增】策略 D: 尝试去掉方括号等特殊字符的模糊匹配 (可选，视情况而定) ===
        // 如果上面都不行，这可能是最后的保底，但通常策略 C 就能解决问题
    }

    // 3. 实在找不到，返回默认路径 (路径 A)，以便下载逻辑使用这个名字保存新文件
    return pathA;
}

void MainWindow::onHashCalculated()
{
    // 获取后台线程的返回值
    QString hash = hashWatcher->result();

    // 检查：如果计算出来的 Hash 为空，说明文件可能被锁或读失败
    if (hash.isEmpty()) {
        ui->lblModelName->setText("错误：无法读取文件或计算 Hash 失败");
        ui->btnForceUpdate->setEnabled(true);
        return;
    }

    // Hash 算完了，现在开始联网
    ui->lblModelName->setText("Hash 计算完成，正在获取元数据...");
    fetchModelInfoFromCivitai(hash); // 调用你原来的联网函数
}

void MainWindow::updateBackgroundImage()
{
    if (!ui->backgroundLabel || !ui->heroFrame || !ui->scrollAreaWidgetContents) return;

    // 1. 强制同步大小 (防止跳动)
    if (ui->backgroundLabel->size() != ui->scrollAreaWidgetContents->size()) {
        ui->backgroundLabel->setGeometry(ui->scrollAreaWidgetContents->rect());
    }

    QSize bgSize = ui->backgroundLabel->size();
    if (bgSize.isEmpty()) return;

    QSize heroSize = ui->heroFrame->size();
    if (heroSize.isEmpty()) heroSize = QSize(bgSize.width(), 400);

    if (currentHeroPath.isEmpty() || !QFile::exists(currentHeroPath)) {
        ui->backgroundLabel->setText("");
        ui->backgroundLabel->setPixmap(QPixmap());
        ui->backgroundLabel->setStyleSheet("background-color: #1b2838;");
        return;
    }

    QPixmap srcPixmap;
    if (!currentHeroPixmap.isNull()) srcPixmap = currentHeroPixmap;
    else srcPixmap.load(currentHeroPath);
    if (srcPixmap.isNull()) return;

    // === 2. 准备绘图 ===
    // 目标图片的真实渲染尺寸 (基于 Hero 缩放)
    double scaleW = (double)heroSize.width() / srcPixmap.width();
    double scaleH = (double)heroSize.height() / srcPixmap.height();
    double scale = qMax(scaleW, scaleH);

    int newW = srcPixmap.width() * scale;
    int newH = srcPixmap.height() * scale;

    int offsetX = (heroSize.width() - newW) / 2;
    int offsetY = (heroSize.height() - newH) / 4;

    QPixmap finalPixmap(bgSize);
    finalPixmap.fill(QColor("#1b2838"));

    QPainter painter(&finalPixmap);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);

    // === 3. 高质量平滑模糊 (核心修改) ===

    // A. 预处理：将原图缩小到一个合理的尺寸进行模糊处理
    // 500px 的宽度足够提供色彩信息，同时计算高斯模糊非常快
    // 相比直接除以 2，固定宽度能保证在不同分辨率屏幕上模糊程度一致
    int processWidth = 500;
    QPixmap tempPix = srcPixmap.scaledToWidth(processWidth, Qt::SmoothTransformation);

    // B. 应用高斯模糊 (使用 QGraphicsScene)
    // 这是消除马赛克的关键！
    if (!tempPix.isNull()) {
        QGraphicsBlurEffect *blur = new QGraphicsBlurEffect;
        blur->setBlurRadius(30); // 半径越大越糊，30 对应 500px 宽度效果很棒
        blur->setBlurHints(QGraphicsBlurEffect::PerformanceHint);

        QGraphicsScene scene;
        QGraphicsPixmapItem *item = new QGraphicsPixmapItem(tempPix);
        item->setGraphicsEffect(blur);
        scene.addItem(item);

        // 渲染模糊后的结果
        QPixmap blurredResult(tempPix.size());
        blurredResult.fill(Qt::transparent);
        QPainter ptr(&blurredResult);
        scene.render(&ptr);

        // 记得清理内存，虽然栈上对象会自动释放，但 blur 是 new 出来的
        // QGraphicsPixmapItem 会接管 blur 的所有权，scene 会接管 item 的所有权
        // 所以这里不需要手动 delete，scene 析构时会搞定一切

        // C. 放大回目标尺寸
        // 因为已经是模糊的图像，再使用 SmoothTransformation 放大，
        // 像素边缘会非常柔和，完全没有马赛克
        painter.drawPixmap(QRect(offsetX, offsetY, newW, newH),
                           blurredResult,
                           blurredResult.rect());
    }

    // === 4. 绘制渐变遮罩 ===
    QLinearGradient gradient(0, 0, 0, bgSize.height());
    gradient.setColorAt(0.0, QColor(27, 40, 56, 120)); // 顶部半透

    double imgBottomY = offsetY + newH;
    double stopRatio = imgBottomY / bgSize.height();

    if (stopRatio > 1.0) stopRatio = 1.0;
    if (stopRatio < 0.0) stopRatio = 0.5;

    // 可以在图片结束处让遮罩变黑，过渡更自然
    gradient.setColorAt(qMax(0.0, stopRatio - 0.2), QColor(27, 40, 56, 210));
    gradient.setColorAt(stopRatio, QColor(27, 40, 56, 255));

    painter.fillRect(finalPixmap.rect(), gradient);
    painter.end();

    ui->backgroundLabel->setPixmap(finalPixmap);
    ui->backgroundLabel->setStyleSheet("");
}
