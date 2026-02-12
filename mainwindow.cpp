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
#include <QDirIterator>
#include <QProcess>
#include <QtEndian>

#include "imageloader.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    currentUserAgent = getRandomUserAgent();

    // 初始化运行时状态
    isFirstTreeRefresh = true;
    startupTreeScrollPos = 0;
    // 线程池初始化 (此时还没有读取配置，先不设最大数)
    threadPool = new QThreadPool(this);
    backgroundThreadPool = new QThreadPool(this);
    // Hash 计算器
    hashWatcher = new QFutureWatcher<QString>(this);
    connect(hashWatcher, &QFutureWatcherBase::finished, this, &MainWindow::onHashCalculated);
    // 图片加载器
    imageLoadWatcher = new QFutureWatcher<ImageLoadResult>(this);
    connect(imageLoadWatcher, &QFutureWatcher<ImageLoadResult>::finished, this, [this](){
        // A. 获取后台加载的原图
        ImageLoadResult result = imageLoadWatcher->result();
        if (result.path != currentHeroPath) {
            qDebug() << "Discarding obsolete image load:" << result.path;
            return;
        }
        if (!result.valid) {
            // 图片无效，淡出
            nextHeroPixmap = QPixmap();
            nextBlurredBgPix = QPixmap();
        } else {
            QPixmap rawPix = QPixmap::fromImage(result.originalImg);

            // --- NSFW 大图处理 ---
            bool shouldBlur = false;
            // 判断这张图是否为 NSFW。可以通过 path 在 currentMeta.images 里查找
            // 简单处理：如果是当前详情页的封面，直接看 currentMeta.nsfw
            if (optFilterNSFW && optNSFWMode == 1) {
                // 检查该图在详情页列表中是否标记为 NSFW
                for(const auto& img : currentMeta.images) {
                    if(result.path.contains(img.hash) || result.path == currentMeta.previewPath) {
                        if(img.nsfwLevel > optNSFWLevel) shouldBlur = true;
                        break;
                    }
                }
            }
            if (shouldBlur) {
                nextHeroPixmap = applyNSFWBlur(rawPix);
            } else {
                nextHeroPixmap = rawPix;
            }
            // C. 准备背景图
            QSize targetSize = ui->backgroundLabel->size();
            if (targetSize.isEmpty()) targetSize = QSize(1920, 1080);
            QSize heroSize = ui->heroFrame->size();
            if (heroSize.isEmpty()) heroSize = QSize(targetSize.width(), 400);
            if (currentBlurredBgPix.isNull() && !currentHeroPixmap.isNull()) {
                currentBlurredBgPix = applyBlurToImage(currentHeroPixmap.toImage(), targetSize, heroSize);
            }
            nextBlurredBgPix = applyBlurToImage(result.originalImg, targetSize, heroSize);
        }
        transitionOpacity = 0.0;
        if (transitionAnim->state() == QAbstractAnimation::Running) {
            transitionAnim->stop();
        }
        transitionAnim->start();
    });

    // 动画初始化
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
    placeholderIcon = generatePlaceholderIcon();

    settings = new QSettings("MyAiTools", "LoraManager", this);
    netManager = new QNetworkAccessManager(this);

    // === 1. 初始化菜单栏 ===
    initMenuBar();
    // === 2. 加载配置 ===
    loadPathSettings();   // 从注册表读路径
    loadGlobalConfig();   // 从 JSON 读选项
    // === 应用线程数 ===
    threadPool->setMaxThreadCount(optRenderThreadCount);
    backgroundThreadPool->setMaxThreadCount(optRenderThreadCount);
    // === 3. 连接路径设置信号 ===
    connect(ui->btnBrowseLora, &QPushButton::clicked, this, &MainWindow::onBrowseLoraPath);
    connect(ui->btnBrowseGallery, &QPushButton::clicked, this, &MainWindow::onBrowseGalleryPath);
    connect(ui->btnBrowseTrans, &QPushButton::clicked, this, &MainWindow::onBrowseTranslationPath);

    // 样式设置
    QGraphicsDropShadowEffect *shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(20);
    shadow->setColor(Qt::black);
    shadow->setOffset(0, 0);
    ui->lblModelName->setGraphicsEffect(shadow);

    ui->heroFrame->installEventFilter(this);
    ui->heroFrame->setCursor(Qt::PointingHandCursor);

    ui->btnFavorite->setContextMenuPolicy(Qt::CustomContextMenu);

    // 1. 开启像素滚动
    ui->homeGalleryList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    ui->listUserImages->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // 2. 设置滚轮滚一下移动的像素距离
    ui->homeGalleryList->verticalScrollBar()->setSingleStep(40);
    ui->listUserImages->verticalScrollBar()->setSingleStep(40);

    ui->collectionTree->setHeaderHidden(true); // 隐藏 "Collection / Model" 表头

    ui->btnModelsTab->setCheckable(true);
    ui->btnCollectionsTab->setCheckable(true);
    ui->btnModelsTab->setAutoExclusive(true);
    ui->btnCollectionsTab->setAutoExclusive(true);
    ui->btnModelsTab->setChecked(true);

    // 关于页版本号显示
    ui->lblAboutVersion->setText("Version " + CURRENT_VERSION);
    // 检查更新按钮
    connect(ui->btnCheckUpdate, &QPushButton::clicked, this, &MainWindow::onCheckUpdateClicked);

    // === 主界面信号连接 ===
    connect(ui->modelList, &QListWidget::itemClicked, this, &MainWindow::onModelListClicked);
    connect(ui->comboSort, QOverload<int>::of(&QComboBox::currentIndexChanged),this, &MainWindow::onSortIndexChanged);
    connect(ui->comboBaseModel, &QComboBox::currentTextChanged,this, &MainWindow::onFilterBaseModelChanged);
    connect(ui->btnModelsTab, &QPushButton::clicked, this, &MainWindow::onModelsTabButtonClicked);
    connect(ui->btnCollectionsTab, &QPushButton::clicked, this, &MainWindow::onCollectionsTabButtonClicked);
    connect(ui->collectionTree, &QTreeWidget::itemClicked, this, &MainWindow::onCollectionTreeItemClicked);
    // 侧边栏右键菜单
    ui->modelList->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->modelList->setSelectionMode(QAbstractItemView::ExtendedSelection); // 开启 Shift/Ctrl 多选
    connect(ui->modelList, &QListWidget::customContextMenuRequested, this, &MainWindow::onSidebarContextMenu);
    ui->collectionTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->collectionTree, &QTreeWidget::customContextMenuRequested, this, &MainWindow::onCollectionTreeContextMenu);
    ui->collectionTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // 工具栏按钮
    connect(ui->btnOpenUrl, &QPushButton::clicked, this, &MainWindow::onOpenUrlClicked);
    connect(ui->btnScanLocal, &QPushButton::clicked, this, &MainWindow::onScanLocalClicked);
    connect(ui->btnForceUpdate, &QPushButton::clicked, this, &MainWindow::onForceUpdateClicked);
    connect(ui->searchEdit, &QLineEdit::textChanged, this, &MainWindow::onSearchTextChanged);
    // 主页与画廊按钮
    connect(ui->btnHome, &QPushButton::clicked, this, &MainWindow::onHomeButtonClicked);
    connect(ui->homeGalleryList, &QListWidget::itemClicked, this, &MainWindow::onHomeGalleryClicked);
    connect(ui->btnAddCollection, &QPushButton::clicked, this, &MainWindow::onCreateCollection);
    connect(ui->btnGallery, &QPushButton::clicked, this, &MainWindow::onGalleryButtonClicked);

    // === 用户图库页面初始化 ===
    // 1. 初始化 Tag 流式控件，放入 XML 定义好的 scrollAreaTags 中
    tagFlowWidget = new TagFlowWidget();
    tagFlowWidget->setTranslationMap(&translationMap);
    tagFlowWidget->setObjectName("tagFlowContainer");
    tagFlowWidget->setAttribute(Qt::WA_TranslucentBackground);
    ui->scrollAreaTags->viewport()->setAutoFillBackground(false);
    ui->scrollAreaTags->setWidget(tagFlowWidget);
    ui->scrollAreaTags->viewport()->setAutoFillBackground(false);
    connect(ui->scrollAreaTags->verticalScrollBar(), &QScrollBar::valueChanged,
            ui->scrollAreaTags->viewport(), [this](){
                ui->scrollAreaTags->viewport()->update();});
    // 设置右键菜单策略
    ui->listUserImages->setContextMenuPolicy(Qt::CustomContextMenu);
    // 连接右键信号
    connect(ui->listUserImages, &QListWidget::customContextMenuRequested,
            this, &MainWindow::onUserGalleryContextMenu);
    // 2. 双击列表项查看大图 ===
    connect(ui->listUserImages, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item){
        if (!item) return;
        QString path = item->data(ROLE_USER_IMAGE_PATH).toString(); // 取出全路径
        if (!path.isEmpty()) {
            showFullImageDialog(path); // 调用已有的显示大图函数
        }
    });
    // 3. 信号连接
    // 切换 Tab 按钮
    connect(ui->btnShowUserGallery, &QPushButton::clicked, this, &MainWindow::onToggleDetailTab);
    // SD 目录与扫描
    connect(ui->btnSetSdFolder, &QPushButton::clicked, this, &MainWindow::onSetSdFolderClicked);
    connect(ui->btnRescanUser, &QPushButton::clicked, this, &MainWindow::onRescanUserClicked);
    connect(ui->btnTranslate, &QPushButton::toggled, this, [this](bool checked){
        if (checked) {
            // 用户想开启翻译，检查是否有数据
            if (translationMap.isEmpty()) {
                // 1. 临时阻断信号，把勾选状态取消掉（因为开启失败）
                ui->btnTranslate->blockSignals(true);
                ui->btnTranslate->setChecked(false);
                ui->btnTranslate->blockSignals(false);

                // 2. 弹窗提示
                QMessageBox::StandardButton reply;
                reply = QMessageBox::question(this, "未加载翻译",
                                              "尚未加载翻译词表 (CSV)。\n是否现在前往设置页面进行设置？\n\n(格式: 英文,中文)",
                                              QMessageBox::Yes|QMessageBox::No);

                if (reply == QMessageBox::Yes) {
                    ui->rootStack->setCurrentIndex(1); // 跳转到设置页
                    ui->editTransPath->setFocus();
                }
                return;
            }
        }
        // 如果有数据（或者用户关闭翻译），通知控件切换模式
        tagFlowWidget->setShowTranslation(checked);
    });
    // 图片点击
    connect(ui->listUserImages, &QListWidget::itemClicked, this, &MainWindow::onUserImageClicked);
    // Tag 筛选
    connect(tagFlowWidget, &TagFlowWidget::filterChanged, this, &MainWindow::onTagFilterChanged);
    // 2. 右键点击 -> 弹出菜单
    connect(ui->btnFavorite, &QPushButton::customContextMenuRequested, this, [this](const QPoint &pos){
        // 获取当前选中的所有模型
        QList<QListWidgetItem*> selectedItems = ui->modelList->selectedItems();

        // 如果没有多选，但有当前焦点的单选项，也把它加进去
        if (selectedItems.isEmpty() && ui->modelList->currentItem()) {
            selectedItems.append(ui->modelList->currentItem());
        }

        // 只有非空时才弹出
        if (!selectedItems.isEmpty()) {
            // 在按钮下方弹出菜单
            showCollectionMenu(selectedItems, ui->btnFavorite->mapToGlobal(pos));
        }
    });
    connect(ui->btnFavorite, &QPushButton::clicked, this, &MainWindow::onBtnFavoriteClicked);

    // 设置 Splitter
    ui->splitter->setSizes(QList<int>() << 260 << 1000);

    // 默认显示主页 (Page 0)
    ui->rootStack->setCurrentIndex(0);          // 库页面
    ui->mainStack->setCurrentIndex(0);          // 库页面中的主页 (大图网格)
    ui->sidebarStack->setCurrentIndex(1);       // 侧边栏默认显示收藏夹树
    ui->btnCollectionsTab->setChecked(true);    // 确保收藏夹按钮选中

    bgResizeTimer = new QTimer(this);
    bgResizeTimer->setSingleShot(true);
    // 当定时器时间到，执行更新背景函数
    connect(bgResizeTimer, &QTimer::timeout, this, &MainWindow::updateBackgroundImage);

    if (ui->backgroundLabel && ui->scrollAreaWidgetContents) {
        ui->scrollAreaWidgetContents->installEventFilter(this);
        ui->backgroundLabel->setScaledContents(true);
        ui->backgroundLabel->setGeometry(ui->scrollAreaWidgetContents->rect());
    }

    clearDetailView();

    QTimer::singleShot(10, this, [this](){
        ui->statusbar->showMessage("正在扫描本地模型库...");
        loadCollections();
        if (!currentLoraPath.isEmpty()) scanModels(currentLoraPath);
        ui->comboSort->setCurrentIndex(0);
        executeSort();
        refreshCollectionTreeView();
        ui->statusbar->showMessage(QString("加载完成，共 %1 个模型").arg(ui->modelList->count()), 3000);
    });

    loadUserGalleryCache();
}

