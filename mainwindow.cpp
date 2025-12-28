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

    imageLoadWatcher = new QFutureWatcher<ImageLoadResult>(this);
    connect(imageLoadWatcher, &QFutureWatcher<ImageLoadResult>::finished, this, [this](){
        // A. 获取后台加载的原图
        ImageLoadResult result = imageLoadWatcher->result();
        if (!result.valid) return;

        // B. 转换 Hero 图片 (QImage -> QPixmap)
        nextHeroPixmap = QPixmap::fromImage(result.originalImg);

        // C. 准备背景图 (在主线程进行，但因为基于小图操作，速度极快)
        QSize targetSize = ui->backgroundLabel->size();
        if (targetSize.isEmpty()) targetSize = QSize(1920, 1080); // 保底

        QSize heroSize = ui->heroFrame->size();
        if (heroSize.isEmpty()) heroSize = QSize(targetSize.width(), 400);

        // 如果没有旧背景，生成一个
        if (currentBlurredBgPix.isNull() && !currentHeroPixmap.isNull()) {
            currentBlurredBgPix = applyBlurToImage(currentHeroPixmap.toImage(), targetSize, heroSize);
        }

        // 生成新背景 (核心优化算法在 applyBlurToImage 里)
        nextBlurredBgPix = applyBlurToImage(result.originalImg, targetSize, heroSize);

        // D. 启动动画
        transitionOpacity = 0.0;
        transitionAnim->start();
    });

    transitionAnim = new QVariantAnimation(this);
    transitionAnim->setStartValue(0.0f);
    transitionAnim->setEndValue(1.0f);
    transitionAnim->setDuration(250);
    transitionAnim->setEasingCurve(QEasingCurve::InOutQuad);

    connect(transitionAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &val){
        transitionOpacity = val.toFloat();
        ui->heroFrame->update();
        updateBackgroundDuringTransition();
    });

    connect(transitionAnim, &QVariantAnimation::finished, this, [this](){
        currentHeroPixmap = nextHeroPixmap;
        currentBlurredBgPix = nextBlurredBgPix;
        nextHeroPixmap = QPixmap();
        nextBlurredBgPix = QPixmap();
        transitionOpacity = 0.0;
        ui->heroFrame->update();
        updateBackgroundDuringTransition();
    });

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

    ui->btnFavorite->setContextMenuPolicy(Qt::CustomContextMenu);

    // 1. 确保开启像素滚动 (如果在 XML 里设了，这句可以省略)
    ui->homeGalleryList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    // 2. 设置滚轮滚一下移动的像素距离 (默认通常较小，比如20)
    ui->homeGalleryList->verticalScrollBar()->setSingleStep(40);

    initMenu();

    // === 信号连接 ===
    connect(ui->modelList, &QListWidget::itemClicked, this, &MainWindow::onModelListClicked);

    connect(ui->comboSort, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onSortIndexChanged);

    connect(ui->comboBaseModel, &QComboBox::currentTextChanged,
            this, &MainWindow::onFilterBaseModelChanged);

    // 侧边栏右键菜单
    ui->modelList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->modelList, &QListWidget::customContextMenuRequested, this, &MainWindow::onSidebarContextMenu);

    connect(ui->btnOpenUrl, &QPushButton::clicked, this, &MainWindow::onOpenUrlClicked);
    connect(ui->btnScanLocal, &QPushButton::clicked, this, &MainWindow::onScanLocalClicked);
    connect(ui->btnForceUpdate, &QPushButton::clicked, this, &MainWindow::onForceUpdateClicked);

    connect(ui->searchEdit, &QLineEdit::textChanged, this, &MainWindow::onSearchTextChanged);

    // 主页相关
    connect(ui->btnHome, &QPushButton::clicked, this, &MainWindow::onHomeButtonClicked);
    connect(ui->homeGalleryList, &QListWidget::itemClicked, this, &MainWindow::onHomeGalleryClicked);
    connect(ui->btnAddCollection, &QPushButton::clicked, this, &MainWindow::onCreateCollection);

    // 2. 右键点击 -> 弹出菜单
    connect(ui->btnFavorite, &QPushButton::customContextMenuRequested, this, [this](const QPoint &pos){
        // 获取当前选中的模型名称
        if (ui->modelList->currentItem()) {
            QString name = ui->modelList->currentItem()->text();
            // 在按钮位置弹出菜单
            showCollectionMenu(name, ui->btnFavorite->mapToGlobal(pos));
        }
    });
    connect(ui->btnFavorite, &QPushButton::clicked, this, &MainWindow::onBtnFavoriteClicked);

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
    // loadCollections(); // 加载收藏夹配置
    // loadSettings();    // 扫描模型

    QTimer::singleShot(10, this, [this](){
        // 显示一个加载中的状态（可选）
        ui->statusbar->showMessage("正在扫描本地模型库...");

        // 开始加载
        loadCollections();
        loadSettings();

        ui->statusbar->showMessage(QString("加载完成，共 %1 个模型").arg(ui->modelList->count()), 3000);
    });
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
    while (layout->count() > 1) { // 假设索引0是 "新建" 按钮
        item = layout->takeAt(1);
        if (item->widget()) delete item->widget();
        delete item;
    }

    // === 1. 修改新建按钮样式 ===
    ui->btnAddCollection->setProperty("class", "collectionBtn");

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
        if (displayName.length() > 20) displayName = displayName.left(18) + "..";

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

            QAction *title = menu.addAction(QString("管理: %1").arg(name));
            title->setEnabled(false);
            menu.addSeparator();

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
                                                   QString("确定要删除收藏夹 \"%1\" 吗？\n(里面的模型不会被删除，仅删除分类)").arg(name),
                                                   QMessageBox::Yes | QMessageBox::No);
                if (reply == QMessageBox::Yes) {
                    // 1. 从数据中移除
                    collections.remove(name);

                    // 2. 如果当前正看着这个收藏夹，被删了就得回到"全部"
                    if (currentCollectionFilter == name) {
                        currentCollectionFilter = "";
                    }

                    // 3. 保存并刷新
                    saveCollections();
                    refreshHomeGallery(); // 刷新一下主页大图，因为过滤条件变了
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

    ui->homeGalleryList->setContextMenuPolicy(Qt::CustomContextMenu);
    disconnect(ui->homeGalleryList, &QListWidget::customContextMenuRequested, this, &MainWindow::onHomeGalleryContextMenu);
    connect(ui->homeGalleryList, &QListWidget::customContextMenuRequested, this, &MainWindow::onHomeGalleryContextMenu);

    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *sideItem = ui->modelList->item(i);

        if (sideItem->isHidden()) continue;

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

    // 直接调用通用函数
    showCollectionMenu(baseName, ui->modelList->mapToGlobal(pos));
}