MainWindow::~MainWindow()
{
    saveGlobalConfig();
    cancelPendingTasks();
    threadPool->waitForDone(500);
    backgroundThreadPool->waitForDone(500);
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
    cancelPendingTasks();
    ui->mainStack->setCurrentIndex(0); // 切换到主页
    ui->modelList->clearSelection();   // 清除侧边栏选中
    ui->collectionTree->clearSelection();
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
            refreshCollectionTreeView();
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

    // === 未分类按钮 ===
    QPushButton *btnUncat = new QPushButton("📦\n未分类");
    btnUncat->setFixedSize(90, 90);
    btnUncat->setProperty("class", "collectionBtn");
    btnUncat->setCheckable(true);
    btnUncat->setChecked(currentCollectionFilter == FILTER_UNCATEGORIZED);
    connect(btnUncat, &QPushButton::clicked, this, [this](){
        currentCollectionFilter = FILTER_UNCATEGORIZED;
        refreshHomeGallery();
        refreshHomeCollectionsUI();
    });
    layout->addWidget(btnUncat);

    // === 3. 添加收藏夹按钮 (带右键功能) ===
    for (auto it = collections.begin(); it != collections.end(); ++it) {
        QString name = it.key();
        if (name == FILTER_UNCATEGORIZED) continue; // 健壮性屏蔽

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
    cancelPendingTasks();
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

    QString searchText = ui->searchEdit->text().trimmed();
    QString targetBaseModel = ui->comboBaseModel->currentText();

    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *sideItem = ui->modelList->item(i);


        int nsfwLevel = sideItem->data(ROLE_NSFW_LEVEL).toInt();
        bool isNSFW = nsfwLevel > optNSFWLevel;
        QString baseName = sideItem->text();
        QString previewPath = sideItem->data(ROLE_PREVIEW_PATH).toString();
        QString filePath = sideItem->data(ROLE_FILE_PATH).toString();
        QString itemBaseModel = sideItem->data(ROLE_FILTER_BASE).toString();

        // --- NSFW 拦截逻辑 ---
        if (optFilterNSFW && isNSFW && optNSFWMode == 0) {
            continue; // 完全不显示模式：直接跳过此模型
        }

        if (!searchText.isEmpty()) {
            if (!baseName.contains(searchText, Qt::CaseInsensitive)) continue;
        }

        if (targetBaseModel != "All") {
            if (itemBaseModel != targetBaseModel) continue;
        }

        if (!currentCollectionFilter.isEmpty()) {
            if (currentCollectionFilter == FILTER_UNCATEGORIZED) {
                // 如果当前选的是“未分类”：
                // 检查这个 baseName 是否存在于任何一个已有的收藏夹 List 中
                bool categorized = false;
                for (auto it = collections.begin(); it != collections.end(); ++it) {
                    if (it.value().contains(baseName)) {
                        categorized = true;
                        break;
                    }
                }
                if (categorized) continue; // 已分类的模型，不显示在“未分类”中
            } else {
                // 正常的收藏夹筛选逻辑
                QStringList list = collections.value(currentCollectionFilter);
                if (!list.contains(baseName)) continue;
            }
        }

        QListWidgetItem *item = new QListWidgetItem();
        item->setToolTip(baseName);
        item->setData(ROLE_FILE_PATH, filePath);
        item->setData(ROLE_PREVIEW_PATH, previewPath);
        item->setData(ROLE_NSFW_LEVEL, nsfwLevel);
        item->setData(ROLE_MODEL_NAME, baseName);

        item->setIcon(placeholderIcon);
        ui->homeGalleryList->addItem(item);

        if (!filePath.isEmpty()) {
            QString pathToSend = previewPath.isEmpty() ? "invalid_path" : previewPath;

            QString taskId = "HOME:" + filePath;

            // 依然使用主 threadPool (因为主页大图需要点击即停，响应优先)
            IconLoaderTask *task = new IconLoaderTask(pathToSend, iconSize, 12, this, taskId);
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

    cancelPendingTasks();
    ui->mainStack->setCurrentIndex(1);

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
        syncTreeSelection(targetPath);
        onModelListClicked(matchItem);
    }
}

// 侧边栏右键菜单
void MainWindow::onSidebarContextMenu(const QPoint &pos)
{
    // 获取当前选中的所有项目
    QList<QListWidgetItem*> selectedItems = ui->modelList->selectedItems();

    // 如果右键点击的位置不在选区内，Qt通常会清除选区并选中新项。
    // 但为了保险，如果 selectedItems 为空，尝试获取点击位置的单项
    if (selectedItems.isEmpty()) {
        QListWidgetItem *item = ui->modelList->itemAt(pos);
        if (item) selectedItems.append(item);
    }

    if (selectedItems.isEmpty()) return;

    // 调用重构后的菜单函数
    showCollectionMenu(selectedItems, ui->modelList->mapToGlobal(pos));
}

void MainWindow::onBtnFavoriteClicked()
{
    // 获取当前选中的模型（支持多选）
    QList<QListWidgetItem*> selectedItems = ui->modelList->selectedItems();
    if (selectedItems.isEmpty()) return;

    QPoint pos = ui->btnFavorite->mapToGlobal(QPoint(0, ui->btnFavorite->height()));
    showCollectionMenu(selectedItems, pos);
}

void MainWindow::onHomeGalleryContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = ui->homeGalleryList->itemAt(pos);
    if (!item) return; // 点击了空白处

    // 构造一个包含当前单项的列表
    QList<QListWidgetItem*> items;
    items.append(item);

    // 复用通用的菜单逻辑
    showCollectionMenu(items, ui->homeGalleryList->mapToGlobal(pos));
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
QIcon MainWindow::getSquareIcon(const QPixmap &srcPix)
{
    if (srcPix.isNull()) return QIcon();

    // 1. 计算裁剪区域 (短边裁剪)
    int side = qMin(srcPix.width(), srcPix.height());
    // X轴居中，Y轴顶端对齐 (适合人物)
    int x = (srcPix.width() - side) / 2;
    int y = 0;

    // 获取原始的正方形裁剪图
    QPixmap square = srcPix.copy(x, y, side, side);

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
            QSize newSize = ui->scrollAreaWidgetContents->size();
            // 只有当尺寸不一致时才去 resize，避免循环触发
            if (ui->backgroundLabel->size() != newSize) {
                ui->backgroundLabel->resize(newSize);
                // 启动防抖更新图片
                bgResizeTimer->start(0); // 稍微增加一点延迟，减少高频模糊计算
            }
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
    // 1. 锁定 UI 更新，防止闪烁
    ui->modelList->setUpdatesEnabled(false);
    ui->modelList->clear();

    ui->comboBaseModel->blockSignals(true);
    ui->comboBaseModel->clear();
    ui->comboBaseModel->addItem("All");

    QSet<QString> foundBaseModels; // 用于去重记录发现的底模

    // 2. 准备文件名过滤器
    QStringList nameFilters;
    nameFilters << "*.safetensors" << "*.pt";

    // 3. 准备目录过滤器 (只看文件，不包含 . 和 ..)
    QDir::Filters dirFilters = QDir::Files | QDir::NoDotAndDotDot;

    // 4. 准备迭代器标志 (是否递归)
    QDirIterator::IteratorFlags iterFlags = QDirIterator::NoIteratorFlags;
    if (optLoraRecursive) {
        iterFlags = QDirIterator::Subdirectories; // 开启递归
    }

    // 5. 初始化迭代器
    // 构造函数签名: QDirIterator(path, nameFilters, filters, flags)
    QDirIterator it(path, nameFilters, dirFilters, iterFlags);

    int scannedCount = 0;

    while (it.hasNext()) {
        it.next();
        QFileInfo fileInfo = it.fileInfo();
        scannedCount++;

        QString baseName = fileInfo.completeBaseName();
        QString fullPath = fileInfo.absoluteFilePath();

        // 获取当前文件所在的目录 (递归模式下可能是子目录)
        QDir currentFileDir = fileInfo.dir();

        // 6. 寻找预览图
        QString previewPath = "";
        QStringList imgExts = {".preview.png", ".png", ".jpg", ".jpeg"};
        for (const QString &ext : imgExts) {
            // 在当前模型文件的同级目录下找图片
            QString tryPath = currentFileDir.absoluteFilePath(baseName + ext);
            if (QFile::exists(tryPath)) {
                previewPath = tryPath;
                break;
            }
        }

        // 7. 创建列表项
        QListWidgetItem *item = new QListWidgetItem(baseName);
        item->setToolTip(fullPath);
        item->setData(ROLE_MODEL_NAME, baseName);
        item->setData(ROLE_FILE_PATH, fullPath);
        item->setData(ROLE_PREVIEW_PATH, previewPath);

        QString jsonPath = currentFileDir.filePath(baseName + ".json");
        preloadItemMetadata(item, jsonPath);

        int nsfwLevel = item->data(ROLE_NSFW_LEVEL).toInt();
        bool isNSFW = nsfwLevel > optNSFWLevel;

        if (optFilterNSFW && isNSFW && optNSFWMode == 0) {
            // 如果开启过滤 + 是NSFW + 模式为隐藏(0) -> 直接删除item并跳过
            delete item;
            continue;
        }

        QString civitaiName = item->data(ROLE_CIVITAI_NAME).toString();
        if (optUseCivitaiName && !civitaiName.isEmpty()) {
            item->setText(civitaiName);
        } else {
            item->setText(baseName); // 默认使用文件名
        }

        item->setIcon(placeholderIcon);

        // 9. 处理底模过滤器
        QString baseModel = item->data(ROLE_FILTER_BASE).toString();
        if (!baseModel.isEmpty() && !foundBaseModels.contains(baseModel)) {
            foundBaseModels.insert(baseModel);
            ui->comboBaseModel->addItem(baseModel);
        }

        ui->modelList->addItem(item);
        if (!previewPath.isEmpty()) {
            // 【修改】添加 "SIDEBAR:" 前缀
            QString taskId = "SIDEBAR:" + fullPath;

            // 使用 backgroundThreadPool (静默加载)
            IconLoaderTask *task = new IconLoaderTask(previewPath, 64, 8, this, taskId);
            task->setAutoDelete(true);
            backgroundThreadPool->start(task);
        }
    }

    // 10. 恢复 UI 更新
    ui->statusbar->showMessage(QString("扫描完成，共 %1 个模型").arg(ui->modelList->count()));
    ui->comboBaseModel->blockSignals(false);
    ui->modelList->setUpdatesEnabled(true);

    // 11. 刷新主页大图视图
    executeSort();
    refreshHomeGallery();
    // 刷新收藏夹树状视图
    refreshCollectionTreeView();
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
    downloadQueue.clear();
    isDownloading = false;

    if (meta.images.isEmpty()) {
        ui->layoutGallery->addWidget(new QLabel("No preview images."));
    } else {
        // === 核心修复：获取绝对标准化的模型目录 ===
        QFileInfo modelFileInfo(meta.filePath);
        QString modelDir = modelFileInfo.absolutePath();
        QString currentStandardBaseName;

        QListWidgetItem *listItem = ui->modelList->currentItem();
        if (listItem) {
            currentStandardBaseName = listItem->data(ROLE_MODEL_NAME).toString();
        }
        if (currentStandardBaseName.isEmpty()) {
            currentStandardBaseName = modelFileInfo.completeBaseName();
        }

        for (int i = 0; i < meta.images.count(); ++i) {
            const ImageInfo &img = meta.images[i];

            // === NSFW 过滤逻辑 ===
            bool isNsfw = (img.nsfwLevel > optNSFWLevel);
            if (optFilterNSFW && isNsfw) {
                if (optNSFWMode == 0) {
                    continue; // 模式 0：直接跳过这张图，不生成按钮
                }
            }

            QPushButton *thumbBtn = new QPushButton();
            thumbBtn->setFixedSize(100, 150);
            thumbBtn->setCheckable(true);
            thumbBtn->setAutoExclusive(true);
            thumbBtn->setCursor(Qt::PointingHandCursor);
            thumbBtn->setProperty("class", "galleryThumb");
            thumbBtn->setProperty("isNSFW", isNsfw);

            // 计算文件名
            QString suffix = (i == 0) ? ".preview.png" : QString(".preview.%1.png").arg(i);
            // === 强制使用 QFileInfo 再次标准化路径字符串 ===
            QString rawPath = QDir(modelDir).filePath(currentStandardBaseName + suffix);
            QString strictLocalPath = QFileInfo(rawPath).absoluteFilePath();

            // 绑定标准化后的路径
            thumbBtn->setProperty("fullImagePath", strictLocalPath);
            thumbBtn->installEventFilter(this);

            if (QFile::exists(strictLocalPath)) {
                thumbBtn->setText("Loading...");
                IconLoaderTask *task = new IconLoaderTask(strictLocalPath, 100, 0, this, strictLocalPath, true);
                task->setAutoDelete(true);
                threadPool->start(task);
            } else {
                if (i == 0) {
                    // 封面图正在下载
                    thumbBtn->setText("Downloading...");
                } else {
                    // 后续图进入队列，确保传入的是 strictLocalPath
                    thumbBtn->setText("Queueing...");
                    enqueueDownload(img.url, strictLocalPath, thumbBtn);
                }
            }

            connect(thumbBtn, &QPushButton::clicked, this, [this, i](){
                onGalleryImageClicked(i);
            });
            ui->layoutGallery->addWidget(thumbBtn);
        }
        ui->layoutGallery->addStretch();

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
    QFileInfo fi(meta.filePath);
    QDateTime addedTime = fi.birthTime();
    if(!addedTime.isValid()) addedTime = fi.lastModified();
    QString addedStr = addedTime.toString("yyyy-MM-dd");
    ui->lblFileInfo->setText(QString("Filename: %1\nSize: %2 MB\nSHA256: %3\nAdded: %4")
                                 .arg(meta.fileNameServer.isEmpty() ? meta.fileName : meta.fileNameServer)
                                 .arg(meta.fileSizeMB, 0, 'f', 1)
                                 .arg(meta.sha256.left(10) + "...")
                                 .arg(addedStr));

    QTimer::singleShot(0, this, [this, meta](){
        ui->scrollAreaWidgetContents->adjustSize();
        transitionToImage(meta.previewPath);
    });
}

void MainWindow::onGalleryImageClicked(int index)
{
    if (index < 0 || index >= currentMeta.images.count()) return;

    const ImageInfo &img = currentMeta.images[index];

    // 1. 更新 Prompt 显示
    ui->textImgPrompt->setPlainText(img.prompt.isEmpty() ? "No positive prompt." : img.prompt);
    ui->textImgNegPrompt->setPlainText(img.negativePrompt.isEmpty() ? "No negative prompt." : img.negativePrompt);

    // 更新参数行
    QString params = QString("Sampler: <span style='color:white'>%1</span> | Steps: <span style='color:white'>%2</span> | CFG: <span style='color:white'>%3</span> | Seed: <span style='color:white'>%4</span>")
                         .arg(img.sampler)
                         .arg(img.steps)
                         .arg(img.cfgScale)
                         .arg(img.seed);
    ui->lblImgParams->setText(params);

    // === 动态获取模型所在的子目录 ===
    QString currentBaseName;
    QString modelDir; // 用于存储该模型实际所在的文件夹路径

    QListWidgetItem *item = ui->modelList->currentItem();
    if (item) {
        // A. 获取模型名称标识
        currentBaseName = item->data(ROLE_MODEL_NAME).toString();
        if (currentBaseName.isEmpty()) currentBaseName = item->text();

        // B. 获取模型文件所在的绝对目录 (支持子文件夹的关键)
        QString fullModelPath = item->data(ROLE_FILE_PATH).toString();
        if (!fullModelPath.isEmpty()) {
            modelDir = QFileInfo(fullModelPath).absolutePath();
        }
    } else {
        // 兜底逻辑：如果侧边栏没选中，尝试从 currentMeta 推断
        currentBaseName = currentMeta.name;
        modelDir = QFileInfo(currentMeta.filePath).absolutePath();
    }

    // 如果因为某种异常没拿到目录，则回退到 Lora 根目录
    if (modelDir.isEmpty()) {
        modelDir = currentLoraPath;
    }

    // 2. 寻找本地图片路径 (使用解析出的 modelDir 而不是全局 currentLoraPath)
    QString localPath = findLocalPreviewPath(modelDir, currentBaseName, currentMeta.fileNameServer, index);

    // 3. 执行过渡
    if (QFile::exists(localPath)) {
        transitionToImage(localPath);
    } else {
        qDebug() << "[Debug] Preview image not found at:" << localPath;
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
}

// ---------------------------------------------------------
// 文件与网络部分
// ---------------------------------------------------------
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
    executeSort();
}

// 点击列表项
void MainWindow::onModelListClicked(QListWidgetItem *item) {
    if (!item) return;

    cancelPendingTasks();

    // === 恢复 UI 状态 ===
    ui->btnForceUpdate->setVisible(true);
    ui->btnFavorite->setVisible(true);
    ui->btnShowUserGallery->setVisible(true);
    ui->btnShowUserGallery->setEnabled(true);

    QString filePath = item->data(ROLE_FILE_PATH).toString();
    QString modelDir = QFileInfo(filePath).absolutePath();
    ui->modelList->setProperty("current_model_dir", modelDir);

    if (currentMeta.filePath == filePath && !currentMeta.name.isEmpty()) {
        // 如果当前不在详情页（比如在主页），则只切换页面，不重新加载数据
        if (ui->mainStack->currentIndex() != 1) {
            ui->mainStack->setCurrentIndex(1);
        }
        // 无论是否切换了页面，都直接返回，不再执行后续繁重的 JSON 解析
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
    QString baseName = item->data(ROLE_MODEL_NAME).toString();

    ModelMeta meta;
    meta.name = baseName;
    meta.filePath = filePath;
    meta.previewPath = previewPath;

    // 2. 尝试读取本地 JSON
    bool hasLocalData = readLocalJson(modelDir, baseName, meta);

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
        ui->modelList->setProperty("current_processing_path", filePath);

        // === 启动后台线程计算 Hash ===
        // 使用 QtConcurrent::run 把耗时函数丢到后台
        QFuture<QString> future = QtConcurrent::run([this, filePath]() {
            return calculateFileHash(filePath); // 这里是你原来的耗时函数
        });
        hashWatcher->setFuture(future);
    }

    // 如果当前正处于 "本地返图" 页面 (Index 1)，立即刷新数据
    if (ui->detailContentStack->currentIndex() == 1) {
        // 使用当前选中的模型名进行扫描
        scanForUserImages(baseName);
    } else {
        // 如果在主详情页，先清空返图页的旧数据，防止用户等会儿切过去看到上一个模型的图
        ui->listUserImages->clear();
        ui->textUserPrompt->clear();
        tagFlowWidget->setData({});
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
    QString modelDir = ui->modelList->property("current_model_dir").toString();
    QString urlStr = QString("https://civitai.com/api/v1/model-versions/by-hash/%1").arg(hash);
    QString filePath = ui->modelList->property("current_processing_path").toString();
    QNetworkRequest request((QUrl(urlStr)));

    request.setHeader(QNetworkRequest::UserAgentHeader, currentUserAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = netManager->get(request);
    // 将本地文件名绑定到 Reply 对象上，确保回调时知道是哪个模型
    reply->setProperty("localBaseName", localBaseName);
    reply->setProperty("modelDir", modelDir);
    reply->setProperty("localFilePath", filePath);
    reply->setProperty("filePath", filePath);

    connect(reply, &QNetworkReply::finished, this, [this, reply](){
        this->onApiMetadataReceived(reply);
    });
}

// 解析 JSON
bool MainWindow::readLocalJson(const QString &dirPath, const QString &baseName, ModelMeta &meta)
{
    if (dirPath.isEmpty()) return false;
    QString jsonPath = QDir(dirPath).filePath(baseName + ".json");

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
        imgInfo.nsfwLevel = imgObj["nsfwLevel"].toInt();
        imgInfo.nsfw = (imgInfo.nsfwLevel > 1);

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

    QString bestPreviewPath = findLocalPreviewPath(dirPath, baseName, meta.fileNameServer, 0);

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
    QString modelDir = reply->property("modelDir").toString();
    QString filePath = reply->property("filePath").toString();
    reply->deleteLater();
    ui->btnForceUpdate->setEnabled(true);

    if (reply->error() != QNetworkReply::NoError) {
        clearLayout(ui->layoutTriggerStack); // 清空触发词区域

        // === 在标题栏醒目显示错误 ===
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
    QString fullName = modelRealName + " [" + versionName + "]"; // 组合名称
    meta.name = modelRealName + " [" + versionName + "]";
    meta.filePath = filePath;

    // 更新 UI 列表项
    // 找到对应的 Item (可能通过 localBaseName 查找)
    for(int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (item->data(ROLE_MODEL_NAME).toString() == localBaseName) {
            item->setData(ROLE_CIVITAI_NAME, fullName); // 更新缓存的名称

            // 如果开启了选项，立即更新显示文本
            if (optUseCivitaiName) {
                item->setText(fullName);
            }
            break;
        }
    }

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
        imgInfo.nsfwLevel = imgObj["nsfwLevel"].toInt();
        imgInfo.nsfw = (imgInfo.nsfwLevel > 1);

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

    if (!meta.images.isEmpty()) {
        // 强制使用本地文件名构造图片路径，解决重名和冲突问题
        QString savePath = QDir::cleanPath(QDir(modelDir).filePath(localBaseName + ".preview.png"));

        if (!QFile::exists(savePath)) {
            QNetworkRequest req((QUrl(meta.images[0].url)));

            req.setHeader(QNetworkRequest::UserAgentHeader, currentUserAgent);
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

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
    saveLocalMetadata(modelDir, localBaseName, root);

    currentMeta = meta; // 缓存到成员变量
    updateDetailView(meta);
}

void MainWindow::onImageDownloaded(QNetworkReply *reply)
{
    // 1. 获取上下文
    QString localBaseName = reply->property("localBaseName").toString();
    QString savePath = QFileInfo(reply->property("savePath").toString()).absoluteFilePath();
    reply->deleteLater();


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

        QIcon newIcon = getSquareIcon(QPixmap(savePath)); // 或者 getFitIcon
        QIcon fitIcon = getFitIcon(savePath);

        for(int i = 0; i < ui->modelList->count(); ++i) {
            QListWidgetItem *item = ui->modelList->item(i);
            // 必须比对 UserRole (即 baseName) 或 FILE_PATH
            if (item->data(ROLE_MODEL_NAME).toString() == localBaseName) {
                item->setData(ROLE_PREVIEW_PATH, savePath); // 更新数据
                item->setIcon(newIcon); // 刷新图标
            }
        }

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
        
        if (ui->layoutGallery) {
            for (int k = 0; k < ui->layoutGallery->count(); ++k) {
                if (QLayoutItem *li = ui->layoutGallery->itemAt(k)) {
                    if (QPushButton *btn = qobject_cast<QPushButton*>(li->widget())) {
                        QString btnPath = QFileInfo(btn->property("fullImagePath").toString()).absoluteFilePath();
                        if (btnPath == savePath) {
                            btn->setIcon(fitIcon);
                            btn->setIconSize(QSize(90, 135));
                            btn->setText(""); // 清除文字
                        }
                    }
                }
            }
        }

        QListWidgetItem *currentItem = ui->modelList->currentItem();
        if (currentItem && currentItem->data(ROLE_MODEL_NAME).toString() == localBaseName) {
            if (savePath.endsWith(".preview.png")) {
                currentHeroPath = "";
                transitionToImage(savePath);
            }
        }
    }
}

void MainWindow::saveLocalMetadata(const QString &modelDir, const QString &baseName, const QJsonObject &data) {
    if (modelDir.isEmpty()) return;
    QString savePath = QDir(modelDir).filePath(baseName + ".json");
    QFile file(savePath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(data).toJson());
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

    req.setHeader(QNetworkRequest::UserAgentHeader, currentUserAgent);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = netManager->get(req);
    QPointer<QPushButton> safeBtn = button;

    connect(reply, &QNetworkReply::finished, this, [this, reply, savePath, safeBtn]() {
        reply->deleteLater();

        // 检查网络错误
        if (reply->error() != QNetworkReply::NoError) {
            if (safeBtn) safeBtn->setText("Error");
            qDebug() << "Download error:" << reply->errorString();
            return;
        }

        QByteArray data = reply->readAll();

        // 再次检查数据是否为空 (防止 User-Agent 还是被拦截的情况)
        if (data.isEmpty()) {
            if (safeBtn) safeBtn->setText("Empty");
            return;
        }

        QFile file(savePath);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(data);
            file.close();

            // 启动异步加载图标任务
            IconLoaderTask *task = new IconLoaderTask(savePath, 100, 0, this, savePath, true);
            task->setAutoDelete(true);
            threadPool->start(task);

            if (safeBtn) {
                safeBtn->setText(""); // 清除 Loading 文字
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

void MainWindow::onIconLoaded(const QString &id, const QImage &image)
{
    // =========================================================
    // 1. 解析任务来源 (Parsing ID)
    // =========================================================
    QString filePath = id;
    bool isSidebarTask = false;
    bool isHomeTask = false;

    // 判断是否有特定前缀
    if (id.startsWith("SIDEBAR:")) {
        isSidebarTask = true;
        filePath = id.mid(8); // 去掉 "SIDEBAR:" 前缀 (长度为8)
    } else if (id.startsWith("HOME:")) {
        isHomeTask = true;
        filePath = id.mid(5); // 去掉 "HOME:" 前缀 (长度为5)
    } else {
        // 兼容其他没有加前缀的情况（比如用户返图、详情页点击），默认都允许更新
        isSidebarTask = true;
        isHomeTask = true;
    }

    // =========================================================
    // 2. 准备图片和 NSFW 处理逻辑
    // =========================================================
    QPixmap originalPix = QPixmap::fromImage(image);
    QIcon originalIcon(originalPix); // 默认图标

    // 延迟模糊计算 (Lambda)
    QPixmap blurredPix;
    auto getDisplayPix = [&](bool isNSFW) {
        if (optFilterNSFW && isNSFW && optNSFWMode == 1) {
            if (blurredPix.isNull()) blurredPix = applyNSFWBlur(originalPix);
            return blurredPix;
        }
        return originalPix;
    };

    // =========================================================
    // 3. 更新主页列表 (Home Gallery) - 仅限 HOME 或 通用任务
    // =========================================================
    if (isHomeTask) {
        for(int i = 0; i < ui->homeGalleryList->count(); ++i) {
            QListWidgetItem *item = ui->homeGalleryList->item(i);
            // 匹配路径
            if (item->data(ROLE_FILE_PATH).toString() == filePath) {
                // 处理 NSFW
                bool isNSFW = item->data(ROLE_NSFW_LEVEL).toInt() > optNSFWLevel;
                if (optFilterNSFW && isNSFW && optNSFWMode == 1) {
                    if (blurredPix.isNull()) blurredPix = applyNSFWBlur(originalPix);
                    // 主页使用圆角遮罩
                    QPixmap roundedBlur = applyRoundedMask(blurredPix, 12);
                    item->setIcon(QIcon(roundedBlur));
                } else {
                    item->setIcon(QIcon(originalPix));
                }
            }
        }
    }

    // =========================================================
    // 4. 更新侧边栏 (Sidebar List & Tree) - 仅限 SIDEBAR 或 通用任务
    // =========================================================
    if (isSidebarTask) {
        // --- A. 更新侧边栏列表 ---
        for(int i = 0; i < ui->modelList->count(); ++i) {
            QListWidgetItem *item = ui->modelList->item(i);
            if (item->data(ROLE_FILE_PATH).toString() == filePath) {
                bool isNSFW = item->data(ROLE_NSFW_LEVEL).toInt() > optNSFWLevel;
                if (optFilterNSFW && isNSFW && optNSFWMode == 1) {
                    if (blurredPix.isNull()) blurredPix = applyNSFWBlur(originalPix);
                    // 侧边栏使用 getSquareIcon 处理样式 (方形+内边距)
                    QPixmap roundedBlur = applyRoundedMask(blurredPix, 12);
                    item->setIcon(getSquareIcon(roundedBlur));
                } else {
                    item->setIcon(getSquareIcon(originalPix));
                }
            }
        }

        // --- B. 更新收藏夹树状图 ---
        for(int i = 0; i < ui->collectionTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *parent = ui->collectionTree->topLevelItem(i);
            // 遍历子节点 (模型)
            for(int j = 0; j < parent->childCount(); ++j) {
                QTreeWidgetItem *child = parent->child(j);
                if (child->data(0, ROLE_FILE_PATH).toString() == filePath) {
                    bool isNSFW = child->data(0, ROLE_NSFW_LEVEL).toInt() > optNSFWLevel;
                    if (optFilterNSFW && isNSFW && optNSFWMode == 1) {
                        if (blurredPix.isNull()) blurredPix = applyNSFWBlur(originalPix);
                        QPixmap roundedBlur = applyRoundedMask(blurredPix, 12);
                        child->setIcon(0, getSquareIcon(roundedBlur));
                    } else {
                        child->setIcon(0, getSquareIcon(originalPix));
                    }
                }
            }
        }
    }

    // =========================================================
    // 5. 更新详情页、返图、Hero
    // =========================================================
    // 逻辑：如果这是一个纯粹的 "SIDEBAR" 任务 (通常是64px小图)，
    // 我们不希望它更新 Detail(100px+) 或 Hero(大图)，因为会变糊。
    // 如果是 "HOME" (180px) 或 通用 (原图)，则允许更新。

    bool allowHighResUpdate = !id.startsWith("SIDEBAR:");

    if (allowHighResUpdate) {
        // --- A. 详情页预览列表 (Detail Gallery) ---
        QLayout *layout = ui->layoutGallery;
        if (layout) {
            for (int i = 0; i < layout->count(); ++i) {
                QLayoutItem *item = layout->itemAt(i);
                if (item->widget()) {
                    QPushButton *btn = qobject_cast<QPushButton*>(item->widget());
                    if (btn) {
                        if (btn->property("fullImagePath").toString() == filePath) {
                            bool isNSFW = btn->property("isNSFW").toBool();
                            QPixmap p = getDisplayPix(isNSFW);
                            btn->setIcon(QIcon(p));
                            btn->setIconSize(QSize(90, 135));
                            btn->setText("");
                        }
                    }
                }
            }
        }

        // --- B. 用户返图列表 (User Gallery) ---
        if (ui->listUserImages) {
            for (int i = 0; i < ui->listUserImages->count(); ++i) {
                QListWidgetItem *item = ui->listUserImages->item(i);
                if (item->data(ROLE_USER_IMAGE_PATH).toString() == filePath) {
                    item->setIcon(originalIcon);
                }
            }
        }

        // --- C. Hero 大图过渡 ---
        if (filePath == currentMeta.previewPath) {
            // 只有当路径匹配且当前没有显示这张图时才刷新
            if (currentHeroPath != filePath) {
                transitionToImage(filePath);
            }
        }
    }
}

QString MainWindow::findLocalPreviewPath(const QString &dirPath, const QString &currentBaseName, const QString &serverFileName, int imgIndex)
{
    if (dirPath.isEmpty()) return "";
    QDir dir(dirPath);
    QString suffix = (imgIndex == 0) ? ".preview.png" : QString(".preview.%1.png").arg(imgIndex);
    return QFileInfo(dir.filePath(currentBaseName + suffix)).absoluteFilePath();
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

    if (!currentHeroPixmap.isNull()) {
        currentBlurredBgPix = applyBlurToImage(currentHeroPixmap.toImage(), targetSize, heroSize);
        ui->backgroundLabel->setPixmap(currentBlurredBgPix);
    } else if (!currentHeroPath.isEmpty() && QFile::exists(currentHeroPath)) {
        // 如果缓存丢了但有路径，重新读图生成
        QImage img(currentHeroPath);
        currentBlurredBgPix = applyBlurToImage(img, targetSize, heroSize);
        ui->backgroundLabel->setPixmap(currentBlurredBgPix);
    } else {
        ui->backgroundLabel->clear();
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
        QString modelName = item->data(ROLE_MODEL_NAME).toString();
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

    refreshCollectionTreeView();
}

void MainWindow::showCollectionMenu(const QList<QListWidgetItem*> &items, const QPoint &globalPos)
{
    if (items.isEmpty()) return;

    QMenu menu(this);

    // 1. 标题逻辑
    if (items.count() == 1) {
        QListWidgetItem *first = items.first();
        QString name = first->text();

        if (name.isEmpty()) {
            // 如果 text 为空（主页大图模式），尝试获取 Civitai 名
            name = first->data(ROLE_CIVITAI_NAME).toString();
            // 如果 Civitai 名也为空，获取文件名
            if (name.isEmpty()) {
                name = first->data(ROLE_MODEL_NAME).toString();
            }
        }

        if (name.length() > 20) name = name.left(18) + "..";
        QAction *titleAct = menu.addAction(name);
        titleAct->setEnabled(false);
    } else {
        QAction *titleAct = menu.addAction(QString("已选中 %1 个模型").arg(items.count()));
        titleAct->setEnabled(false);
    }

    menu.addSeparator();

    // 辅助 Lambda：获取 items 对应的所有 BaseName (用于收藏夹数据存储)
    // 收藏夹系统始终使用 ROLE_MODEL_NAME (文件名) 作为 Key，不受显示名称影响
    QStringList targetBaseNames;
    for(auto *item : items) {
        targetBaseNames.append(item->data(ROLE_MODEL_NAME).toString());
    }

    // =========================================================
    // 2. "从收藏夹移除..."
    // =========================================================
    QMenu *removeMenu = menu.addMenu("从指定收藏夹移除...");
    bool canRemoveAny = false;

    for (auto it = collections.begin(); it != collections.end(); ++it) {
        QString colName = it.key();
        if (colName == FILTER_UNCATEGORIZED) continue;

        // 检查选中的模型中，是否有任何一个在这个收藏夹里
        // 逻辑：只要有一个在，就允许点击移除（移除操作只移除在里面的）
        int countInCol = 0;
        for (const QString &bn : targetBaseNames) {
            if (it.value().contains(bn)) countInCol++;
        }

        if (countInCol > 0) {
            canRemoveAny = true;
            QString actionText = QString("%1 (%2)").arg(colName).arg(countInCol);
            QAction *actRemove = removeMenu->addAction(actionText);

            connect(actRemove, &QAction::triggered, this, [this, colName, targetBaseNames](){
                int removedCount = 0;
                for (const QString &bn : targetBaseNames) {
                    if (collections[colName].contains(bn)) {
                        collections[colName].removeAll(bn);
                        removedCount++;
                    }
                }
                saveCollections();
                refreshHomeGallery();
                refreshCollectionTreeView();
                ui->statusbar->showMessage(QString("已从 %1 移除 %2 个模型").arg(colName).arg(removedCount), 2000);
            });
        }
    }
    if (!canRemoveAny) removeMenu->setEnabled(false);

    // =========================================================
    // 3. "添加至收藏夹..."
    // =========================================================
    QMenu *addMenu = menu.addMenu("添加至收藏夹...");

    for (auto it = collections.begin(); it != collections.end(); ++it) {
        QString colName = it.key();
        if (colName == FILTER_UNCATEGORIZED) continue;

        QAction *action = addMenu->addAction(colName);
        action->setCheckable(true);

        // 状态检查逻辑：
        // - 如果所有选中项都在该收藏夹 -> Checked
        // - 如果部分在 -> (可选: PartiallyChecked，这里简单处理为 Unchecked)
        // - 如果都不在 -> Unchecked
        bool allIn = true;
        for (const QString &bn : targetBaseNames) {
            if (!it.value().contains(bn)) {
                allIn = false;
                break;
            }
        }
        action->setChecked(allIn);

        connect(action, &QAction::triggered, this, [this, colName, targetBaseNames, action](){
            bool isAdding = action->isChecked(); // 触发后的状态
            int count = 0;

            if (isAdding) {
                // 批量添加
                for (const QString &bn : targetBaseNames) {
                    if (!collections[colName].contains(bn)) {
                        collections[colName].append(bn);
                        count++;
                    }
                }
                ui->statusbar->showMessage(QString("已将 %1 个模型加入 %2").arg(count).arg(colName), 2000);
            } else {
                // 批量移除（取消勾选）
                for (const QString &bn : targetBaseNames) {
                    if (collections[colName].contains(bn)) {
                        collections[colName].removeAll(bn);
                        count++;
                    }
                }
                ui->statusbar->showMessage(QString("已从 %1 移除 %2 个模型").arg(colName).arg(count), 2000);
            }
            saveCollections();

            // 如果影响了当前视图，刷新
            if (currentCollectionFilter == colName) refreshHomeGallery();
            refreshCollectionTreeView();
        });
    }

    addMenu->addSeparator();
    QAction *newAction = addMenu->addAction("新建收藏夹...");
    connect(newAction, &QAction::triggered, this, [this, targetBaseNames](){
        bool ok;
        QString text = QInputDialog::getText(this, "新建", "名称:", QLineEdit::Normal, "", &ok);
        if(ok && !text.isEmpty()) {
            if(!collections.contains(text)) {
                // 直接将选中的模型全部加入新收藏夹
                collections[text] = targetBaseNames;
                saveCollections();
                refreshHomeCollectionsUI();
                refreshCollectionTreeView();
                ui->statusbar->showMessage(QString("新建收藏夹并加入 %1 个模型").arg(targetBaseNames.count()), 2000);
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
    item->setData(ROLE_NSFW_LEVEL, 1);

    // === 读取本地文件时间 (下载/添加时间) ===
    QString filePath = item->data(ROLE_FILE_PATH).toString();
    QFileInfo fi(filePath);
    QDateTime birthTime = fi.birthTime(); // 获取创建时间
    // 在某些系统(如Linux部分文件系统) birthTime 可能无效，回退到 lastModified
    if (!birthTime.isValid()) {
        birthTime = fi.lastModified();
    }
    item->setData(ROLE_SORT_ADDED, birthTime.toMSecsSinceEpoch());
    // ============================================

    QFile file(jsonPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        // 如果没有 JSON，尝试用文件修改时间作为日期
        QFileInfo fi(item->data(ROLE_FILE_PATH).toString());
        item->setData(ROLE_SORT_DATE, fi.lastModified().toMSecsSinceEpoch());
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    QJsonObject root = doc.object();

    // 读取模型真实名称
    QString modelName = root["model"].toObject()["name"].toString();
    QString versionName = root["name"].toString();
    if (!modelName.isEmpty()) {
        QString fullName = modelName;
        if (!versionName.isEmpty()) fullName += " [" + versionName + "]";
        item->setData(ROLE_CIVITAI_NAME, fullName); // 存入 UserRole
    }

    // NSFW
    int coverLevel = 1; // 默认 Safe

    QJsonArray images = root["images"].toArray();
    if (!images.isEmpty()) {
        // 优先：读取 images[0] 的 nsfwLevel
        // 因为侧边栏和主页显示的都是这张图，我们只关心这张图是否违规
        QJsonObject coverObj = images[0].toObject();
        if (coverObj.contains("nsfwLevel")) {
            coverLevel = coverObj["nsfwLevel"].toInt();
        }
        // 兼容旧数据：有的旧 JSON image 里只有 nsfw (None/Soft/Mature/X)
        else if (coverObj.contains("nsfw")) {
            QString val = coverObj["nsfw"].toString().toLower();
            if (val == "x" || val == "mature") coverLevel = 16;
            else if (val == "soft") coverLevel = 2;
            else coverLevel = 1;
        }
    }
    else {
        // 后备：如果 images 数组为空（极少见），才回退到读取整个模型的等级
        if (root.contains("nsfwLevel")) coverLevel = root["nsfwLevel"].toInt();
        else if (root["nsfw"].toBool()) coverLevel = 16;
    }

    // 存入 Item，供后续判断
    item->setData(ROLE_NSFW_LEVEL, coverLevel);

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
    // 0: Name, 1: Date(New), 2: Downloads, 3: Likes, 4: Date Added
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
            case 0: // Name (A-Z)
            {
                QString nameA = a->text();
                QString nameB = b->text();
                return collator.compare(nameA, nameB) < 0;
            }

            case 1: // Date (Newest First -> Descending)
                return a->data(ROLE_SORT_DATE).toLongLong() > b->data(ROLE_SORT_DATE).toLongLong();

            case 2: // Downloads (High -> Descending)
                return a->data(ROLE_SORT_DOWNLOADS).toInt() > b->data(ROLE_SORT_DOWNLOADS).toInt();

            case 3: // Likes (High -> Descending)
                return a->data(ROLE_SORT_LIKES).toInt() > b->data(ROLE_SORT_LIKES).toInt();

            case 4: // Date Added (Local Created At -> Descending)
                return a->data(ROLE_SORT_ADDED).toLongLong() > b->data(ROLE_SORT_ADDED).toLongLong();

            default:
                return false;
            }
        }
    );

    // 3. 放回 ListWidget
    for(auto *item : items) {
        ui->modelList->addItem(item);
    }

    // 4. 同步刷新主页
    onSearchTextChanged(ui->searchEdit->text());
}

void MainWindow::onFilterBaseModelChanged(const QString &text) {
    onSearchTextChanged(ui->searchEdit->text());
}

// 静态函数，运行在后台线程
ImageLoadResult MainWindow::processImageTask(const QString &path)
{
    ImageLoadResult result;
    result.path = path;

    QImageReader reader(path);
    reader.setAutoTransform(true);
    result.originalImg = reader.read();
    result.valid = !result.originalImg.isNull();
    return result;
}

void MainWindow::transitionToImage(const QString &path)
{
    //if (path == currentHeroPath && transitionAnim->state() != QAbstractAnimation::Running) return;
    // 下面这行替换了上面的，修复了切换模型导致的动画闪烁问题，不过尚不清楚有无其他bug
    if (path == currentHeroPath) {
        return;
    }

    // qDebug() << "Transition to:" << path;

    currentHeroPath = path;

    // 如果正在动画中，立即停止并将状态快进到“当前显示的是上一张图”
    if (transitionAnim->state() == QAbstractAnimation::Running) {
        transitionAnim->stop();
        // 如果 next 已经准备好了，就把它作为 current，以此为基础过渡到最新的 new
        if (!nextHeroPixmap.isNull()) {
            currentHeroPixmap = nextHeroPixmap;
            currentBlurredBgPix = nextBlurredBgPix;
        }
    }

    // 清理 Watcher
    if (imageLoadWatcher->isRunning()) {
        // 虽然 cancel 不能杀线程，但能断开一部分连接，配合上面的 path 校验双重保险
        imageLoadWatcher->cancel();
    }

    // 重置动画参数
    nextHeroPixmap = QPixmap();
    nextBlurredBgPix = QPixmap();
    transitionOpacity = 0.0;

    // 5. 根据路径执行
    if (path.isEmpty()) {
        transitionAnim->start(); // 淡出到黑
    } else {
        QFuture<ImageLoadResult> future = QtConcurrent::run(&MainWindow::processImageTask, path);
        imageLoadWatcher->setFuture(future);
    }
}

QPixmap MainWindow::applyBlurToImage(const QImage &srcImg, const QSize &bgSize, const QSize &heroSize)
{
    if (srcImg.isNull()) return QPixmap();

    QPixmap tempPix;

    // === 修改点：根据设置决定是否缩小 ===
    if (optDownscaleBlur) {
        // 使用配置的缩小尺寸
        tempPix = QPixmap::fromImage(srcImg.scaledToWidth(optBlurProcessWidth, Qt::SmoothTransformation));
    } else {
        // 不缩小，直接使用原图（注意：这在模糊半径较大时非常耗时）
        tempPix = QPixmap::fromImage(srcImg);
    }

    // 2. 高斯模糊
    QGraphicsBlurEffect *blur = new QGraphicsBlurEffect;
    blur->setBlurRadius(optBlurRadius);
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

// 1. 入队函数
void MainWindow::enqueueDownload(const QString &url, const QString &savePath, QPushButton *btn)
{
    DownloadTask task;
    task.url = url;
    task.savePath = savePath;
    task.button = btn;

    downloadQueue.enqueue(task);

    // 如果当前没有在下载，立即开始处理
    if (!isDownloading) {
        processNextDownload();
    }
}

// 2. 队列处理函数 (核心：一张张下)
void MainWindow::processNextDownload()
{
    if (downloadQueue.isEmpty()) {
        isDownloading = false;
        return;
    }

    isDownloading = true;
    DownloadTask task = downloadQueue.dequeue();

    if (task.button.isNull()) {
        processNextDownload();
        return;
    }

    // 设置按钮状态
    task.button->setText("Waiting...");

    QString cleanedSavePath = QFileInfo(task.savePath).absoluteFilePath();

    QNetworkRequest req((QUrl(task.url)));
    req.setHeader(QNetworkRequest::UserAgentHeader, currentUserAgent);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = netManager->get(req);

    // 我们不需要 currentDetailReplies 来管理了，因为这是串行的，
    // 如果你希望切页面时中断队列，可以清空 downloadQueue 并 abort 当前 reply
    // 这里简单起见，让它在后台默默跑完当前这一张

    connect(reply, &QNetworkReply::finished, this, [this, reply, task, cleanedSavePath](){
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (task.button) task.button->setText("Error");
            QTimer::singleShot(500, this, &MainWindow::processNextDownload);
            return;
        }

        QByteArray data = reply->readAll();
        if (!data.isEmpty()) {
            QFile file(cleanedSavePath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(data);
                file.close();

                if (task.button) {
                    QString currentBtnPath = QFileInfo(task.button->property("fullImagePath").toString()).absoluteFilePath();
                    if (currentBtnPath == cleanedSavePath) {
                        IconLoaderTask *iconTask = new IconLoaderTask(cleanedSavePath, 100, 0, this, cleanedSavePath, true);
                        iconTask->setAutoDelete(true);
                        threadPool->start(iconTask);
                        task.button->setText("");
                    }
                }
            }
        }
        QTimer::singleShot(500, this, &MainWindow::processNextDownload);
    });
}

// ==========================================
//  User Gallery (Tab Page 2) Implementation
// ==========================================
void MainWindow::onToggleDetailTab() {
    int currentIndex = ui->detailContentStack->currentIndex();
    int nextIndex = (currentIndex == 0) ? 1 : 0;

    ui->scrollAreaWidgetContents->removeEventFilter(this);

    // 1. 切换页面
    ui->detailContentStack->setCurrentIndex(nextIndex);

    // 2. === 核心修改：动态调整高度约束 ===
    if (nextIndex == 1) {
        ui->detailContentStack->setFixedHeight(750);
    } else {
        ui->detailContentStack->setMinimumHeight(500);
        ui->detailContentStack->setMaximumHeight(16777215); // QWIDGETSIZE_MAX
        QTimer::singleShot(0, this, [this](){
            ui->scrollAreaWidgetContents->adjustSize();
        });
    }

    QTimer::singleShot(50, this, [this, nextIndex](){
        // 恢复事件监听
        ui->scrollAreaWidgetContents->installEventFilter(this);

        // 如果切换到详情页，调整容器大小
        if (nextIndex == 0) {
            ui->scrollAreaWidgetContents->adjustSize();
        }

        // 强制更新一次背景（避免尺寸不对）
        if (ui->backgroundLabel) {
            ui->backgroundLabel->setGeometry(ui->scrollAreaWidgetContents->rect());
        }
        updateBackgroundImage();
    });

    // 3. 自动扫描逻辑 (保持不变)
    if (nextIndex == 1 && ui->listUserImages->count() == 0) {
        onRescanUserClicked();
    }
}

void MainWindow::onRescanUserClicked() {
    QListWidgetItem *item = ui->modelList->currentItem();
    if (item) {
        // 如果有选中项，说明是在查看特定模型，按名称扫描
        scanForUserImages(item->text());
    } else {
        // 如果没有选中项，说明是在 "Global Gallery" 模式
        // 传入空字符串进行全量扫描
        scanForUserImages("");
    }
}
void MainWindow::onSetSdFolderClicked() {
    QString dir = QFileDialog::getExistingDirectory(this, "选择 SD 输出目录 (outputs/txt2img-images)", sdOutputFolder);
    if (!dir.isEmpty()) {
        sdOutputFolder = dir;
        // 保存配置
        QString configDir = qApp->applicationDirPath() + "/config";
        QDir().mkpath(configDir);
        QJsonObject root;
        root["sd_folder"] = sdOutputFolder;
        QFile file(configDir + "/user_gallery.json");
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument(root).toJson());
        }
        onRescanUserClicked();
    }
}


void MainWindow::scanForUserImages(const QString &loraBaseName) {
    ui->listUserImages->clear();
    ui->textUserPrompt->clear();
    tagFlowWidget->setData({}); // 清空 Tag

    // 1. 检查目录
    if (sdOutputFolder.isEmpty() || !QDir(sdOutputFolder).exists()) {
        ui->textUserPrompt->setText("<span style='color:orange'>请先点击右上方按钮设置 Stable Diffusion 图片输出目录。</span>");
        QMessageBox::warning(this, "目录无效", "设置的 SD 输出目录不存在或为空：\n" + sdOutputFolder);
        return;
    }

    bool isGlobalMode = loraBaseName.isEmpty();

    QString scanPrefix;
    if (isGlobalMode) {
        scanPrefix = "正在扫描所有本地图片";
    } else {
        scanPrefix = QString("正在扫描使用 '%1' 的图片").arg(loraBaseName);
    }

    ui->statusbar->showMessage(scanPrefix + "...");

    // =========================================================
    // 2. 构建模糊匹配关键字列表 (仅在非全局模式下)
    // =========================================================
    QStringList searchKeys;

    if(!isGlobalMode){
        QSet<QString> uniqueKeys; // 使用 Set 自动去重

        // === 获取 Safetensors 内部名称 ===
        // 获取当前选中项的完整路径
        QListWidgetItem *currentItem = ui->modelList->currentItem();
        if (currentItem) {
            QString fullPath = currentItem->data(ROLE_FILE_PATH).toString();
            QString internalName = getSafetensorsInternalName(fullPath);

            if (!internalName.isEmpty()) {
                qDebug() << "Found internal LoRA name:" << internalName;
                uniqueKeys.insert(internalName);
                // 对内部名称也生成变体（例如把下划线转空格），以防万一
                QString spaceVer = internalName; spaceVer.replace("_", " "); uniqueKeys.insert(spaceVer);
                QString underVer = internalName; underVer.replace(" ", "_"); uniqueKeys.insert(underVer);
            }
        }

        // --- 回退逻辑 (Fallback) ---
        // 只有当内部名称为空时（例如 .pt 文件，或没有写入 metadata 的旧模型），
        // 我们才退而求其次，使用文件名作为筛选依据。
        if (uniqueKeys.isEmpty()) {
            // A. 获取核心名称 (去除版本号、括号、扩展名)
            // 例如: "Korean_Doll_Likeness [v1.5].safetensors" -> "Korean_Doll_Likeness"
            QString rawName = loraBaseName;
            // 去除 [xxx]
            if (rawName.contains("[")) rawName = rawName.split("[").first().trimmed();
            // 去除 .safetensors / .pt
            QFileInfo fi(rawName);
            QString coreName = fi.completeBaseName();
            // B. 生成变体
            if (!coreName.isEmpty()) {
                uniqueKeys.insert(coreName);// 1. 原始核心名
                QString spaceToUnder = coreName;// 2. 空格 -> 下划线 (My Lora -> My_Lora)
                spaceToUnder.replace(" ", "_");uniqueKeys.insert(spaceToUnder);
                QString underToSpace = coreName;// 3. 下划线 -> 空格 (My_Lora -> My Lora)
                underToSpace.replace("_", " ");uniqueKeys.insert(underToSpace);
                QString noSpace = coreName;// 4. 去除所有空格 (My Lora -> MyLora)
                noSpace.remove(" ");uniqueKeys.insert(noSpace);
                QString noUnder = coreName;// 5. 去除所有下划线 (My_Lora -> MyLora)
                noUnder.remove("_");uniqueKeys.insert(noUnder);
                QString pure = coreName;// 6. 极致纯净版 (同时去除空格和下划线)
                pure.remove(" ").remove("_");uniqueKeys.insert(pure);
            }
        }

        // 将 Set 转为 List 以便传入线程
        searchKeys = uniqueKeys.values();
        // 过滤掉太短的 Key，防止错误匹配 (例如 "v1" 这种太短的词会匹配到所有图片)
        for (auto it = searchKeys.begin(); it != searchKeys.end(); ) {
            if (it->length() < 2) {
                it = searchKeys.erase(it);
            } else {
                ++it;
            }
        }
        qDebug() << "生成的模糊匹配词:" << searchKeys;
    }


    // =========================================================
    // 3. 异步扫描
    // =========================================================
    QMap<QString, UserImageInfo> currentCacheCopy = this->imageCache;
    bool recursive = optGalleryRecursive;
    // 开启异步任务
    QFuture<QPair<QList<UserImageInfo>, QMap<QString, UserImageInfo>>> future = QtConcurrent::run(
        [this, searchKeys, isGlobalMode, recursive, scanPrefix, currentCacheCopy]() {

            QList<UserImageInfo> results;
            QMap<QString, UserImageInfo> newCacheUpdates; // 用于收集需要更新到主缓存的数据

            QDirIterator::IteratorFlag iterFlag = recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
            QDirIterator it(sdOutputFolder, QStringList() << "*.png" << "*.jpg" << "*.jpeg", QDir::Files, iterFlag);

            int scannedFiles = 0;
            int cacheHits = 0;

            while (it.hasNext()) {
                QString path = it.next();
                QFileInfo fi = it.fileInfo();
                qint64 currentModified = fi.lastModified().toMSecsSinceEpoch();

                scannedFiles++;
                if (scannedFiles % 100 == 0) { // 稍微降低一点 UI 刷新频率
                    QMetaObject::invokeMethod(this, [this, scannedFiles, cacheHits](){
                        ui->statusbar->showMessage(QString("扫描中... (%1 张, 缓存命中 %2)").arg(scannedFiles).arg(cacheHits));
                    });
                }

                UserImageInfo info;
                bool needParse = true;

                // === 核心优化：检查缓存 ===
                if (currentCacheCopy.contains(path)) {
                    const UserImageInfo &cachedInfo = currentCacheCopy.value(path);
                    if (cachedInfo.lastModified == currentModified) {
                        // 命中缓存！直接使用，不需要 open 文件
                        info = cachedInfo;
                        needParse = false;
                        cacheHits++;
                    }
                }

                // 如果没命中缓存，或者文件被修改过，则解析
                if (needParse) {
                    info.path = path;
                    info.lastModified = currentModified;
                    parsePngInfo(path, info); // 解析 I/O 操作

                    // 记录到更新列表
                    newCacheUpdates.insert(path, info);
                }

                // === 筛选逻辑 ===
                if (info.prompt.isEmpty()) continue;

                bool matched = false;
                if (isGlobalMode) {
                    matched = true;
                } else {
                    for (const QString &key : searchKeys) {
                        if (info.prompt.contains(key, Qt::CaseInsensitive)) {
                            matched = true;
                            break;
                        }
                    }
                }

                if (matched) {
                    results.append(info);
                }
            }

            // 按时间倒序
            std::sort(results.begin(), results.end(), [](const UserImageInfo &a, const UserImageInfo &b){
                return a.lastModified > b.lastModified; // 使用 timestamp 比较更快
            });

            // 返回结果 和 需要更新的缓存
            return qMakePair(results, newCacheUpdates);
        });

    // 监听结果
    QFutureWatcher<QPair<QList<UserImageInfo>, QMap<QString, UserImageInfo>>> *watcher =
        new QFutureWatcher<QPair<QList<UserImageInfo>, QMap<QString, UserImageInfo>>>(this);

    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher](){
        auto resultPair = watcher->result();
        QList<UserImageInfo> results = resultPair.first;
        QMap<QString, UserImageInfo> newUpdates = resultPair.second;

        // 1. 更新主线程缓存
        if (!newUpdates.isEmpty()) {
            for(auto it = newUpdates.begin(); it != newUpdates.end(); ++it) {
                this->imageCache.insert(it.key(), it.value());
            }
            // 保存到磁盘，下次启动就快了
            saveUserGalleryCache();
        }

        // 2. UI 更新逻辑 (与原代码一致)
        ui->statusbar->showMessage(QString("扫描完成，共 %1 张").arg(results.count()), 3000);

        // 这里的 UI 渲染（new QListWidgetItem）在大量数据时也会卡顿
        // 建议使用 setUpdatesEnabled(false)
        ui->listUserImages->setUpdatesEnabled(false);
        for (const auto &info : results) {
            QListWidgetItem *item = new QListWidgetItem();
            item->setData(ROLE_USER_IMAGE_PATH, info.path);
            item->setData(ROLE_USER_IMAGE_PROMPT, info.prompt);
            item->setData(ROLE_USER_IMAGE_NEG, info.negativePrompt);
            item->setData(ROLE_USER_IMAGE_PARAMS, info.parameters);
            item->setData(ROLE_USER_IMAGE_TAGS, info.cleanTags);
            item->setIcon(placeholderIcon);
            ui->listUserImages->addItem(item);

            // 启动缩略图加载
            IconLoaderTask *task = new IconLoaderTask(info.path, 140, 4, this, info.path);
            task->setAutoDelete(true);
            threadPool->start(task);
        }
        ui->listUserImages->setUpdatesEnabled(true);

        updateUserStats(results);
        watcher->deleteLater();
    });

    watcher->setFuture(future);
}

QString MainWindow::extractPngParameters(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return "";

    // 1. 检查 PNG 签名 (8字节)
    QByteArray signature = file.read(8);
    const char pngSignature[] = {-119, 'P', 'N', 'G', 13, 10, 26, 10};
    if (signature != QByteArray::fromRawData(pngSignature, 8)) {
        return ""; // 不是 PNG
    }

    // 2. 循环读取 Chunk (块)
    while (!file.atEnd()) {
        // 读取长度 (4字节, Big Endian)
        QByteArray lenData = file.read(4);
        if (lenData.size() < 4) break;
        quint32 length = qFromBigEndian<quint32>(lenData.constData());

        // 读取类型 (4字节)
        QByteArray type = file.read(4);

        // 如果是 tEXt 块 (A1111 WebUI 通常存这里)
        if (type == "tEXt") {
            QByteArray data = file.read(length);
            // tEXt 格式: Keyword + \0 + Text
            int nullPos = data.indexOf('\0');
            if (nullPos != -1) {
                QString keyword = QString::fromLatin1(data.left(nullPos));
                if (keyword == "parameters") {
                    // 找到啦！提取内容 (通常是 UTF-8)
                    return QString::fromUtf8(data.mid(nullPos + 1));
                }
            }
        }
        // 如果是 iTXt 块 (国际化文本，偶尔会用)
        else if (type == "iTXt") {
            QByteArray data = file.read(length);
            // iTXt 格式: Keyword + \0 + ... + Text
            // 简单解析：寻找 parameters 关键字
            if (data.startsWith("parameters")) {
                // iTXt 结构比较复杂，前面有压缩标志等，通常 parameters 都在最后
                // 这里做一个偷懒的查找：找到第一个 null 后的非 null 区域
                // 但为了稳妥，A1111 99% 都是用 tEXt，这里略过 iTXt 的复杂解包，
                // 如果您发现某些图还是读不出，我们再加 iTXt 的完整解析。
            }
        }
        else {
            // 跳过数据部分 (如果不是我们要的块)
            // 注意：如果上面 if 读取了 data，这里就不用 skip 了
            // 但因为我们只 read 了 tEXt 的 data，其他类型需要 skip
            file.seek(file.pos() + length);
        }

        // 跳过 CRC (4字节)
        file.seek(file.pos() + 4);
    }

    return ""; // 没找到
}

void MainWindow::parsePngInfo(const QString &path, UserImageInfo &info) {
    QString text = extractPngParameters(path);

    if (text.isEmpty()) {
        // qDebug() << "No PNG Text Found! Path:" << path;
        QImageReader reader(path);
        if (reader.canRead()) {
            text = reader.text("parameters");
            // 兼容 ComfyUI
            if (text.isEmpty()) {
                QString comfy = reader.text("prompt");
                if (!comfy.isEmpty()) {
                    info.prompt = comfy;
                    info.negativePrompt = "ComfyUI Workflow Data (Hidden)";
                    info.cleanTags = parsePromptsToTags(info.prompt);
                    return;
                }
            }
        } else return;
    }

    if (!text.isEmpty()) {
        // === 核心改进：先找参数位置，再向前找 Negative ===

        // 1. 寻找参数的起始位置 (Steps: 通常是参数的开始)
        int stepsIndex = text.lastIndexOf("Steps: ");

        if (stepsIndex == -1) {
            // 没有参数？那就全是 Prompt（极少见的情况）
            info.prompt = text.trimmed();
            info.cleanTags = parsePromptsToTags(info.prompt);
            return;
        }

        // 2. 提取参数部分 (Steps 及其之后)
        info.parameters = text.mid(stepsIndex).trimmed();

        // 3. 提取参数之前的内容 (Positive + 可能的 Negative)
        QString beforeParams = text.left(stepsIndex).trimmed();

        // 4. 在参数之前的内容中寻找 "Negative prompt:"
        int negIndex = beforeParams.indexOf("Negative prompt:");

        if (negIndex != -1) {
            // 有 Negative：分割
            info.prompt = beforeParams.left(negIndex).trimmed();
            info.negativePrompt = beforeParams.mid(negIndex + 16).trimmed(); // 16 = "Negative prompt:".length()
        } else {
            // 没有 Negative：全是 Positive
            info.prompt = beforeParams.trimmed();
            info.negativePrompt = "(empty)";
        }

        // 5. 解析 Tags (只从 Positive Prompt 中提取)
        info.cleanTags = parsePromptsToTags(info.prompt);
    }
}

void MainWindow::updateUserStats(const QList<UserImageInfo> &images) {
    QMap<QString, int> tagCounts;
    for (const auto &img : images) {
        for (const QString &tag : img.cleanTags) {
            if (tag.compare("BREAK", Qt::CaseInsensitive) == 0) continue;
            tagCounts[tag]++;
        }
    }
    tagFlowWidget->setData(tagCounts);
}

void MainWindow::onUserImageClicked(QListWidgetItem *item) {
    if (!item) return;

    QString path = item->data(ROLE_USER_IMAGE_PATH).toString();
    QString prompt = item->data(ROLE_USER_IMAGE_PROMPT).toString();
    QString neg = item->data(ROLE_USER_IMAGE_NEG).toString();
    QString params = item->data(ROLE_USER_IMAGE_PARAMS).toString();

    QString safePrompt = prompt.toHtmlEscaped();
    QString safeNeg = neg.toHtmlEscaped();
    QString safeParams = params.toHtmlEscaped();

    // 格式化显示
    // 使用 <hr> 分割线，参数部分使用较小的字体和灰色
    QString html = QString(
                       "<style>"
                       "  .content { white-space: pre-wrap; }" // 定义一个样式类
                       "</style>"
                       "<p><b><span style='color:#66c0f4'>Positive:</span></b><br>"
                       "<span class='content'>%1</span></p>"
                       "<p><b><span style='color:#ff6666'>Negative:</span></b><br>"
                       "<span class='content'>%2</span></p>"
                       "<hr style='background-color:#444; height:1px; border:none;'>"
                       "<p><b><span style='color:#aaaaaa'>Parameters:</span></b><br>"
                       "<span class='content' style='color:#888888; font-size:11px; font-family:Consolas, monospace;'>%3</span></p>"
                       ).arg(safePrompt).arg(safeNeg).arg(safeParams);

    ui->textUserPrompt->setHtml(html);

    // 联动更新顶部 Hero 大图
    ui->heroFrame->setProperty("fullImagePath", path);
    transitionToImage(path);
}

void MainWindow::onTagFilterChanged(const QSet<QString> &selectedTags) {
    int visibleCount = 0;

    for(int i = 0; i < ui->listUserImages->count(); ++i) {
        QListWidgetItem *item = ui->listUserImages->item(i);
        QString rawPrompt = item->data(ROLE_USER_IMAGE_PROMPT).toString();
        QStringList distinctTags = item->data(ROLE_USER_IMAGE_TAGS).toStringList();

        bool match = true;
        // AND 逻辑：必须包含所有选中的 Tag
        for (const QString &selTag : selectedTags) {
            bool tagFound = false;
            for (const QString &imgTag : distinctTags) {
                if (imgTag.compare(selTag, Qt::CaseInsensitive) == 0) {
                    tagFound = true;
                    break;
                }
            }

            if (!tagFound) {
                match = false;
                break;
            }
        }

        item->setHidden(!match);
        if (match) visibleCount++;
    }

    ui->statusbar->showMessage(QString("筛选: %1 张图片符合条件").arg(visibleCount));
}

void MainWindow::onGalleryButtonClicked()
{
    // 1. 清除侧边栏模型选中状态，表示现在不是看某个具体模型
    ui->modelList->clearSelection();

    // 2. 切换到详情页 (Page 1)
    ui->mainStack->setCurrentIndex(1);

    // 3. 强制切换到“本地返图”标签页 (Index 1)
    // 注意：这里我们手动模拟 onToggleDetailTab 的部分逻辑
    ui->detailContentStack->setCurrentIndex(1);

    // 设置固定高度，确保 ScrollArea 能滚动
    ui->detailContentStack->setFixedHeight(750);

    // 4. 设置 UI 状态 (伪装成一个 Model)
    clearDetailView(); // 清空之前的模型信息

    // 自定义标题
    ui->lblModelName->setText("Global User Gallery / 所有用户返图");
    ui->lblModelName->setStyleSheet(
        "color: #fff;"
        "background-color: rgba(0,0,0,120);"
        "padding: 15px;"
        "border-left: 5px solid #ffcc00;" // 换个颜色，比如黄色，区分一下
        "font-size: 24px;"
        "font-weight: bold;"
    );

    // 隐藏/禁用一些不相关的按钮
    ui->btnForceUpdate->setVisible(false);
    ui->btnOpenUrl->setVisible(false);
    ui->btnFavorite->setVisible(false);
    ui->btnShowUserGallery->setVisible(false);

    // 5. 清除背景图 (或者你可以放一张默认的图库壁纸)
    currentHeroPath = "";
    transitionToImage("");

    // 6. 执行全局扫描 (传入空字符串)
    scanForUserImages("");
}

// 辅助函数：清洗单个 Tag
QString MainWindow::cleanTagText(QString t) {
    t = t.trimmed();
    if (t.isEmpty()) return "";

    // === 特殊保护：颜文字 ===
    // 如果是常见的颜文字，直接返回，不去除括号
    static const QSet<QString> emoticons = {":)", ":-)", ":(", ":-(", "^_^", "T_T", "o_o", "O_O"};
    if (emoticons.contains(t)) return t;

    // === 处理权重和括号 ===
    // 1. 去除尾部的权重数字 (例如 :1.2 或 :0.9)
    // 正则含义：冒号后面跟着数字和小数点，且在字符串末尾
    static QRegularExpression weightRegex(":[0-9.]+$");
    t.remove(weightRegex);

    // 2. 去除所有类型的括号 ( { [ ( ) ] } )
    // 正则含义：匹配所有括号字符
    static QRegularExpression bracketRegex("[\\{\\}\\[\\]\\(\\)]");
    t.remove(bracketRegex);

    return t.trimmed();
}

// 辅助函数：将 Prompt 字符串解析为 Tag 列表
QStringList MainWindow::parsePromptsToTags(const QString &rawPrompt) {
    QStringList result;
    if (rawPrompt.isEmpty()) return result;

    QString processText = rawPrompt;

    // === 1. 处理换行符分割 ===
    if (optSplitOnNewline) {
        // 将所有换行符替换为逗号，这样 split(',') 就能把它们分开
        processText.replace("\r\n", ",");
        processText.replace("\n", ",");
        processText.replace("\r", ",");
    }

    // 2. 按逗号切分
    QStringList parts = processText.split(",", Qt::SkipEmptyParts);

    for (const QString &part : parts) {
        // 3. 清洗 Tag (去除权重括号等)
        QString clean = cleanTagText(part);

        if (clean.isEmpty()) continue;

        // === 4. 过滤黑名单关键词 ===
        bool isBlocked = false;
        for (const QString &filterWord : optFilterTags) {
            // 使用 compare 忽略大小写 (例如 break == BREAK)
            if (clean.compare(filterWord, Qt::CaseInsensitive) == 0) {
                isBlocked = true;
                break;
            }
        }

        if (!isBlocked) {
            result.append(clean);
        }
    }
    return result;
}

void MainWindow::initMenuBar() {
    // 1. 使用 this->menuBar() 这是一个保险措施
    // 它可以确保即便 XML 里的菜单栏丢失或层级错误，这里也能获取到窗口真正的菜单栏
    QMenuBar *bar = this->menuBar();
    bar->clear(); // 清空旧内容

    // 2. 设置样式，确保在深色主题下可见
    // 如果不设置，有时候文字颜色会和背景色一样导致“隐形”
    bar->setStyleSheet(
        "QMenuBar { background-color: #1a1f29; color: #dcdedf; border-bottom: 1px solid #3d4d5d; }"
        "QMenuBar::item { background-color: transparent; padding: 8px 20px; font-size: 14px; font-weight: bold; }"
        "QMenuBar::item:selected { background-color: #3d4450; color: #ffffff; }"
        "QMenuBar::item:pressed { background-color: #66c0f4; color: #000000; }"
    );

    // 3. 直接添加“库”按钮 (Action)
    // 这种直接 addAction 到 bar 的方式，效果就像是点击按钮，而不是弹出下拉菜单
    QAction *actLib = new QAction("📚 库 / Library", this);
    actLib->setShortcut(QKeySequence("Ctrl+1"));
    connect(actLib, &QAction::triggered, this, &MainWindow::onMenuSwitchToLibrary);
    bar->addAction(actLib);

    // 4. 直接添加“设置”按钮 (Action)
    QAction *actSet = new QAction("⚙️ 设置 / Settings", this);
    actSet->setShortcut(QKeySequence("Ctrl+2"));
    connect(actSet, &QAction::triggered, this, &MainWindow::onMenuSwitchToSettings);
    bar->addAction(actSet);

    // 关于按钮
    QAction *btnAbout = new QAction("ℹ️ 关于 / About");
    btnAbout->setShortcut(QKeySequence("Ctrl+3"));
    connect(btnAbout, &QAction::triggered, this, &MainWindow::onMenuSwitchToAbout);
    bar->addAction(btnAbout);

    // 5. 强制显示 (防止被 hidden 属性隐藏)
    bar->setVisible(true);
}

void MainWindow::onMenuSwitchToLibrary() {
    ui->rootStack->setCurrentIndex(0);
}

void MainWindow::onMenuSwitchToSettings() {
    ui->rootStack->setCurrentIndex(1);
}

// === 路径加载与保存 (注册表) ===
void MainWindow::loadPathSettings() {
    // 读取 LoRA 路径
    currentLoraPath = settings->value("lora_path").toString();
    // 读取 Gallery 路径 (迁移到注册表)
    sdOutputFolder = settings->value("gallery_path").toString();
    translationCsvPath = settings->value("translation_path").toString();
    if (ui->editLoraPath) ui->editLoraPath->setText(currentLoraPath);
    if (ui->editGalleryPath) ui->editGalleryPath->setText(sdOutputFolder);
    if (ui->editTransPath) ui->editTransPath->setText(translationCsvPath);
    if (!translationCsvPath.isEmpty()) {
        loadTranslationCSV(translationCsvPath);
    }
}

void MainWindow::savePathSettings() {
    settings->setValue("lora_path", currentLoraPath);
    settings->setValue("gallery_path", sdOutputFolder);
    settings->setValue("translation_path", translationCsvPath);
}

// === 全局配置加载与保存 (JSON) ===
void MainWindow::loadGlobalConfig() {
    QString configPath = qApp->applicationDirPath() + "/config/settings.json";
    QFile file(configPath);
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        QJsonObject root = doc.object();

        // 1. 读取所有配置到成员变量
        optFilterNSFW                   = root["nsfw_filter"].toBool(false);
        optNSFWMode                     = root["nsfw_mode"].toInt(1);
        optNSFWLevel                    = root["nsfw_level_threshold"].toInt(1);
        optLoraRecursive                = root["lora_recursive"].toBool(false);
        optGalleryRecursive             = root["gallery_recursive"].toBool(false);
        optBlurRadius                   = root["blur_radius"].toInt(30);
        optDownscaleBlur                = root["blur_downscale_enabled"].toBool(true);
        optBlurProcessWidth             = root["blur_process_width"].toInt(500);
        optRenderThreadCount            = root["render_thread_count"].toInt(4);
        optRestoreTreeState             = root["restore_tree_state"].toBool(true);
        optSplitOnNewline               = root["split_on_newline"].toBool(true);
        optShowEmptyCollections         = root["show_empty_collections"].toBool(false);
        QString filterStr               = root["filter_tags_string"].toString(DEFAULT_FILTER_TAGS);
        optUseArrangedUA                = root["use_custom_ua"].toBool(false);
        optSavedUAString                = root["custom_user_agent"].toString();
        optUseCivitaiName               = root["use_civitai_name"].toBool(false);

        qDebug() << "Loaded User-Agent:" << currentUserAgent;

        // 解析过滤词
        optFilterTags = filterStr.split(',', Qt::SkipEmptyParts);
        for(QString &s : optFilterTags) s = s.trimmed();

        // 读取树状菜单状态
        if (optRestoreTreeState && root.contains("tree_state")) {
            QJsonObject treeState = root["tree_state"].toObject();
            startupTreeScrollPos = treeState["scroll_pos"].toInt(0);
            QJsonArray arr = treeState["expanded_items"].toArray();
            for (const auto &val : arr) {
                startupExpandedCollections.insert(val.toString());
            }
        }

        // 设置UA
        if (optUseArrangedUA && !optSavedUAString.isEmpty())currentUserAgent = optSavedUAString;
        else currentUserAgent = getRandomUserAgent();

        // 范围校验
        if (optBlurRadius < 0) optBlurRadius = 0;
        if (optBlurRadius > 100) optBlurRadius = 100;
        if (optRenderThreadCount < 1) optRenderThreadCount = 4;
    }

    // 将配置应用到 UI 控件 (初始化 UI 状态)
    ui->chkRecursiveLora->setChecked(optLoraRecursive);
    ui->chkRecursiveGallery->setChecked(optGalleryRecursive);
    ui->sliderBlur->setValue(optBlurRadius);
    ui->lblBlurValue->setText(QString::number(optBlurRadius) + "px");
    ui->chkDownscaleBlur->setChecked(optDownscaleBlur);
    ui->spinBlurWidth->setValue(optBlurProcessWidth);
    ui->spinBlurWidth->setEnabled(optDownscaleBlur);
    ui->chkFilterNSFW->setChecked(optFilterNSFW);
    if (optNSFWMode == 0) ui->radioNSFW_Hide->setChecked(true);
    else ui->radioNSFW_Blur->setChecked(true);
    ui->spinNSFWLevel->setValue(optNSFWLevel);
    bool nsfwEnabled = optFilterNSFW;
    ui->radioNSFW_Hide->setEnabled(nsfwEnabled);
    ui->radioNSFW_Blur->setEnabled(nsfwEnabled);
    ui->spinNSFWLevel->setEnabled(nsfwEnabled);
    ui->spinRenderThreads->setValue(optRenderThreadCount);
    ui->chkRestoreTreeState->setChecked(optRestoreTreeState);
    ui->chkSplitOnNewline->setChecked(optSplitOnNewline);
    ui->editFilterTags->setText(optFilterTags.join(", "));
    ui->chkShowEmptyCollections->setChecked(optShowEmptyCollections);
    ui->chkUseCustomUserAgent->setChecked(optUseArrangedUA);
    ui->editUserAgent->setEnabled(optUseArrangedUA);
    if (!optSavedUAString.isEmpty()) {ui->editUserAgent->setText(optSavedUAString);}
    ui->chkUseCivitaiName->setChecked(optUseCivitaiName);

    // ===============================
    // === 连接 Settings 页面的信号 ===
    // ===============================
    // 递归查找
    connect(ui->chkRecursiveLora, &QCheckBox::toggled, this, &MainWindow::onSettingsChanged);
    connect(ui->chkRecursiveGallery, &QCheckBox::toggled, this, &MainWindow::onSettingsChanged);
    // 模糊滑块
    connect(ui->sliderBlur, &QSlider::valueChanged, this, &MainWindow::onBlurSliderChanged);
    connect(ui->sliderBlur, &QSlider::sliderReleased, this, &MainWindow::saveGlobalConfig);
    // 缩放模糊
    connect(ui->chkDownscaleBlur, &QCheckBox::toggled, this, [this](bool checked){
        optDownscaleBlur = checked;
        ui->spinBlurWidth->setEnabled(checked);
        saveGlobalConfig();
    });
    connect(ui->spinBlurWidth, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int val){
        optBlurProcessWidth = val;
        saveGlobalConfig();
    });
    // NSFW 设置
    connect(ui->chkFilterNSFW, &QCheckBox::toggled, this, [this](bool checked){
        optFilterNSFW = checked;
        ui->radioNSFW_Hide->setEnabled(checked);
        ui->radioNSFW_Blur->setEnabled(checked);
        ui->spinNSFWLevel->setEnabled(checked);
        saveGlobalConfig();
    });
    connect(ui->radioNSFW_Hide, &QRadioButton::toggled, this, [this](bool checked){
        if(checked) optNSFWMode = 0;
        saveGlobalConfig();
    });
    connect(ui->radioNSFW_Blur, &QRadioButton::toggled, this, [this](bool checked){
        if(checked) optNSFWMode = 1;
        saveGlobalConfig();
    });
    connect(ui->spinNSFWLevel, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int val){
        optNSFWLevel = val;
        saveGlobalConfig();
    });
    // 线程数
    connect(ui->spinRenderThreads, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int val){
        optRenderThreadCount = val;
        threadPool->setMaxThreadCount(val);
        backgroundThreadPool->setMaxThreadCount(val);
        saveGlobalConfig();
    });
    // 树状态恢复
    connect(ui->chkRestoreTreeState, &QCheckBox::toggled, this, [this](bool checked){
        optRestoreTreeState = checked;
        saveGlobalConfig();
    });
    // 换行符开关
    connect(ui->chkSplitOnNewline, &QCheckBox::toggled, this, [this](bool checked){
        optSplitOnNewline = checked;
        saveGlobalConfig();
    });
    // 过滤词输入框
    connect(ui->editFilterTags, &QLineEdit::editingFinished, this, [this](){
        QString text = ui->editFilterTags->text();
        optFilterTags = text.split(',', Qt::SkipEmptyParts);
        for(QString &s : optFilterTags) s = s.trimmed();
        saveGlobalConfig();
    });
    // 重置按钮
    connect(ui->btnResetFilterTags, &QPushButton::clicked, this, [this](){
        // 弹出确认对话框
        QMessageBox::StandardButton reply;
        reply = QMessageBox::question(this,
                                      "确认重置 / Confirm Reset",
                                      "确定要将过滤提示词重置为默认值吗？\n此操作将覆盖当前的自定义设置。\n\n"
                                      "Are you sure you want to reset filter tags to default?",
                                      QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            // 用户点击了 Yes，执行重置逻辑
            ui->editFilterTags->setText(DEFAULT_FILTER_TAGS);
            // 解析并保存
            optFilterTags = DEFAULT_FILTER_TAGS.split(',', Qt::SkipEmptyParts);
            for(QString &s : optFilterTags) s = s.trimmed();
            saveGlobalConfig();
            ui->statusbar->showMessage("过滤词已重置", 2000);
        }
    });
    // 显示空收藏夹开关
    connect(ui->chkShowEmptyCollections, &QCheckBox::toggled, this, [this](bool checked){
        optShowEmptyCollections = checked;
        saveGlobalConfig();
        // 修改此设置后，必须立刻刷新树状图才能看到效果
        refreshCollectionTreeView();
    });
    // 复选框切换：控制输入框可用性 + 立即切换 UA 策略 + 自动保存
    connect(ui->chkUseCustomUserAgent, &QCheckBox::toggled, this, [this](bool checked){
        ui->editUserAgent->setEnabled(checked);
        if (checked) {
            // 勾选瞬间：如果框里有字，就用框里的；没字就随机填一个
            if (ui->editUserAgent->text().trimmed().isEmpty()) {
                ui->editUserAgent->setText(getRandomUserAgent());
                ui->editUserAgent->setEnabled(true);
            }
            currentUserAgent = ui->editUserAgent->text().trimmed();
        } else {
            // 取消勾选瞬间：立即切换回随机 UA，但保留输入框里的字
            currentUserAgent = getRandomUserAgent();
            ui->editUserAgent->setEnabled(false);
        }
        qDebug() << "UA Changed to:" << currentUserAgent;
        saveGlobalConfig(); // 状态改变立即保存
    });
    // 勾选时更新当前 UA (编辑完成时保存，避免每打一个字都存硬盘)
    connect(ui->editUserAgent, &QLineEdit::editingFinished, this, [this](){
        if (ui->chkUseCustomUserAgent->isChecked()) {
            currentUserAgent = ui->editUserAgent->text().trimmed();
        }
        saveGlobalConfig();
    });
    // 生成随机按钮：填入框 + (如果勾选)更新当前UA + 保存
    connect(ui->btnResetUA, &QPushButton::clicked, this, [this](){
        QString newUA = getRandomUserAgent();
        ui->editUserAgent->setText(newUA);

        if (ui->chkUseCustomUserAgent->isChecked()) {
            currentUserAgent = newUA;
        }
        saveGlobalConfig();
    });
    // 刷新文字
    connect(ui->chkUseCivitaiName, &QCheckBox::toggled, this, [this](bool checked){
        optUseCivitaiName = checked;
        updateModelListNames(); // 切换时立即刷新列表文字
        executeSort();          // 刷新文字后可能需要重新排序
        saveGlobalConfig();
    });
}

void MainWindow::saveGlobalConfig() {
    QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);

    QJsonObject root;
    root["lora_recursive"]              = optLoraRecursive;
    root["gallery_recursive"]           = optGalleryRecursive;
    root["blur_radius"]                 = optBlurRadius;
    root["blur_downscale_enabled"]      = optDownscaleBlur;
    root["blur_process_width"]          = optBlurProcessWidth;
    root["nsfw_filter"]                 = optFilterNSFW;
    root["nsfw_mode"]                   = optNSFWMode;
    root["nsfw_level_threshold"]        = optNSFWLevel;
    root["render_thread_count"]         = optRenderThreadCount;
    root["restore_tree_state"]          = optRestoreTreeState;
    root["split_on_newline"]            = optSplitOnNewline;
    root["filter_tags_string"]          = ui->editFilterTags->text();
    root["show_empty_collections"]      = optShowEmptyCollections;
    root["use_custom_ua"]               = ui->chkUseCustomUserAgent->isChecked();
    root["custom_user_agent"]           = ui->editUserAgent->text();
    root["use_civitai_name"]            = optUseCivitaiName;

    if (optRestoreTreeState) {
        QJsonObject treeState;

        // 1. 获取当前展开的项
        QJsonArray expandedArr;
        // 如果 UI 还没初始化完(比如刚启动就关闭)，尽量保留读取到的旧状态，防止清空
        // 但这里我们主要处理运行时保存：
        if (ui->collectionTree->topLevelItemCount() > 0) {
            for (int i = 0; i < ui->collectionTree->topLevelItemCount(); ++i) {
                QTreeWidgetItem *item = ui->collectionTree->topLevelItem(i);
                if (item->isExpanded()) {
                    expandedArr.append(item->data(0, ROLE_COLLECTION_NAME).toString());
                }
            }
            treeState["expanded_items"] = expandedArr;
            treeState["scroll_pos"] = ui->collectionTree->verticalScrollBar()->value();
        } else {
            // 如果树是空的（极少情况），可能还没加载，保留启动时读到的缓存
            QJsonArray cachedArr;
            for (const QString &s : startupExpandedCollections) cachedArr.append(s);
            treeState["expanded_items"] = cachedArr;
            treeState["scroll_pos"] = startupTreeScrollPos;
        }

        root["tree_state"] = treeState;
    }

    QFile file(configDir + "/settings.json");
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson());
    }
}

// === 设置页交互 ===

void MainWindow::onBrowseLoraPath() {
    QString dir = QFileDialog::getExistingDirectory(this, "选择 LoRA 文件夹", currentLoraPath);
    if (!dir.isEmpty()) {
        currentLoraPath = dir;
        ui->editLoraPath->setText(dir);
        savePathSettings();
        // 立即触发重新扫描? 或者等用户切回库时扫描?
        // 体验最好的是询问，这里简单起见，如果切回库会自动刷新(如果没有，可以手动刷新)
        QMessageBox::information(this, "提示", "LoRA 路径已更新，请返回库界面点击刷新按钮。");
    }
}

void MainWindow::onBrowseGalleryPath() {
    QString dir = QFileDialog::getExistingDirectory(this, "选择图库文件夹", sdOutputFolder);
    if (!dir.isEmpty()) {
        sdOutputFolder = dir;
        ui->editGalleryPath->setText(dir);
        savePathSettings();
    }
}

void MainWindow::onSettingsChanged() {
    // 从 UI 更新变量
    optLoraRecursive = ui->chkRecursiveLora->isChecked();
    optGalleryRecursive = ui->chkRecursiveGallery->isChecked();

    // 保存
    saveGlobalConfig();
}

void MainWindow::onBlurSliderChanged(int value) {
    optBlurRadius = value;
    ui->lblBlurValue->setText(QString::number(value) + "px");

    // 实时更新当前背景 (如果有)
    updateBackgroundImage();
}

QIcon MainWindow::generatePlaceholderIcon()
{
    // === 修改点 1：将基准尺寸设为 180 (适配主页大图) ===
    int fullSize = 180;

    // === 修改点 2：按比例调整内边距 ===
    // 之前 64px 用 8px 边距 (12.5%)
    // 现在 180px 对应约 20px 边距，这样缩小后在侧边栏看着比例才对
    int padding = 20;

    int contentSize = fullSize - (padding * 2);

    // 创建透明底图
    QPixmap finalPix(fullSize, fullSize);
    finalPix.fill(Qt::transparent);

    QPainter painter(&finalPix);
    // 开启高质量抗锯齿
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // 计算中间内容的区域
    QRect contentRect(padding, padding, contentSize, contentSize);

    // 绘制深色背景框 (圆角加大一点)
    painter.setBrush(QColor("#25282f"));
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(contentRect, 12, 12);

    // 绘制“×”符号
    QPen pen(QColor("#3d4450")); // 线条颜色稍微调深一点，更有质感
    pen.setWidth(5); // 线条加粗，适应大尺寸
    pen.setCapStyle(Qt::RoundCap); // 线条端点圆润
    painter.setPen(pen);

    // 画两条交叉线 (Margin 也要按比例放大)
    int margin = 40;
    painter.drawLine(contentRect.left() + margin, contentRect.top() + margin,
                     contentRect.right() - margin, contentRect.bottom() - margin);
    painter.drawLine(contentRect.right() - margin, contentRect.top() + margin,
                     contentRect.left() + margin, contentRect.bottom() - margin);

    return QIcon(finalPix);
}

QString MainWindow::getSafetensorsInternalName(const QString &path)
{
    if (!path.endsWith(".safetensors", Qt::CaseInsensitive)) {
        return "";
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return "";

    // 1. 读取前8个字节 (uint64, Little Endian)，表示 JSON 头部的长度
    qint64 headerLen = 0;
    if (file.read((char*)&headerLen, 8) != 8) return "";

    // 安全检查：防止读取过大的垃圾数据（一般 header 不会超过 100MB）
    if (headerLen <= 0 || headerLen > 100 * 1024 * 1024) return "";

    // 2. 读取 JSON 头部数据
    QByteArray headerData = file.read(headerLen);
    file.close();

    // 3. 解析 JSON
    QJsonDocument doc = QJsonDocument::fromJson(headerData);
    if (doc.isNull()) return "";

    QJsonObject root = doc.object();

    // 4. 提取 __metadata__ 中的 ss_output_name
    if (root.contains("__metadata__")) {
        QJsonObject meta = root["__metadata__"].toObject();
        // ss_output_name 是最常用的内部名称字段
        if (meta.contains("ss_output_name")) {
            return meta["ss_output_name"].toString();
        }
        // 部分模型可能使用 ss_tag_frequency 等字段，但 name 最准确
    }

    return "";
}

QPixmap MainWindow::applyNSFWBlur(const QPixmap &pix) {
    if (pix.isNull()) return pix;

    QGraphicsBlurEffect *blur = new QGraphicsBlurEffect;
    blur->setBlurRadius(40); // 强度大一点，确保看不清内容

    QGraphicsScene scene;
    QGraphicsPixmapItem item;
    item.setPixmap(pix);
    item.setGraphicsEffect(blur);
    scene.addItem(&item);

    QPixmap result(pix.size());
    result.fill(Qt::transparent);
    QPainter painter(&result);
    scene.render(&painter);
    return result;
}

QPixmap MainWindow::applyRoundedMask(const QPixmap &src, int radius)
{
    if (src.isNull()) return QPixmap();
    if (radius <= 0) return src;

    QPixmap result(src.size());
    result.fill(Qt::transparent);

    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // 创建圆角路径
    QPainterPath path;
    path.addRoundedRect(src.rect(), radius, radius);

    // 裁剪并绘制
    painter.setClipPath(path);
    painter.drawPixmap(0, 0, src);

    return result;
}

// 浏览翻译文件
void MainWindow::onBrowseTranslationPath() {
    QString fileName = QFileDialog::getOpenFileName(this, "选择翻译文件 (CSV)",
                                                    QFileInfo(translationCsvPath).path(),
                                                    "CSV Files (*.csv);;All Files (*.*)");
    if (!fileName.isEmpty()) {
        translationCsvPath = fileName;
        ui->editTransPath->setText(fileName);

        // 保存到配置
        settings->setValue("translation_path", translationCsvPath);

        // 立即加载
        loadTranslationCSV(translationCsvPath);

        QMessageBox::information(this, "设置", "翻译词表已加载。");
    }
}

// 加载 CSV 逻辑
void MainWindow::loadTranslationCSV(const QString &path) {
    translationMap.clear();
    if (path.isEmpty() || !QFile::exists(path)) return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;

    QTextStream in(&file);

    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line.trimmed().isEmpty()) continue;

        // 格式：1girl,1女孩
        // 使用第一次出现的逗号进行分割，以防翻译内容中也有逗号（虽然CSV通常会有引号处理，这里做简易处理）
        int firstComma = line.indexOf(',');
        if (firstComma != -1) {
            QString en = line.left(firstComma).trimmed();
            QString cn = line.mid(firstComma + 1).trimmed();

            // 去除可能存在的引号（如果CSV是标准格式）
            if (en.startsWith('"') && en.endsWith('"')) en = en.mid(1, en.length()-2);
            if (cn.startsWith('"') && cn.endsWith('"')) cn = cn.mid(1, cn.length()-2);

            if (!en.isEmpty() && !cn.isEmpty()) {
                translationMap.insert(en, cn);
            }
        }
    }
    file.close();
    qDebug() << "Loaded translation entries:" << translationMap.size();
}

void MainWindow::onUserGalleryContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = ui->listUserImages->itemAt(pos);
    if (!item) return;

    QString filePath = item->data(ROLE_USER_IMAGE_PATH).toString();
    if (filePath.isEmpty()) return;
    QString prompt = item->data(ROLE_USER_IMAGE_PROMPT).toString();
    QString neg = item->data(ROLE_USER_IMAGE_NEG).toString();
    QString params = item->data(ROLE_USER_IMAGE_PARAMS).toString();

    QMenu menu(this);

    QAction *actCopyGenParams = menu.addAction("复制生成参数 / Copy Gen Params");
    actCopyGenParams->setToolTip("复制符合SD WebUI格式的完整参数，\n粘贴进提示词框后可直接点击↙️按钮读取。");
    menu.addSeparator(); // 分隔线
    QAction *actOpenImg = menu.addAction("打开图片 / Open Image");
    QAction *actOpenDir = menu.addAction("打开文件位置 / Show in Folder");
    menu.addSeparator();
    QAction *actCopyPath = menu.addAction("复制路径 / Copy Path");

    QAction *selected = menu.exec(ui->listUserImages->mapToGlobal(pos));
    
    if (selected == actCopyGenParams) {
        QStringList parts;

        // 1. 正向提示词
        if (!prompt.isEmpty()) {
            parts.append(prompt);
        }

        // 2. 反向提示词 (需要补上标准前缀 "Negative prompt: ")
        // 注意：在 parsePngInfo 里如果是 "(empty)" 则跳过
        if (!neg.isEmpty() && neg != "(empty)") {
            parts.append("Negative prompt: " + neg);
        }

        // 3. 参数行 (Steps: 20, Sampler: ...)
        if (!params.isEmpty()) {
            parts.append(params);
        }

        // 用换行符连接，这是 SD WebUI 识别的标准格式
        QString fullParams = parts.join("\n");

        QClipboard *clip = QGuiApplication::clipboard();
        clip->setText(fullParams);

        ui->statusbar->showMessage("已复制 SD 生成参数到剪贴板", 2000);
    }
    else if (selected == actOpenImg) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
    }
    else if (selected == actOpenDir) {
#ifdef Q_OS_WIN
        // === Windows 终极修复方案 ===
        // 使用 QProcess 的 setNativeArguments 绕过 Qt 的自动引号机制
        QProcess *process = new QProcess(this);
        process->setProgram("explorer.exe");

        // 转换为 Windows 反斜杠格式
        QString nativePath = QDir::toNativeSeparators(filePath);

        // 手动构建参数：/select,"C:\Path\File.png"
        // 这里的引号是我们手动加的，Qt 不会再外面再包一层引号
        QString args = QString("/select,\"%1\"").arg(nativePath);

        process->setNativeArguments(args);
        process->start();

        // 进程结束后自动删除对象
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                process, &QProcess::deleteLater);
#else
        // Mac / Linux
        QFileInfo fi(filePath);
        QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
#endif
    }
    else if (selected == actCopyPath) {
        QGuiApplication::clipboard()->setText(QDir::toNativeSeparators(filePath));
        ui->statusbar->showMessage("路径已复制", 2000);
    }
}

void MainWindow::onModelsTabButtonClicked()
{
    ui->sidebarStack->setCurrentIndex(0);
    ui->btnModelsTab->setChecked(true);
    ui->btnCollectionsTab->setChecked(false);
}

void MainWindow::onCollectionsTabButtonClicked()
{
    ui->sidebarStack->setCurrentIndex(1);
    ui->btnCollectionsTab->setChecked(true);
    ui->btnModelsTab->setChecked(false);
}

void MainWindow::onCollectionTreeItemClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);

    // 判断是收藏夹节点还是模型节点
    if (item->data(0, ROLE_IS_COLLECTION_NODE).toBool()) {
        // === 收藏夹节点 ===
        QString collectionName = item->data(0, ROLE_COLLECTION_NAME).toString();
        int count = item->data(0, ROLE_ITEM_COUNT).toInt();

        // 2. 展开/折叠节点
        bool wasExpanded = item->isExpanded();
        item->setExpanded(!wasExpanded);

        // 3. 切换文本前缀
        QString displayName = (collectionName == FILTER_UNCATEGORIZED) ? "未分类 / Uncategorized" : collectionName;
        QString prefix = (!wasExpanded) ? " - " : " + "; // 此时已切换状态
        QString newText = QString("%1%2 (%3)").arg(prefix).arg(displayName).arg(count);
        item->setText(0, newText);
    } else {
        // 模型节点
        QString filePath = item->data(0, ROLE_FILE_PATH).toString();
        if (filePath.isEmpty()) return;

        QListWidgetItem* matchItem = nullptr;
        for(int i = 0; i < ui->modelList->count(); ++i) {
            QListWidgetItem* sideItem = ui->modelList->item(i);
            if (sideItem->data(ROLE_FILE_PATH).toString() == filePath) {
                matchItem = sideItem;
                break;
            }
        }

        if (matchItem) {
            ui->modelList->setCurrentItem(matchItem);
            onModelListClicked(matchItem);
            ui->mainStack->setCurrentIndex(1); // 跳转详情页
        }
    }
}