void MainWindow::onBtnFavoriteClicked()
{
    // 获取当前详情页正在展示的模型
    // 优先从 modelList 的当前选中项获取，这是最准确的
    QListWidgetItem *item = ui->modelList->currentItem();
    if (!item) return;

    QString baseName = item->text();

    // 在按钮正下方弹出
    QPoint pos = ui->btnFavorite->mapToGlobal(QPoint(0, ui->btnFavorite->height()));
    showCollectionMenu(baseName, pos);
}

void MainWindow::onHomeGalleryContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = ui->homeGalleryList->itemAt(pos);
    if (!item) return; // 点击了空白处

    // 注意：主页 Item 没有 text，我们需要从 data 里找或者通过 ToolTip
    // 之前我们在 refreshHomeGallery 里设置了 tooltip 为 baseName
    QString baseName = item->toolTip();

    if (baseName.isEmpty()) return;

    // 复用通用的菜单逻辑
    showCollectionMenu(baseName, ui->homeGalleryList->mapToGlobal(pos));
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

// === 核心：事件过滤器 (绘图 + 点击) ===
bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->heroFrame) {
        if (event->type() == QEvent::Paint) {
            QPainter painter(ui->heroFrame);

            // 绘制背景黑底（防止透明度叠加时看到底色）
            painter.fillRect(ui->heroFrame->rect(), Qt::black);

            // 辅助 Lambda：用于绘制单张图片 (Cover 模式)
            auto drawPix = [&](const QPixmap &pix, qreal opacity) {
                if (pix.isNull()) return;
                QSize widgetSize = ui->heroFrame->size();
                QSize imgSize = pix.size();
                if (imgSize.isEmpty()) return;

                // Cover 算法
                double scaleW = (double)widgetSize.width() / imgSize.width();
                double scaleH = (double)widgetSize.height() / imgSize.height();
                double scale = qMax(scaleW, scaleH);

                double newW = imgSize.width() * scale;
                double newH = imgSize.height() * scale;
                double offsetX = (widgetSize.width() - newW) / 2.0;
                double offsetY = (widgetSize.height() - newH) / 4.0;

                painter.setOpacity(opacity);
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.drawPixmap(QRectF(offsetX, offsetY, newW, newH), pix, pix.rect());
            };

            // 情况 A: 正在切换到一张新图片 (Next 存在)
            if (!nextHeroPixmap.isNull()) {
                // 1. 底层：画旧图 (始终 1.0，让新图盖在上面，这样没有黑缝)
                drawPix(currentHeroPixmap, 1.0);
                // 2. 顶层：画新图 (透明度从 0 -> 1)
                drawPix(nextHeroPixmap, transitionOpacity);
            }
            // 情况 B: 正在切换到“无图片”状态 (Next 为空，且正在动画中)
            else if (transitionAnim->state() == QAbstractAnimation::Running) {
                // 让旧图慢慢消失 (透明度 1 -> 0)
                drawPix(currentHeroPixmap, 1.0 - transitionOpacity);
            }
            // 情况 C: 静止状态 (动画结束)
            else {
                drawPix(currentHeroPixmap, 1.0);
            }

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
    ui->modelList->setUpdatesEnabled(false);

    ui->modelList->clear();

    ui->comboBaseModel->blockSignals(true);
    ui->comboBaseModel->clear();
    ui->comboBaseModel->addItem("All");
    QSet<QString> foundBaseModels; // 用于去重记录发现的底模

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
        item->setData(Qt::UserRole, baseName);
        item->setData(ROLE_FILE_PATH, fullPath);
        item->setData(ROLE_PREVIEW_PATH, previewPath);

        // 设置图标
        if (!previewPath.isEmpty()) {
            item->setIcon(getSquareIcon(previewPath));
        }

        QString jsonPath = dir.filePath(baseName + ".json");
        preloadItemMetadata(item, jsonPath);
        QString baseModel = item->data(ROLE_FILTER_BASE).toString();
        if (!baseModel.isEmpty() && !foundBaseModels.contains(baseModel)) {
            foundBaseModels.insert(baseModel);
            ui->comboBaseModel->addItem(baseModel);
        }

        ui->modelList->addItem(item);
    }
    ui->comboBaseModel->blockSignals(false);
    ui->modelList->setUpdatesEnabled(true);
    refreshHomeGallery(); // 刷新主页
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
        ui->backgroundLabel->resize(ui->scrollAreaWidgetContents->size());
        // 调用异步加载
        transitionToImage(meta.previewPath);
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
    QString currentBaseName;
    QListWidgetItem *item = ui->modelList->currentItem();
    if (item) {
        // 优先从 UserRole 获取完整名 (之前在 scanModels 里存进去的)
        currentBaseName = item->data(Qt::UserRole).toString();
        // 如果 UserRole 是空的 (防止异常)，才回退到 text()
        if (currentBaseName.isEmpty()) currentBaseName = item->text();
    } else {
        currentBaseName = currentMeta.name;
    }

    // 2. 寻找本地图片路径
    QString localPath = findLocalPreviewPath(currentLoraPath, currentBaseName, currentMeta.fileNameServer, index);

    // 3. 执行过渡
    if (QFile::exists(localPath)) {
        transitionToImage(localPath);
    } else {
        qDebug() << "Preview image not found at:" << localPath; // 方便调试
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
    ui->lblModelName->setStyleSheet(
        "color: #fff;"
        "background-color: rgba(0,0,0,120);"
        "padding: 15px;"
        "border-left: 5px solid #66c0f4;" // 恢复蓝色条
        "font-size: 24px;"
        "font-weight: bold;"
    );
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

    // transitionToImage("");
    // ui->heroFrame->setProperty("fullImagePath", "");
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

    QString filePath = item->data(ROLE_FILE_PATH).toString();
    if (currentMeta.filePath == filePath && !currentMeta.name.isEmpty()) {
        // 已经是这个模型了，直接忽略本次点击
        return;
    }

    // 1. 如果正在计算上一个，先取消或忽略
    if (hashWatcher->isRunning()) {
        // 简单处理：提示用户稍等，或者强制让 UI 变动
        // 更好的做法是 cancel，但 SHA 计算很难中途 cancel，所以我们用标志位判断
    }

    ui->mainStack->setCurrentIndex(1); // 进详情页
    clearDetailView(); // 清空旧数据

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
    // 获取当前正在处理的文件名 (从属性或当前选中项)
    // 建议直接传参进来，或者确保 ui->modelList->property("current_processing_file") 是本地文件名(BaseName)
    QString localBaseName = ui->modelList->property("current_processing_file").toString();

    QString urlStr = QString("https://civitai.com/api/v1/model-versions/by-hash/%1").arg(hash);
    QNetworkRequest request((QUrl(urlStr)));
    request.setHeader(QNetworkRequest::UserAgentHeader, "MyLoraManager/1.0");

    QNetworkReply *reply = netManager->get(request);

    // === 关键修改：将本地文件名绑定到 Reply 对象上 ===
    reply->setProperty("localBaseName", localBaseName);

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
    QString localBaseName = reply->property("localBaseName").toString();
    reply->deleteLater();
    ui->btnForceUpdate->setEnabled(true);

    if (reply->error() != QNetworkReply::NoError) {
        clearLayout(ui->layoutTriggerStack); // 清空触发词区域

        // === 修改：在标题栏醒目显示错误 ===
        ui->lblModelName->setText(QString("⚠️ 连接失败 / Error: %1").arg(reply->errorString()));

        // 设置醒目的红色样式
        // 注意：这里我们给它设了一个 UserProperty 标记它是错误状态，
        // 虽然不一定用到，但是个好习惯
        ui->lblModelName->setStyleSheet(
            "color: #ff4c4c;"               // 红字
            "background-color: rgba(45, 20, 20, 0.8);" // 深红半透背景
            "border-left: 5px solid #ff0000;" // 左侧红条
            "padding: 15px;"
            "font-size: 15px;"
        );

        transitionToImage("");
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
        QString savePath = QDir(currentLoraPath).filePath(localBaseName + ".preview.png");

        // 如果本地已存在，直接用；否则下载第一张
        if (!QFile::exists(savePath)) {
            QNetworkRequest req((QUrl(meta.images[0].url)));
            QNetworkReply *imgReply = netManager->get(req);

            // === 关键：把本地文件名和保存路径都传给图片下载回调 ===
            imgReply->setProperty("localBaseName", localBaseName);
            imgReply->setProperty("savePath", savePath);

            connect(imgReply, &QNetworkReply::finished, this, [this, imgReply](){
                this->onImageDownloaded(imgReply);
            });

            // 暂时先把 meta 的路径设为这个（虽然还没下载完），以便保存到 JSON
            meta.previewPath = savePath;
        } else {
            meta.previewPath = savePath;
        }
    }

    // 保存并更新UI
    saveLocalMetadata(localBaseName, root);

    currentMeta = meta; // 缓存到成员变量
    updateDetailView(meta);
}

void MainWindow::onImageDownloaded(QNetworkReply *reply)
{
    reply->deleteLater();

    // 1. 获取上下文
    QString localBaseName = reply->property("localBaseName").toString();
    QString savePath = reply->property("savePath").toString();

    if (reply->error() != QNetworkReply::NoError) {
        qDebug() << "Image download failed:" << reply->errorString();
        return;
    }

    QByteArray imgData = reply->readAll();
    if (savePath.isEmpty() || localBaseName.isEmpty()) return;

    // 2. 保存文件
    QFile file(savePath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(imgData);
        file.close();

        // 生成图标 (耗时操作建议放线程，这里简单处理先用主线程，或复用你的 Task)
        // 为了立即反馈，先生成一个小图标
        QIcon newIcon = getSquareIcon(savePath); // 或者 getFitIcon

        // === 修复问题 1：更新 SideBar (modelList) 图标 ===
        // 遍历列表找到对应的 Item (可能有多个，如果同一个文件被加了多次，虽不常见)
        for(int i = 0; i < ui->modelList->count(); ++i) {
            QListWidgetItem *item = ui->modelList->item(i);
            // 必须比对 UserRole (即 baseName) 或 FILE_PATH
            if (item->data(Qt::UserRole).toString() == localBaseName) {
                item->setData(ROLE_PREVIEW_PATH, savePath); // 更新数据
                item->setIcon(newIcon); // 刷新图标
            }
        }

        // === 修复问题 1：更新 Home Gallery (homeGalleryList) 图标 ===
        // 主页列表没有存 UserRole (baseName)，但存了 ROLE_FILE_PATH
        // 我们通过 savePath 推导 filePath，或者更简单的：遍历检查 previewPath
        QString targetFilePath = QDir(currentLoraPath).filePath(localBaseName + ".safetensors");
        // 假如你的模型扩展名不确定，这里最好存一个 map，或者遍历检查

        for(int i = 0; i < ui->homeGalleryList->count(); ++i) {
            QListWidgetItem *item = ui->homeGalleryList->item(i);
            // 检查 Item 对应的文件路径是否包含 localBaseName
            QString itemPath = item->data(ROLE_FILE_PATH).toString();
            QFileInfo fi(itemPath);
            if (fi.completeBaseName() == localBaseName) {
                item->setData(ROLE_PREVIEW_PATH, savePath);
                item->setIcon(newIcon);
            }
        }

        // === 修复问题 3：立即更新详情页 Hero 和 背景 ===
        // 判断当前正在查看的是不是这个模型
        // 判定标准：当前详情页记录的文件路径 == 下载图片所属的文件的路径
        QString currentViewingPath = currentMeta.filePath;
        QFileInfo currentFi(currentViewingPath);

        if (currentFi.completeBaseName() == localBaseName) {
            // 更新内存中的 meta，防止下次点击还没更新
            currentMeta.previewPath = savePath;
            ui->heroFrame->setProperty("fullImagePath", savePath); // 更新大图查看路径

            // 强制触发过渡动画
            // 此时文件已落地，transitionToImage 会读取成功并刷新 UI
            transitionToImage(savePath);
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
        if (currentHeroPixmap.isNull()) {
            transitionToImage(filePath);
        }
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

    // 1. 强制同步大小
    if (ui->backgroundLabel->size() != ui->scrollAreaWidgetContents->size()) {
        ui->backgroundLabel->setGeometry(ui->scrollAreaWidgetContents->rect());
    }

    // 如果正在动画，不处理 Resize，由动画循环处理
    if (transitionAnim && transitionAnim->state() == QAbstractAnimation::Running) return;

    QSize targetSize = ui->backgroundLabel->size();
    if (targetSize.isEmpty()) return;

    // 获取 Hero 尺寸用于对齐
    QSize heroSize = ui->heroFrame->size();
    if (heroSize.isEmpty()) heroSize = QSize(targetSize.width(), 400);

    // === 修复逻辑：始终基于原图重新生成 ===
    // 之前的问题在于复用 currentBlurredBgPix 时导致了：
    // 1. 双重遮罩 (Mask on Mask) -> 变暗
    // 2. 双重偏移 (Offset on Offset) -> 抖动/错位

    if (!currentHeroPixmap.isNull()) {
        // 直接用当前的高清原图生成新的背景，保证比例、位置、遮罩都是全新的且正确的
        currentBlurredBgPix = applyBlurToImage(currentHeroPixmap.toImage(), targetSize, heroSize);

        // 刷新显示
        ui->backgroundLabel->setPixmap(currentBlurredBgPix);
    }
    else if (!currentHeroPath.isEmpty() && QFile::exists(currentHeroPath)) {
        // 如果缓存丢了但有路径，重新读图生成
        QImage img(currentHeroPath);
        currentBlurredBgPix = applyBlurToImage(img, targetSize, heroSize);
        ui->backgroundLabel->setPixmap(currentBlurredBgPix);
    }
    else {
        // 既没图也没路径，清空背景
        ui->backgroundLabel->clear();
        // 或者保留纯色底
        QPixmap empty(targetSize);
        empty.fill(QColor("#1b2838"));
        ui->backgroundLabel->setPixmap(empty);
    }
}

void MainWindow::onSearchTextChanged(const QString &text)
{
    QString query = text.trimmed();
    QString targetBaseModel = ui->comboBaseModel->currentText();

    // 1. 自动重置收藏夹 (保留逻辑)
    if (!query.isEmpty() && !currentCollectionFilter.isEmpty()) {
        currentCollectionFilter = "";
        refreshHomeCollectionsUI();
    }

    // 2. 遍历筛选
    for(int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);

        // === 修改：获取名称的逻辑 ===
        // 优先用 UserRole (排序用的也是这个，保持一致)，如果为空则用显示的文本
        QString modelName = item->data(Qt::UserRole).toString();
        if (modelName.isEmpty()) modelName = item->text();

        // A. 名称匹配
        bool nameMatch = modelName.contains(query, Qt::CaseInsensitive);

        // B. 底模匹配
        bool baseMatch = true;
        if (targetBaseModel != "All") {
            QString itemBase = item->data(ROLE_FILTER_BASE).toString();
            if (itemBase != targetBaseModel) baseMatch = false;
        }

        // 综合判断
        item->setHidden(!(nameMatch && baseMatch));
    }

    // 3. 刷新主页
    refreshHomeGallery();

    // 4. 切回主页优化 (保留逻辑)
    if (ui->mainStack->currentIndex() == 1) {
        QListWidgetItem *currentItem = ui->modelList->currentItem();
        if (currentItem && currentItem->isHidden()) {
            ui->mainStack->setCurrentIndex(0);
        }
    }
}

void MainWindow::showCollectionMenu(const QString &baseName, const QPoint &globalPos)
{
    if (baseName.isEmpty()) return;

    QMenu menu(this);

    // 1. 标题 (显示模型名)
    QString displayName = baseName;
    if (displayName.length() > 20) displayName = displayName.left(18) + "..";
    QAction *titleAct = menu.addAction(displayName);
    titleAct->setEnabled(false);

    // 如果当前正在某个收藏夹视图下，显示快捷移除 (保留之前的逻辑)
    if (!currentCollectionFilter.isEmpty()) {
        if (collections[currentCollectionFilter].contains(baseName)) {
            QString removeText = QString("从当前 \"%1\" 移除").arg(currentCollectionFilter);
            QAction *actQuickRemove = menu.addAction(removeText);
            connect(actQuickRemove, &QAction::triggered, this, [this, baseName](){
                collections[currentCollectionFilter].removeAll(baseName);
                saveCollections();
                refreshHomeGallery();
            });
        }
    }

    menu.addSeparator();

    // =========================================================
    // 2. 核心修改：新增 "从收藏夹移除..." 二级菜单
    // =========================================================
    QMenu *removeMenu = menu.addMenu("从指定收藏夹移除...");
    bool isInAnyCollection = false;

    for (auto it = collections.begin(); it != collections.end(); ++it) {
        QString colName = it.key();
        // 只有当模型【在】这个收藏夹里时，才添加到移除列表中
        if (it.value().contains(baseName)) {
            isInAnyCollection = true;
            QAction *actRemove = removeMenu->addAction(colName);
            // 鼠标悬停变红提示删除（可选样式）

            connect(actRemove, &QAction::triggered, this, [this, colName, baseName](){
                // 执行移除逻辑
                collections[colName].removeAll(baseName);
                saveCollections();

                // 如果当前正处于该收藏夹视图，或者处于全部视图，刷新一下界面
                // (虽然在全部视图下移除收藏不影响显示，但刷新一下比较稳妥)
                refreshHomeGallery();

                // 提示用户
                ui->statusbar->showMessage(QString("已从 %1 中移除").arg(colName), 2000);
            });
        }
    }

    // 如果该模型不在任何收藏夹，禁用这个菜单
    if (!isInAnyCollection) {
        removeMenu->setTitle("未加入任何收藏夹");
        removeMenu->setEnabled(false);
    }

    // =========================================================
    // 3. "添加到收藏夹..." 二级菜单 (保持原有逻辑，带复选框)
    // =========================================================
    QMenu *addMenu = menu.addMenu("添加至收藏夹...");

    for (auto it = collections.begin(); it != collections.end(); ++it) {
        QString colName = it.key();
        QAction *action = addMenu->addAction(colName);
        action->setCheckable(true);
        // 勾选状态反映当前是否在其中
        action->setChecked(it.value().contains(baseName));

        connect(action, &QAction::triggered, this, [this, colName, baseName, action](){
            if (action->isChecked()) {
                if (!collections[colName].contains(baseName))
                    collections[colName].append(baseName);
            } else {
                collections[colName].removeAll(baseName);
            }
            saveCollections();

            // 如果操作影响了当前视图，刷新
            if (currentCollectionFilter == colName || !currentCollectionFilter.isEmpty()) {
                refreshHomeGallery();
            }
        });
    }

    addMenu->addSeparator();
    QAction *newAction = addMenu->addAction("新建收藏夹...");
    connect(newAction, &QAction::triggered, this, [this, baseName](){
        bool ok;
        QString text = QInputDialog::getText(this, "新建", "名称:", QLineEdit::Normal, "", &ok);
        if(ok && !text.isEmpty()) {
            if(!collections.contains(text)) {
                collections[text] = QStringList() << baseName;
                saveCollections();
                refreshHomeCollectionsUI(); // 别忘了刷新顶部的按钮
            }
        }
    });

    menu.exec(globalPos);
}

void MainWindow::preloadItemMetadata(QListWidgetItem *item, const QString &jsonPath)
{
    // 初始化默认值 (方便排序)
    item->setData(ROLE_SORT_DATE, 0);
    item->setData(ROLE_SORT_DOWNLOADS, 0);
    item->setData(ROLE_SORT_LIKES, 0);
    item->setData(ROLE_FILTER_BASE, "Unknown");

    QFile file(jsonPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        // 如果没有 JSON，尝试用文件修改时间作为日期
        QFileInfo fi(item->data(ROLE_FILE_PATH).toString());
        item->setData(ROLE_SORT_DATE, fi.lastModified().toMSecsSinceEpoch());
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    QJsonObject root = doc.object();

    // 1. 底模 (Base Model)
    QString baseModel = root["baseModel"].toString();
    if (!baseModel.isEmpty()) item->setData(ROLE_FILTER_BASE, baseModel);

    // 2. 时间 (Created At)
    QString dateStr = root["createdAt"].toString();
    if (!dateStr.isEmpty()) {
        QDateTime dt = QDateTime::fromString(dateStr, Qt::ISODate);
        if (dt.isValid()) item->setData(ROLE_SORT_DATE, dt.toMSecsSinceEpoch());
    } else {
        // 后备：使用文件时间
        QFileInfo fi(item->data(ROLE_FILE_PATH).toString());
        item->setData(ROLE_SORT_DATE, fi.lastModified().toMSecsSinceEpoch());
    }

    // 3. 数据 (Stats)
    QJsonObject stats = root["stats"].toObject();
    item->setData(ROLE_SORT_DOWNLOADS, stats["downloadCount"].toInt());
    item->setData(ROLE_SORT_LIKES, stats["thumbsUpCount"].toInt());
}

void MainWindow::onSortIndexChanged(int index) {
    executeSort();
}

void MainWindow::executeSort()
{
    // 0: Name, 1: Date(New), 2: Downloads, 3: Likes
    int sortType = ui->comboSort->currentIndex();

    // 1. 取出所有 Item
    QList<QListWidgetItem*> items;
    while(ui->modelList->count() > 0) {
        items.append(ui->modelList->takeItem(0));
    }

    // === 准备自然排序器 (用于 Case 0) ===
    QCollator collator;
    collator.setNumericMode(true); // 开启数字模式 (让 v2 排在 v10 前面)
    collator.setCaseSensitivity(Qt::CaseInsensitive); // 忽略大小写 (让 a 和 A 排在一起)
    collator.setIgnorePunctuation(false); // 不忽略标点 (保证 [ 能参与排序)

    // 2. 使用 Lambda 表达式排序
    std::sort(items.begin(), items.end(),
              [sortType, &collator](QListWidgetItem *a, QListWidgetItem *b) { // 注意这里捕获了 &collator

                  switch (sortType) {
                  case 0: // Name (A-Z, Windows Explorer Style)
                  {
                      QString nameA = a->data(Qt::UserRole).toString();
                      QString nameB = b->data(Qt::UserRole).toString();
                      // 使用 collator 进行自然比较，结果 < 0 表示 A 在 B 前
                      return collator.compare(nameA, nameB) < 0;
                  }

                  case 1: // Date (Newest First -> Descending)
                      return a->data(ROLE_SORT_DATE).toLongLong() > b->data(ROLE_SORT_DATE).toLongLong();

                  case 2: // Downloads (High -> Descending)
                      return a->data(ROLE_SORT_DOWNLOADS).toInt() > b->data(ROLE_SORT_DOWNLOADS).toInt();

                  case 3: // Likes (High -> Descending)
                      return a->data(ROLE_SORT_LIKES).toInt() > b->data(ROLE_SORT_LIKES).toInt();

                  default:
                      return false;
                  }
              });

    // 3. 放回 ListWidget
    for(auto *item : items) {
        ui->modelList->addItem(item);
    }

    // 4. 同步刷新主页
    refreshHomeGallery();
}

void MainWindow::onFilterBaseModelChanged(const QString &text) {
    // 触发统一筛选
    // 这里我们简单复用 onSearchTextChanged 里的逻辑，或者重写一个 unifiedFilter
    // 建议直接调用 onSearchTextChanged 并传入当前搜索框的字
    onSearchTextChanged(ui->searchEdit->text());
}

// 静态函数，运行在后台线程
ImageLoadResult MainWindow::processImageTask(const QString &path)
{
    ImageLoadResult result;
    result.path = path;

    // 1. 加载原图 (耗时: 30ms - 200ms)
    QImageReader reader(path);
    reader.setAutoTransform(true);
    // 稍微优化：如果原图是 8K 的，没必要读全分辨率，读个适合屏幕的就行
    // reader.setScaledSize(QSize(2560, 1440)); // 可选优化
    result.originalImg = reader.read();

    result.valid = !result.originalImg.isNull();
    return result;
}

void MainWindow::transitionToImage(const QString &path)
{
    if (path == currentHeroPath) return;

    currentHeroPath = path;

    if (transitionAnim->state() == QAbstractAnimation::Running) {
        transitionAnim->stop();
        if (!nextHeroPixmap.isNull()) {
            currentHeroPixmap = nextHeroPixmap;
            currentBlurredBgPix = nextBlurredBgPix;
        }
    }

    // 重置动画参数
    nextHeroPixmap = QPixmap();
    nextBlurredBgPix = QPixmap();
    transitionOpacity = 0.0;

    if (path.isEmpty()) {
        // === 目标是空图（Fade to Black）===
        // 不要立即清空 currentHeroPixmap！
        // 而是启动动画，让 eventFilter 里的 "情况 B" 去处理淡出
        transitionAnim->start();
    } else {
        // === 目标是新图 ===
        // 启动后台加载，加载完后在回调里设置 nextHeroPixmap 并启动动画
        QFuture<ImageLoadResult> future = QtConcurrent::run(&MainWindow::processImageTask, path);
        imageLoadWatcher->setFuture(future);
    }
}

QPixmap MainWindow::applyBlurToImage(const QImage &srcImg, const QSize &bgSize, const QSize &heroSize)
{
    if (srcImg.isNull()) return QPixmap();

    // 1. 缩小图片 (制作模糊源)
    int processWidth = 500;
    QPixmap tempPix = QPixmap::fromImage(srcImg.scaledToWidth(processWidth, Qt::SmoothTransformation));

    // 2. 高斯模糊
    QGraphicsBlurEffect *blur = new QGraphicsBlurEffect;
    blur->setBlurRadius(30);
    blur->setBlurHints(QGraphicsBlurEffect::PerformanceHint);
    QGraphicsScene scene;
    QGraphicsPixmapItem *item = new QGraphicsPixmapItem(tempPix);
    item->setGraphicsEffect(blur);
    scene.addItem(item);
    QPixmap blurredResult(tempPix.size());
    blurredResult.fill(Qt::transparent);
    QPainter ptr(&blurredResult);
    scene.render(&ptr);

    // 3. 合成最终背景
    QPixmap finalBg(bgSize);
    finalBg.fill(QColor("#1b2838")); // 填充底色
    QPainter painter(&finalBg);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);

    // === 核心修复：使用 heroSize 进行计算 ===
    // 这样算法就和 eventFilter 里的 Hero 绘制逻辑完全一致了

    // 保底：防止 heroSize 为空导致除以0
    int heroW = heroSize.width() > 0 ? heroSize.width() : bgSize.width();
    int heroH = heroSize.height() > 0 ? heroSize.height() : 400;

    double scaleW = (double)heroW / blurredResult.width();
    double scaleH = (double)heroH / blurredResult.height();
    double scale = qMax(scaleW, scaleH); // Cover 模式

    int newW = blurredResult.width() * scale;
    int newH = blurredResult.height() * scale;

    // 使用 heroH 来计算 Y 轴偏移
    int offsetX = (heroW - newW) / 2;
    int offsetY = (heroH - newH) / 4;

    // 绘制图片
    painter.drawPixmap(QRect(offsetX, offsetY, newW, newH), blurredResult);

    // 4. 绘制渐变遮罩 (自然融合到底部背景色)
    QLinearGradient gradient(0, 0, 0, bgSize.height());
    gradient.setColorAt(0.0, QColor(27, 40, 56, 120)); // 顶部半透

    // 计算图片结束的位置，让渐变在图片下方自然过渡
    double imgBottomY = offsetY + newH;
    double stopRatio = imgBottomY / bgSize.height(); // 归一化位置

    // 限制范围，防止越界
    if (stopRatio > 1.0) stopRatio = 1.0;
    if (stopRatio < 0.0) stopRatio = 0.1;

    // 在图片结束前一点点开始变深，直到图片结束处完全变为背景色
    gradient.setColorAt(qMax(0.0, stopRatio - 0.2), QColor(27, 40, 56, 210));
    gradient.setColorAt(stopRatio, QColor(27, 40, 56, 255));
    // 之后全是背景色
    if (stopRatio < 0.99) {
        gradient.setColorAt(1.0, QColor(27, 40, 56, 255));
    }

    painter.fillRect(finalBg.rect(), gradient);
    painter.end();

    return finalBg;
}

void MainWindow::updateBackgroundDuringTransition()
{
    if (!ui->backgroundLabel) return;
    QSize bgSize = ui->backgroundLabel->size();
    if (bgSize.isEmpty()) return;

    QPixmap canvas(bgSize);
    canvas.fill(QColor("#1b2838")); // 纯色打底，防止交叉淡化时露出底色

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // === 核心修改：使用交叉淡化 (Cross-Fade) ===

    // 情况 A: 正在切换新图
    if (!nextBlurredBgPix.isNull()) {
        // 旧图：随着 transitionOpacity 增加而减少 (1.0 -> 0.0)
        if (!currentBlurredBgPix.isNull()) {
            painter.setOpacity(1.0 - transitionOpacity);
            painter.drawPixmap(0, 0, currentBlurredBgPix);
        }

        // 新图：随着 transitionOpacity 增加而增加 (0.0 -> 1.0)
        painter.setOpacity(transitionOpacity);
        painter.drawPixmap(0, 0, nextBlurredBgPix);
    }
    // 情况 B: 正在变为空图 (Fade out)
    else {
        if (!currentBlurredBgPix.isNull()) {
            qreal alpha = 1.0 - transitionOpacity;
            if (alpha < 0.0) alpha = 0.0;
            painter.setOpacity(alpha);
            painter.drawPixmap(0, 0, currentBlurredBgPix);
        }
    }

    painter.end();
    ui->backgroundLabel->setPixmap(canvas);
}