void MainWindow::onCollectionTreeContextMenu(const QPoint &pos)
{
    // 获取点击位置的 Item (作为上下文判断依据)
    QTreeWidgetItem *clickedItem = ui->collectionTree->itemAt(pos);
    if (!clickedItem) return;

    // 获取所有选中的 Items
    QList<QTreeWidgetItem*> selectedTreeItems = ui->collectionTree->selectedItems();

    // 如果右键点击的项不在选区内，Qt 的默认行为会清除选区并单选点击项。
    // 这里做一个保险，如果选区为空，就把点击项加进去
    if (selectedTreeItems.isEmpty()) {
        selectedTreeItems.append(clickedItem);
    }

    // 判断是收藏夹节点还是模型节点
    if (clickedItem->data(0, ROLE_IS_COLLECTION_NODE).toBool()) {
        // --- 收藏夹节点右键菜单 ---
        QString collectionName = clickedItem->data(0, ROLE_COLLECTION_NAME).toString();

        QMenu menu(this);
        QAction *title = menu.addAction(QString("管理收藏夹: %1").arg(collectionName));
        title->setEnabled(false);
        menu.addSeparator();

        if (collectionName == FILTER_UNCATEGORIZED) {
            // "未分类"特殊处理，不能重命名和删除
            QAction *dummy = menu.addAction("无法操作此项");
            dummy->setEnabled(false);
        } else {
            QAction *actRename = menu.addAction("重命名 / Rename Collection");
            QAction *actDelete = menu.addAction("删除 / Delete Collection");

            QAction *selected = menu.exec(ui->collectionTree->mapToGlobal(pos));

            if (selected == actRename) {
                bool ok;
                QString newName = QInputDialog::getText(this, "重命名收藏夹", "新名称:", QLineEdit::Normal, collectionName, &ok);
                if (ok && !newName.trimmed().isEmpty() && newName != collectionName) {
                    if (collections.contains(newName)) {
                        QMessageBox::warning(this, "错误", "该名称已存在！");
                        return;
                    }
                    QStringList files = collections.value(collectionName);
                    collections.insert(newName, files);
                    collections.remove(collectionName);

                    if (currentCollectionFilter == collectionName) currentCollectionFilter = newName;

                    saveCollections();
                    refreshHomeCollectionsUI(); // 刷新主页的收藏夹按钮
                    refreshCollectionTreeView(); // 刷新收藏夹树状视图
                }
            } else if (selected == actDelete) {
                auto reply = QMessageBox::question(this, "确认删除",
                                                   QString("确定要删除收藏夹 \"%1\" 吗？\n(里面的模型不会被删除，仅删除分类)").arg(collectionName),
                                                   QMessageBox::Yes | QMessageBox::No);
                if (reply == QMessageBox::Yes) {
                    collections.remove(collectionName);
                    if (currentCollectionFilter == collectionName) currentCollectionFilter = "";
                    saveCollections();
                    refreshHomeCollectionsUI();
                    refreshCollectionTreeView();
                    // 如果删除了当前正在过滤的收藏夹，需要重置主页过滤
                    if (currentCollectionFilter.isEmpty()) refreshHomeGallery();
                }
            }
        }
    } else {
        // --- 模型节点逻辑 (核心修改：支持多选) ---

        QList<QListWidgetItem*> targetListItems;

        // 遍历所有选中的树节点
        for (QTreeWidgetItem *tItem : selectedTreeItems) {
            // 跳过选中的“文件夹”节点，只处理“模型”节点
            if (tItem->data(0, ROLE_IS_COLLECTION_NODE).toBool()) continue;

            QString baseName = tItem->data(0, ROLE_MODEL_NAME).toString();
            if (baseName.isEmpty()) baseName = tItem->text(0); // 兜底

            // 在 modelList 中查找对应的 Item (因为 showCollectionMenu 需要 QListWidgetItem)
            // 优化：这里不需要每次都遍历 modelList，可以构建一个查找 Map，或者简单遍历
            for (int i = 0; i < ui->modelList->count(); ++i) {
                QListWidgetItem *listItem = ui->modelList->item(i);
                if (listItem->data(ROLE_MODEL_NAME).toString() == baseName) {
                    targetListItems.append(listItem);
                    break;
                }
            }
        }

        // 如果找到了对应的模型列表项，弹出批量操作菜单
        if (!targetListItems.isEmpty()) {
            showCollectionMenu(targetListItems, ui->collectionTree->mapToGlobal(pos));
        }
    }
}

void MainWindow::refreshCollectionTreeView()
{
    // 保存菜单
    QSet<QString> expandedCollections;
    int scrollPos = 0;

    if (isFirstTreeRefresh && optRestoreTreeState) {
        // A. 首次启动：使用从 JSON 读取的缓存
        expandedCollections = startupExpandedCollections;
        scrollPos = startupTreeScrollPos;
        isFirstTreeRefresh = false; // 标记已使用，下次刷新就走逻辑 B
    } else {
        // B. 运行时刷新：使用 UI 当前的状态
        for (int i = 0; i < ui->collectionTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = ui->collectionTree->topLevelItem(i);
            if (item->isExpanded()) {
                expandedCollections.insert(item->data(0, ROLE_COLLECTION_NAME).toString());
            }
        }
        scrollPos = ui->collectionTree->verticalScrollBar()->value();
    }

    // 清理和生成
    ui->collectionTree->clear();
    ui->collectionTree->setAnimated(true);
    ui->collectionTree->setIconSize(QSize(32, 32));
    ui->collectionTree->setRootIsDecorated(false);
    ui->collectionTree->setIndentation(10); // 子节点的缩进保持正常
    ui->collectionTree->setExpandsOnDoubleClick(false);

    QFont categoryFont = ui->collectionTree->font();
    categoryFont.setBold(true);
    categoryFont.setPointSize(10);

    const QString PRE_OPEN   = " - "; // 展开时：减号 + 空格
    const QString PRE_CLOSED = " + "; // 折叠时：加号 + 空格

    QMap<QString, QListWidgetItem*> visibleItemMap; // BaseName -> Item
    QMap<QString, int> visibleItemRank;             // BaseName -> SortIndex (排名)

    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);

        // 关键点：如果它被搜索/底模筛选隐藏了，就不放入 Map
        if (item->isHidden()) continue;

        QString baseName = item->data(ROLE_MODEL_NAME).toString();
        visibleItemMap.insert(baseName, item);
        visibleItemRank.insert(baseName, i); // 记录它在列表中的顺序
    }

    // 定义排序 Lambda：让树节点按照 modelList 的顺序排列
    auto rankSort = [&](const QString &s1, const QString &s2) {
        return visibleItemRank.value(s1) < visibleItemRank.value(s2);
    };

    // 辅助 Lambda：添加子节点
    auto addModelChildren = [&](QTreeWidgetItem *parent, QStringList models) {
        // 1. 过滤：移除不可见的模型
        QMutableStringListIterator it(models);
        while (it.hasNext()) {
            if (!visibleItemMap.contains(it.next())) {
                it.remove();
            }
        }

        // 2. 排序：按照 modelList 的顺序重排
        std::sort(models.begin(), models.end(), rankSort);

        // 3. 生成节点
        for (const QString &baseName : models) {
            if (visibleItemMap.contains(baseName)) {
                QListWidgetItem *sourceItem = visibleItemMap.value(baseName);

                QTreeWidgetItem *child = new QTreeWidgetItem(parent);
                child->setText(0, sourceItem->text());
                child->setData(0, ROLE_FILE_PATH, sourceItem->data(ROLE_FILE_PATH));
                child->setData(0, ROLE_PREVIEW_PATH, sourceItem->data(ROLE_PREVIEW_PATH));
                child->setData(0, ROLE_NSFW_LEVEL, sourceItem->data(ROLE_NSFW_LEVEL));
                child->setData(0, ROLE_MODEL_NAME, sourceItem->data(ROLE_MODEL_NAME));
                child->setIcon(0, sourceItem->icon());
            }
        }
    };

    // =========================================================
    // 3. 生成节点 (使用上面的可见数据源)
    // =========================================================

    // --- 未分类 ---
    QSet<QString> categorizedSet;
    for (auto it = collections.begin(); it != collections.end(); ++it) {
        for (const QString &m : it.value()) categorizedSet.insert(m);
    }
    QStringList uncatModels;
    // 只遍历可见的模型
    for (auto it = visibleItemMap.begin(); it != visibleItemMap.end(); ++it) {
        if (!categorizedSet.contains(it.key())) uncatModels.append(it.key());
    }

    int uncatCount = uncatModels.count();
    if (uncatCount > 0 || optShowEmptyCollections){
        // 只有当有内容时才显示“未分类” (可选，这里我设置为始终显示，保持结构稳定)
        QTreeWidgetItem *uncategorizedNode = new QTreeWidgetItem(ui->collectionTree);
        bool isUncatExpanded = expandedCollections.contains(FILTER_UNCATEGORIZED);
        uncategorizedNode->setExpanded(isUncatExpanded);
        uncategorizedNode->setText(0, (isUncatExpanded ? PRE_OPEN : PRE_CLOSED) + "未分类 / Uncategorized (" + QString::number(uncatCount) + ")");
        uncategorizedNode->setData(0, ROLE_IS_COLLECTION_NODE, true);
        uncategorizedNode->setData(0, ROLE_COLLECTION_NAME, FILTER_UNCATEGORIZED);
        uncategorizedNode->setData(0, ROLE_ITEM_COUNT, uncatCount);
        uncategorizedNode->setFont(0, categoryFont);

        // 排序并添加
        std::sort(uncatModels.begin(), uncatModels.end(), rankSort);
        // 这里手动添加循环，因为 uncatModels 已经是过滤好的了
        for (const QString &baseName : uncatModels) {
            QListWidgetItem *sourceItem = visibleItemMap.value(baseName);
            QTreeWidgetItem *child = new QTreeWidgetItem(uncategorizedNode);
            child->setText(0, sourceItem->text());
            child->setData(0, ROLE_FILE_PATH, sourceItem->data(ROLE_FILE_PATH));
            child->setData(0, ROLE_PREVIEW_PATH, sourceItem->data(ROLE_PREVIEW_PATH));
            child->setData(0, ROLE_NSFW_LEVEL, sourceItem->data(ROLE_NSFW_LEVEL));
            child->setData(0, ROLE_MODEL_NAME, sourceItem->data(ROLE_MODEL_NAME));
            child->setIcon(0, sourceItem->icon());
        }
    }
    // --- 收藏夹 ---
    // 对收藏夹名字排序 (使用自然排序 QCollator)
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setIgnorePunctuation(false);

    QList<QString> collectionNames = collections.keys();
    collectionNames.removeAll(FILTER_UNCATEGORIZED);
    std::sort(collectionNames.begin(), collectionNames.end(), [&](const QString &s1, const QString &s2){
        return collator.compare(s1, s2) < 0;
    });

    for (const QString &colName : collectionNames) {
        // 获取该收藏夹的所有模型
        QStringList models = collections.value(colName);

        int visibleCount = 0;
        for(const QString &m : models) {
            if(visibleItemMap.contains(m)) visibleCount++;
        }

        if (visibleCount > 0 || optShowEmptyCollections) {
            QTreeWidgetItem *collectionNode = new QTreeWidgetItem(ui->collectionTree);
            collectionNode->setData(0, ROLE_IS_COLLECTION_NODE, true);
            collectionNode->setData(0, ROLE_COLLECTION_NAME, colName);
            collectionNode->setData(0, ROLE_ITEM_COUNT, visibleCount);
            collectionNode->setFont(0, categoryFont);

            bool isColExpanded = expandedCollections.contains(colName);
            collectionNode->setExpanded(isColExpanded);

            collectionNode->setText(0, (isColExpanded ? PRE_OPEN : PRE_CLOSED) + colName + " (" + QString::number(visibleCount) + ")");

            // 调用辅助函数，它会自动过滤掉不匹配搜索的模型，并按当前规则排序
            addModelChildren(collectionNode, models);
        }
    }

    // 恢复滚动条
    // 使用 QTimer 0 延时，确保 UI 布局完成后再滚动，否则可能滚不到位
    if (scrollPos > 0) {
        QTimer::singleShot(0, this, [this, scrollPos](){
            ui->collectionTree->verticalScrollBar()->setValue(scrollPos);
        });
    }
}

// 辅助函数：添加占位符
void MainWindow::addPlaceholderChild(QTreeWidgetItem *parent) {
    QTreeWidgetItem *dummy = new QTreeWidgetItem();
    dummy->setText(0, "Loading...");
    dummy->setData(0, ROLE_IS_PLACEHOLDER, true);
    parent->addChild(dummy);
}

void MainWindow::filterModelsByCollection(const QString &collectionName)
{
    currentCollectionFilter = collectionName; // 更新当前的过滤状态

    // 刷新 modelList 的可见性
    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        QString baseName = item->data(ROLE_MODEL_NAME).toString(); // 获取模型的基础名称

        bool shouldBeVisible = false;

        if (currentCollectionFilter.isEmpty()) { // "ALL / 全部模型"
            shouldBeVisible = true;
        } else if (currentCollectionFilter == FILTER_UNCATEGORIZED) {
            bool categorized = false;
            for (auto it = collections.begin(); it != collections.end(); ++it) {
                if (it.value().contains(baseName)) {
                    categorized = true;
                    break;
                }
            }
            shouldBeVisible = !categorized;
        } else { // 特定收藏夹
            QStringList modelsInSelectedCollection = collections.value(currentCollectionFilter);
            shouldBeVisible = modelsInSelectedCollection.contains(baseName);
        }

        // --- NSFW 过滤逻辑 (需要复制 scanModels 中的逻辑以保持一致) ---
        int nsfwLevel = item->data(ROLE_NSFW_LEVEL).toInt();
        bool isNSFW = nsfwLevel > optNSFWLevel;
        if (optFilterNSFW && isNSFW && optNSFWMode == 0) {
            shouldBeVisible = false; // 隐藏模式下，NSFW 不可见
        }

        item->setHidden(!shouldBeVisible);
    }

    // 清除搜索框（因为现在是按收藏夹过滤）
    ui->searchEdit->clear();

    // 重新执行排序（排序是基于可见项进行的）
    // executeSort(); // 理论上这里不需要重新排序，只需要刷新可见性。
    // 但为了保持排序结果和可见性一致，可以调用。
    // 这里暂时不调，因为 modelList 的 item 隐藏后排序没意义。

    // 刷新主页大图列表
    refreshHomeGallery();

    ui->statusbar->showMessage(QString("当前过滤: %1").arg(currentCollectionFilter.isEmpty() ? "全部模型" : currentCollectionFilter));
}

void MainWindow::cancelPendingTasks()
{
    // QThreadPool::clear() 会移除所有尚未开始的任务
    // 正在运行的任务无法强制停止，但它们很快就会结束
    threadPool->clear();

    // 可选：如果之前的逻辑有正在下载的队列，也可以在这里清空
    // downloadQueue.clear();
    // isDownloading = false;
}

void MainWindow::syncTreeSelection(const QString &filePath)
{
    if (filePath.isEmpty()) return;

    // 临时阻断信号，防止 setExpanded 触发不需要的逻辑（视你的信号连接情况而定）
    // 这里通常不需要阻断，因为我们需要 setExpanded 触发 updateText (+/-) 的逻辑
    // ui->collectionTree->blockSignals(true);

    bool found = false;

    // 1. 遍历所有顶级节点 (收藏夹/未分类)
    for (int i = 0; i < ui->collectionTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *parent = ui->collectionTree->topLevelItem(i);

        // 2. 遍历子节点 (模型)
        for (int j = 0; j < parent->childCount(); ++j) {
            QTreeWidgetItem *child = parent->child(j);

            // 比较文件路径
            if (child->data(0, ROLE_FILE_PATH).toString() == filePath) {

                // === 核心动作 A: 展开父节点 ===
                if (!parent->isExpanded()) {
                    parent->setExpanded(true);
                    // 提示：如果你使用了上一轮的信号槽 (onTreeItemExpanded)
                    // 这里 setExpanded(true) 会自动触发信号把 "+" 变成 "-"，非常完美
                }

                // === 核心动作 B: 选中并滚动 ===
                ui->collectionTree->setCurrentItem(child);
                child->setSelected(true); // 显式选中
                ui->collectionTree->scrollToItem(child, QAbstractItemView::PositionAtCenter);

                found = true;
                break;
            }
        }
        if (found) break; // 找到后跳出外层循环
    }

    // ui->collectionTree->blockSignals(false);
}

void MainWindow::onMenuSwitchToAbout()
{
    // 切换到 rootStack 的最后一页 (即 pageAbout)
    ui->rootStack->setCurrentWidget(ui->pageAbout);
}

void MainWindow::onCheckUpdateClicked()
{
    ui->statusbar->showMessage("正在连接 GitHub 检查更新...", 3000);
    ui->btnCheckUpdate->setText("⏳ Checking...");
    ui->btnCheckUpdate->setEnabled(false);

    QNetworkRequest request((QUrl(GITHUB_REPO_API)));
    request.setHeader(QNetworkRequest::UserAgentHeader, currentUserAgent);

    QNetworkReply *reply = netManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply](){
        onUpdateApiReceived(reply);
    });
}

void MainWindow::onUpdateApiReceived(QNetworkReply *reply)
{
    ui->btnCheckUpdate->setText("🚀 检查更新 / Check for Updates");
    ui->btnCheckUpdate->setEnabled(true);
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        QMessageBox::warning(this, "检查失败", "无法连接到 GitHub API:\n" + reply->errorString());
        return;
    }

    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    QJsonObject root = doc.object();

    // GitHub API 返回的 tag_name (例如 "1.1.1" 或 "1.1.5")
    QString remoteTag = root["tag_name"].toString();
    QString htmlUrl = root["html_url"].toString();
    QString body = root["body"].toString();

    if (remoteTag.isEmpty()) {
        QMessageBox::warning(this, "错误", "无法解析版本信息 (Rate Limit Exceeded?)。");
        return;
    }

    // === 版本比较逻辑 (适配不带 v 的情况) ===
    // 如果 remoteTag 带有 'v' 前缀而 CURRENT_VERSION 没有，可以手动去除
    if (remoteTag.startsWith("v", Qt::CaseInsensitive)) {
        remoteTag = remoteTag.mid(1);
    }

    // 简单比较：只要字符串不相等，且远程版本号通常比本地长或大
    // 更严谨的方法是用 semver 库，但这里我们可以写个简单的辅助判断

    bool hasNewVersion = false;

    if (remoteTag != CURRENT_VERSION) {
        // 分割版本号进行数字比较 (1.1.1 vs 1.0.0)
        QStringList remoteParts = remoteTag.split('.');
        QStringList localParts = CURRENT_VERSION.split('.');

        int len = qMax(remoteParts.size(), localParts.size());
        for (int i = 0; i < len; ++i) {
            int r = (i < remoteParts.size()) ? remoteParts[i].toInt() : 0;
            int l = (i < localParts.size()) ? localParts[i].toInt() : 0;

            if (r > l) {
                hasNewVersion = true;
                break;
            } else if (r < l) {
                // 本地版本比远程还新（开发版），不算更新
                hasNewVersion = false;
                break;
            }
        }
    }

    if (hasNewVersion) {
        // 发现新版本
        QMessageBox msgBox(this);
        msgBox.setWindowTitle("发现新版本");
        msgBox.setTextFormat(Qt::RichText);
        msgBox.setText(QString("<h3>🚀 发现新版本: %1</h3>"
                               "<p>当前版本: %2</p>"
                               "<hr>"
                               "<p><b>更新日志:</b></p><pre style='font-size:11px'>%3</pre>")
                           .arg(remoteTag)
                           .arg(CURRENT_VERSION)
                           .arg(body));

        QPushButton *btnGo = msgBox.addButton("前往下载 / Download", QMessageBox::AcceptRole);
        msgBox.addButton("稍后 / Later", QMessageBox::RejectRole);
        msgBox.exec();

        if (msgBox.clickedButton() == btnGo) {
            QDesktopServices::openUrl(QUrl(htmlUrl));
        }
    } else {
        QMessageBox::information(this, "检查更新", QString("当前已是最新版本 (%1)。").arg(CURRENT_VERSION));
    }
}

QString MainWindow::getRandomUserAgent() {
    QStringList uas;
    // Chrome Win10
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/132.0.6834.83 Safari/537.36";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/133.0.6943.50 Safari/537.36";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36";
    uas << "Mozilla/5.0 (Windows NT 11.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
    // Edge Win10
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36 Edg/120.0.0.0";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36 Edg/121.0.0.0";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36 Edg/131.0.0.0";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/132.0.0.0 Safari/537.36 Edg/132.0.0.0";
    uas << "Mozilla/5.0 (Windows NT 11.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36 Edg/131.0.2903.99";
    // Windows Firefox
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:132.0) Gecko/20100101 Firefox/132.0";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:133.0) Gecko/20100101 Firefox/133.0";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:134.0) Gecko/20100101 Firefox/134.0";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:123.0) Gecko/20100101 Firefox/123.0";
    uas << "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:122.0) Gecko/20100101 Firefox/122.0";
    // macOS Chrome
    // Intel Mac
    uas << "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
    uas << "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/132.0.0.0 Safari/537.36";
    // macOS Safari
    uas << "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_7_1) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.6 Safari/605.1.15";
    uas << "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_1) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.1 Safari/605.1.15";
    // macOS Firefox
    uas << "Mozilla/5.0 (Macintosh; Intel Mac OS X 14.7; rv:132.0) Gecko/20100101 Firefox/132.0";
    // Linux
    uas << "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
    uas << "Mozilla/5.0 (X11; Linux x86_64; rv:132.0) Gecko/20100101 Firefox/132.0";
    uas << "Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:133.0) Gecko/20100101 Firefox/133.0";

    // 随机取一个
    int index = QRandomGenerator::global()->bounded(uas.size());
    return uas.at(index);
}

// 加载缓存
void MainWindow::loadUserGalleryCache() {
    imageCache.clear();
    QString configDir = qApp->applicationDirPath() + "/config";
    QFile file(configDir + "/user_gallery_cache.json");
    if (!file.open(QIODevice::ReadOnly)) return;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    QJsonObject root = doc.object();

    for (auto it = root.begin(); it != root.end(); ++it) {
        QJsonObject obj = it.value().toObject();
        UserImageInfo info;
        info.path = it.key();
        info.prompt = obj["p"].toString();
        info.negativePrompt = obj["np"].toString();
        info.parameters = obj["param"].toString();
        info.lastModified = obj["t"].toVariant().toLongLong();

        // 恢复 Tags (为了节省空间，JSON里可以不存tags，读取时解析，或者也存进去)
        // 这里建议直接解析，因为 parsePromptsToTags 是纯内存操作，很快
        info.cleanTags = parsePromptsToTags(info.prompt);

        imageCache.insert(info.path, info);
    }
}

// 保存缓存
void MainWindow::saveUserGalleryCache() {
    QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);

    QJsonObject root;
    // 遍历当前的 imageCache
    for (auto it = imageCache.begin(); it != imageCache.end(); ++it) {
        const UserImageInfo &info = it.value();
        QJsonObject obj;
        obj["p"] = info.prompt;
        obj["np"] = info.negativePrompt;
        obj["param"] = info.parameters;
        obj["t"] = QString::number(info.lastModified); // 存为字符串避免精度问题

        root.insert(info.path, obj);
    }

    QFile file(configDir + "/user_gallery_cache.json");
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)); // Compact 模式减小体积
    }
}

void MainWindow::updateModelListNames()
{
    // 暂时关闭排序，防止修改文本时列表乱跳
    ui->modelList->setSortingEnabled(false);

    for(int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);

        // 获取文件名 (BaseName)
        QString baseName = item->data(ROLE_MODEL_NAME).toString();
        // 获取 Civitai 名
        QString civitName = item->data(ROLE_CIVITAI_NAME).toString();

        if (optUseCivitaiName && !civitName.isEmpty()) {
            item->setText(civitName);
        } else {
            item->setText(baseName);
        }
    }

    // 恢复排序 (executeSort 会处理)
}
