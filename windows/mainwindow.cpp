#include "utils/applanguage.h"
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "utils/tagusage.h"
#include "utils/modelbrowserurl.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDebug>
#include <QClipboard>
#include <QScreen>
#include <QMouseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QPainter>
#include <QPushButton>
#include <QStyleOptionButton>
#include <QLabel>
#include <QScrollBar>
#include <QScrollArea>
#include <QMenu>
#include <QActionGroup>
#include <QInputDialog>
#include <QComboBox>
#include <QJsonArray>
#include <QPainterPath>
#include <QImageReader>
#include <QPair>
#include <QTimer>
#include <QTextDocument>
#include <QTextBoundaryFinder>
#include <QTextEdit>
#include <QRegularExpression>
#include <QImage>
#include <QDirIterator>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSignalBlocker>
#include <QColorDialog>
#include <QBrush>
#include <QStyledItemDelegate>
#include <QApplication>
#include <QStyleHints>
#include <QCoreApplication>
#include <QStyle>
#include <QSizePolicy>
#include <QProgressBar>
#include <QSaveFile>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <utility>
#include <QElapsedTimer>
#include <QSharedPointer>

#include "utils/imageloader.h"
#include "utils/imagemetadataparser.h"
#include "utils/translationcsv.h"
#include "tools/comfyworkflowviewer.h"
#include "tools/llmpromptwidget.h"
#include "dialogs/pathlistdialog.h"
#include "tools/tagbrowserwidget.h"
#include "tools/syncwidget.h"
#include "tools/promptparserwidget.h"
#include "tools/usageanalysiswidget.h"
#include "tools/prompttemplatelibrarywidget.h"
#include "pages/launcherwidget.h"
#include "pages/downloadmanager.h"
#include "pages/downloadspage.h"
#include "pages/settingspage.h"
#include "pages/aboutpage.h"
#include "pages/localmetaeditorpage.h"
#include "pages/usergallerypage.h"
#include "pages/homegallerypage.h"
#include "dialogs/modelnotedialog.h"
#include "dialogs/imageviewerdialog.h"
#include "dialogs/textviewerdialog.h"
#include "dialogs/themeeditordialog.h"
#include "utils/styleconstants.h"
#include "utils/fileutils.h"
#include "utils/downloadstatus.h"
#include "utils/modelmetadatacodec.h"
#include "utils/modelscanner.h"
#include "utils/modelimagematcher.h"
#include "utils/civarchiveparser.h"
#include "utils/previewimagestore.h"
#include "utils/modelfilter.h"
#include "utils/pathutils.h"
#include "utils/gallerymetadata.h"
#include "utils/metadatainspection.h"
#include "utils/imagepresentation.h"
#include "utils/tagutils.h"
#include "utils/modeluserdatastore.h"
#include "utils/usergallerycache.h"
#include "utils/civitaiclient.h"
#include "services/appupdatechecker.h"
#include "services/previewdownloadqueue.h"
#include "services/metadatasyncservice.h"
#include "services/modelupdatecoordinator.h"
#include "widgets/flowlayout.h"
#include "widgets/highlightitemdelegate.h"

namespace {
QVector<TagTranslationSource> buildTagTranslationSources(const QStringList &paths,
                                                         const QSet<QString> &disabledPaths)
{
    QVector<TagTranslationSource> sources;
    sources.reserve(paths.size());
    for (const QString &path : paths) {
        TagTranslationSource source;
        source.path = QFileInfo(path).absoluteFilePath();
        source.enabled = !disabledPaths.contains(source.path);
        sources.append(source);
    }
    return sources;
}

QString normalizedHomeTagKey(const QString &tag)
{
    return tag.trimmed().toCaseFolded();
}

QLabel *createMutedEmptyLabel(const QString &text, Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter)
{
    auto *label = new QLabel(text);
    label->setStyleSheet(AppStyle::MutedLabelStyle());
    label->setAlignment(alignment);
    return label;
}

template<typename ButtonT>
ButtonT *setupFilterChipButton(ButtonT *button,
                               const QString &tooltip,
                               const QString &tagSource = QString(),
                               int maxWidth = -1)
{
    button->setProperty("class", "filterChip");
    if (!tagSource.isEmpty()) {
        button->setProperty("tagSource", tagSource);
    }
    button->setCursor(Qt::PointingHandCursor);
    if (!tooltip.isEmpty()) {
        button->setToolTip(tooltip);
    }
    if (maxWidth > 0) {
        button->setMaximumWidth(maxWidth);
        button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    }
    return button;
}

template<typename AssignRoleFn>
void copyCommonModelRoles(const QListWidgetItem *source, AssignRoleFn assign)
{
    static constexpr int kCopiedRoles[] = {
        ROLE_FILE_PATH,
        ROLE_PREVIEW_PATH,
        ROLE_NSFW_LEVEL,
        ROLE_MODEL_NAME,
        ROLE_CIVITAI_NAME,
        ROLE_MODEL_TYPE,
        ROLE_MODEL_TRAINED_WORDS,
        ROLE_MODEL_CREATOR,
        ROLE_MODEL_TAGS,
        ROLE_USER_RATING,
        ROLE_USER_NOTE,
        ROLE_USER_TAGS,
        ROLE_USER_CUSTOM_TRIGGERS,
        ROLE_MODEL_PREVIEW_STATE,
    };
    for (int role : kCopiedRoles) {
        assign(role, source->data(role));
    }
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    applyApplicationTheme(optThemeId, optCustomThemePath, false);
    // 顶级页面统一直接加入 rootStack（与 LauncherWidget 一致，不再用 .ui 里的空壳页面）。
    downloadsPage = new DownloadsPage(this);
    ui->rootStack->addWidget(downloadsPage);
    settingsPage = new SettingsPage(this);
    ui->rootStack->addWidget(settingsPage);
    aboutPage = new AboutPage(this);
    ui->rootStack->addWidget(aboutPage);
    aboutPage->setVersionText(CURRENT_VERSION);
    connect(aboutPage, &AboutPage::checkUpdateRequested, this, &MainWindow::onCheckUpdateClicked);
    homeGalleryPage = new HomeGalleryPage(this);
    ui->mainStack->insertWidget(0, homeGalleryPage);
    connect(homeGalleryPage, &HomeGalleryPage::createCollectionRequested, this, &MainWindow::onCreateCollection);
    connect(homeGalleryPage, &HomeGalleryPage::collectionFilterRequested, this, &MainWindow::onCollectionFilterClicked);
    connect(homeGalleryPage, &HomeGalleryPage::renameCollectionRequested, this, &MainWindow::renameCollectionByName);
    connect(homeGalleryPage, &HomeGalleryPage::deleteCollectionRequested, this, &MainWindow::deleteCollectionByName);
    connect(homeGalleryPage, &HomeGalleryPage::filtersChanged, this, [this]() {
        refreshHomeFilterChips();
        refreshHomeGallery();
    });
    connect(homeGalleryPage, &HomeGalleryPage::filterChipsRefreshRequested, this, &MainWindow::refreshHomeFilterChips);
    connect(homeGalleryPage, &HomeGalleryPage::modelClicked, this, &MainWindow::onHomeModelClicked);
    connect(homeGalleryPage, &HomeGalleryPage::contextMenuRequested, this, &MainWindow::showCollectionMenu);
    userGalleryPage = new UserGalleryPage(this);
    ui->detailContentStack->addWidget(userGalleryPage);
    userGalleryPage->setTranslationMap(&translationMap);
    userGalleryPage->setImageCache(&imageCache);
    connect(userGalleryPage, &UserGalleryPage::setSdFolderRequested, this, &MainWindow::onSetSdFolderClicked);
    connect(userGalleryPage, &UserGalleryPage::rescanRequested, this, &MainWindow::onRescanUserClicked);
    connect(userGalleryPage, &UserGalleryPage::openSettingsForTranslationRequested, this, [this]() {
        ui->rootStack->setCurrentWidget(settingsPage);
        if (settingsPage) settingsPage->focusTranslationPath();
    });
    connect(userGalleryPage, &UserGalleryPage::openGraphRequested,
            this, [this](int scope, const QString &selectedTagFocus) {
                QString focusQuery = selectedTagFocus;
                if (focusQuery.isEmpty() && !userGalleryGlobalMode && !currentMeta.filePath.isEmpty()) {
                    focusQuery = QFileInfo(currentMeta.filePath).completeBaseName();
                }
                openTagModelGraph(scope, focusQuery);
            });
    connect(userGalleryPage, &UserGalleryPage::imageSelected, this, [this](const QString &path) {
        ui->heroFrame->setProperty("fullImagePath", path);
        transitionToImage(path);
    });
    connect(userGalleryPage, &UserGalleryPage::openImageViewerRequested, this, &MainWindow::openImageViewer);
    connect(userGalleryPage, &UserGalleryPage::showInFolderRequested, this, &MainWindow::showFileInFolder);
    connect(userGalleryPage, &UserGalleryPage::showComfyWorkflowRequested, this, &MainWindow::showComfyWorkflowViewer);
    connect(userGalleryPage, &UserGalleryPage::showRawMetadataRequested, this, &MainWindow::showRawImageMetadataDialog);
    connect(userGalleryPage, &UserGalleryPage::jumpToModelRequested, this, &MainWindow::jumpToModelOrLora);
    connect(userGalleryPage, &UserGalleryPage::statusMessage, this, [this](const QString &text, int timeoutMs) {
        ui->statusbar->showMessage(text, timeoutMs);
    });
    connect(userGalleryPage, &UserGalleryPage::cacheEntriesParsed,
            this, [this](const QMap<QString, UserImageInfo> &newUpdates) {
                for (auto it = newUpdates.cbegin(); it != newUpdates.cend(); ++it) {
                    imageCache.insert(it.key(), it.value());
                }
                saveUserGalleryCache();
                refreshModelUsageStatsAsync();
            });
    localMetaEditorPage = new LocalMetaEditorPage(this);
    ui->detailContentStack->addWidget(localMetaEditorPage);
    connect(localMetaEditorPage, &LocalMetaEditorPage::metadataSaved, this, &MainWindow::onLocalMetaSaved);
    connect(localMetaEditorPage, &LocalMetaEditorPage::resetRequested, this, &MainWindow::onLocalMetaResetClicked);
    connect(localMetaEditorPage, &LocalMetaEditorPage::imagesChanged,
            this, [this](const QList<ImageInfo> &images, const QString &previewPath, const QString &invalidatedIconPath) {
                currentMeta.images = images;
                currentMeta.previewPath = previewPath;
                if (!invalidatedIconPath.isEmpty()) {
                    invalidateIconCacheForPath(invalidatedIconPath);
                }
            });

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
            if (!shouldBlur && optFilterGalleryNSFW && optGalleryNSFWMode == 1 &&
                ((ui && ui->detailContentStack && ui->detailContentStack->currentIndex() == 1) ||
                 userGalleryGlobalMode ||
                 (userGalleryPage && result.path == userGalleryPage->currentImagePath()))) {
                if (isGalleryImageNsfwByPath(result.path)) {
                    shouldBlur = true;
                }
            }
            if (shouldBlur) {
                nextHeroPixmap = ImagePresentation::applyNSFWBlur(rawPix);
            } else {
                nextHeroPixmap = rawPix;
            }
            // C. 准备背景图
            QSize targetSize = ui->scrollAreaWidgetContents ? ui->scrollAreaWidgetContents->size() : ui->backgroundLabel->size();
            if (QWidget *viewport = ui->scrollAreaWidgetContents ? ui->scrollAreaWidgetContents->parentWidget() : nullptr) {
                targetSize.setWidth(qMax(targetSize.width(), viewport->width()));
                targetSize.setHeight(qMax(targetSize.height(), viewport->height()));
            }
            if (targetSize.isEmpty()) targetSize = QSize(1920, 1080);
            if (ui->backgroundLabel) {
                const QRect bgRect(QPoint(0, 0), targetSize);
                if (ui->backgroundLabel->geometry() != bgRect) {
                    ui->backgroundLabel->setGeometry(bgRect);
                }
            }
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
    noPreviewIcon = generateNoPreviewIcon(false);
    smallNoPreviewIcon = generateNoPreviewIcon(true);
    if (localMetaEditorPage) {
        localMetaEditorPage->setThreadPool(threadPool);
        localMetaEditorPage->setPlaceholderIcon(placeholderIcon);
    }
    if (userGalleryPage) {
        userGalleryPage->setThreadPools(threadPool, backgroundThreadPool);
        userGalleryPage->setPlaceholderIcon(placeholderIcon);
    }
    if (homeGalleryPage) {
        homeGalleryPage->setThreadPools(threadPool, backgroundThreadPool);
        homeGalleryPage->setPlaceholderIcons(placeholderIcon, noPreviewIcon);
    }

    netManager = new QNetworkAccessManager(this);
    initServices();

    // === 1. 初始化菜单栏 ===
    initMenuBar();
    // === 2. 加载配置 ===
    loadGlobalConfig();   // 从 JSON 读选项
    syncUserGalleryOptions();
    syncHomeGalleryOptions();
    loadModelSyncFailures();
    // === 应用线程数 ===
    threadPool->setMaxThreadCount(optRenderThreadCount);
    backgroundThreadPool->setMaxThreadCount(optRenderThreadCount);
    // 样式设置
    QGraphicsDropShadowEffect *shadow = new QGraphicsDropShadowEffect;
    shadow->setBlurRadius(20);
    shadow->setColor(Qt::black);
    shadow->setOffset(0, 0);
    ui->lblModelName->setGraphicsEffect(shadow);

    ui->heroFrame->installEventFilter(this);
    ui->heroFrame->setCursor(Qt::PointingHandCursor);

    ui->btnFavorite->setContextMenuPolicy(Qt::CustomContextMenu);

    if (ui->scrollArea) {
        ui->scrollArea->installEventFilter(this);
        if (ui->scrollArea->viewport()) ui->scrollArea->viewport()->installEventFilter(this);
    }

    ui->collectionTree->setHeaderHidden(true); // 隐藏 "Collection / Model" 表头

    ui->btnModelsTab->setCheckable(true);
    ui->btnCollectionsTab->setCheckable(true);
    ui->btnModelsTab->setAutoExclusive(true);
    ui->btnCollectionsTab->setAutoExclusive(true);
    ui->btnModelsTab->setChecked(true);

    // === 主界面信号连接 ===
    connect(ui->modelList, &QListWidget::itemClicked, this, &MainWindow::onModelListClicked);
    connect(ui->comboSort, QOverload<int>::of(&QComboBox::currentIndexChanged),this, &MainWindow::onSortIndexChanged);
    connect(ui->comboBaseModel, &QComboBox::currentTextChanged,this, &MainWindow::onFilterBaseModelChanged);
    connect(ui->comboModelType, &QComboBox::currentTextChanged, this, [this](const QString &){ onSearchTextChanged(ui->searchEdit->text()); });

    // 侧栏“排序/筛选”按钮：用一个多级菜单承载 排序 / 底模 / 类型（三个原下拉框已隐藏，仅作状态存储）。
    QMenu *sortFilterMenu = new QMenu(this);
    ui->btnSortFilter->setMenu(sortFilterMenu);
    connect(sortFilterMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildSortFilterMenu);
    // 三个下拉任一变化时同步刷新按钮文案。
    connect(ui->comboSort, &QComboBox::currentTextChanged, this, [this](const QString &){ updateSortFilterButtonText(); });
    connect(ui->comboBaseModel, &QComboBox::currentTextChanged, this, [this](const QString &){ updateSortFilterButtonText(); });
    connect(ui->comboModelType, &QComboBox::currentTextChanged, this, [this](const QString &){ updateSortFilterButtonText(); });
    updateSortFilterButtonText();
    connect(ui->btnModelsTab, &QPushButton::clicked, this, &MainWindow::onModelsTabButtonClicked);
    connect(ui->btnCollectionsTab, &QPushButton::clicked, this, &MainWindow::onCollectionsTabButtonClicked);
    connect(ui->collectionTree, &QTreeWidget::itemClicked, this, &MainWindow::onCollectionTreeItemClicked);
    // 侧边栏右键菜单
    ui->modelList->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->modelList->setSelectionMode(QAbstractItemView::ExtendedSelection); // 开启 Shift/Ctrl 多选
    ui->modelList->setItemDelegate(new HighlightItemDelegate(ui->modelList));
    connect(ui->modelList, &QListWidget::customContextMenuRequested, this, &MainWindow::onSidebarContextMenu);
    ui->collectionTree->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->collectionTree->setItemDelegate(new HighlightItemDelegate(ui->collectionTree));
    connect(ui->collectionTree, &QTreeWidget::customContextMenuRequested, this, &MainWindow::onCollectionTreeContextMenu);
    ui->collectionTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // 工具栏按钮
    connect(ui->btnOpenUrl, &QPushButton::clicked, this, &MainWindow::onOpenUrlClicked);
    connect(ui->btnCopyLoraTag, &QPushButton::clicked, this, &MainWindow::onCopyLoraTagClicked);
    connect(ui->btnScanLocal, &QPushButton::clicked, this, &MainWindow::onScanLocalClicked);
    connect(ui->btnForceUpdate, &QPushButton::clicked, this, &MainWindow::onForceUpdateClicked);
    connect(ui->btnCheckModelUpdate, &QPushButton::clicked, this, [this]() {
        QList<QListWidgetItem*> items;
        if (QListWidgetItem *item = ui->modelList->currentItem(); isModelListItem(item)) items << item;
        checkUpdatesForItems(items, false, true);
    });
    connect(ui->btnEditMeta, &QPushButton::clicked, this, &MainWindow::onEditMetaTabClicked);
    connect(ui->btnShowDescriptionDetail, &QPushButton::clicked, this, &MainWindow::showModelDescriptionDialog);
    connect(ui->btnEditUserNote, &QPushButton::clicked, this, [this]() {
        if (QListWidgetItem *item = ui->modelList->currentItem(); isModelListItem(item)) {
            openModelNoteDialog(item);
        }
    });
    initDownloadsPage();
    searchDebounceTimer = new QTimer(this);
    searchDebounceTimer->setSingleShot(true);
    searchDebounceTimer->setInterval(120);
    connect(searchDebounceTimer, &QTimer::timeout, this, [this]() {
        if (ui && ui->searchEdit) onSearchTextChanged(ui->searchEdit->text());
    });
    connect(ui->searchEdit, &QLineEdit::textChanged, this, [this](const QString &) {
        if (searchDebounceTimer) searchDebounceTimer->start();
    });
    // 主页与画廊按钮
    connect(ui->btnHome, &QPushButton::clicked, this, &MainWindow::onHomeButtonClicked);
    connect(ui->btnGallery, &QPushButton::clicked, this, &MainWindow::onGalleryButtonClicked);

    // 切换 Tab 与图谱按钮
    connect(ui->btnShowUserGallery, &QPushButton::clicked, this, &MainWindow::onToggleDetailTab);
    if (ui->btnOpenModelGraph) {
        connect(ui->btnOpenModelGraph, &QPushButton::clicked, this, &MainWindow::onOpenModelGraphClicked);
    }
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

    detailGalleryBuildTimer = new QTimer(this);
    detailGalleryBuildTimer->setSingleShot(true);
    connect(detailGalleryBuildTimer, &QTimer::timeout, this, &MainWindow::buildGalleryBatch);

    if (ui->backgroundLabel && ui->scrollAreaWidgetContents) {
        ui->scrollAreaWidgetContents->installEventFilter(this);
        ui->backgroundLabel->setScaledContents(false);
        ui->backgroundLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        updateBackgroundImage();
    }

    clearDetailView();
    refreshHomeFilterChips();

    // 事件循环启动后再刷一次主题：setColorScheme 的调色板传播是异步的，会重置启动期
    // 各 West 标签条/视口在构造时钉好的 palette；这里在传播之后补钉一次，避免重启需手动切主题。
    QTimer::singleShot(0, this, [this](){ refreshLoadedToolPageThemes(); });

    QTimer::singleShot(0, this, [this](){
        ui->statusbar->showMessage(AppText::tr("正在扫描本地模型库..."));
        loadCollections();
        loadModelHighlightColors();
        loadModelUserNotes();

        if (optRestoreFilterSortState) {
            if (startupSidebarTab == 0) {
                onModelsTabButtonClicked();
            } else {
                onCollectionsTabButtonClicked();
            }
            {
                const QSignalBlocker blocker(ui->comboSort);
                const int maxIdx = qMax(0, ui->comboSort->count() - 1);
                ui->comboSort->setCurrentIndex(qBound(0, startupSortIndex, maxIdx));
            }
            if (ui->searchEdit) {
                const QSignalBlocker blocker(ui->searchEdit);
                ui->searchEdit->setText(startupSearchText);
            }
            currentCollectionFilter = startupCollectionFilter;
            if (homeGalleryPage) {
                HomeGalleryFilterState hs;
                hs.authorFilter = startupHomeAuthorFilter;
                hs.tagFilters = startupHomeTagFilters;
                hs.filterExpanded = startupHomeFilterExpanded;
                hs.tagsSortByCount = startupHomeFilterTagsSortByCount;
                homeGalleryPage->restoreFilterState(hs);
            }
            if (userGalleryPage) {
                UserGalleryFilterState state;
                state.userTagSortMode = startupUserTagSortMode;
                state.userTagLoraOnly = startupUserTagLoraOnly;
                state.userTagIncludeNegative = startupUserTagIncludeNegative;
                state.galleryPromptSortMode = startupGalleryPromptSortMode;
                state.galleryTagViewActive = startupGalleryTagViewActive;
                userGalleryPage->restoreFilterState(state);
            }
            refreshHomeCollectionsUI();
            refreshHomeFilterChips();
            updateSortFilterButtonText();
        } else {
            const QSignalBlocker blocker(ui->comboSort);
            ui->comboSort->setCurrentIndex(0);
            refreshHomeCollectionsUI();
        }

        // 扫描完成后（异步）刷新状态栏计数，并按需检查软件更新。
        auto afterScan = [this]() {
            int modelCount = 0;
            for (int i = 0; i < ui->modelList->count(); ++i) {
                if (isModelListItem(ui->modelList->item(i))) modelCount++;
            }
            ui->statusbar->showMessage(AppText::tr("加载完成，共 %1 个模型").arg(modelCount), 3000);
            if (optAutoCheckUpdatesOnStartup) {
                QTimer::singleShot(800, this, [this]() {
                    startAppUpdateCheck(true);
                });
            }
        };

        const QStringList activeLoraPaths = PathUtils::collectEnabledPaths(loraPaths, disabledLoraPaths);
        if (!activeLoraPaths.isEmpty()) {
            scanModels(activeLoraPaths, afterScan);
        } else {
            isFirstFilterSortRestore = false;
            executeSort();
            afterScan();
        }
        // Dispatch the model scan first; unrelated large caches must not delay it.
        loadUserGalleryCache();
        reloadTranslationMaps(true, true);
    });

}

QString forceWrap(const QString &text) { // 在“字形簇”之间插入零宽空格以允许换行
    // 不能按 QChar(UTF-16 码元)逐个插入：星标平面的 emoji（如 🟥 U+1F7E5）由代理对组成，
    // 在两个代理之间插入零宽空格会拆散代理对，导致渲染成 “?”。用 Grapheme 边界按整个字形簇切分，
    // 这样代理对、ZWJ 组合 emoji、组合附加符号都不会被破坏。
    if (text.isEmpty()) return text;
    QString result;
    result.reserve(text.size() * 2);
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    int start = 0;
    int end;
    while ((end = finder.toNextBoundary()) != -1) {
        result += QStringView(text).mid(start, end - start);
        result += QChar(0x200B); // 零宽空格
        start = end;
    }
    return result;
}

// 把 Civitai 模型类型规范化为侧栏「类型」筛选用的类别标签；返回空表示该模型没有类型信息。
// LoCon / DoRA / LyCORIS 等都归到 LoRA，避免下拉项过于零碎。
static ModelFilter::Record modelFilterRecord(const QListWidgetItem *item)
{
    ModelFilter::Record record;
    record.searchableText << item->data(ROLE_MODEL_NAME).toString()
                          << item->text()
                          << item->data(ROLE_CIVITAI_NAME).toString()
                          << item->data(ROLE_MODEL_CREATOR).toString()
                          << item->data(ROLE_MODEL_TAGS).toStringList()
                          << item->data(ROLE_USER_NOTE).toString()
                          << item->data(ROLE_USER_TAGS).toStringList()
                          << item->data(ROLE_USER_CUSTOM_TRIGGERS).toStringList();
    record.baseModel = item->data(ROLE_FILTER_BASE).toString();
    record.modelType = item->data(ROLE_MODEL_TYPE).toString();
    return record;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 启动器中有 A1111/ComfyUI 进程在运行时，关闭软件会一并结束它们，先弹窗确认（可在设置里关闭此提醒）。
    if (optConfirmCloseWhileLauncherRunning && launcherWidget && launcherWidget->hasRunningProcess()) {
        const QMessageBox::StandardButton choice = QMessageBox::question(
            this,
            AppText::tr("确认退出 / Confirm Exit"),
            AppText::tr("启动器中仍有 A1111/ComfyUI 进程正在运行，关闭软件会一并结束它们。\n确定要退出吗？"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (choice != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }
    QMainWindow::closeEvent(event);
}

MainWindow::~MainWindow()
{
    isShuttingDown = true;
    if (searchDebounceTimer) searchDebounceTimer->stop();
    if (homeGalleryPage) homeGalleryPage->shutdown();
    if (userGalleryPage) userGalleryPage->shutdown();
    if (bgResizeTimer) bgResizeTimer->stop();
    if (detailGalleryBuildTimer) detailGalleryBuildTimer->stop();
    if (transitionAnim) transitionAnim->stop();
    if (modelUpdateCoordinator) modelUpdateCoordinator->cancelAll();
    if (previewDownloadQueue) previewDownloadQueue->clearQueueOnly();
    if (netManager) {
        const QList<QNetworkReply*> replies = netManager->findChildren<QNetworkReply*>();
        for (QNetworkReply *reply : replies) {
            if (!reply) continue;
            reply->disconnect(this);
            reply->abort();
        }
    }
    if (downloadManager) downloadManager->shutdown();
    if (hashWatcher && hashWatcher->isRunning()) hashWatcher->waitForFinished();
    if (metadataScanWatcher && metadataScanWatcher->isRunning()) metadataScanWatcher->waitForFinished();
    if (metadataHealthWatcher && metadataHealthWatcher->isRunning()) metadataHealthWatcher->waitForFinished();
    if (imageLoadWatcher && imageLoadWatcher->isRunning()) imageLoadWatcher->waitForFinished();
    saveGlobalConfig();
    cancelPendingTasks();
    if (backgroundThreadPool) backgroundThreadPool->clear();
    threadPool->waitForDone();
    backgroundThreadPool->waitForDone();
    QCoreApplication::removePostedEvents(this);
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
    refreshHomeFilterChips();
    refreshHomeGallery();
    refreshHomeCollectionsUI();
}

void MainWindow::loadCollections()
{
    collections = ModelUserDataStore::loadCollections(qApp->applicationDirPath() + "/config/collections.json");
    refreshHomeCollectionsUI();
}

void MainWindow::saveCollections()
{
    ModelUserDataStore::saveCollections(qApp->applicationDirPath() + "/config/collections.json", collections);
    refreshHomeCollectionsUI();
}

void MainWindow::loadModelHighlightColors()
{
    modelHighlightColors = ModelUserDataStore::loadHighlightColors(qApp->applicationDirPath() + "/config/model_colors.json");
}

void MainWindow::saveModelHighlightColors()
{
    ModelUserDataStore::saveHighlightColors(qApp->applicationDirPath() + "/config/model_colors.json", modelHighlightColors);
}

void MainWindow::onCreateCollection()
{
    bool ok;
    QString text = QInputDialog::getText(this, AppText::tr("新建收藏夹"), AppText::tr("收藏夹名称:"), QLineEdit::Normal, "", &ok);
    if (ok && !text.trimmed().isEmpty()) {
        if (!collections.contains(text)) {
            collections.insert(text, QStringList());
            saveCollections();
            refreshCollectionTreeView();
        }
    }
}

void MainWindow::syncHomeGalleryOptions()
{
    if (!homeGalleryPage) return;
    HomeGalleryOptions opts;
    opts.filterNsfw = optFilterNSFW;
    opts.nsfwMode = optNSFWMode;
    opts.nsfwLevel = optNSFWLevel;
    homeGalleryPage->setOptions(opts);
}

void MainWindow::renameCollectionByName(const QString &name)
{
    bool ok = false;
    const QString newName = QInputDialog::getText(
        this, AppText::tr("重命名收藏夹"), AppText::tr("新名称:"), QLineEdit::Normal, name, &ok);
    if (!ok || newName.trimmed().isEmpty() || newName == name) return;
    if (collections.contains(newName)) {
        QMessageBox::warning(this, AppText::tr("错误"), AppText::tr("该名称已存在！"));
        return;
    }
    const QStringList files = collections.value(name);
    collections.insert(newName, files);
    collections.remove(name);
    if (currentCollectionFilter == name) currentCollectionFilter = newName;
    saveCollections();
}

void MainWindow::deleteCollectionByName(const QString &name)
{
    const auto reply = QMessageBox::question(
        this,
        AppText::tr("确认删除"),
        AppText::tr("确定要删除收藏夹 \"%1\" 吗？\n(里面的模型不会被删除，仅删除分类)").arg(name),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    collections.remove(name);
    if (currentCollectionFilter == name) {
        currentCollectionFilter.clear();
    }
    saveCollections();
    refreshHomeGallery();
}

void MainWindow::refreshHomeCollectionsUI()
{
    if (!homeGalleryPage) return;
    homeGalleryPage->refreshCollectionsBar(collections.keys(), currentCollectionFilter, FILTER_UNCATEGORIZED);
}

void MainWindow::refreshHomeGallery()
{
    if (!homeGalleryPage) return;

    placeholderIcon = generatePlaceholderIcon();
    noPreviewIcon = generateNoPreviewIcon(false);
    homeGalleryPage->setPlaceholderIcons(placeholderIcon, noPreviewIcon);
    syncHomeGalleryOptions();
    cancelPendingTasks();

    const QString searchText = ui->searchEdit->text().trimmed();
    const QString targetBaseModel = ui->comboBaseModel->currentIndex() == 0 ? "All" : ui->comboBaseModel->currentText();
    const QString targetModelType = ui->comboModelType->currentIndex() == 0 ? "All" : ui->comboModelType->currentText();
    const QString authorFilter = homeGalleryPage->authorFilter();
    const QSet<QString> tagFilters = homeGalleryPage->tagFilters();

    QSet<QString> requiredTagKeys;
    if (!tagFilters.isEmpty()) {
        requiredTagKeys.reserve(tagFilters.size());
        for (const QString &tag : tagFilters) {
            const QString key = tag.trimmed().toCaseFolded();
            if (!key.isEmpty()) requiredTagKeys.insert(key);
        }
    }

    const bool filterByCollection = !currentCollectionFilter.isEmpty();
    const bool filterUncategorized = (currentCollectionFilter == FILTER_UNCATEGORIZED);
    QSet<QString> categorizedSet;
    QSet<QString> activeCollectionSet;
    if (filterUncategorized) {
        for (auto it = collections.cbegin(); it != collections.cend(); ++it) {
            for (const QString &member : it.value()) {
                categorizedSet.insert(member);
            }
        }
    } else if (filterByCollection) {
        const QStringList members = collections.value(currentCollectionFilter);
        activeCollectionSet.reserve(members.size());
        for (const QString &member : members) {
            activeCollectionSet.insert(member);
        }
    }

    QVector<HomeGalleryEntry> entries;
    entries.reserve(ui->modelList->count());

    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *sideItem = ui->modelList->item(i);
        if (!isModelListItem(sideItem)) continue;

        const int nsfwLevel = sideItem->data(ROLE_NSFW_LEVEL).toInt();
        const bool isNSFW = nsfwLevel > optNSFWLevel;
        QString modelKey = sideItem->data(ROLE_MODEL_NAME).toString();
        if (modelKey.isEmpty()) modelKey = QFileInfo(sideItem->data(ROLE_FILE_PATH).toString()).completeBaseName();
        QString displayName = sideItem->text();
        if (displayName.isEmpty()) displayName = modelKey;
        const QString filePath = sideItem->data(ROLE_FILE_PATH).toString();

        if (optFilterNSFW && isNSFW && optNSFWMode == 0) {
            continue;
        }

        if (!ModelFilter::matches(modelFilterRecord(sideItem), searchText,
                                  targetBaseModel, targetModelType)) continue;

        if (!authorFilter.isEmpty()) {
            const QString itemCreator = sideItem->data(ROLE_MODEL_CREATOR).toString();
            if (itemCreator.compare(authorFilter, Qt::CaseInsensitive) != 0) {
                continue;
            }
        }

        if (!requiredTagKeys.isEmpty()) {
            const QStringList itemModelTags = sideItem->data(ROLE_MODEL_TAGS).toStringList();
            const QStringList itemUserTags = sideItem->data(ROLE_USER_TAGS).toStringList();
            if (itemModelTags.isEmpty() && itemUserTags.isEmpty()) continue;

            QSet<QString> modelTagSet;
            modelTagSet.reserve(itemModelTags.size() + itemUserTags.size());
            auto collectNormalized = [&modelTagSet](const QStringList &tags) {
                for (const QString &tag : tags) {
                    const QString key = tag.trimmed().toCaseFolded();
                    if (!key.isEmpty()) modelTagSet.insert(key);
                }
            };
            collectNormalized(itemModelTags);
            collectNormalized(itemUserTags);
            bool allTagsMatched = true;
            for (const QString &reqKey : std::as_const(requiredTagKeys)) {
                if (!modelTagSet.contains(reqKey)) {
                    allTagsMatched = false;
                    break;
                }
            }
            if (!allTagsMatched) continue;
        }

        if (filterByCollection) {
            if (filterUncategorized) {
                if (categorizedSet.contains(modelKey)) continue;
            } else {
                if (!activeCollectionSet.contains(modelKey)) continue;
            }
        }

        entries.append({sideItem, formatModelUserNoteTooltip(filePath, displayName)});
    }

    homeGalleryPage->setModelEntries(entries);
}

void MainWindow::invalidateIconCacheForPath(const QString &previewPath)
{
    if (previewPath.isEmpty()) return;
    const QString cleaned = QDir::cleanPath(previewPath);
    const QString abs = QFileInfo(previewPath).absoluteFilePath();
    const QStringList paths = {previewPath, cleaned, abs};
    for (const QString &p : paths) {
        if (p.isEmpty()) continue;
        sidebarIconCache.remove(QStringLiteral("norm|") + p);
        sidebarIconCache.remove(QStringLiteral("blur|") + p);
    }
    if (homeGalleryPage) homeGalleryPage->invalidateIconCacheForPath(previewPath);
}

void MainWindow::refreshHomeFilterChips()
{
    if (!homeGalleryPage) return;
    homeGalleryPage->refreshFilterChips(collectAvailableHomeFilterTags());
}

QList<HomeFilterTagInfo> MainWindow::collectAvailableHomeFilterTags() const
{
    QHash<QString, HomeFilterTagInfo> tagInfoByKey;
    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!isModelListItem(item)) continue;

        QSet<QString> itemTagKeys;
        auto registerTags = [&](const QStringList &tags, bool fromModel) {
            for (const QString &raw : tags) {
                const QString tag = raw.trimmed();
                if (tag.isEmpty()) continue;
                const QString key = normalizedHomeTagKey(tag);
                if (key.isEmpty()) continue;

                HomeFilterTagInfo &info = tagInfoByKey[key];
                if (info.tag.isEmpty()) info.tag = tag;
                if (fromModel) {
                    info.fromModel = true;
                } else {
                    info.fromUser = true;
                }
                itemTagKeys.insert(key);
            }
        };

        registerTags(item->data(ROLE_MODEL_TAGS).toStringList(), true);
        registerTags(item->data(ROLE_USER_TAGS).toStringList(), false);

        for (const QString &key : itemTagKeys) {
            tagInfoByKey[key].count += 1;
        }
    }

    const bool sortByCount = !homeGalleryPage || homeGalleryPage->isTagsSortByCount();
    QList<HomeFilterTagInfo> tags = tagInfoByKey.values();
    std::sort(tags.begin(), tags.end(), [sortByCount](const HomeFilterTagInfo &a, const HomeFilterTagInfo &b) {
        if (sortByCount && a.count != b.count) return a.count > b.count;
        return QString::localeAwareCompare(a.tag, b.tag) < 0;
    });
    return tags;
}

void MainWindow::setHomeAuthorFilter(const QString &author)
{
    if (homeGalleryPage) homeGalleryPage->setAuthorFilter(author);
}

void MainWindow::addHomeTagFilter(const QString &tag)
{
    if (homeGalleryPage) homeGalleryPage->addTagFilter(tag);
}

void MainWindow::clearHomeFilters()
{
    if (homeGalleryPage) homeGalleryPage->clearFilters();
}

// 点击主页的大图 -> 跳转详情页
void MainWindow::onHomeModelClicked(const QString &targetPath)
{
    if (targetPath.isEmpty()) return;

    cancelPendingTasks();

    // 在侧边栏 (modelList) 中寻找匹配该路径的项 (O(1) 查找)
    QListWidgetItem *matchItem = modelItemByPath.value(targetPath, nullptr);
    if (!matchItem) {
        for (int i = 0; i < ui->modelList->count(); ++i) {
            QListWidgetItem *sideItem = ui->modelList->item(i);
            if (sideItem->data(ROLE_FILE_PATH).toString() == targetPath) {
                matchItem = sideItem;
                break;
            }
        }
    }

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

// ---------------------------------------------------------
// 主页与收藏夹逻辑结束
// ---------------------------------------------------------

// === 辅助：生成正方形图标 ===

// === 核心：事件过滤器 (绘图 + 点击) ===
bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->heroFrame) {
        if (event->type() == QEvent::Paint) {
            QPainter painter(ui->heroFrame);

            // 绘制背景底色（浅色主题下使用浅色底，深色主题下保持黑底）
            const bool isLightTheme = AppStyle::color("windowBg").lightness() > 128;
            const QColor heroFallbackBg = isLightTheme ? AppStyle::color("headerBg") : QColor(Qt::black);
            painter.fillRect(ui->heroFrame->rect(), heroFallbackBg);

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
                    openImageViewerForPath(currentHeroPath);
                    return true;
                }
            }
        }
    }

    if (watched == ui->scrollAreaWidgetContents && event->type() == QEvent::Resize) {
        if (ui->backgroundLabel) {
            bgResizeTimer->start(0);
        }
    }

    if (event->type() == QEvent::Wheel && userGalleryPage && userGalleryPage->isVisible()) {
        if (watched == ui->scrollArea ||
            (ui->scrollArea && watched == ui->scrollArea->viewport()) ||
            watched == ui->scrollAreaWidgetContents) {
            if (userGalleryPage->isCursorInsideScrollAreas()) {
                userGalleryPage->handleWheelScroll(watched, static_cast<QWheelEvent*>(event));
                return true; // 拦截事件，防止穿透到外层的 scrollArea
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
                openImageViewer(currentModelPreviewPaths(), path);
                return true; // 消费事件
            }
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

// ---------------------------------------------------------
// 业务逻辑
// ---------------------------------------------------------

namespace {

// 纯函数：从 JSON root 解析模型标签（与 MainWindow::readModelTagsFromJson 同逻辑，可在工作线程调用）。

// 纯函数：从 JSON root 解析作者名（与 MainWindow::readModelCreatorFromJson 同逻辑）。

// 模型列表项的元数据（侧边栏/排序/筛选所需），可在工作线程中解析。


// 把解析好的元数据写入列表项（必须在主线程调用）。
void applyModelListMetadataToItem(QListWidgetItem *item, const ModelScanner::ModelListMetadata &m)
{
    item->setData(ROLE_SORT_DATE, m.sortDate);
    item->setData(ROLE_SORT_ADDED, m.sortAdded);
    item->setData(ROLE_SORT_DOWNLOADS, m.downloads);
    item->setData(ROLE_SORT_LIKES, m.likes);
    item->setData(ROLE_SORT_USAGE_COUNT, 0);
    item->setData(ROLE_SORT_LAST_USED, 0);
    item->setData(ROLE_FILTER_BASE, m.filterBase);
    item->setData(ROLE_NSFW_LEVEL, m.nsfwLevel);
    item->setData(ROLE_LOCAL_EDITED, m.localEdited);
    item->setData(ROLE_CIVITAI_MODEL_ID, m.modelId);
    item->setData(ROLE_CIVITAI_VERSION_ID, m.versionId);
    item->setData(ROLE_CIVITAI_SHA256, m.civitaiSha256);
    item->setData(ROLE_MODEL_CREATOR, m.creator);
    item->setData(ROLE_MODEL_TAGS, m.modelTags);
    item->setData(ROLE_MODEL_TYPE, m.modelType);
    item->setData(ROLE_MODEL_TRAINED_WORDS, m.trainedWords);
    item->setData(ROLE_MODEL_PREVIEW_STATE, static_cast<int>(m.previewState));
    if (!m.civitaiName.isEmpty()) item->setData(ROLE_CIVITAI_NAME, m.civitaiName);
}

// 扫描结果中的单个条目：路径信息 + 解析好的元数据。


} // namespace

void MainWindow::scanModels(const QString &path)
{
    scanModels(QStringList() << path);
}

void MainWindow::scanModels(const QStringList &paths, std::function<void()> onComplete)
{
    QElapsedTimer loadTimer;
    loadTimer.start();
    modelScanRunning = true;
    ++modelUsageStatsToken;
    // 同步清空列表，随后把繁重的目录遍历 + JSON 解析放到后台线程，避免大模型库卡 UI。
    modelItemByPath.clear();
    if (homeGalleryPage) homeGalleryPage->clearItems();
    ui->modelList->clear();
    ui->comboBaseModel->blockSignals(true);
    ui->comboBaseModel->clear();
    ui->comboBaseModel->addItem(AppText::tr("All"));
    ui->comboBaseModel->blockSignals(false);

    ui->comboModelType->blockSignals(true);
    ui->comboModelType->clear();
    ui->comboModelType->addItem(AppText::tr("All"));
    ui->comboModelType->blockSignals(false);

    ui->statusbar->showMessage(AppText::tr("正在扫描模型..."));

    const int token = ++modelScanToken;
    const bool recursive = optLoraRecursive;
    const QString cachePath = qApp->applicationDirPath() + "/config/model_list_cache.json";
    QFuture<QList<ModelScanner::ScannedModelEntry>> future = QtConcurrent::run(
        backgroundThreadPool,
        [paths, recursive, cachePath]() { return ModelScanner::scanModelsWorker(paths, recursive, cachePath); });

    auto *watcher = new QFutureWatcher<QList<ModelScanner::ScannedModelEntry>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, token, loadTimer, onComplete = std::move(onComplete)]() {
        const QList<ModelScanner::ScannedModelEntry> entries = watcher->result();
        watcher->deleteLater();
        // 期间又触发了新的扫描：丢弃过期结果，避免把旧条目塞进已被清空的列表。
        if (token != modelScanToken || isShuttingDown) return;

        smallPlaceholderIcon = generateSmallPlaceholderIcon(); // 侧边栏用带内边距的小占位X（当前主题色）
        smallNoPreviewIcon = generateNoPreviewIcon(true);
        modelItemByPath.clear();
        ui->modelList->setUpdatesEnabled(false);
        ui->comboBaseModel->blockSignals(true);

        QSet<QString> foundBaseModels;
        QSet<QString> foundModelTypes;
        int addedCount = 0;
        for (const ModelScanner::ScannedModelEntry &e : entries) {
            const bool isNSFW = e.meta.nsfwLevel > optNSFWLevel;
            if (optFilterNSFW && isNSFW && optNSFWMode == 0) continue; // 隐藏模式下跳过

            QListWidgetItem *item = new QListWidgetItem(e.baseName);
            item->setToolTip(e.fullPath);
            item->setData(ROLE_MODEL_NAME, e.baseName);
            item->setData(ROLE_FILE_PATH, e.fullPath);
            item->setData(ROLE_PREVIEW_PATH, e.previewPath);
            item->setData(ROLE_MODEL_ROOT_PATH, e.rootPath);
            item->setData(ROLE_MODEL_ROOT_NAME, e.rootName);
            item->setData(ROLE_MODEL_FILTER_VISIBLE, true);
            applyModelListMetadataToItem(item, e.meta);

            const QString civitaiName = item->data(ROLE_CIVITAI_NAME).toString();
            item->setText(optUseCivitaiName && !civitaiName.isEmpty() ? civitaiName : e.baseName);

            const ModelPreviewState previewState = static_cast<ModelPreviewState>(
                item->data(ROLE_MODEL_PREVIEW_STATE).toInt());
            const bool blurItem = (optFilterNSFW && isNSFW && optNSFWMode == 1);
            const QString sideCacheKey = (blurItem ? QStringLiteral("blur|") : QStringLiteral("norm|")) + e.previewPath;
            if (!e.previewPath.isEmpty() && sidebarIconCache.contains(sideCacheKey)) {
                item->setIcon(sidebarIconCache.value(sideCacheKey));
                item->setData(ROLE_PREVIEW_PLACEHOLDER, false);
                item->setData(ROLE_MODEL_PREVIEW_STATE, static_cast<int>(ModelPreviewState::RealPreview));
            } else {
                item->setIcon(previewState == ModelPreviewState::KnownNoPreview
                                  ? smallNoPreviewIcon
                                  : smallPlaceholderIcon);
                item->setData(ROLE_PREVIEW_PLACEHOLDER, true);
            }
            applyModelHighlightColor(item);
            applyModelUserNoteData(item);

            const QString baseModel = item->data(ROLE_FILTER_BASE).toString();
            if (!baseModel.isEmpty() && !foundBaseModels.contains(baseModel)) {
                foundBaseModels.insert(baseModel);
                ui->comboBaseModel->addItem(baseModel);
            }

            const QString modelType = ModelFilter::normalizeModelType(item->data(ROLE_MODEL_TYPE).toString());
            if (!modelType.isEmpty()) foundModelTypes.insert(modelType);

            ui->modelList->addItem(item);
            modelItemByPath.insert(e.fullPath, item);
            addedCount++;
            if (!e.previewPath.isEmpty() && !sidebarIconCache.contains(sideCacheKey)) {
                const QString taskId = "SIDEBAR:" + e.fullPath;
                IconLoaderTask *task = new IconLoaderTask(e.previewPath, 64, 8, this, taskId);
                task->setAutoDelete(true);
                backgroundThreadPool->start(task);
            }
        }

        ui->statusbar->showMessage(AppText::tr("扫描完成，共 %1 个模型").arg(addedCount));
        if (isFirstFilterSortRestore && optRestoreFilterSortState && !startupFilterBaseModel.isEmpty()) {
            const int idx = ui->comboBaseModel->findText(startupFilterBaseModel, Qt::MatchFixedString);
            if (idx >= 0) ui->comboBaseModel->setCurrentIndex(idx);
        }
        ui->comboBaseModel->blockSignals(false);

        ui->comboModelType->blockSignals(true);
        QStringList typeList = foundModelTypes.values();
        std::sort(typeList.begin(), typeList.end());
        ui->comboModelType->addItems(typeList);
        if (isFirstFilterSortRestore && optRestoreFilterSortState && !startupFilterModelType.isEmpty()) {
            const int idx = ui->comboModelType->findText(startupFilterModelType, Qt::MatchFixedString);
            if (idx >= 0) ui->comboModelType->setCurrentIndex(idx);
        }
        ui->comboModelType->blockSignals(false);
        isFirstFilterSortRestore = false;

        updateSortFilterButtonText(); // 下拉框在静默状态下重建了，手动刷新按钮文案

        ui->modelList->setUpdatesEnabled(true);

        modelScanRunning = false;
        // executeSort already rebuilds both home gallery and collection tree.
        executeSort();
        refreshModelUsageStatsAsync();

        qDebug() << "Model list ready:" << addedCount << "models," << loadTimer.elapsed() << "ms";
        if (onComplete) onComplete();
    });
    watcher->setFuture(future);
}

// 更新界面显示
void MainWindow::updateDetailView(const ModelMeta &meta)
{
    // 1. 基础信息
    ui->lblModelName->setWordWrap(true);
    ui->lblModelName->setText(forceWrap(meta.name));
    ui->heroFrame->setProperty("fullImagePath", meta.previewPath);
    ui->btnCheckModelUpdate->setVisible(true);

    if (!meta.modelUrl.isEmpty()) {
        ui->btnOpenUrl->setVisible(true);
        ui->btnOpenUrl->setProperty("url", meta.modelUrl);
        ui->btnOpenUrl->setToolTip(meta.modelUrl.contains("civarchive.com", Qt::CaseInsensitive)
                                   ? AppText::tr("访问 CivArchive")
                                   : AppText::tr("访问 Civitai"));
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
    if (meta.isLocalOnly) addBadge("LOCAL");
    if (meta.isLocalEdited) addBadge("EDITED", true);

    ((QHBoxLayout*)ui->badgesFrame->layout())->addStretch(); // 左对齐

    // 3. 动态生成触发词框 (Trigger Words)
    refreshTriggerWordsPanel(meta);

    // 4. 图库 (Gallery)
    clearLayout(ui->layoutGallery);

    // 中止正在进行的画廊预览图下载任务，防止后台积累僵尸队列
    if (previewDownloadQueue) previewDownloadQueue->cancelActiveAndQueued();
    beginGalleryBuild(meta);

    // 5. 右侧信息
    ui->textDescription->setHtml(meta.description);
    QFileInfo fi(meta.filePath);
    QDateTime addedTime = fi.birthTime();
    if(!addedTime.isValid()) addedTime = fi.lastModified();
    QString addedStr = addedTime.toString("yyyy-MM-dd");
    ui->lblFileInfo->setWordWrap(true);
    const QString displayFileName = meta.fileNameServer.isEmpty() ? meta.fileName : meta.fileNameServer;
    ui->lblFileInfo->setText(AppText::tr("Filename: %1\nSize: %2 MB\nSHA256: %3\nAdded: %4")
                                 .arg(forceWrap(displayFileName))
                                 .arg(meta.fileSizeMB, 0, 'f', 1)
                                 .arg(meta.sha256.left(10) + "...")
                                 .arg(addedStr));
    refreshModelAttributionPanel(meta);
    refreshModelUserNotePanel(meta.filePath);

    updateLocalEditorFromMeta(meta);

    if (!meta.images.isEmpty()) {
        onGalleryImageClicked(0);
    }

    QTimer::singleShot(0, this, [this, meta](){
        fitDetailContentToCurrentPage();
        transitionToImage(meta.previewPath);
    });
}

void MainWindow::fitDetailContentToCurrentPage()
{
    if (!ui || !ui->detailContentStack || !ui->contentAreaWidget) return;

    QWidget *page = ui->detailContentStack->currentWidget();
    if (!page) return;

    int targetHeight = 0;
    const int currentIndex = ui->detailContentStack->currentIndex();
    if (currentIndex == 1) {
        targetHeight = 750;
    } else if (currentIndex == 2) {
        targetHeight = 900;
    } else {
        if (QLayout *pageLayout = page->layout()) {
            pageLayout->activate();
        }
        page->updateGeometry();
        targetHeight = qMax(page->minimumSizeHint().height(), page->sizeHint().height());
    }

    targetHeight = qMax(1, targetHeight);
    ui->detailContentStack->setFixedHeight(targetHeight);
    ui->contentAreaWidget->setFixedHeight(targetHeight);
    ui->detailContentStack->updateGeometry();
    ui->contentAreaWidget->updateGeometry();
    if (ui->scrollAreaWidgetContents) {
        ui->scrollAreaWidgetContents->adjustSize();
    }
}

void MainWindow::cancelGalleryBuild()
{
    ++galleryBuildToken;
    pendingGalleryIndices.clear();
    pendingGalleryMeta = ModelMeta();
    pendingGalleryModelDir.clear();
    pendingGalleryBaseName.clear();
    if (detailGalleryBuildTimer) {
        detailGalleryBuildTimer->stop();
    }
}

void MainWindow::beginGalleryBuild(const ModelMeta &meta)
{
    cancelGalleryBuild();

    if (meta.images.isEmpty()) {
        ui->layoutGallery->addWidget(new QLabel(AppText::tr("No preview images.")));
        return;
    }

    pendingGalleryMeta = meta;
    QFileInfo modelFileInfo(meta.filePath);
    pendingGalleryModelDir = modelFileInfo.absolutePath();

    if (QListWidgetItem *listItem = ui->modelList->currentItem()) {
        pendingGalleryBaseName = listItem->data(ROLE_MODEL_NAME).toString();
    }
    if (pendingGalleryBaseName.isEmpty()) {
        pendingGalleryBaseName = modelFileInfo.completeBaseName();
    }

    for (int i = 0; i < meta.images.count(); ++i) {
        pendingGalleryIndices.append(i);
    }

    const int initialBatch = qMin(8, pendingGalleryIndices.size());
    for (int i = 0; i < initialBatch; ++i) {
        int imageIndex = pendingGalleryIndices.takeFirst();
        addGalleryThumbButton(pendingGalleryMeta, imageIndex, pendingGalleryModelDir, pendingGalleryBaseName);
    }

    if (ui->layoutGallery->count() > 0) {
        QPushButton *firstBtn = qobject_cast<QPushButton*>(ui->layoutGallery->itemAt(0)->widget());
        if (firstBtn) {
            firstBtn->setChecked(true);
        }
    }

    if (!pendingGalleryIndices.isEmpty()) {
        detailGalleryBuildTimer->start(0);
    } else {
        ui->layoutGallery->addStretch();
    }
}

void MainWindow::buildGalleryBatch()
{
    if (pendingGalleryIndices.isEmpty()) {
        if (ui->layoutGallery->count() == 0 || ui->layoutGallery->itemAt(ui->layoutGallery->count() - 1)->spacerItem() == nullptr) {
            ui->layoutGallery->addStretch();
        }
        return;
    }

    const int batchSize = 12;
    for (int i = 0; i < batchSize && !pendingGalleryIndices.isEmpty(); ++i) {
        int imageIndex = pendingGalleryIndices.takeFirst();
        addGalleryThumbButton(pendingGalleryMeta, imageIndex, pendingGalleryModelDir, pendingGalleryBaseName);
    }

    if (!pendingGalleryIndices.isEmpty()) {
        detailGalleryBuildTimer->start(0);
    } else {
        ui->layoutGallery->addStretch();
    }
}

void MainWindow::addGalleryThumbButton(const ModelMeta &meta, int index, const QString &modelDir, const QString &baseName)
{
    if (index < 0 || index >= meta.images.count()) return;

    const ImageInfo &img = meta.images[index];
    bool isNsfw = (img.nsfwLevel > optNSFWLevel);
    if (optFilterNSFW && isNsfw && optNSFWMode == 0) {
        return;
    }

    QPushButton *thumbBtn = new QPushButton();
    thumbBtn->setFixedSize(100, 150);
    thumbBtn->setCheckable(true);
    thumbBtn->setAutoExclusive(true);
    thumbBtn->setCursor(Qt::PointingHandCursor);
    thumbBtn->setProperty("class", "galleryThumb");
    thumbBtn->setProperty("isNSFW", isNsfw);
    thumbBtn->setContextMenuPolicy(Qt::CustomContextMenu);

    QString suffix = (index == 0) ? ".preview.png" : QString(".preview.%1.png").arg(index);
    QString rawPath = QDir(modelDir).filePath(baseName + suffix);
    QString strictLocalPath = QFileInfo(rawPath).absoluteFilePath();

    QString effectivePath = strictLocalPath;
    if (index == 0 && !QFile::exists(effectivePath) && !meta.previewPath.isEmpty() && QFile::exists(meta.previewPath)) {
        effectivePath = QFileInfo(meta.previewPath).absoluteFilePath();
    }

    thumbBtn->setProperty("fullImagePath", effectivePath);
    thumbBtn->setProperty("downloadUrl", img.url);
    thumbBtn->setProperty("savePath", strictLocalPath);
    thumbBtn->setProperty("localBaseName", baseName);
    thumbBtn->setProperty("imageIndex", index);
    thumbBtn->installEventFilter(this);

    const PreviewMetadataPayload previewPayload = PreviewImageStore::previewPayloadFromImageInfo(img);
    if (QFile::exists(effectivePath) && !(m_forceResyncPreview && index > 0 && !img.url.isEmpty())) {
        thumbBtn->setText(AppText::tr("Loading..."));
        IconLoaderTask *task = new IconLoaderTask(effectivePath, 100, 0, this, effectivePath, true);
        task->setAutoDelete(true);
        threadPool->start(task);
    } else {
        if (m_skipPreviewSync) {
            thumbBtn->setText(AppText::tr("Skipped"));
        } else if (img.url.isEmpty()) {
            thumbBtn->setText(AppText::tr("Missing"));
        } else {
            if (QFile::exists(strictLocalPath) && index > 0 && m_forceResyncPreview) {
                QFile::remove(strictLocalPath);
            }
            thumbBtn->setText(index == 0 ? AppText::tr("Downloading...") : AppText::tr("Queueing..."));
            enqueueDownload(img.url, strictLocalPath, thumbBtn, baseName, index, previewPayload);
        }
    }

    connect(thumbBtn, &QPushButton::clicked, this, [this, index](){
        onGalleryImageClicked(index);
    });
    connect(thumbBtn, &QPushButton::customContextMenuRequested, this, [this, thumbBtn](const QPoint &pos){
        if (!thumbBtn) return;

        const QString downloadUrl = thumbBtn->property("downloadUrl").toString();
        const QString savePath = QFileInfo(thumbBtn->property("savePath").toString()).absoluteFilePath();
        const QString localBaseName = thumbBtn->property("localBaseName").toString();
        const int imageIndex = thumbBtn->property("imageIndex").toInt();

        QMenu menu(this);
        QAction *actRedownload = menu.addAction(AppText::tr("重新下载 / Re-download"));
        actRedownload->setEnabled(!downloadUrl.isEmpty() && !savePath.isEmpty());

        QAction *actOpenLocation = menu.addAction(AppText::tr("打开所在位置 / Show in Folder"));
        actOpenLocation->setEnabled(!savePath.isEmpty());

        QAction *chosen = menu.exec(thumbBtn->mapToGlobal(pos));
        if (!chosen) return;

        if (chosen == actRedownload) {
            if (QFile::exists(savePath)) {
                QFile::remove(savePath);
            }
            thumbBtn->setIcon(QIcon());
            thumbBtn->setText(imageIndex == 0 ? AppText::tr("Downloading...") : AppText::tr("Queueing..."));
            PreviewMetadataPayload payload;
            if (imageIndex >= 0 && imageIndex < currentMeta.images.size()) {
                payload = PreviewImageStore::previewPayloadFromImageInfo(currentMeta.images.at(imageIndex));
            }
            enqueueDownload(downloadUrl, savePath, thumbBtn, localBaseName, imageIndex, payload);
            return;
        }

        if (chosen == actOpenLocation) {
            showFileInFolder(savePath);
        }
    });

    ui->layoutGallery->addWidget(thumbBtn);
}

void MainWindow::onGalleryImageClicked(int index)
{
    if (index < 0 || index >= currentMeta.images.count()) return;

    const ImageInfo &img = currentMeta.images[index];

    // 1. 更新 Prompt 显示
    ui->textImgPrompt->setPlainText(img.prompt.isEmpty() ? "No positive prompt." : img.prompt);
    ui->textImgNegPrompt->setPlainText(img.negativePrompt.isEmpty() ? "No negative prompt." : img.negativePrompt);

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
    QString localPath = ModelMetadata::previewPath(modelDir, currentBaseName, index);

    int width = img.width;
    int height = img.height;
    if ((width <= 0 || height <= 0) && QFile::exists(localPath)) {
        QImageReader reader(localPath);
        const QSize size = reader.size();
        width = size.width();
        height = size.height();
    }

    const QString resolution = (width > 0 && height > 0)
        ? QString("%1 x %2").arg(width).arg(height)
        : "--";
    auto richValue = [](const QString &value) {
        return value.trimmed().isEmpty() ? QStringLiteral("--") : value.toHtmlEscaped();
    };
    const QString sampler = img.sampler.trimmed().isEmpty()
        ? QStringLiteral("--")
        : forceWrap(img.sampler.trimmed()).toHtmlEscaped();
    // 数值用 primaryText（Steam 仍为白、Light 为深色），避免浅色主题下白字看不清。
    const QString vc = AppStyle::str("primaryText");
    QString params = QString("Resolution: <span style='color:%1'>%2</span> | Steps: <span style='color:%1'>%3</span> | CFG: <span style='color:%1'>%4</span><br/>Sampler: <span style='color:%1'>%5</span> | Seed: <span style='color:%1'>%6</span>")
                         .arg(vc)
                         .arg(resolution.toHtmlEscaped())
                         .arg(richValue(img.steps))
                         .arg(richValue(img.cfgScale))
                         .arg(sampler)
                         .arg(richValue(img.seed));
    ui->lblImgParams->setText(params);

    // 3. 执行过渡
    if (QFile::exists(localPath)) {
        transitionToImage(localPath);
    } else {
        qDebug() << "[Debug] Preview image not found at:" << localPath;
    }
}

void MainWindow::refreshTriggerWordsPanel(const ModelMeta &meta)
{
    if (!ui || !ui->layoutTriggerStack) return;

    clearLayout(ui->layoutTriggerStack);

    const QString modelNoteKey = meta.filePath.isEmpty() ? QString() : QFileInfo(meta.filePath).absoluteFilePath();
    const QStringList customTriggerGroups = normalizeModelCustomTriggers(modelUserNotes.value(modelNoteKey).customTriggers);

    auto addTriggerGroup = [this](const QString &words, const QString &sourceLabel, const QString &accentColor) {
        QWidget *rowWidget = new QWidget();
        QVBoxLayout *outerLayout = new QVBoxLayout(rowWidget);
        outerLayout->setContentsMargins(0, 0, 0, 10);
        outerLayout->setSpacing(4);

        QLabel *source = new QLabel(sourceLabel);
        source->setStyleSheet(QString("color:%1;font-weight:bold;background:transparent;margin-left:2px;").arg(accentColor));
        outerLayout->addWidget(source);

        QHBoxLayout *rowLayout = new QHBoxLayout();
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(5);

        QTextBrowser *tb = new QTextBrowser();
        tb->setText(words);
        tb->setFixedHeight(90);
        tb->setStyleSheet(QString("QTextBrowser { border: 1px solid %1; border-radius: 8px; }").arg(accentColor));

        QPushButton *btnCopy = new QPushButton(AppText::tr("Copy"));
        btnCopy->setFixedSize(60, 90);
        btnCopy->setCursor(Qt::PointingHandCursor);
        btnCopy->setProperty("class", "copyBtn");

        connect(btnCopy, &QPushButton::clicked, this, [words, this]() {
            QClipboard *clip = QGuiApplication::clipboard();
            clip->setText(words);
            ui->statusbar->showMessage(AppText::tr("Copied trigger words!"), 1500);
        });

        rowLayout->addWidget(tb);
        rowLayout->addWidget(btnCopy);
        outerLayout->addLayout(rowLayout);
        ui->layoutTriggerStack->addWidget(rowWidget);
    };

    if (meta.trainedWordsGroups.isEmpty() && customTriggerGroups.isEmpty()) {
        QLabel *lbl = new QLabel(AppText::tr("No trigger words provided."));
        lbl->setStyleSheet("color: #666; font-style: italic; margin-left: 10px;");
        ui->layoutTriggerStack->addWidget(lbl);
        return;
    }

    for (const QString &words : meta.trainedWordsGroups) {
        addTriggerGroup(words, "Civitai / Metadata", AppStyle::AccentBlue());
    }
    for (const QString &words : customTriggerGroups) {
        addTriggerGroup(words, AppText::tr("Custom / 用户自定义"), AppStyle::CustomTriggerGreen());
    }

    ui->layoutTriggerStack->addStretch(1);
}

// 辅助函数
void MainWindow::addBadge(QString text, bool isRed)
{
    QLabel *lbl = new QLabel(text);
    if (text == "LOCAL") {
        lbl->setProperty("class", "tagGreen");
    } else {
        lbl->setProperty("class", isRed ? "tagRed" : "tag");
    }
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
    cancelGalleryBuild();
    ui->lblModelName->setText(AppText::tr("请选择一个模型 / Select a Model"));
    setModelTitleNormal();
    ui->lblModelName->setStyleSheet(AppStyle::modelTitleNormalStyle());
    ui->textDescription->clear();
    ui->textDescription->setPlaceholderText(AppText::tr("暂无简介 / No description."));
    ui->lblFileInfo->setText(AppText::tr("Filename: --\nSize: --\nHash: --"));
    refreshModelAttributionPanel(ModelMeta());
    refreshModelUserNotePanel("");

    ui->textImgPrompt->clear();
    ui->textImgNegPrompt->clear();
    ui->lblImgParams->setText(AppText::tr("Resolution: -- | Steps: -- | CFG: --<br/>Sampler: -- | Seed: --"));

    ui->btnOpenUrl->setVisible(false);
    ui->btnCopyLoraTag->setVisible(false);
    if (ui->btnOpenModelGraph) ui->btnOpenModelGraph->setVisible(false);

    clearLayout(ui->badgesFrame->layout());

    clearLayout(ui->layoutTriggerStack);
    clearLayout(ui->layoutGallery);

    // 中止正在进行的画廊预览图下载任务
    if (previewDownloadQueue) previewDownloadQueue->cancelActiveAndQueued();

    if (localMetaEditorPage) localMetaEditorPage->clear();
    QTimer::singleShot(0, this, &MainWindow::fitDetailContentToCurrentPage);
}

void MainWindow::setModelTitleNormal()
{
    ui->lblModelName->setStyleSheet(AppStyle::modelTitleNormalStyle());
}

void MainWindow::setModelTitleError(const QString &message)
{
    ui->lblModelName->setText(AppText::tr("连接失败 / Error: %1").arg(message));
    ui->lblModelName->setStyleSheet(AppStyle::modelTitleErrorStyle());
}

void MainWindow::showPendingLocalModelDetail(const ModelMeta &meta, const QString &message)
{
    clearDetailView();
    currentMeta = meta;
    // 同步失败/待同步模型：保留复制 LoRA 标签，隐藏联网相关按钮
    ui->btnCopyLoraTag->setVisible(true);
    ui->btnOpenUrl->setVisible(false);
    ui->btnCheckModelUpdate->setVisible(false);
    setModelTitleNormal();
    ui->lblModelName->setWordWrap(true);
    ui->lblModelName->setText(forceWrap(meta.name.isEmpty() ? meta.fileName : meta.name));
    ui->textDescription->setPlainText(message);
    QFileInfo fi(meta.filePath);
    ui->lblFileInfo->setWordWrap(true);
    ui->lblFileInfo->setText(AppText::tr("Filename: %1\nSize: %2 MB\nHash: --")
                                 .arg(forceWrap(fi.fileName()))
                                 .arg(fi.exists() ? QString::number(fi.size() / 1024.0 / 1024.0, 'f', 2) : "--"));
    refreshModelAttributionPanel(meta);
    refreshModelUserNotePanel(meta.filePath);
    if (!meta.previewPath.isEmpty() && QFile::exists(meta.previewPath)) {
        transitionToImage(meta.previewPath);
    } else {
        transitionToImage("");
    }
    updateLocalEditorFromMeta(meta);
    QTimer::singleShot(0, this, &MainWindow::fitDetailContentToCurrentPage);
}

LocalMetaEditorContext MainWindow::buildLocalMetaEditorContext(const ModelMeta &meta) const
{
    LocalMetaEditorContext ctx;
    QListWidgetItem *item = (ui && ui->modelList) ? ui->modelList->currentItem() : nullptr;

    if (item) {
        const QString itemPath = item->data(ROLE_FILE_PATH).toString();
        if (!itemPath.isEmpty()) ctx.filePath = itemPath;
        ctx.baseName = item->data(ROLE_MODEL_NAME).toString();
        if (ctx.baseName.isEmpty()) ctx.baseName = item->text();
        ctx.usageCount = item->data(ROLE_SORT_USAGE_COUNT).toInt();
        ctx.lastUsedMs = item->data(ROLE_SORT_LAST_USED).toLongLong();
    }
    if (ctx.filePath.isEmpty() && !meta.filePath.isEmpty()) {
        ctx.filePath = meta.filePath;
    }
    if (ctx.baseName.isEmpty() && !ctx.filePath.isEmpty()) {
        ctx.baseName = QFileInfo(ctx.filePath).completeBaseName();
    }
    if (!ctx.filePath.isEmpty()) {
        ctx.modelDir = QFileInfo(ctx.filePath).absolutePath();
        const QString absPath = QFileInfo(ctx.filePath).absoluteFilePath();
        if (modelSyncFailures.contains(absPath)) {
            ctx.hasSyncFailure = true;
            ctx.syncFailureError = modelSyncFailures.value(absPath)["error"].toString();
        }
    } else {
        ctx.modelDir = currentLoraPath;
    }
    return ctx;
}

void MainWindow::updateLocalEditorFromMeta(const ModelMeta &meta)
{
    if (!localMetaEditorPage) return;
    const bool isVisibleNow = ui && ui->detailContentStack
        && ui->detailContentStack->currentWidget() == localMetaEditorPage;
    localMetaEditorPage->loadModel(meta, buildLocalMetaEditorContext(meta), isVisibleNow);
}

void MainWindow::setLocalMetaStatus(const ModelMeta &meta)
{
    if (!localMetaEditorPage) return;
    localMetaEditorPage->updateStatus(meta, buildLocalMetaEditorContext(meta));
}

void MainWindow::refreshCurrentDetailCacheStatus()
{
    if (!currentMeta.filePath.isEmpty()) setLocalMetaStatus(currentMeta);
}

void MainWindow::refreshUsageAnalysisWidget()
{
    if (!usageAnalysisWidget || !ui || !ui->modelList) return;

    const int token = ++usageAnalysisRefreshToken;

    QVector<UsageAnalysisModel> modelSnapshots;
    modelSnapshots.reserve(ui->modelList->count());
    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!isModelListItem(item)) continue;

        UsageAnalysisModel model;
        const QString rawPath = item->data(ROLE_FILE_PATH).toString();
        model.filePath = rawPath.isEmpty() ? QString() : QFileInfo(rawPath).absoluteFilePath();
        model.displayName = item->text();
        model.baseName = item->data(ROLE_MODEL_NAME).toString();
        if (model.baseName.isEmpty() && !model.filePath.isEmpty()) {
            model.baseName = QFileInfo(model.filePath).completeBaseName();
        }
        model.rootName = item->data(ROLE_MODEL_ROOT_NAME).toString();
        model.baseModel = item->data(ROLE_FILTER_BASE).toString();
        model.modelType = item->data(ROLE_MODEL_TYPE).toString();
        model.previewPath = item->data(ROLE_PREVIEW_PATH).toString();
        model.modelId = item->data(ROLE_CIVITAI_MODEL_ID).toInt();
        model.versionId = item->data(ROLE_CIVITAI_VERSION_ID).toInt();
        model.sha256 = item->data(ROLE_CIVITAI_SHA256).toString().trimmed();
        model.hasSha256 = !model.sha256.isEmpty();
        model.localEdited = item->data(ROLE_LOCAL_EDITED).toBool();
        model.usageCount = item->data(ROLE_SORT_USAGE_COUNT).toInt();
        model.lastUsed = item->data(ROLE_SORT_LAST_USED).toLongLong();
        if (!model.filePath.isEmpty()) {
            const QJsonObject failure = modelSyncFailures.value(model.filePath);
            model.syncFailure = failure["error"].toString();
        }
        modelSnapshots.append(model);
    }

    QSet<QString> currentGalleryPaths = userGalleryPage ? userGalleryPage->allImagePaths() : QSet<QString>{};

    const QMap<QString, UserImageInfo> cacheSnapshot = imageCache;
    const QStringList rootPaths = PathUtils::collectEnabledPaths(loraPaths, disabledLoraPaths);
    const bool recursive = optLoraRecursive;
    const QDateTime generatedAt = QDateTime::currentDateTime();

    auto *watcher = new QFutureWatcher<UsageAnalysisData>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, token]() {
        const UsageAnalysisData data = watcher->result();
        watcher->deleteLater();
        if (isShuttingDown || token != usageAnalysisRefreshToken || !usageAnalysisWidget) return;

        usageAnalysisWidget->setTranslationMap(&translationMap);
        usageAnalysisWidget->setAnalysisData(data);
    });

    watcher->setFuture(QtConcurrent::run(
        backgroundThreadPool,
        [models = std::move(modelSnapshots),
         cacheSnapshot,
         currentGalleryPaths = std::move(currentGalleryPaths),
         rootPaths,
         recursive,
         generatedAt]() mutable {
            for (UsageAnalysisModel &model : models) {
                if (!model.filePath.isEmpty()) {
                    const QFileInfo fi(model.filePath);
                    model.jsonPath = fi.absoluteDir().filePath(fi.completeBaseName() + QStringLiteral(".json"));
                    if (fi.exists()) model.modelFileBytes = fi.size();
                    const QFileInfo jsonFi(model.jsonPath);
                    if (jsonFi.exists()) model.sidecarBytes += jsonFi.size();
                    if (!model.previewPath.isEmpty()) {
                        const QFileInfo previewFi(model.previewPath);
                        if (previewFi.exists()) model.sidecarBytes += previewFi.size();
                    }
                }
            }

            TagUsage::Counts tagCounts;
            for (auto it = cacheSnapshot.cbegin(); it != cacheSnapshot.cend(); ++it) {
                tagCounts.addImage(it.value().cleanTags);
            }

            UsageAnalysisData data;
            data.models = std::move(models);
            data.galleryImageCount = cacheSnapshot.size();
            data.generatedAt = generatedAt;
            for (auto it = tagCounts.values.cbegin(); it != tagCounts.values.cend(); ++it) {
                data.positiveTagCounts.insert(tagCounts.displayTags.value(it.key()), it.value());
            }
            data.rawImageCache = std::move(cacheSnapshot);
            data.currentGalleryImageCount = currentGalleryPaths.size();
            data.currentGalleryPaths = std::move(currentGalleryPaths);
            data.loraRootPaths = rootPaths;
            data.loraRecursive = recursive;
            return data;
        }));
}

void MainWindow::refreshPromptTemplateModelTriggerRows()
{
    if (!promptTemplateLibraryWidget || !ui || !ui->modelList) return;

    QVector<PromptTemplateLibraryWidget::ModelTriggerRow> rows;
    QSet<QString> seen;
    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!isModelListItem(item)) continue;

        const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
        if (filePath.isEmpty()) continue;
        const QString baseName = item->data(ROLE_MODEL_NAME).toString();
        const QString modelName = item->text().trimmed().isEmpty() ? baseName : item->text().trimmed();
        const QString modelType = item->data(ROLE_MODEL_TYPE).toString();
        const QString previewPath = item->data(ROLE_PREVIEW_PATH).toString();
        // LoRA 模型的 <lora:name:1> 名称（取文件名去扩展名）。
        const QString loraName = modelType.contains("lora", Qt::CaseInsensitive)
            ? QFileInfo(filePath).completeBaseName()
            : QString();

        auto appendTrigger = [&](const QString &trigger, const QString &source) {
            const QString clean = trigger.trimmed();
            if (clean.isEmpty()) return;
            const QString key = (filePath + "|" + source + "|" + clean).toCaseFolded();
            if (seen.contains(key)) return;
            seen.insert(key);
            PromptTemplateLibraryWidget::ModelTriggerRow row;
            row.modelKey = filePath;
            row.modelName = modelName;
            row.previewPath = previewPath;
            row.previewIcon = item->icon();
            row.trigger = clean;
            row.source = source;
            row.modelType = modelType;
            row.loraName = loraName;
            rows.append(row);
        };

        for (const QString &trigger : item->data(ROLE_MODEL_TRAINED_WORDS).toStringList()) {
            appendTrigger(trigger, "Metadata");
        }

        for (const QString &trigger : item->data(ROLE_USER_CUSTOM_TRIGGERS).toStringList()) {
            appendTrigger(trigger, "Custom");
        }
    }

    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        const int modelCmp = QString::compare(a.modelName, b.modelName, Qt::CaseInsensitive);
        if (modelCmp != 0) return modelCmp < 0;
        const int triggerCmp = QString::compare(a.trigger, b.trigger, Qt::CaseInsensitive);
        if (triggerCmp != 0) return triggerCmp < 0;
        return a.source < b.source;
    });
    promptTemplateLibraryWidget->setModelTriggerRows(rows);
}

int MainWindow::countLocalEditedModels() const
{
    int count = 0;
    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!isModelListItem(item)) continue;
        if (item->data(ROLE_LOCAL_EDITED).toBool()) count++;
    }
    return count;
}

bool MainWindow::confirmLocalEditOverwrite(QListWidgetItem *item)
{
    if (!item) return true;
    if (!item->data(ROLE_LOCAL_EDITED).toBool()) return true;

    QMessageBox::StandardButton reply = QMessageBox::warning(
        this,
        AppText::tr("本地元数据提示"),
        AppText::tr("当前模型存在本地/已编辑元数据，继续同步将覆盖本地修改。\n是否继续？"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );
    return reply == QMessageBox::Yes;
}

QString MainWindow::modelSyncFailurePath() const
{
    return qApp->applicationDirPath() + "/config/model_sync_failures.json";
}

void MainWindow::loadModelSyncFailures()
{
    modelSyncFailures = ModelUserDataStore::loadSyncFailures(modelSyncFailurePath());
}

QString MainWindow::modelUserNotesPath() const
{
    return qApp->applicationDirPath() + "/config/model_user_notes.json";
}

QStringList MainWindow::normalizeModelUserTags(const QStringList &tags) const
{
    return ModelUserDataStore::normalizeDistinctTrimmedList(tags);
}

QStringList MainWindow::normalizeModelUserTagsText(const QString &text) const
{
    return ModelUserDataStore::normalizeUserTagsText(text);
}

QStringList MainWindow::normalizeModelCustomTriggers(const QStringList &triggers) const
{
    return ModelUserDataStore::normalizeDistinctTrimmedList(triggers);
}

void MainWindow::loadModelUserNotes()
{
    modelUserNotes = ModelUserDataStore::loadUserNotes(modelUserNotesPath());
}

void MainWindow::saveModelUserNotes() const
{
    ModelUserDataStore::saveUserNotes(modelUserNotesPath(), modelUserNotes);
}

QString MainWindow::formatModelRating(double rating) const
{
    return ModelUserDataStore::formatRating(rating, AppText::tr("未评分"));
}

QString MainWindow::formatModelUserNoteTooltip(const QString &filePath, const QString &baseTooltip) const
{
    const QString key = QFileInfo(filePath).absoluteFilePath();
    return ModelUserDataStore::formatUserNoteTooltip(modelUserNotes.value(key), baseTooltip, AppText::tr("未评分"));
}

void MainWindow::applyModelUserNoteData(QListWidgetItem *item)
{
    if (!item || !isModelListItem(item)) return;
    const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
    const ModelUserNote note = modelUserNotes.value(filePath);
    item->setData(ROLE_USER_RATING, note.rating);
    item->setData(ROLE_USER_NOTE, note.note);
    item->setData(ROLE_USER_TAGS, note.tags);
    item->setData(ROLE_USER_CUSTOM_TRIGGERS, note.customTriggers);
    item->setToolTip(formatModelUserNoteTooltip(filePath, filePath));
}

void MainWindow::applyModelUserNoteData(QTreeWidgetItem *item)
{
    if (!item || item->data(0, ROLE_FILE_PATH).toString().isEmpty()) return;
    const QString filePath = QFileInfo(item->data(0, ROLE_FILE_PATH).toString()).absoluteFilePath();
    const ModelUserNote note = modelUserNotes.value(filePath);
    item->setData(0, ROLE_USER_RATING, note.rating);
    item->setData(0, ROLE_USER_NOTE, note.note);
    item->setData(0, ROLE_USER_TAGS, note.tags);
    item->setData(0, ROLE_USER_CUSTOM_TRIGGERS, note.customTriggers);
    item->setToolTip(0, formatModelUserNoteTooltip(filePath, item->text(0)));
}

void MainWindow::refreshModelUserNoteItems(const QString &filePath)
{
    const QString key = QFileInfo(filePath).absoluteFilePath();
    if (QListWidgetItem *sideItem = findModelItemByFilePath(key)) {
        applyModelUserNoteData(sideItem);
    }
    if (QListWidgetItem *homeItem = homeGalleryPage ? homeGalleryPage->itemForPath(key) : nullptr) {
        applyModelUserNoteData(homeItem);
    }
}

void MainWindow::refreshModelUserNotePanel(const QString &filePath)
{
    const QString sourcePath = filePath.isNull() ? currentMeta.filePath : filePath;
    const QString key = sourcePath.isEmpty() ? QString() : QFileInfo(sourcePath).absoluteFilePath();
    const ModelUserNote note = modelUserNotes.value(key);

    ui->lblUserRating->setText(AppText::tr("评分: ") + formatModelRating(note.rating));
    ui->textUserNotePreview->setPlainText(note.note);
    ui->textUserNotePreview->setPlaceholderText(AppText::tr("暂无备注 / No note."));

    clearLayout(ui->layoutUserTags);
    if (note.tags.isEmpty()) {
        ui->layoutUserTags->addWidget(createMutedEmptyLabel(AppText::tr("无用户标签")));
    } else {
        for (const QString &tag : note.tags) {
            QPushButton *button = setupFilterChipButton(
                new QPushButton(forceWrap(tag)),
                AppText::tr("点击在主页添加该用户 Tag 筛选"),
                QStringLiteral("user"),
                150);
            connect(button, &QPushButton::clicked, this, [this, tag]() {
                ui->rootStack->setCurrentWidget(ui->pageLibrary);
                ui->mainStack->setCurrentIndex(0);
                addHomeTagFilter(tag);
                ui->statusbar->showMessage(AppText::tr("已添加用户 Tag 筛选: ") + tag, 2000);
            });
            ui->layoutUserTags->addWidget(button);
        }
    }
    ui->layoutUserTags->addStretch();
}

void MainWindow::refreshModelAttributionPanel(const ModelMeta &meta)
{
    if (!ui || !ui->layoutModelAuthor || !ui->layoutModelTags) return;

    clearLayout(ui->layoutModelAuthor);
    clearLayout(ui->layoutModelTags);

    if (meta.creatorName.trimmed().isEmpty()) {
        ui->layoutModelAuthor->addWidget(createMutedEmptyLabel(AppText::tr("暂无作者信息")));
    } else {
        QPushButton *author = setupFilterChipButton(
            new QPushButton(forceWrap(meta.creatorName)),
            AppText::tr("点击在主页筛选该作者"),
            QString(),
            260);
        connect(author, &QPushButton::clicked, this, [this, name = meta.creatorName]() {
            ui->rootStack->setCurrentWidget(ui->pageLibrary);
            ui->mainStack->setCurrentIndex(0);
            setHomeAuthorFilter(name);
            ui->statusbar->showMessage(AppText::tr("已按作者筛选: ") + name, 2000);
        });
        ui->layoutModelAuthor->addWidget(author);
    }
    ui->layoutModelAuthor->addStretch();

    if (meta.modelTags.isEmpty()) {
        ui->layoutModelTags->addWidget(createMutedEmptyLabel(AppText::tr("暂无模型标签")));
    } else {
        QWidget *tagFlowWidget = new QWidget();
        tagFlowWidget->setStyleSheet("background:transparent;");
        tagFlowWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        FlowLayout *tagFlowLayout = new FlowLayout(tagFlowWidget, 0, 6, 6);
        for (const QString &tag : meta.modelTags) {
            QPushButton *tagButton = setupFilterChipButton(
                new QPushButton(forceWrap(tag)),
                AppText::tr("点击在主页添加该 Tag 筛选"),
                QString(),
                150);
            connect(tagButton, &QPushButton::clicked, this, [this, tag]() {
                ui->rootStack->setCurrentWidget(ui->pageLibrary);
                ui->mainStack->setCurrentIndex(0);
                addHomeTagFilter(tag);
                ui->statusbar->showMessage(AppText::tr("已添加 Tag 筛选: ") + tag, 2000);
            });
            tagFlowLayout->addWidget(tagButton);
        }
        ui->layoutModelTags->addWidget(tagFlowWidget);
    }
}

void MainWindow::openModelNoteDialog(QListWidgetItem *item)
{
    if (!isModelListItem(item)) return;
    const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
    if (filePath.isEmpty()) return;

    ModelUserNote current = modelUserNotes.value(filePath);
    ModelNoteDialog dialog(this);
    dialog.setModelName(item->text());
    dialog.setRating(current.rating);
    dialog.setNote(current.note);
    dialog.setTags(current.tags);
    dialog.setCustomTriggers(current.customTriggers);
    if (dialog.exec() != QDialog::Accepted) return;

    current.rating = dialog.rating();
    if (current.rating < 0.5) current.rating = 0.0;
    current.rating = std::round(current.rating * 2.0) / 2.0;
    current.note = dialog.note();
    current.tags = normalizeModelUserTags(dialog.tags());
    current.customTriggers = normalizeModelCustomTriggers(dialog.customTriggers());
    current.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    if (current.rating <= 0.0 && current.note.isEmpty() && current.tags.isEmpty() && current.customTriggers.isEmpty()) {
        modelUserNotes.remove(filePath);
    } else {
        modelUserNotes.insert(filePath, current);
    }

    refreshModelUserNoteItems(filePath);
    saveModelUserNotes();
    refreshModelUserNotePanel(filePath);
    if (QFileInfo(currentMeta.filePath).absoluteFilePath() == filePath) {
        refreshTriggerWordsPanel(currentMeta);
    }
    refreshPromptTemplateModelTriggerRows();
    executeSort();
    refreshCollectionTreeView();
    refreshHomeGallery();
    if (QListWidgetItem *restoredItem = findModelItemByFilePath(filePath)) {
        ui->modelList->setCurrentItem(restoredItem);
        restoredItem->setSelected(true);
        syncTreeSelection(filePath);
    }
    ui->statusbar->showMessage(AppText::tr("模型评分、备注与自定义触发词已保存。"), 2000);
}

void MainWindow::setUserRatingForItems(const QList<QListWidgetItem*> &items, double rating)
{
    const double normalizedRating = rating < 0.5 ? 0.0 : qBound(0.0, std::round(rating * 2.0) / 2.0, 5.0);
    for (QListWidgetItem *item : items) {
        if (!isModelListItem(item)) continue;
        const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
        if (filePath.isEmpty()) continue;
        ModelUserNote note = modelUserNotes.value(filePath);
        note.rating = normalizedRating;
        note.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        if (note.rating <= 0.0 && note.note.isEmpty() && note.tags.isEmpty() && note.customTriggers.isEmpty()) {
            modelUserNotes.remove(filePath);
        } else {
            modelUserNotes.insert(filePath, note);
        }
        refreshModelUserNoteItems(filePath);
    }
    saveModelUserNotes();
    refreshModelUserNotePanel();
    executeSort();
    refreshCollectionTreeView();
    refreshHomeGallery();
}

void MainWindow::addUserTagsForItems(const QList<QListWidgetItem*> &items, const QStringList &tags)
{
    const QStringList cleanTags = normalizeModelUserTags(tags);
    if (cleanTags.isEmpty()) return;
    for (QListWidgetItem *item : items) {
        if (!isModelListItem(item)) continue;
        const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
        if (filePath.isEmpty()) continue;
        ModelUserNote note = modelUserNotes.value(filePath);
        note.tags = normalizeModelUserTags(note.tags + cleanTags);
        note.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        modelUserNotes.insert(filePath, note);
        refreshModelUserNoteItems(filePath);
    }
    saveModelUserNotes();
    refreshModelUserNotePanel();
    refreshCollectionTreeView();
    refreshHomeGallery();
}

void MainWindow::removeUserTagsForItems(const QList<QListWidgetItem*> &items, const QStringList &tags)
{
    const QStringList cleanTags = normalizeModelUserTags(tags);
    if (cleanTags.isEmpty()) return;
    QSet<QString> removeSet;
    for (const QString &tag : cleanTags) removeSet.insert(tag.toCaseFolded());
    for (QListWidgetItem *item : items) {
        if (!isModelListItem(item)) continue;
        const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
        if (filePath.isEmpty()) continue;
        ModelUserNote note = modelUserNotes.value(filePath);
        QStringList nextTags;
        for (const QString &tag : note.tags) {
            if (!removeSet.contains(tag.toCaseFolded())) nextTags.append(tag);
        }
        note.tags = nextTags;
        note.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        if (note.rating <= 0.0 && note.note.isEmpty() && note.tags.isEmpty() && note.customTriggers.isEmpty()) {
            modelUserNotes.remove(filePath);
        } else {
            modelUserNotes.insert(filePath, note);
        }
        refreshModelUserNoteItems(filePath);
    }
    saveModelUserNotes();
    refreshModelUserNotePanel();
    refreshCollectionTreeView();
    refreshHomeGallery();
}

void MainWindow::saveModelSyncFailures() const
{
    ModelUserDataStore::saveSyncFailures(modelSyncFailurePath(), modelSyncFailures);
}

void MainWindow::recordModelSyncFailure(const QString &filePath, const QString &baseName, const QString &error)
{
    const QString key = QFileInfo(filePath).absoluteFilePath();
    if (key.isEmpty()) return;
    QJsonObject obj;
    obj["filePath"] = key;
    obj["baseName"] = baseName;
    obj["error"] = error;
    obj["failedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    modelSyncFailures.insert(key, obj);
    saveModelSyncFailures();
    if (QListWidgetItem *item = findModelItemByFilePath(key)) {
        item->setData(ROLE_SYNC_FAILED, true);
        item->setData(ROLE_SYNC_ERROR, error);
    }
}

void MainWindow::clearModelSyncFailure(const QString &filePath)
{
    const QString key = QFileInfo(filePath).absoluteFilePath();
    if (key.isEmpty()) return;
    if (modelSyncFailures.remove(key)) saveModelSyncFailures();
    if (QListWidgetItem *item = findModelItemByFilePath(key)) {
        item->setData(ROLE_SYNC_FAILED, false);
        item->setData(ROLE_SYNC_ERROR, QString());
    }
}

QString MainWindow::modelSyncFailureMessage(const QString &filePath) const
{
    const QString key = QFileInfo(filePath).absoluteFilePath();
    const QJsonObject obj = modelSyncFailures.value(key);
    return obj.value("error").toString();
}

void MainWindow::onScanLocalClicked() {
    int localCount = countLocalEditedModels();
    if (!optSuppressLocalWarnings && localCount > 0) {
        QMessageBox::information(this, AppText::tr("提示"),
                                 AppText::tr("检测到 %1 个本地/已编辑模型。\n刷新不会删除本地元数据，但后续同步可能覆盖本地修改。").arg(localCount));
    }
    const QStringList activeLoraPaths = PathUtils::collectEnabledPaths(loraPaths, disabledLoraPaths);
    if (!activeLoraPaths.isEmpty()) {
        scanModels(activeLoraPaths);
    } else {
        QMessageBox::information(this, AppText::tr("提示"), AppText::tr("请先设置并启用至少一个 LoRA 路径。"));
    }
    executeSort();
}

// 点击列表项
void MainWindow::onModelListClicked(QListWidgetItem *item) {
    if (item && item->data(ROLE_IS_FOLDER_HEADER).toBool()) {
        toggleModelFolderCollapsed(item->data(ROLE_MODEL_FOLDER_KEY).toString());
        return;
    }
    if (!isModelListItem(item)) return;

    cancelPendingTasks();

    // === 恢复 UI 状态 ===
    ui->btnForceUpdate->setVisible(true);
    ui->btnFavorite->setVisible(true);
    ui->btnShowUserGallery->setVisible(true);
    ui->btnShowUserGallery->setEnabled(true);
    if (ui->btnOpenModelGraph) ui->btnOpenModelGraph->setVisible(true);
    ui->btnEditMeta->setVisible(true);
    ui->btnCopyLoraTag->setVisible(true);

    QString filePath = item->data(ROLE_FILE_PATH).toString();
    QString modelDir = QFileInfo(filePath).absolutePath();
    ui->modelList->setProperty("current_model_dir", modelDir);

    if (currentMeta.filePath == filePath && !currentMeta.name.isEmpty() && !userGalleryGlobalMode) {
        // 如果当前不在详情页（比如在主页），则只切换页面，不重新加载数据
        if (ui->mainStack->currentIndex() != 1) {
            ui->mainStack->setCurrentIndex(1);
        }
        QTimer::singleShot(0, this, &MainWindow::fitDetailContentToCurrentPage);
        // 无论是否切换了页面，都直接返回，不再执行后续繁重的 JSON 解析
        return;
    }

    userGalleryGlobalMode = false;
    if (userGalleryPage) userGalleryPage->invalidateAndReset();
    pendingHashSyncPath.clear();
    pendingHashSyncBaseName.clear();
    pendingHashSyncForceRefresh = false;
    m_forceResyncPreview = false;
    m_skipPreviewSync = false;
    ui->btnForceUpdate->setEnabled(true);

    // 1. 如果正在计算上一个，先取消或忽略
    if (hashWatcher->isRunning()) {
        // 简单处理：提示用户稍等，或者强制让 UI 变动
        // 更好的做法是 cancel，但 SHA 计算很难中途 cancel，所以我们用标志位判断
    }

    if (ui->mainStack->currentIndex() != 1) {
        ui->mainStack->setCurrentIndex(1); // 立即进入详情页
    }

    QString previewPath = item->data(ROLE_PREVIEW_PATH).toString();
    QString baseName = item->data(ROLE_MODEL_NAME).toString();

    ModelMeta meta;
    meta.modelName = baseName;
    meta.versionName = "";
    meta.name = baseName;
    meta.filePath = filePath;
    meta.previewPath = previewPath;
    meta.fileName = QFileInfo(filePath).fileName();

    // 2. 尝试读取本地 JSON
    bool hasLocalData = ModelMetadata::readLocalJson(modelDir, baseName, meta);

    if (hasLocalData) {
        // === 情况 A: 有本地数据，直接显示 (秒开) ===
        clearModelSyncFailure(filePath);
        currentMeta = meta;
        setModelTitleNormal();
        ui->scrollAreaWidgetContents->setUpdatesEnabled(false);
        updateDetailView(meta);
        QTimer::singleShot(0, this, [this]() {
            ui->scrollAreaWidgetContents->setUpdatesEnabled(true);
            ui->scrollAreaWidgetContents->update();
        });
    } else {
        meta.isLocalOnly = true;
        meta.isLocalEdited = false;
        const QString failedMessage = modelSyncFailureMessage(filePath);
        if (!failedMessage.isEmpty()) {
            showPendingLocalModelDetail(
                meta,
                AppText::tr("上次同步失败，请点击刷新模型详情重新同步。\n%1").arg(failedMessage)
            );
            ui->btnForceUpdate->setEnabled(true);
            if (ui->detailContentStack->currentIndex() == 1) {
                scanForUserImages(baseName);
            } else {
                if (userGalleryPage) userGalleryPage->clearGalleryView();
            }
            return;
        }

        showPendingLocalModelDetail(meta, AppText::tr("正在分析模型文件 (计算 Hash)..."));

        // === 情况 B: 无本地数据，需要计算 Hash 然后联网 ===

        // UI 状态反馈：显示“正在分析模型...”
        ui->lblModelName->setText(AppText::tr("正在分析模型文件 (计算 Hash)..."));
        ui->btnForceUpdate->setEnabled(false);
        startModelHashSync(filePath, baseName, false);
    }

    // 如果当前正处于 "本地返图" 页面 (Index 1)，立即刷新数据
    if (ui->detailContentStack->currentIndex() == 1) {
        // 使用当前选中的模型名进行扫描
        scanForUserImages(baseName);
    } else {
        // 如果在主详情页，先清空返图页的旧数据，防止用户等会儿切过去看到上一个模型的图
        if (userGalleryPage) userGalleryPage->clearGalleryView();
    }
}

// 强制联网
void MainWindow::onForceUpdateClicked() {
    QListWidgetItem *item = ui->modelList->currentItem();
    if (!item) return;

    if (!confirmLocalEditOverwrite(item)) return;
    int localCount = countLocalEditedModels();
    if (!optSuppressLocalWarnings && !item->data(ROLE_LOCAL_EDITED).toBool() && localCount > 0) {
        QMessageBox::information(this, AppText::tr("提示"),
                                 AppText::tr("检测到 %1 个本地/已编辑模型。\n同步其它模型的元数据不会影响它们，但强制更新时请注意覆盖风险。").arg(localCount));
    }

    ui->btnForceUpdate->setEnabled(false);

    QString baseName = item->data(ROLE_MODEL_NAME).toString();
    if (baseName.isEmpty()) baseName = item->text();
    QString filePath = item->data(ROLE_FILE_PATH).toString();
    ui->modelList->setProperty("current_processing_path", filePath);

    QString modelDir = ui->modelList->property("current_model_dir").toString();
    bool hasRemotePreview = false;
    if (!modelDir.isEmpty()) {
        QString jsonPath = QDir(modelDir).filePath(baseName + ".json");
        QFile jsonFile(jsonPath);
        if (jsonFile.exists() && jsonFile.open(QIODevice::ReadOnly)) {
            QJsonDocument doc = QJsonDocument::fromJson(jsonFile.readAll());
            QJsonObject root = doc.object();
            QJsonArray images = root["images"].toArray();
            if (!images.isEmpty()) {
                QString url = images[0].toObject()["url"].toString();
                if (!url.isEmpty()) hasRemotePreview = true;
            }
            jsonFile.close();
        }
    }
    if (!hasRemotePreview && !currentMeta.images.isEmpty() && !currentMeta.images[0].url.isEmpty()) {
        hasRemotePreview = true;
    }
    QString previewPath = ModelMetadata::previewPath(modelDir, baseName, 0);
    bool hasLocalPreview = !previewPath.isEmpty() && QFile::exists(previewPath);
    if (hasRemotePreview && hasLocalPreview) {
        QMessageBox msg(this);
        msg.setWindowTitle(AppText::tr("预览图同步"));
        msg.setText(AppText::tr("检测到已同步的 Civitai 预览图，是否重新同步预览图？"));
        QPushButton *btnYes = msg.addButton(AppText::tr("是"), QMessageBox::YesRole);
        QPushButton *btnMetaOnly = msg.addButton(AppText::tr("仅元数据"), QMessageBox::AcceptRole);
        QPushButton *btnCancel = msg.addButton(AppText::tr("取消同步"), QMessageBox::RejectRole);
        msg.setDefaultButton(btnMetaOnly);
        msg.exec();

        if (msg.clickedButton() == btnCancel) {
            m_forceResyncPreview = false;
            m_skipPreviewSync = false;
            ui->btnForceUpdate->setEnabled(true);
            ui->statusbar->showMessage(AppText::tr("已取消同步"), 2000);
            return;
        }

        if (msg.clickedButton() == btnYes) {
            m_forceResyncPreview = true;
            m_skipPreviewSync = false;
        } else if (msg.clickedButton() == btnMetaOnly) {
            m_forceResyncPreview = false;
            m_skipPreviewSync = true;
        } else {
            m_forceResyncPreview = false;
            m_skipPreviewSync = false;
        }
    } else {
        m_forceResyncPreview = false;
        m_skipPreviewSync = false;
    }

    clearModelSyncFailure(filePath);
    ModelMeta meta;
    meta.modelName = baseName;
    meta.name = baseName;
    meta.filePath = filePath;
    meta.fileName = QFileInfo(filePath).fileName();
    meta.previewPath = item->data(ROLE_PREVIEW_PATH).toString();
    meta.isLocalOnly = true;
    showPendingLocalModelDetail(meta, AppText::tr("正在重新同步模型详情 (计算 Hash)..."));
    startModelHashSync(filePath, baseName, true);
}

void MainWindow::onLocalMetaSaved(const QString &filePath, const QString &jsonPath, const QString &previewPath)
{
    QListWidgetItem *item = ui->modelList->currentItem();
    if (!item || QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath() != QFileInfo(filePath).absoluteFilePath()) {
        item = findModelItemByFilePath(filePath);
    }
    if (!item) return;

    const QFileInfo fi(filePath);
    const QString baseName = item->data(ROLE_MODEL_NAME).toString().isEmpty()
        ? fi.completeBaseName()
        : item->data(ROLE_MODEL_NAME).toString();
    const QString modelDir = fi.absolutePath();

    // 重新读取并刷新详情
    ModelMeta meta;
    meta.filePath = filePath;
    meta.previewPath = !previewPath.isEmpty() ? previewPath : item->data(ROLE_PREVIEW_PATH).toString();
    meta.fileName = fi.fileName();
    if (ModelMetadata::readLocalJson(modelDir, baseName, meta)) {
        currentMeta = meta;
        updateDetailView(meta);
    } else {
        updateLocalEditorFromMeta(meta);
    }

    preloadItemMetadata(item, jsonPath);
    refreshCollectionTreeView();
    refreshHomeFilterChips();
    refreshHomeGallery();
    const QString civitaiName = item->data(ROLE_CIVITAI_NAME).toString();
    if (optUseCivitaiName && !civitaiName.isEmpty()) {
        item->setText(civitaiName);
    } else {
        item->setText(baseName);
    }

    ui->statusbar->showMessage(AppText::tr("本地元数据已保存。"), 2000);
}

void MainWindow::onLocalMetaResetClicked()
{
    QListWidgetItem *item = ui->modelList->currentItem();
    if (!item) return;

    QString baseName = item->data(ROLE_MODEL_NAME).toString();
    QString filePath = item->data(ROLE_FILE_PATH).toString();
    if (baseName.isEmpty() || filePath.isEmpty()) return;

    QString modelDir = QFileInfo(filePath).absolutePath();
    QString jsonPath = QDir(modelDir).filePath(baseName + ".json");

    ModelMeta meta;
    meta.modelName = baseName;
    meta.versionName = "";
    meta.name = baseName;
    meta.filePath = filePath;
    meta.previewPath = item->data(ROLE_PREVIEW_PATH).toString();
    meta.fileName = QFileInfo(filePath).fileName();

    if (QFile::exists(jsonPath) && ModelMetadata::readLocalJson(modelDir, baseName, meta)) {
        currentMeta = meta;
        updateDetailView(meta);
        ui->statusbar->showMessage(AppText::tr("已从本地元数据恢复。"), 2000);
    } else {
        updateLocalEditorFromMeta(meta);
        ui->statusbar->showMessage(AppText::tr("未找到本地元数据文件，已恢复默认值。"), 2000);
    }
}

void MainWindow::onEditMetaTabClicked()
{
    if (!ui->modelList->currentItem()) {
        QMessageBox::information(this, AppText::tr("提示"), AppText::tr("请先选择一个模型。"));
        return;
    }

    const int editIndex = localMetaEditorPage ? ui->detailContentStack->indexOf(localMetaEditorPage) : 2;
    const int currentIndex = ui->detailContentStack->currentIndex();
    const int targetIndex = (currentIndex == editIndex) ? 0 : editIndex;

    ui->scrollAreaWidgetContents->removeEventFilter(this);
    ui->detailContentStack->setCurrentIndex(targetIndex);
    if (targetIndex == editIndex && localMetaEditorPage) {
        localMetaEditorPage->ensureImagesRefreshed();
    }

    fitDetailContentToCurrentPage();

    QTimer::singleShot(50, this, [this, targetIndex](){
        ui->scrollAreaWidgetContents->installEventFilter(this);
        if (targetIndex == 0) {
            fitDetailContentToCurrentPage();
        }
        updateBackgroundImage();
    });
}

QString MainWindow::civitaiApiKey() const
{
    return optCivitaiApiKey.trimmed();
}

bool MainWindow::isCivitaiUrl(const QUrl &url) const
{
    return CivitaiClient::isCivitaiUrl(url);
}

bool MainWindow::shouldUseCivitaiBearerAuth(const QUrl &url) const
{
    return CivitaiClient::shouldUseBearerAuth(url);
}

QUrl MainWindow::civitaiUrlWithToken(const QUrl &url) const
{
    return CivitaiClient::urlWithToken(url, civitaiApiKey());
}

QNetworkRequest MainWindow::makeNetworkRequest(const QUrl &url, bool allowCivitaiAuth) const
{
    return CivitaiClient::makeNetworkRequest(url, currentUserAgent, civitaiApiKey(), allowCivitaiAuth);
}

QString MainWindow::civitaiNetworkErrorMessage(QNetworkReply *reply) const
{
    return CivitaiClient::networkErrorMessage(reply);
}

void MainWindow::applyCivitaiAttributionToItem(QListWidgetItem *item, const QString &creator, const QStringList &tags)
{
    if (!item) return;
    item->setData(ROLE_MODEL_CREATOR, creator);
    item->setData(ROLE_MODEL_TAGS, tags);
}

void MainWindow::fetchModelInfoFromCivitai(const QString &hash) {
    // 获取当前正在处理的文件名 (从属性或当前选中项)
    // 建议直接传参进来，或者确保 ui->modelList->property("current_processing_file") 是本地文件名(BaseName)
    QString localBaseName = ui->modelList->property("current_processing_file").toString();
    QString modelDir = ui->modelList->property("current_model_dir").toString();
    QString urlStr = QString("https://civitai.com/api/v1/model-versions/by-hash/%1").arg(hash);
    QString filePath = ui->modelList->property("current_processing_path").toString();
    if (modelDir.isEmpty() && !filePath.isEmpty()) {
        modelDir = QFileInfo(filePath).absolutePath();
    }
    QNetworkRequest request = makeNetworkRequest(QUrl(urlStr));
    QNetworkReply *reply = netManager->get(request);
    // 将本地文件名绑定到 Reply 对象上，确保回调时知道是哪个模型
    reply->setProperty("localBaseName", localBaseName);
    reply->setProperty("modelDir", modelDir);
    reply->setProperty("localFilePath", filePath);
    reply->setProperty("filePath", filePath);
    reply->setProperty("currentSha256", hash.trimmed());

    connect(reply, &QNetworkReply::finished, this, [this, reply](){
        this->onApiMetadataReceived(reply);
    });
}

// 解析 JSON

// 联网回调
void MainWindow::onApiMetadataReceived(QNetworkReply *reply)
{
    QString localBaseName = reply->property("localBaseName").toString();
    QString modelDir = reply->property("modelDir").toString();
    QString filePath = reply->property("filePath").toString();
    QString currentSha256 = reply->property("currentSha256").toString();
    reply->deleteLater();

    if (modelDir.isEmpty() && !filePath.isEmpty()) {
        modelDir = QFileInfo(filePath).absolutePath();
    }

    QString currentSelectedPath;
    if (QListWidgetItem *currentItem = ui->modelList->currentItem()) {
        currentSelectedPath = currentItem->data(ROLE_FILE_PATH).toString();
    }
    if (filePath.isEmpty() || currentSelectedPath != filePath) {
        return;
    }

    ui->btnForceUpdate->setEnabled(true);
    currentHashSyncForceRefresh = false;

    const QByteArray versionJsonBytes = reply->property("versionJson").toByteArray();
    if (reply->error() != QNetworkReply::NoError) {
        if (!versionJsonBytes.isEmpty()) {
            qDebug() << "Civitai model detail fetch failed, falling back to version metadata:" << reply->errorString();
        } else {
            clearLayout(ui->layoutTriggerStack); // 清空触发词区域
            const QString err = civitaiNetworkErrorMessage(reply);
            if (startDetailCivArchiveFallback(filePath, localBaseName, modelDir, err, currentSha256)) {
                ui->statusbar->showMessage(AppText::tr("Civitai 获取失败，正在尝试 CivArchive..."), 4000);
                return;
            }
            recordModelSyncFailure(filePath, localBaseName, err);
            setModelTitleError(err);
            ui->textDescription->setPlainText(QString("上次同步失败，请点击刷新模型详情重新同步。\n%1").arg(err));
            transitionToImage("");
            m_forceResyncPreview = false;
            m_skipPreviewSync = false;
            ui->statusbar->showMessage(AppText::tr("元数据获取失败: ") + err, 4000);
            return;
        }
    }

    const QByteArray responseBody = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(responseBody);
    const bool usingVersionFallback = !versionJsonBytes.isEmpty() && reply->error() != QNetworkReply::NoError;
    QJsonObject root = usingVersionFallback ? QJsonDocument::fromJson(versionJsonBytes).object() : doc.object();
    if ((!usingVersionFallback && doc.isNull()) || root.isEmpty()) {
        const QString err = "返回数据为空或不是有效 JSON";
        if (startDetailCivArchiveFallback(filePath, localBaseName, modelDir, err, currentSha256)) {
            ui->statusbar->showMessage(AppText::tr("Civitai 元数据解析失败，正在尝试 CivArchive..."), 4000);
            return;
        }
        recordModelSyncFailure(filePath, localBaseName, err);
        setModelTitleError(err);
        ui->textDescription->setPlainText(QString("上次同步失败，请点击刷新模型详情重新同步。\n%1").arg(err));
        transitionToImage("");
        m_forceResyncPreview = false;
        m_skipPreviewSync = false;
        ui->statusbar->showMessage(AppText::tr("元数据解析失败"), 3000);
        return;
    }

    if (!versionJsonBytes.isEmpty()) {
        const QJsonObject versionRoot = QJsonDocument::fromJson(versionJsonBytes).object();
        if (reply->error() == QNetworkReply::NoError) {
            root = ModelMetadata::mergeCivitaiModelIntoVersion(versionRoot, root);
        } else {
            root = versionRoot;
        }
    } else {
        const int modelIdForDetail = root["modelId"].toInt();
        if (modelIdForDetail > 0) {
            QNetworkReply *detailReply = netManager->get(makeNetworkRequest(QUrl(QString("https://civitai.com/api/v1/models/%1").arg(modelIdForDetail))));
            detailReply->setProperty("localBaseName", localBaseName);
            detailReply->setProperty("modelDir", modelDir);
            detailReply->setProperty("localFilePath", filePath);
            detailReply->setProperty("filePath", filePath);
            detailReply->setProperty("currentSha256", currentSha256);
            detailReply->setProperty("versionJson", QJsonDocument(root).toJson(QJsonDocument::Compact));
            connect(detailReply, &QNetworkReply::finished, this, [this, detailReply]() {
                this->onApiMetadataReceived(detailReply);
            });
            return;
        }
    }

    clearModelSyncFailure(filePath);
    setModelTitleNormal();
    root["syncedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    ModelMeta meta = ModelMetadata::parseVersion(root, filePath, localBaseName, currentSha256);

    QListWidgetItem *targetItem = findModelItemByFilePath(filePath);

    // 更新 UI 列表项
    if (targetItem) {
        targetItem->setData(ROLE_CIVITAI_NAME, meta.name);
        targetItem->setData(ROLE_LOCAL_EDITED, false);
        targetItem->setData(ROLE_CIVITAI_MODEL_ID, meta.modelId);
        targetItem->setData(ROLE_CIVITAI_VERSION_ID, meta.versionId);
        targetItem->setData(ROLE_MODEL_TYPE, meta.type);
        applyCivitaiAttributionToItem(targetItem, meta.creatorName, meta.modelTags);
        if (optUseCivitaiName) targetItem->setText(meta.name);
    }

    if (targetItem) {
        targetItem->setData(ROLE_MODEL_TRAINED_WORDS, meta.trainedWordsGroups);
        targetItem->setData(ROLE_CIVITAI_SHA256, meta.sha256);
    }

    if (!meta.images.isEmpty()) {
        // 强制使用本地文件名构造图片路径，解决重名和冲突问题
        QString savePath = QDir::cleanPath(QDir(modelDir).filePath(localBaseName + ".preview.png"));

        if (m_skipPreviewSync) {
            if (QFile::exists(savePath)) {
                meta.previewPath = savePath;
            }
        } else {
            meta.previewPath = savePath;
        }
    }

    // 保留本地私有状态字段，避免同步 Civitai metadata 时丢失用户本地信息。
    QFile existingMetadata(QDir(modelDir).filePath(localBaseName + ".json"));
    if (existingMetadata.exists() && existingMetadata.open(QIODevice::ReadOnly)) {
        const QJsonObject oldRoot = QJsonDocument::fromJson(existingMetadata.readAll()).object();
        const QStringList localKeys = {"localEdited", "localEditedAt", "localOnly", "modelUrl"};
        for (const QString &key : localKeys) {
            if (oldRoot.contains(key)) root[key] = oldRoot.value(key);
        }
    } else {
        root["localOnly"] = false;
    }
    meta.modelUrl = ModelMetadata::metadataBrowserUrlFromRoot(root);
    meta.isLocalEdited = root["localEdited"].toBool(false);
    meta.isLocalOnly = root["localOnly"].toBool(false);
    if (targetItem) {
        targetItem->setData(ROLE_LOCAL_EDITED, meta.isLocalEdited || meta.isLocalOnly);
        applyCivitaiAttributionToItem(targetItem, meta.creatorName, meta.modelTags);
    }

    // 保存并更新UI
    saveLocalMetadata(modelDir, localBaseName, root);
    if (targetItem) preloadItemMetadata(targetItem, QDir(modelDir).filePath(localBaseName + ".json"));
    if (!m_skipPreviewSync && m_forceResyncPreview && !meta.images.isEmpty()) {
        const QString coverPath = QFileInfo(QDir(modelDir).filePath(localBaseName + ".preview.png")).absoluteFilePath();
        const ImageInfo &cover = meta.images.first();
        if (QFile::exists(coverPath) && !cover.url.isEmpty()) {
            enqueueDownload(cover.url,
                            coverPath,
                            nullptr,
                            localBaseName,
                            0,
                            PreviewImageStore::previewPayloadFromImageInfo(cover),
                            true,
                            true,
                            false);
        }
    }

    currentMeta = meta; // 缓存到成员变量
    ui->scrollAreaWidgetContents->setUpdatesEnabled(false);
    updateDetailView(meta);
    QTimer::singleShot(0, this, [this]() {
        ui->scrollAreaWidgetContents->setUpdatesEnabled(true);
        ui->scrollAreaWidgetContents->update();
    });
    m_forceResyncPreview = false;
    m_skipPreviewSync = false;
    ui->statusbar->showMessage(AppText::tr("元数据已更新"), 2000);
}

void MainWindow::applyDownloadedPreviewToUi(const QString &localBaseName, const QString &savePath)
{
    if (localBaseName.isEmpty() || savePath.isEmpty()) return;
    invalidateIconCacheForPath(savePath);

    const QFileInfo downloadedInfo(savePath);
    const QString downloadedFileName = downloadedInfo.fileName();
    const QString downloadedDirectory = QDir::cleanPath(downloadedInfo.absolutePath());
    const bool isCoverPreview = (downloadedFileName == localBaseName + ".preview.png");
    auto belongsToDownloadedModel = [&](const QString &modelPath) {
        const QFileInfo modelInfo(modelPath);
        return modelInfo.completeBaseName().compare(localBaseName, Qt::CaseSensitive) == 0
               && QDir::cleanPath(modelInfo.absolutePath()).compare(downloadedDirectory, Qt::CaseInsensitive) == 0;
    };

    QSet<QString> sidebarModelPaths;
    bool needsDetailThumbnail = false;

    if (isCoverPreview) {
        for (int i = 0; i < ui->modelList->count(); ++i) {
            QListWidgetItem *item = ui->modelList->item(i);
            const QString modelPath = item->data(ROLE_FILE_PATH).toString();
            if (belongsToDownloadedModel(modelPath)) {
                item->setData(ROLE_PREVIEW_PATH, savePath);
                item->setData(ROLE_MODEL_PREVIEW_STATE, static_cast<int>(ModelPreviewState::RealPreview));
                applyModelHighlightColor(item);
                if (!modelPath.isEmpty()) sidebarModelPaths.insert(modelPath);
            }
        }

        if (homeGalleryPage) {
            homeGalleryPage->updateDownloadedCoverPreview(localBaseName, downloadedDirectory, savePath);
        }

        std::function<void(QTreeWidgetItem*)> updateTreeNode = [&](QTreeWidgetItem *node) {
            if (!node) return;
            const QString modelPath = node->data(0, ROLE_FILE_PATH).toString();
            if (belongsToDownloadedModel(modelPath)) {
                node->setData(0, ROLE_PREVIEW_PATH, savePath);
                node->setData(0, ROLE_MODEL_PREVIEW_STATE, static_cast<int>(ModelPreviewState::RealPreview));
                applyModelHighlightColor(node);
                if (!modelPath.isEmpty()) sidebarModelPaths.insert(modelPath);
            }
            for (int i = 0; i < node->childCount(); ++i) updateTreeNode(node->child(i));
        };
        for (int i = 0; i < ui->collectionTree->topLevelItemCount(); ++i) {
            updateTreeNode(ui->collectionTree->topLevelItem(i));
        }
    }

    if (ui->layoutGallery) {
        for (int k = 0; k < ui->layoutGallery->count(); ++k) {
            if (QLayoutItem *li = ui->layoutGallery->itemAt(k)) {
                if (QPushButton *btn = qobject_cast<QPushButton*>(li->widget())) {
                    QString btnPath = QFileInfo(btn->property("savePath").toString()).absoluteFilePath();
                    if (btnPath == savePath) {
                        btn->setProperty("fullImagePath", savePath);
                        btn->setText("");
                        needsDetailThumbnail = true;
                    }
                }
            }
        }
    }

    QListWidgetItem *currentItem = ui->modelList->currentItem();
    if (currentItem && belongsToDownloadedModel(currentItem->data(ROLE_FILE_PATH).toString())) {
        if (isCoverPreview) {
            currentMeta.previewPath = savePath;
            currentHeroPath = "";
            needsDetailThumbnail = true;
        }
    }

    // Reading and scaling a newly downloaded 4K preview can take noticeable time.
    // Keep the GUI update above lightweight and let the existing image workers do it.
    for (const QString &modelPath : std::as_const(sidebarModelPaths)) {
        auto *task = new IconLoaderTask(savePath, 64, 8, this, "SIDEBAR:" + modelPath);
        task->setAutoDelete(true);
        backgroundThreadPool->start(task);
    }
    if (needsDetailThumbnail) {
        auto *task = new IconLoaderTask(savePath, 100, 0, this, savePath, true);
        task->setAutoDelete(true);
        backgroundThreadPool->start(task);
    }
}

void MainWindow::saveLocalMetadata(const QString &modelDir, const QString &baseName, const QJsonObject &data) {
    if (modelDir.isEmpty()) return;
    QString savePath = QDir(modelDir).filePath(baseName + ".json");
    QSaveFile file(savePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Unable to open metadata for writing:" << savePath << file.errorString();
        return;
    }
    const QByteArray payload = QJsonDocument(data).toJson();
    if (file.write(payload) != payload.size() || !file.commit()) {
        qWarning() << "Unable to save metadata atomically:" << savePath << file.errorString();
    }
}

void MainWindow::startModelHashSync(const QString &filePath, const QString &baseName, bool forceRefresh)
{
    if (filePath.isEmpty() || baseName.isEmpty()) {
        ui->btnForceUpdate->setEnabled(true);
        return;
    }

    if (hashWatcher->isRunning()) {
        if (currentProcessingPath == filePath) {
            currentHashSyncForceRefresh = currentHashSyncForceRefresh || forceRefresh;
        } else {
            // QFile hashing cannot be canceled cooperatively. Keep only the
            // newest requested model and start it after the active hash exits.
            pendingHashSyncPath = filePath;
            pendingHashSyncBaseName = baseName;
            pendingHashSyncForceRefresh = forceRefresh;
        }
        ui->btnForceUpdate->setEnabled(false);
        ui->statusbar->showMessage(AppText::tr("正在等待上一项 Hash 计算完成..."), 0);
        ui->lblModelName->setText(AppText::tr("正在等待模型文件分析..."));
        return;
    }

    currentProcessingPath = filePath;
    currentProcessingBaseName = baseName;
    currentHashSyncForceRefresh = forceRefresh;
    ui->modelList->setProperty("current_processing_file", baseName);
    ui->modelList->setProperty("current_processing_path", filePath);
    ui->modelList->setProperty("current_model_dir", QFileInfo(filePath).absolutePath());
    ui->btnForceUpdate->setEnabled(false);
    ui->statusbar->showMessage(AppText::tr("正在计算 Hash..."), 0);
    ui->lblModelName->setText(AppText::tr("正在分析模型文件 (计算 Hash)..."));

    hashWatcher->setFuture(QtConcurrent::run(backgroundThreadPool, [filePath]() {
        return FileUtils::calculateSha256Hex(filePath);
    }));
}

void MainWindow::onOpenUrlClicked() {
    const QString url = ui->btnOpenUrl->property("url").toString();
    const QString host = settingsPage ? settingsPage->state().civitaiBrowserHost : QStringLiteral("civitai.red");
    if (!url.isEmpty()) QDesktopServices::openUrl(ModelBrowser::preferredUrl(QUrl(url), host));
}

QString MainWindow::currentModelLoraTagName() const
{
    QString filePath;
    if (QListWidgetItem *item = ui->modelList->currentItem()) {
        filePath = item->data(ROLE_FILE_PATH).toString();
    }
    if (filePath.isEmpty()) filePath = currentMeta.filePath;

    QString tagName;
    if (!filePath.isEmpty()) {
        tagName = ModelImageMatcher::safetensorsInternalName(filePath);
        if (tagName.isEmpty()) tagName = QFileInfo(filePath).completeBaseName();
    }
    if (tagName.isEmpty()) tagName = currentMeta.fileNameServer;
    if (tagName.isEmpty()) tagName = currentMeta.name;

    if (tagName.endsWith(".safetensors", Qt::CaseInsensitive) ||
        tagName.endsWith(".ckpt", Qt::CaseInsensitive) ||
        tagName.endsWith(".pt", Qt::CaseInsensitive)) {
        tagName = QFileInfo(tagName).completeBaseName();
    }
    return tagName.trimmed();
}

void MainWindow::onCopyLoraTagClicked()
{
    const QString tagName = currentModelLoraTagName();
    if (tagName.isEmpty()) {
        ui->statusbar->showMessage(AppText::tr("无法生成 LoRA 标签：未选中模型"), 2500);
        return;
    }

    const QString tag = QString("<lora:%1:1>").arg(tagName);
    QApplication::clipboard()->setText(tag);
    ui->statusbar->showMessage(AppText::tr("已复制 LoRA 标签: ") + tag, 2500);
}

void MainWindow::downloadThumbnail(const QString &url, const QString &savePath, QPushButton *button)
{
    if (previewDownloadQueue) {
        previewDownloadQueue->downloadThumbnail(url, savePath, button, this);
    }
}

QStringList MainWindow::currentModelPreviewPaths() const
{
    QStringList result;
    QSet<QString> seen;

    QString modelDir = QFileInfo(currentMeta.filePath).absolutePath();
    QString baseName = QFileInfo(currentMeta.filePath).completeBaseName();
    if (QListWidgetItem *item = ui->modelList->currentItem(); isModelListItem(item)) {
        const QString filePath = item->data(ROLE_FILE_PATH).toString();
        if (!filePath.isEmpty()) modelDir = QFileInfo(filePath).absolutePath();
        const QString roleName = item->data(ROLE_MODEL_NAME).toString();
        if (!roleName.isEmpty()) baseName = roleName;
    }

    for (int index = 0; index < currentMeta.images.size(); ++index) {
        const ImageInfo &image = currentMeta.images.at(index);
        if (optFilterNSFW && image.nsfwLevel > optNSFWLevel && optNSFWMode == 0) continue;

        QString path = ModelMetadata::previewPath(modelDir, baseName, index);
        if (index == 0 && !QFileInfo::exists(path)
            && !currentMeta.previewPath.isEmpty() && QFileInfo::exists(currentMeta.previewPath)) {
            path = QFileInfo(currentMeta.previewPath).absoluteFilePath();
        }
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile()) continue;

        QImageReader reader(info.absoluteFilePath());
        if (!reader.canRead()) continue;
        const QString absolutePath = info.absoluteFilePath();
        const QString key = QDir::cleanPath(absolutePath).toCaseFolded();
        if (seen.contains(key)) continue;
        seen.insert(key);
        result.append(absolutePath);
    }

    if (result.isEmpty() && !currentMeta.previewPath.isEmpty()) {
        const QFileInfo fallback(currentMeta.previewPath);
        if (fallback.exists() && fallback.isFile()) result.append(fallback.absoluteFilePath());
    }
    return result;
}

QStringList MainWindow::visibleUserGalleryImagePaths() const
{
    return userGalleryPage ? userGalleryPage->visibleImagePaths() : QStringList{};
}

void MainWindow::openImageViewer(const QStringList &paths, const QString &currentPath)
{
    QStringList normalizedPaths;
    QSet<QString> seen;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile()) continue;
        const QString absolutePath = info.absoluteFilePath();
        const QString key = QDir::cleanPath(absolutePath).toCaseFolded();
        if (seen.contains(key)) continue;
        seen.insert(key);
        normalizedPaths.append(absolutePath);
    }
    if (normalizedPaths.isEmpty()) return;

    const QString requested = QFileInfo(currentPath).absoluteFilePath();
    int initialIndex = 0;
    for (int i = 0; i < normalizedPaths.size(); ++i) {
        if (QDir::cleanPath(normalizedPaths.at(i)).compare(QDir::cleanPath(requested), Qt::CaseInsensitive) == 0) {
            initialIndex = i;
            break;
        }
    }

    const bool newWindow = settingsPage && settingsPage->state().imageViewerNewWindow;
    if (!newWindow && imageViewer) {
        imageViewer->setImages(normalizedPaths, initialIndex);
        if (imageViewer->isMinimized()) imageViewer->showNormal();
        else imageViewer->show();
        imageViewer->raise();
        imageViewer->activateWindow();
        return;
    }

    auto *dialog = new ImageViewerDialog(normalizedPaths, initialIndex, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    imageViewer = dialog;
    connect(dialog, &QDialog::finished, this, [this, dialog]() {
        // A closed window may still be waiting for its deferred deletion.
        if (imageViewer == dialog) imageViewer.clear();
    });
    dialog->show();
}

void MainWindow::openImageViewerForPath(const QString &currentPath)
{
    const QString requested = QFileInfo(currentPath).absoluteFilePath();
    if (ui->detailContentStack->currentIndex() == 1) {
        openImageViewer(visibleUserGalleryImagePaths(), requested);
        return;
    }
    openImageViewer(currentModelPreviewPaths(), requested);
}

// 新增：适应比例图标 (Fit 模式)

void MainWindow::onIconLoaded(const QString &id, const QImage &image)
{
    if (isShuttingDown) return;

    // 空图 = 预览缺失/加载失败：保留各项已设置的主题化占位X（含 ROLE_PREVIEW_PLACEHOLDER 标记），
    // 不覆盖、不清标记，交给 recolorPlaceholderItems 在切主题时重染。
    if (image.isNull()) {
        return;
    }

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
            if (blurredPix.isNull()) blurredPix = ImagePresentation::applyNSFWBlur(originalPix);
            return blurredPix;
        }
        return originalPix;
    };

    // =========================================================
    // 3. 更新主页列表 (Home Gallery) - 仅限 HOME 或 通用任务
    // =========================================================
    if (isHomeTask && homeGalleryPage) {
        homeGalleryPage->applyExternalImageUpdate(filePath, originalPix, originalIcon);
    }

    // =========================================================
    // 4. 更新侧边栏 (Sidebar List & Tree) - 仅限 SIDEBAR 或 通用任务 (O(1) 查找 + 内存缓存)
    // =========================================================
    if (isSidebarTask) {
        auto getSidebarIcon = [&](bool isNSFW, const QString &previewPath) -> QIcon {
            const bool blurItem = (optFilterNSFW && isNSFW && optNSFWMode == 1);
            const QString cacheKey = (blurItem ? QStringLiteral("blur|") : QStringLiteral("norm|")) + previewPath;
            QIcon cached = sidebarIconCache.value(cacheKey);
            if (!cached.isNull()) return cached;
            QIcon built;
            if (blurItem) {
                if (blurredPix.isNull()) blurredPix = ImagePresentation::applyNSFWBlur(originalPix);
                built = ImagePresentation::getSquareIcon(ImagePresentation::applyRoundedMask(blurredPix, 12));
            } else {
                built = ImagePresentation::getSquareIcon(originalPix);
            }
            if (!previewPath.isEmpty()) sidebarIconCache.insert(cacheKey, built);
            return built;
        };

        // --- A. 更新侧边栏列表 ---
        if (QListWidgetItem *item = modelItemByPath.value(filePath, nullptr)) {
            const bool isNSFW = item->data(ROLE_NSFW_LEVEL).toInt() > optNSFWLevel;
            item->setIcon(getSidebarIcon(isNSFW, item->data(ROLE_PREVIEW_PATH).toString()));
            item->setData(ROLE_PREVIEW_PLACEHOLDER, false);
            item->setData(ROLE_MODEL_PREVIEW_STATE, static_cast<int>(ModelPreviewState::RealPreview));
            applyModelHighlightColor(item);
        } else {
            for (int i = 0; i < ui->modelList->count(); ++i) {
                QListWidgetItem *item = ui->modelList->item(i);
                if (item && item->data(ROLE_FILE_PATH).toString() == filePath) {
                    const bool isNSFW = item->data(ROLE_NSFW_LEVEL).toInt() > optNSFWLevel;
                    item->setIcon(getSidebarIcon(isNSFW, item->data(ROLE_PREVIEW_PATH).toString()));
                    item->setData(ROLE_PREVIEW_PLACEHOLDER, false);
                    item->setData(ROLE_MODEL_PREVIEW_STATE, static_cast<int>(ModelPreviewState::RealPreview));
                    applyModelHighlightColor(item);
                }
            }
        }

        // --- B. 更新收藏夹树状图 ---
        const QList<QTreeWidgetItem*> treeNodes = collectionTreeItemsByPath.values(filePath);
        for (QTreeWidgetItem *node : treeNodes) {
            if (!node) continue;
            const bool isNSFW = node->data(0, ROLE_NSFW_LEVEL).toInt() > optNSFWLevel;
            node->setIcon(0, getSidebarIcon(isNSFW, node->data(0, ROLE_PREVIEW_PATH).toString()));
            node->setData(0, ROLE_PREVIEW_PLACEHOLDER, false);
            node->setData(0, ROLE_MODEL_PREVIEW_STATE, static_cast<int>(ModelPreviewState::RealPreview));
            applyModelHighlightColor(node);
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
        if (userGalleryPage) {
            userGalleryPage->applyExternalImageUpdate(filePath, originalPix, originalIcon);
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

void MainWindow::onHashCalculated()
{
    // 获取后台线程的返回值
    const QString hash = hashWatcher->result();
    const QString filePath = currentProcessingPath;
    const QString baseName = currentProcessingBaseName;

    currentProcessingPath.clear();
    currentProcessingBaseName.clear();

    auto startPendingHashForCurrentModel = [this]() {
        const QString pendingPath = pendingHashSyncPath;
        const QString pendingBaseName = pendingHashSyncBaseName;
        const bool pendingForceRefresh = pendingHashSyncForceRefresh;
        pendingHashSyncPath.clear();
        pendingHashSyncBaseName.clear();
        pendingHashSyncForceRefresh = false;

        QListWidgetItem *selectedItem = ui->modelList->currentItem();
        const QString selectedPath = selectedItem ? selectedItem->data(ROLE_FILE_PATH).toString() : QString();
        if (!pendingPath.isEmpty() && selectedPath == pendingPath) {
            startModelHashSync(pendingPath, pendingBaseName, pendingForceRefresh);
        }
    };

    QString currentSelectedPath;
    if (QListWidgetItem *currentItem = ui->modelList->currentItem()) {
        currentSelectedPath = currentItem->data(ROLE_FILE_PATH).toString();
    }
    if (filePath.isEmpty() || filePath != currentSelectedPath) {
        currentHashSyncForceRefresh = false;
        startPendingHashForCurrentModel();
        return;
    }

    // 检查：如果计算出来的 Hash 为空，说明文件可能被锁或读失败
    if (hash.isEmpty()) {
        const QString err = "无法读取文件或计算 Hash 失败";
        recordModelSyncFailure(filePath, baseName, err);
        setModelTitleError(err);
        ui->textDescription->setPlainText(QString("上次同步失败，请点击刷新模型详情重新同步。\n%1").arg(err));
        ui->btnForceUpdate->setEnabled(true);
        currentHashSyncForceRefresh = false;
        startPendingHashForCurrentModel();
        return;
    }

    // Hash 算完了，现在开始联网
    setModelTitleNormal();
    ui->lblModelName->setText(AppText::tr("Hash 计算完成，正在获取元数据..."));
    ui->statusbar->showMessage(AppText::tr("Hash 计算完成，正在获取元数据..."), 2000);
    fetchModelInfoFromCivitai(hash); // 调用你原来的联网函数
    startPendingHashForCurrentModel();
}

void MainWindow::updateBackgroundImage()
{
    if (!ui->backgroundLabel || !ui->heroFrame || !ui->scrollAreaWidgetContents) return;

    // The background remains in content coordinates so it scrolls together
    // with the hero, while the blur composition itself uses stable absolute
    // fade positions and no QLabel stretching.
    QSize targetSize = ui->scrollAreaWidgetContents->size();
    if (QWidget *viewport = ui->scrollAreaWidgetContents->parentWidget()) {
        targetSize.setWidth(qMax(targetSize.width(), viewport->width()));
        targetSize.setHeight(qMax(targetSize.height(), viewport->height()));
    }
    if (targetSize.isEmpty()) targetSize = QSize(1920, 1080);
    const QRect bgRect(QPoint(0, 0), targetSize);
    if (ui->backgroundLabel->geometry() != bgRect) {
        ui->backgroundLabel->setGeometry(bgRect);
    }

    // 如果正在动画，不处理 Resize，由动画循环处理
    if (transitionAnim && transitionAnim->state() == QAbstractAnimation::Running) return;

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
        empty.fill(QColor(AppStyle::MainBackground()));
        ui->backgroundLabel->setPixmap(empty);
    }
}

void MainWindow::onSearchTextChanged(const QString &text)
{
    QString query = text.trimmed();
    QString targetBaseModel = ui->comboBaseModel->currentIndex() == 0 ? "All" : ui->comboBaseModel->currentText();
    QString targetModelType = ui->comboModelType->currentIndex() == 0 ? "All" : ui->comboModelType->currentText();

    // 1. 自动重置收藏夹 (保留逻辑)
    if (!query.isEmpty() && !currentCollectionFilter.isEmpty()) {
        currentCollectionFilter = "";
        refreshHomeCollectionsUI();
    }

    // 2. 遍历筛选
    ui->modelList->setUpdatesEnabled(false);
    for(int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!isModelListItem(item)) {
            if (item) item->setHidden(true);
            if (item) item->setData(ROLE_MODEL_FILTER_VISIBLE, false);
            continue;
        }

        item->setData(ROLE_MODEL_FILTER_VISIBLE,
                      ModelFilter::matches(modelFilterRecord(item), query,
                                           targetBaseModel, targetModelType));
    }
    applyModelFolderVisibility();
    ui->modelList->setUpdatesEnabled(true);

    // 3. 刷新主页
    refreshHomeGallery();

    // 4. 切回主页优化 (保留逻辑)
    if (ui->mainStack->currentIndex() == 1) {
        QListWidgetItem *currentItem = ui->modelList->currentItem();
        if (currentItem && !currentItem->data(ROLE_MODEL_FILTER_VISIBLE).toBool()) {
            ui->mainStack->setCurrentIndex(0);
        }
    }

    refreshCollectionTreeView();
}

void MainWindow::showCollectionMenu(const QList<QListWidgetItem*> &items, const QPoint &globalPos)
{
    QList<QListWidgetItem*> modelItems;
    for (QListWidgetItem *item : items) {
        if (isModelListItem(item)) modelItems.append(item);
    }
    if (modelItems.isEmpty()) return;

    QMenu menu(this);

    // 1. 标题逻辑
    if (modelItems.count() == 1) {
        QListWidgetItem *first = modelItems.first();
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
        QAction *titleAct = menu.addAction(AppText::tr("已选中 %1 个模型").arg(modelItems.count()));
        titleAct->setEnabled(false);
    }

    menu.addSeparator();

    QColor defaultColor(102, 192, 244, 96);
    if (modelItems.count() == 1) {
        const QString filePath = QFileInfo(modelItems.first()->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
        if (modelHighlightColors.contains(filePath)) {
            defaultColor = modelHighlightColors.value(filePath);
        }
    }

    QAction *actSetHighlightColor = menu.addAction(AppText::tr("设置高亮颜色... / Set Highlight Color..."));
    connect(actSetHighlightColor, &QAction::triggered, this, [this, modelItems, defaultColor]() {
        QColorDialog dialog(defaultColor, this);
        dialog.setWindowTitle(AppText::tr("设置模型高亮颜色"));
        dialog.setOption(QColorDialog::ShowAlphaChannel, true);
        if (dialog.exec() != QDialog::Accepted) return;

        const QColor color = dialog.selectedColor();
        if (!color.isValid()) return;
        setHighlightColorForItems(modelItems, color);
    });

    QAction *actClearHighlightColor = menu.addAction(AppText::tr("清除高亮颜色 / Clear Highlight Color"));
    bool hasHighlightColor = false;
    for (QListWidgetItem *item : modelItems) {
        const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
        if (modelHighlightColors.contains(filePath)) {
            hasHighlightColor = true;
            break;
        }
    }
    actClearHighlightColor->setEnabled(hasHighlightColor);
    connect(actClearHighlightColor, &QAction::triggered, this, [this, modelItems]() {
        clearHighlightColorForItems(modelItems);
    });

    menu.addSeparator();

    QAction *actEditUserNote = menu.addAction(AppText::tr("编辑评分、备注与触发词... / Edit Rating, Note && Triggers..."));
    actEditUserNote->setEnabled(modelItems.count() == 1);
    connect(actEditUserNote, &QAction::triggered, this, [this, modelItems]() {
        if (modelItems.count() == 1) openModelNoteDialog(modelItems.first());
    });

    QMenu *ratingMenu = menu.addMenu(AppText::tr("设置评分 / Set Rating"));
    QAction *clearRatingAct = ratingMenu->addAction(AppText::tr("未评分 / Clear Rating"));
    connect(clearRatingAct, &QAction::triggered, this, [this, modelItems]() {
        setUserRatingForItems(modelItems, 0.0);
    });
    ratingMenu->addSeparator();
    for (int half = 2; half <= 10; ++half) {
        const double rating = half / 2.0;
        QAction *act = ratingMenu->addAction(formatModelRating(rating));
        connect(act, &QAction::triggered, this, [this, modelItems, rating]() {
            setUserRatingForItems(modelItems, rating);
        });
    }

    QAction *actAddUserTags = menu.addAction(AppText::tr("添加用户标签... / Add User Tags..."));
    connect(actAddUserTags, &QAction::triggered, this, [this, modelItems]() {
        bool ok = false;
        const QString text = QInputDialog::getMultiLineText(this,
                                                            AppText::tr("添加用户标签"),
                                                            AppText::tr("输入标签，逗号或换行分隔："),
                                                            QString(),
                                                            &ok);
        if (!ok) return;
        addUserTagsForItems(modelItems, normalizeModelUserTagsText(text));
    });

    QAction *actRemoveUserTags = menu.addAction(AppText::tr("移除用户标签... / Remove User Tags..."));
    connect(actRemoveUserTags, &QAction::triggered, this, [this, modelItems]() {
        bool ok = false;
        const QString text = QInputDialog::getMultiLineText(this,
                                                            AppText::tr("移除用户标签"),
                                                            AppText::tr("输入要移除的标签，逗号或换行分隔："),
                                                            QString(),
                                                            &ok);
        if (!ok) return;
        removeUserTagsForItems(modelItems, normalizeModelUserTagsText(text));
    });

    menu.addSeparator();

    // 打开模型文件所在位置
    QStringList targetFilePaths;
    for (QListWidgetItem *item : modelItems) {
        if (!item) continue;
        QString path = item->data(ROLE_FILE_PATH).toString().trimmed();
        if (path.isEmpty()) continue;
        if (!targetFilePaths.contains(path)) targetFilePaths.append(path);
    }

    QAction *actOpenModelLocation = menu.addAction(AppText::tr("打开模型位置 / Show Model in Folder"));
    actOpenModelLocation->setEnabled(!targetFilePaths.isEmpty());
    connect(actOpenModelLocation, &QAction::triggered, this, [this, targetFilePaths]() {
        if (targetFilePaths.isEmpty()) return;

        const QString filePath = targetFilePaths.first();
        showFileInFolder(filePath);
        if (targetFilePaths.size() > 1) {
            ui->statusbar->showMessage(AppText::tr("已打开首个模型位置（共选中 %1 个）").arg(targetFilePaths.size()), 2500);
        }
    });

    QAction *actSyncMetadata = menu.addAction(AppText::tr("🔄 批量同步元数据与预览图 / Sync Metadata..."));
    actSyncMetadata->setEnabled(!targetFilePaths.isEmpty());
    connect(actSyncMetadata, &QAction::triggered, this, [this, targetFilePaths]() {
        if (targetFilePaths.isEmpty()) return;
        onMenuSwitchToDownloads();
        startMetadataSyncForPaths(targetFilePaths, true);
    });

    menu.addSeparator();

    // 辅助 Lambda：获取 items 对应的所有 BaseName (用于收藏夹数据存储)
    // 收藏夹系统始终使用 ROLE_MODEL_NAME (文件名) 作为 Key，不受显示名称影响
    QStringList targetBaseNames;
    for(auto *item : modelItems) {
        targetBaseNames.append(item->data(ROLE_MODEL_NAME).toString());
    }

    // =========================================================
    // 2. "从收藏夹移除..."
    // =========================================================
    QMenu *removeMenu = menu.addMenu(AppText::tr("从指定收藏夹移除..."));
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
                ui->statusbar->showMessage(AppText::tr("已从 %1 移除 %2 个模型").arg(colName).arg(removedCount), 2000);
            });
        }
    }
    if (!canRemoveAny) removeMenu->setEnabled(false);

    // =========================================================
    // 3. "添加至收藏夹..."
    // =========================================================
    QMenu *addMenu = menu.addMenu(AppText::tr("添加至收藏夹..."));

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
                ui->statusbar->showMessage(AppText::tr("已将 %1 个模型加入 %2").arg(count).arg(colName), 2000);
            } else {
                // 批量移除（取消勾选）
                for (const QString &bn : targetBaseNames) {
                    if (collections[colName].contains(bn)) {
                        collections[colName].removeAll(bn);
                        count++;
                    }
                }
                ui->statusbar->showMessage(AppText::tr("已从 %1 移除 %2 个模型").arg(colName).arg(count), 2000);
            }
            saveCollections();

            // 如果影响了当前视图，刷新
            if (currentCollectionFilter == colName) refreshHomeGallery();
            refreshCollectionTreeView();
        });
    }

    addMenu->addSeparator();
    QAction *newAction = addMenu->addAction(AppText::tr("新建收藏夹..."));
    connect(newAction, &QAction::triggered, this, [this, targetBaseNames](){
        bool ok;
        QString text = QInputDialog::getText(this, AppText::tr("新建"), AppText::tr("名称:"), QLineEdit::Normal, "", &ok);
        if(ok && !text.isEmpty()) {
            if(!collections.contains(text)) {
                // 直接将选中的模型全部加入新收藏夹
                collections[text] = targetBaseNames;
                saveCollections();
                refreshHomeCollectionsUI();
                refreshCollectionTreeView();
                ui->statusbar->showMessage(AppText::tr("新建收藏夹并加入 %1 个模型").arg(targetBaseNames.count()), 2000);
            }
        }
    });

    menu.exec(globalPos);
}

void MainWindow::applyModelHighlightColor(QListWidgetItem *item)
{
    if (!isModelListItem(item)) return;

    const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
    if (modelHighlightColors.contains(filePath)) {
        item->setData(ROLE_MODEL_HIGHLIGHT_COLOR, modelHighlightColors.value(filePath));
    } else {
        item->setData(ROLE_MODEL_HIGHLIGHT_COLOR, QVariant());
    }
}

void MainWindow::applyModelHighlightColor(QTreeWidgetItem *item)
{
    if (!item || item->data(0, ROLE_FILE_PATH).toString().isEmpty()) return;

    const QString filePath = QFileInfo(item->data(0, ROLE_FILE_PATH).toString()).absoluteFilePath();
    if (modelHighlightColors.contains(filePath)) {
        item->setData(0, ROLE_MODEL_HIGHLIGHT_COLOR, modelHighlightColors.value(filePath));
    } else {
        item->setData(0, ROLE_MODEL_HIGHLIGHT_COLOR, QVariant());
    }
}

void MainWindow::setHighlightColorForItems(const QList<QListWidgetItem*> &items, const QColor &color)
{
    if (!color.isValid()) return;

    int changed = 0;
    for (QListWidgetItem *item : items) {
        if (!isModelListItem(item)) continue;
        const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
        if (filePath.isEmpty()) continue;
        modelHighlightColors.insert(filePath, color);
        applyModelHighlightColor(item);
        changed++;
    }
    if (changed <= 0) return;

    saveModelHighlightColors();
    refreshCollectionTreeView();
    ui->modelList->viewport()->update();
    ui->statusbar->showMessage(AppText::tr("已设置 %1 个模型的高亮颜色").arg(changed), 2000);
}

void MainWindow::clearHighlightColorForItems(const QList<QListWidgetItem*> &items)
{
    int changed = 0;
    for (QListWidgetItem *item : items) {
        if (!isModelListItem(item)) continue;
        const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
        if (filePath.isEmpty()) continue;
        if (modelHighlightColors.remove(filePath)) {
            applyModelHighlightColor(item);
            changed++;
        }
    }
    if (changed <= 0) return;

    saveModelHighlightColors();
    refreshCollectionTreeView();
    ui->modelList->viewport()->update();
    ui->statusbar->showMessage(AppText::tr("已清除 %1 个模型的高亮颜色").arg(changed), 2000);
}

void MainWindow::preloadItemMetadata(QListWidgetItem *item, const QString &jsonPath)
{
    // 解析（纯 I/O）与写入列表项（UI）分离，扫描时可在工作线程复用 ModelScanner::parseModelListMetadata。
    const ModelScanner::ModelListMetadata m = ModelScanner::parseModelListMetadata(item->data(ROLE_FILE_PATH).toString(), jsonPath);
    applyModelListMetadataToItem(item, m);

    ModelPreviewState state = m.previewState;
    const QString previewPath = item->data(ROLE_PREVIEW_PATH).toString();
    if (!previewPath.isEmpty() && QFileInfo::exists(previewPath)) {
        QImageReader reader(previewPath);
        state = reader.canRead() ? ModelPreviewState::RealPreview
                                 : ModelPreviewState::MissingOrUnknown;
    }
    item->setData(ROLE_MODEL_PREVIEW_STATE, static_cast<int>(state));
    if (state != ModelPreviewState::RealPreview) {
        item->setData(ROLE_PREVIEW_PLACEHOLDER, true);
        item->setIcon(state == ModelPreviewState::KnownNoPreview
                          ? smallNoPreviewIcon
                          : smallPlaceholderIcon);
    }
    syncModelPreviewStateToViews(item);
}

void MainWindow::syncModelPreviewStateToViews(QListWidgetItem *sourceItem)
{
    if (!sourceItem) return;
    const QString sourcePath = sourceItem->data(ROLE_FILE_PATH).toString();
    if (sourcePath.isEmpty()) return;
    const QString filePath = QFileInfo(sourcePath).absoluteFilePath();

    const QVariant state = sourceItem->data(ROLE_MODEL_PREVIEW_STATE);
    const ModelPreviewState previewState = static_cast<ModelPreviewState>(state.toInt());
    if (previewState == ModelPreviewState::RealPreview) return;

    if (homeGalleryPage) {
        homeGalleryPage->syncPreviewState(filePath, previewState);
    }

    std::function<void(QTreeWidgetItem *)> updateTreeItem = [&](QTreeWidgetItem *node) {
        if (!node) return;
        if (QFileInfo(node->data(0, ROLE_FILE_PATH).toString()).absoluteFilePath() == filePath) {
            node->setData(0, ROLE_MODEL_PREVIEW_STATE, state);
            node->setData(0, ROLE_PREVIEW_PLACEHOLDER, true);
            node->setIcon(0, previewState == ModelPreviewState::KnownNoPreview
                                 ? smallNoPreviewIcon
                                 : smallPlaceholderIcon);
        }
        for (int i = 0; i < node->childCount(); ++i) updateTreeItem(node->child(i));
    };
    for (int i = 0; i < ui->collectionTree->topLevelItemCount(); ++i) {
        updateTreeItem(ui->collectionTree->topLevelItem(i));
    }

    if (downloadManager && downloadManager->containsInfo(sourcePath)) {
        ModelUpdateInfo info = downloadManager->info(sourcePath);
        info.previewState = previewState;
        downloadManager->setInfo(info);
        downloadManager->resetPreview(sourcePath);
    }
}

void MainWindow::refreshModelUsageStatsAsync()
{
    if (!ui || !ui->modelList) return;

    const int currentToken = ++modelUsageStatsToken;
    if (userGalleryCacheLoading || modelScanRunning) return;

    QList<ModelImageMatcher::ModelUsageInput> models;
    models.reserve(ui->modelList->count());

    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!isModelListItem(item)) continue;

        item->setData(ROLE_SORT_USAGE_COUNT, 0);
        item->setData(ROLE_SORT_LAST_USED, 0);

        const QString filePath = item->data(ROLE_FILE_PATH).toString();
        const QString baseName = item->data(ROLE_MODEL_NAME).toString();
        if (!filePath.isEmpty()) {
            ModelImageMatcher::ModelUsageInput input;
            input.filePath = filePath;
            input.baseName = baseName;
            input.type = item->data(ROLE_MODEL_TYPE).toString();
            input.civitaiName = item->data(ROLE_CIVITAI_NAME).toString();
            input.sha256 = item->data(ROLE_CIVITAI_SHA256).toString();
            models.append(input);
        }
    }

    if (models.isEmpty()) return;

    if (imageCache.isEmpty()) {
        if (ui->comboSort->currentIndex() == 5 || ui->comboSort->currentIndex() == 6) {
            executeSort();
        }
        refreshCurrentDetailCacheStatus();
        refreshUsageAnalysisWidget();
        return;
    }

    const QMap<QString, UserImageInfo> cacheCopy = imageCache;
    const int matchMode = optUserGalleryMatchMode;

    auto *watcher = new QFutureWatcher<QList<ModelImageMatcher::ModelUsageStatResult>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, currentToken]() {
        const QList<ModelImageMatcher::ModelUsageStatResult> stats = watcher->result();
        watcher->deleteLater();
        if (currentToken != modelUsageStatsToken) return;

        QMap<QString, ModelImageMatcher::ModelUsageStatResult> statsByPath;
        for (const ModelImageMatcher::ModelUsageStatResult &stat : stats) {
            statsByPath.insert(QFileInfo(stat.filePath).absoluteFilePath(), stat);
        }

        for (int i = 0; i < ui->modelList->count(); ++i) {
            QListWidgetItem *item = ui->modelList->item(i);
            if (!isModelListItem(item)) continue;

            const QString filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
            const ModelImageMatcher::ModelUsageStatResult stat = statsByPath.value(filePath);
            item->setData(ROLE_SORT_USAGE_COUNT, stat.usageCount);
            item->setData(ROLE_SORT_LAST_USED, stat.lastUsed);
        }

        const int sortType = ui->comboSort->currentIndex();
        if (sortType == 5 || sortType == 6) {
            executeSort();
        }
        refreshCurrentDetailCacheStatus();
        refreshUsageAnalysisWidget();
    });

    const bool comfyModelNameFallback = optComfyModelNameFallback;
    watcher->setFuture(QtConcurrent::run(
        backgroundThreadPool,
        [models, cacheCopy, matchMode, comfyModelNameFallback]() {
            return ModelImageMatcher::calculateUsage(models, cacheCopy, matchMode, comfyModelNameFallback);
        }));
}

void MainWindow::onSortIndexChanged(int index) {
    executeSort();
}

bool MainWindow::isModelListItem(const QListWidgetItem *item) const
{
    return item
           && !item->data(ROLE_IS_FOLDER_HEADER).toBool()
           && !item->data(ROLE_FILE_PATH).toString().isEmpty();
}

QListWidgetItem *MainWindow::createModelFolderHeader(const QString &folderName, const QString &folderKey) const
{
    QListWidgetItem *header = new QListWidgetItem(folderName);
    header->setData(ROLE_IS_FOLDER_HEADER, true);
    header->setData(ROLE_MODEL_FOLDER_KEY, folderKey);
    header->setData(ROLE_MODEL_FOLDER_COLLAPSED, collapsedModelFolders.contains(folderKey));
    header->setData(ROLE_MODEL_ROOT_NAME, folderName);
    header->setFlags(Qt::ItemIsEnabled);
    QFont font = header->font();
    font.setBold(true);
    header->setFont(font);
    header->setForeground(QColor(AppStyle::AccentBlue()));
    header->setBackground(QColor(AppStyle::HeaderBackground()));
    return header;
}

void MainWindow::applyModelFolderVisibility()
{
    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!item) continue;
        if (!item->data(ROLE_IS_FOLDER_HEADER).toBool()) {
            if (!optModelListFolderGrouping && isModelListItem(item)) {
                item->setHidden(!item->data(ROLE_MODEL_FILTER_VISIBLE).toBool());
            }
            continue;
        }

        const QString folderKey = item->data(ROLE_MODEL_FOLDER_KEY).toString();
        const bool collapsed = collapsedModelFolders.contains(folderKey);
        int visibleChildCount = 0;
        for (int j = i + 1; j < ui->modelList->count(); ++j) {
            QListWidgetItem *child = ui->modelList->item(j);
            if (!child) continue;
            if (child->data(ROLE_IS_FOLDER_HEADER).toBool()) break;
            if (!isModelListItem(child)) continue;

            const bool filterVisible = child->data(ROLE_MODEL_FILTER_VISIBLE).toBool();
            if (filterVisible) {
                visibleChildCount++;
            }
            child->setHidden(!filterVisible || collapsed);
        }

        const QString folderName = item->data(ROLE_MODEL_ROOT_NAME).toString().isEmpty()
                                       ? item->text().mid(2).section(" (", 0, 0).trimmed()
                                       : item->data(ROLE_MODEL_ROOT_NAME).toString();
        item->setData(ROLE_MODEL_FOLDER_COLLAPSED, collapsed);
        item->setText(QString("%1 %2 (%3)").arg(collapsed ? " + " : " - ", folderName).arg(visibleChildCount));
        item->setHidden(!optModelListFolderGrouping || visibleChildCount <= 0);
    }
}

void MainWindow::toggleModelFolderCollapsed(const QString &folderKey)
{
    if (folderKey.isEmpty()) return;

    if (collapsedModelFolders.contains(folderKey)) {
        collapsedModelFolders.remove(folderKey);
    } else {
        collapsedModelFolders.insert(folderKey);
    }
    applyModelFolderVisibility();
    saveGlobalConfig();
}

void MainWindow::executeSort()
{
    // 0: Name, 1: Date(New), 2: Downloads, 3: Likes, 4: Date Added, 5: Usage, 6: Recently Used, 7: User Rating
    int sortType = ui->comboSort->currentIndex();

    // 1. 取出所有 Item
    QList<QListWidgetItem*> items;
    while(ui->modelList->count() > 0) {
        QListWidgetItem *item = ui->modelList->takeItem(0);
        if (item && item->data(ROLE_IS_FOLDER_HEADER).toBool()) {
            delete item;
        } else if (item) {
            items.append(item);
        }
    }

    // === 准备自然排序器 (用于 Case 0) ===
    QCollator collator;
    collator.setNumericMode(true); // 开启数字模式 (让 v2 排在 v10 前面)
    collator.setCaseSensitivity(Qt::CaseInsensitive); // 忽略大小写 (让 a 和 A 排在一起)
    collator.setIgnorePunctuation(false); // 不忽略标点 (保证 [ 能参与排序)

    // 2. 使用 Lambda 表达式排序
    std::sort(items.begin(), items.end(),
        [sortType, &collator](QListWidgetItem *a, QListWidgetItem *b) { // 注意这里捕获了 &collator
        auto compareByName = [&collator](QListWidgetItem *left, QListWidgetItem *right) {
            return collator.compare(left->text(), right->text()) < 0;
        };

        switch (sortType) {
            case 0: // Name (A-Z)
                return compareByName(a, b);

            case 1: // Date (Newest First -> Descending)
                return a->data(ROLE_SORT_DATE).toLongLong() > b->data(ROLE_SORT_DATE).toLongLong();

            case 2: // Downloads (High -> Descending)
                return a->data(ROLE_SORT_DOWNLOADS).toInt() > b->data(ROLE_SORT_DOWNLOADS).toInt();

            case 3: // Likes (High -> Descending)
                return a->data(ROLE_SORT_LIKES).toInt() > b->data(ROLE_SORT_LIKES).toInt();

            case 4: // Date Added (Local Created At -> Descending)
                return a->data(ROLE_SORT_ADDED).toLongLong() > b->data(ROLE_SORT_ADDED).toLongLong();

            case 5: // Usage Count (Local Gallery)
            {
                const int usageA = a->data(ROLE_SORT_USAGE_COUNT).toInt();
                const int usageB = b->data(ROLE_SORT_USAGE_COUNT).toInt();
                if (usageA != usageB) return usageA > usageB;
                return compareByName(a, b);
            }

            case 6: // Recently Used (Local Gallery)
            {
                const qint64 usedA = a->data(ROLE_SORT_LAST_USED).toLongLong();
                const qint64 usedB = b->data(ROLE_SORT_LAST_USED).toLongLong();
                if (usedA != usedB) return usedA > usedB;
                return compareByName(a, b);
            }

            case 7: // Rating (User)
            {
                const double ratingA = a->data(ROLE_USER_RATING).toDouble();
                const double ratingB = b->data(ROLE_USER_RATING).toDouble();
                if (!qFuzzyCompare(ratingA + 1.0, ratingB + 1.0)) return ratingA > ratingB;
                return compareByName(a, b);
            }

            default:
                return compareByName(a, b);
            }
        }
    );

    // 3. 放回 ListWidget
    if (optModelListFolderGrouping) {
        QMap<QString, QList<QListWidgetItem*>> grouped;
        QMap<QString, QString> folderLabels;
        QStringList folderOrder;
        for (QListWidgetItem *item : items) {
            const QString rootPath = item->data(ROLE_MODEL_ROOT_PATH).toString();
            const QString rootKey = rootPath.isEmpty() ? "__unknown__" : QFileInfo(rootPath).absoluteFilePath();
            QString rootName = item->data(ROLE_MODEL_ROOT_NAME).toString();
            if (rootName.isEmpty()) rootName = "未指定文件夹";
            if (!grouped.contains(rootKey)) {
                folderOrder.append(rootKey);
                folderLabels.insert(rootKey, rootName);
            }
            grouped[rootKey].append(item);
        }

        QCollator folderCollator;
        folderCollator.setNumericMode(true);
        folderCollator.setCaseSensitivity(Qt::CaseInsensitive);
        std::sort(folderOrder.begin(), folderOrder.end(), [&](const QString &a, const QString &b) {
            return folderCollator.compare(folderLabels.value(a), folderLabels.value(b)) < 0;
        });

        for (const QString &folderKey : folderOrder) {
            ui->modelList->addItem(createModelFolderHeader(folderLabels.value(folderKey), folderKey));
            for (QListWidgetItem *item : grouped.value(folderKey)) {
                ui->modelList->addItem(item);
            }
        }
    } else {
        for(auto *item : items) {
            ui->modelList->addItem(item);
        }
    }

    // 4. 同步刷新主页
    onSearchTextChanged(ui->searchEdit->text());
}

void MainWindow::onFilterBaseModelChanged(const QString &text) {
    onSearchTextChanged(ui->searchEdit->text());
}

// 重建侧栏“排序/筛选”多级菜单。每次弹出前调用，使底模/类型项与最近一次扫描保持一致，
// 并标记当前选中项。各项触发时只是改动隐藏下拉框的当前值，从而复用既有的排序/筛选逻辑。
void MainWindow::rebuildSortFilterMenu() {
    QMenu *menu = ui->btnSortFilter->menu();
    if (!menu) return;
    menu->clear();

    auto buildSubmenu = [this, menu](const QString &title, QComboBox *combo, bool byIndex) {
        QMenu *sub = menu->addMenu(title);
        QActionGroup *group = new QActionGroup(sub);
        group->setExclusive(true);
        for (int i = 0; i < combo->count(); ++i) {
            QAction *act = sub->addAction(combo->itemText(i));
            act->setCheckable(true);
            act->setChecked(i == combo->currentIndex());
            group->addAction(act);
            if (byIndex) {
                connect(act, &QAction::triggered, this, [combo, i]() { combo->setCurrentIndex(i); });
            } else {
                const QString text = combo->itemText(i);
                connect(act, &QAction::triggered, this, [combo, text]() { combo->setCurrentText(text); });
            }
        }
    };

    buildSubmenu(AppText::tr("排序 / Sort"), ui->comboSort, true);
    menu->addSeparator();
    buildSubmenu(AppText::tr("底模 / Base"), ui->comboBaseModel, false);
    buildSubmenu(AppText::tr("类型 / Type"), ui->comboModelType, false);
}

// 用当前排序与已启用的筛选拼出按钮文案，方便用户不展开菜单也能看到当前状态。
void MainWindow::updateSortFilterButtonText() {
    QString text = "⚙ " + ui->comboSort->currentText();
    QStringList active;
    if (ui->comboBaseModel->currentIndex() > 0) active << ui->comboBaseModel->currentText();
    if (ui->comboModelType->currentIndex() > 0) active << ui->comboModelType->currentText();
    if (!active.isEmpty()) text += " · " + active.join(" · ");
    ui->btnSortFilter->setText(text);
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
        QFuture<ImageLoadResult> future = QtConcurrent::run(threadPool, &MainWindow::processImageTask, path);
        imageLoadWatcher->setFuture(future);
    }
}

QPixmap MainWindow::applyBlurToImage(const QImage &srcImg, const QSize &bgSize, const QSize &heroSize)
{
    const ImagePresentation::BlurOptions options{
        optDownscaleBlur, optBlurProcessWidth, optBlurRadius, QColor(AppStyle::MainBackground())};
    return ImagePresentation::blurredDetailBackground(srcImg, bgSize, heroSize, options);
}

void MainWindow::updateBackgroundDuringTransition()
{
    if (!ui->backgroundLabel) return;
    QSize bgSize = ui->backgroundLabel->size();
    if (bgSize.isEmpty()) return;

    QPixmap canvas(bgSize);
    canvas.fill(QColor(AppStyle::MainBackground())); // 纯色打底，防止交叉淡化时露出底色

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

void MainWindow::initServices()
{
    auto requestFactory = [this](const QUrl &url, bool allowAuth) {
        return makeNetworkRequest(url, allowAuth);
    };
    auto apiKeyProvider = [this]() {
        return civitaiApiKey();
    };

    appUpdateChecker = new AppUpdateChecker(netManager, this);
    connect(appUpdateChecker, &AppUpdateChecker::checkingChanged, this, [this](bool checking) {
        if (aboutPage) aboutPage->setCheckingForUpdates(checking);
    });
    connect(appUpdateChecker, &AppUpdateChecker::statusMessage, this, [this](const QString &msg, int timeoutMs) {
        if (ui && ui->statusbar) ui->statusbar->showMessage(msg, timeoutMs);
    });
    connect(appUpdateChecker, &AppUpdateChecker::updateAvailable, this,
            [this](const QString &remoteTag, const QString &htmlUrl, const QString &body) {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(AppText::tr("发现新版本"));
        msgBox.setTextFormat(Qt::RichText);
        msgBox.setText(AppText::tr("<h3>🚀 发现新版本: %1</h3>"
                               "<p>当前版本: %2</p>"
                               "<hr>"
                               "<p><b>更新日志:</b></p><pre style='font-size:11px'>%3</pre>")
                           .arg(remoteTag, CURRENT_VERSION, body));

        QPushButton *btnGo = msgBox.addButton(AppText::tr("前往下载 / Download"), QMessageBox::AcceptRole);
        msgBox.addButton(AppText::tr("稍后 / Later"), QMessageBox::RejectRole);
        msgBox.exec();

        if (msgBox.clickedButton() == btnGo) {
            QDesktopServices::openUrl(QUrl(htmlUrl));
        }
    });
    connect(appUpdateChecker, &AppUpdateChecker::alreadyUpToDate, this,
            [this](const QString &version, bool silent) {
        if (silent) {
            ui->statusbar->showMessage(AppText::tr("当前已是最新版本 (%1)").arg(version), 3000);
        } else {
            QMessageBox::information(this, AppText::tr("检查更新"), AppText::tr("当前已是最新版本 (%1)。").arg(version));
        }
    });
    connect(appUpdateChecker, &AppUpdateChecker::checkFailed, this,
            [this](const QString &errorMsg, bool parseError, bool silent) {
        if (parseError) {
            if (silent) {
                ui->statusbar->showMessage(AppText::tr("自动检查更新失败: 无法解析版本信息"), 4000);
            } else {
                QMessageBox::warning(this, AppText::tr("错误"), AppText::tr("无法解析版本信息 (Rate Limit Exceeded?)。"));
            }
        } else if (silent) {
            ui->statusbar->showMessage(AppText::tr("自动检查更新失败: ") + errorMsg, 4000);
        } else {
            QMessageBox::warning(this, AppText::tr("检查失败"), AppText::tr("无法连接到 GitHub API:\n") + errorMsg);
        }
    });

    previewDownloadQueue = new PreviewDownloadQueue(
        netManager, backgroundThreadPool, threadPool, requestFactory, apiKeyProvider, this);
    connect(previewDownloadQueue, &PreviewDownloadQueue::previewSaved,
            this, &MainWindow::applyDownloadedPreviewToUi);
    connect(previewDownloadQueue, &PreviewDownloadQueue::statusMessage, this,
            [this](const QString &msg, int timeoutMs) {
        if (ui && ui->statusbar) ui->statusbar->showMessage(msg, timeoutMs);
    });
    connect(previewDownloadQueue, &PreviewDownloadQueue::metadataStatusText, this,
            [this](const QString &text) {
        if (downloadsPage) downloadsPage->setStatusText(text);
    });

    metadataSyncService = new MetadataSyncService(
        netManager,
        backgroundThreadPool,
        previewDownloadQueue,
        requestFactory,
        [this]() {
            return MetadataSyncOptions{optRecalculateKnownMetadataHash, optTryCivArchiveOnMetadataFail};
        },
        [this](const MetadataSyncJob &job,
               const QJsonObject &modelRoot,
               const QJsonObject &versionHint,
               bool syncPreviewImages,
               bool batchRunning) {
            return saveMetadataFromModelRoot(job, modelRoot, versionHint, syncPreviewImages, batchRunning);
        },
        this);
    connect(previewDownloadQueue, &PreviewDownloadQueue::metadataPreviewTaskFinished,
            metadataSyncService, &MetadataSyncService::onMetadataPreviewTaskFinished);
    connect(metadataSyncService, &MetadataSyncService::itemStatusChanged, this,
            [this](const QString &filePath, const QString &status, const QString &category,
                   const QString &timestamp, const QString &sourceLabel) {
        if (downloadsPage) {
            downloadsPage->updateMetadataScanItemStatus(filePath, status, category, timestamp, sourceLabel);
        }
    });
    connect(metadataSyncService, &MetadataSyncService::batchStatusChanged, this,
            [this](const QString &text) {
        if (downloadsPage) downloadsPage->setStatusText(text);
    });
    connect(metadataSyncService, &MetadataSyncService::hashCalculated, this,
            [this](const QString &filePath, const QString &sha256) {
        if (QListWidgetItem *item = findModelItemByFilePath(filePath)) {
            item->setData(ROLE_CIVITAI_SHA256, sha256);
        }
    });
    connect(metadataSyncService, &MetadataSyncService::jobFailed,
            this, &MainWindow::handleMetadataSyncJobFailed);
    connect(metadataSyncService, &MetadataSyncService::batchCompleted, this, [this](int) {
        refreshHomeGallery();
        refreshCollectionTreeView();
        startMetadataScan();
        refreshUsageAnalysisWidget();
    });

    modelUpdateCoordinator = new ModelUpdateCoordinator(
        netManager, backgroundThreadPool, requestFactory, this);
    connect(modelUpdateCoordinator, &ModelUpdateCoordinator::statusChanged, this,
            [this](const QString &text) {
        if (downloadsPage) downloadsPage->setStatusText(text);
    });
    connect(modelUpdateCoordinator, &ModelUpdateCoordinator::buttonsEnabledChanged, this,
            [this](bool enabled) {
        if (downloadsPage) downloadsPage->setUpdateCheckButtonsEnabled(enabled);
    });
    connect(modelUpdateCoordinator, &ModelUpdateCoordinator::cardUpdated,
            this, &MainWindow::addOrUpdateDownloadCard);
    connect(modelUpdateCoordinator, &ModelUpdateCoordinator::modelRootReceived,
            this, &MainWindow::handleModelUpdateRootReceived);
    connect(modelUpdateCoordinator, &ModelUpdateCoordinator::allChecksCompleted,
            this, &MainWindow::handleAllModelUpdateChecksCompleted);
}

void MainWindow::syncPreviewImagesFromMetadata(const QString &modelDir,
                                               const QString &baseName,
                                               const QVector<ImageInfo> &images,
                                               bool forceNonCoverDownload,
                                               bool countForMetadataSync)
{
    if (previewDownloadQueue) {
        previewDownloadQueue->syncPreviewImagesFromMetadata(
            modelDir, baseName, images, forceNonCoverDownload, countForMetadataSync);
    }
}

void MainWindow::enqueueDownload(const QString &url,
                                 const QString &savePath,
                                 QPushButton *btn,
                                 const QString &localBaseName,
                                 int imageIndex,
                                 const PreviewMetadataPayload &previewMeta,
                                 bool allowNoButton,
                                 bool metadataOnly,
                                 bool countForMetadataSync)
{
    if (!previewDownloadQueue) return;
    PreviewDownloadTask task;
    task.url = url;
    task.savePath = savePath;
    task.localBaseName = localBaseName;
    task.button = btn;
    task.imageIndex = imageIndex;
    task.previewMeta = previewMeta;
    task.allowNoButton = allowNoButton;
    task.metadataOnly = metadataOnly;
    task.countForMetadataSync = countForMetadataSync;
    previewDownloadQueue->enqueue(task);
}

// ==========================================
//  User Gallery (Tab Page 2) Implementation
// ==========================================
void MainWindow::onToggleDetailTab() {
    int currentIndex = ui->detailContentStack->currentIndex();
    int nextIndex = (currentIndex == 1) ? 0 : 1;

    ui->scrollAreaWidgetContents->removeEventFilter(this);

    // 1. 切换页面
    ui->detailContentStack->setCurrentIndex(nextIndex);

    // 2. 按当前页实际内容收紧外壳高度，避免主详情页底部留下可滚动空白
    fitDetailContentToCurrentPage();

    QTimer::singleShot(50, this, [this, nextIndex](){
        // 恢复事件监听
        ui->scrollAreaWidgetContents->installEventFilter(this);

        // 如果切换到详情页，调整容器大小
        if (nextIndex == 0) {
            fitDetailContentToCurrentPage();
        }

        // 强制更新一次背景（避免尺寸不对）
        updateBackgroundImage();
    });

    // 3. 自动扫描逻辑 (保持不变)
    if (nextIndex == 1 && (!userGalleryPage || userGalleryPage->imageCount() == 0)) {
        onRescanUserClicked();
    }
}

void MainWindow::openTagModelGraph(int scope, const QString &focusQuery)
{
    if (!ui->toolsTabWidget) return;

    const bool wasLoaded = (usageAnalysisWidget != nullptr);
    ui->rootStack->setCurrentWidget(ui->toolsTabWidget);
    ui->toolsTabWidget->setCurrentIndex(4);
    ensureToolTabLoaded(4);
    QTimer::singleShot(0, this, [this, scope, focusQuery, wasLoaded]() {
        if (!usageAnalysisWidget) return;
        if (wasLoaded) {
            refreshUsageAnalysisWidget();
        }
        usageAnalysisWidget->openTagModelGraph(scope, focusQuery);
    });
}

void MainWindow::onOpenModelGraphClicked()
{
    QString focusQuery;
    if (!currentMeta.filePath.isEmpty()) {
        focusQuery = QFileInfo(currentMeta.filePath).completeBaseName();
    } else if (!currentMeta.name.isEmpty()) {
        focusQuery = currentMeta.name;
    }
    openTagModelGraph(0, focusQuery);
}

void MainWindow::onRescanUserClicked() {
    if (userGalleryGlobalMode) {
        scanForUserImages("");
        return;
    }

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
    editGalleryPaths(true);
}

void MainWindow::onClearUserGalleryCacheClicked()
{
    QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        AppText::tr("清除图库缓存"),
        AppText::tr("这会删除本地图库提示词缓存文件，下次扫描图库时会重新解析所有图片。\n是否继续？"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );
    if (reply != QMessageBox::Yes) return;

    ++userGalleryCacheLoadToken;
    userGalleryCacheLoading = false;
    imageCache.clear();
    QString cachePath = qApp->applicationDirPath() + "/config/user_gallery_cache.json";
    QFile::remove(cachePath);
    refreshModelUsageStatsAsync();
    ui->statusbar->showMessage(AppText::tr("本地图库缓存已清除"), 3000);
    resumePendingUserGalleryScan();
}

void MainWindow::syncUserGalleryOptions()
{
    if (!userGalleryPage) return;
    UserGalleryOptions opts;
    opts.filterNsfw = optFilterGalleryNSFW;
    opts.nsfwMode = optGalleryNSFWMode;
    opts.nsfwRules = optGalleryNSFWRules;
    opts.splitOnNewline = optSplitOnNewline;
    opts.filterTags = optFilterTags;
    opts.recursive = optGalleryRecursive;
    opts.matchMode = optUserGalleryMatchMode;
    opts.comfyModelNameFallback = optComfyModelNameFallback;
    userGalleryPage->setOptions(opts);
}

void MainWindow::scanForUserImages(const QString &loraBaseName)
{
    if (!userGalleryPage) return;
    if (userGalleryCacheLoading) {
        // Keep only the latest request; never scan/save against a half-loaded cache.
        pendingUserGalleryScan = true;
        pendingUserGalleryModel = loraBaseName;
        const QListWidgetItem *item = ui->modelList->currentItem();
        pendingUserGalleryModelPath = isModelListItem(item)
                                          ? item->data(ROLE_FILE_PATH).toString() : currentMeta.filePath;
        ui->statusbar->showMessage(AppText::tr("正在后台加载图库缓存，完成后开始扫描..."));
        return;
    }

    userGalleryGlobalMode = loraBaseName.isEmpty();
    placeholderIcon = generatePlaceholderIcon();
    userGalleryPage->setPlaceholderIcon(placeholderIcon);
    syncUserGalleryOptions();

    const QStringList activeGalleryPaths = PathUtils::collectEnabledPaths(galleryPaths, disabledGalleryPaths);
    const QStringList validGalleryPaths = PathUtils::collectValidPaths(activeGalleryPaths);
    if (validGalleryPaths.isEmpty()) {
        userGalleryPage->invalidateAndReset();
        userGalleryPage->clearGalleryView();
        userGalleryPage->showInvalidFolderHint();
        QMessageBox::warning(this, AppText::tr("目录无效"), AppText::tr("设置的 SD 输出目录不存在、为空，或全部被禁用。"));
        return;
    }

    QListWidgetItem *currentItem = ui->modelList->currentItem();
    QString selectedFilePath;
    QString selectedBaseName;
    QString selectedModelType;
    QString selectedCivitaiName;
    QString selectedSha256;
    if (currentItem) {
        selectedFilePath = currentItem->data(ROLE_FILE_PATH).toString();
        selectedBaseName = currentItem->data(ROLE_MODEL_NAME).toString().trimmed();
        selectedModelType = currentItem->data(ROLE_MODEL_TYPE).toString();
        selectedCivitaiName = currentItem->data(ROLE_CIVITAI_NAME).toString();
        selectedSha256 = currentItem->data(ROLE_CIVITAI_SHA256).toString();
    }
    if (selectedFilePath.isEmpty()) selectedFilePath = currentMeta.filePath;
    if (selectedBaseName.isEmpty() && !selectedFilePath.isEmpty()) {
        selectedBaseName = QFileInfo(selectedFilePath).completeBaseName();
    }
    if (selectedBaseName.isEmpty()) selectedBaseName = loraBaseName;
    if (selectedModelType.isEmpty()) selectedModelType = currentMeta.type;
    if (selectedCivitaiName.isEmpty()) selectedCivitaiName = currentMeta.name;
    if (selectedSha256.isEmpty()) selectedSha256 = currentMeta.sha256;

    const ModelImageMatcher::ModelUsageInput modelInput{
        selectedFilePath, selectedBaseName, selectedModelType, selectedCivitaiName, selectedSha256};

    userGalleryPage->startScan(validGalleryPaths, loraBaseName.isEmpty(), loraBaseName, modelInput);
}

bool MainWindow::isGalleryImageNsfw(const QString &prompt, const QStringList &cleanTags) const
{
    return GalleryMetadata::matchesPositivePromptNsfwRules(prompt, cleanTags, optGalleryNSFWRules);
}

bool MainWindow::isGalleryImageNsfwByPath(const QString &imagePath) const
{
    if (imagePath.isEmpty() || optGalleryNSFWRules.isEmpty()) return false;

    auto it = imageCache.constFind(imagePath);
    if (it == imageCache.constEnd()) {
        const QString cleanTarget = QDir::cleanPath(QFileInfo(imagePath).absoluteFilePath());
        for (auto cacheIt = imageCache.cbegin(); cacheIt != imageCache.cend(); ++cacheIt) {
            if (QDir::cleanPath(QFileInfo(cacheIt.key()).absoluteFilePath()).compare(cleanTarget, Qt::CaseInsensitive) == 0) {
                it = cacheIt;
                break;
            }
        }
    }
    if (it != imageCache.constEnd()) {
        return isGalleryImageNsfw(it.value().prompt, it.value().cleanTags);
    }

    return userGalleryPage ? userGalleryPage->isImageNsfwByPath(imagePath) : false;
}

void MainWindow::refreshGalleryNsfwDisplay()
{
    if (userGalleryPage) {
        placeholderIcon = generatePlaceholderIcon();
        userGalleryPage->setPlaceholderIcon(placeholderIcon);
        syncUserGalleryOptions();
        userGalleryPage->refreshNsfwDisplay();

        const QString currentGalleryPath = userGalleryPage->currentImagePath();
        if (!currentGalleryPath.isEmpty() &&
            ((ui && ui->detailContentStack && ui->detailContentStack->currentIndex() == 1) || userGalleryGlobalMode)) {
            currentHeroPath.clear();
            transitionToImage(currentGalleryPath);
        }
    }
}

QListWidgetItem *MainWindow::findModelItemByNameOrHash(const QString &name, const QString &hash) const
{
    const QString cleanHash = hash.trimmed().toLower();

    // 1. 优先根据 sha256 匹配
    if (!cleanHash.isEmpty() && cleanHash.length() >= 6) {
        for (int i = 0; i < ui->modelList->count(); ++i) {
            QListWidgetItem *item = ui->modelList->item(i);
            if (!isModelListItem(item)) continue;
            const QString itemSha = item->data(ROLE_CIVITAI_SHA256).toString().trimmed().toLower();
            if (!itemSha.isEmpty()) {
                if (itemSha.startsWith(cleanHash) || cleanHash.startsWith(itemSha)) {
                    return item;
                }
            }
        }
    }

    // 2. 根据名称匹配（单次遍历同时记录 exact / civitai / fuzzy 最高优先级命中）
    if (name.trimmed().isEmpty()) return nullptr;

    const QString searchNorm = GalleryMetadata::cleanModelOrLoraBaseName(name).toLower();
    if (searchNorm.isEmpty()) return nullptr;

    const bool allowFuzzy = searchNorm.length() >= 3;
    auto fuzzyMatches = [&searchNorm](const QString &target) -> bool {
        if (target.length() < 3) return false;
        if (target.contains(searchNorm)) return true;
        if (target.length() >= 4 && searchNorm.contains(target)) return true;
        return false;
    };

    QListWidgetItem *civitaiMatch = nullptr;
    QListWidgetItem *fuzzyMatch = nullptr;
    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!isModelListItem(item)) continue;
        const QString itemName = GalleryMetadata::cleanModelOrLoraBaseName(item->data(ROLE_MODEL_NAME).toString()).toLower();
        const QString itemFile = QFileInfo(item->data(ROLE_FILE_PATH).toString()).completeBaseName().toLower();
        if ((!itemName.isEmpty() && itemName == searchNorm) ||
            (!itemFile.isEmpty() && itemFile == searchNorm)) {
            return item;
        }
        const QString civModel = GalleryMetadata::cleanModelOrLoraBaseName(item->data(ROLE_CIVITAI_NAME).toString()).toLower();
        if (!civitaiMatch && !civModel.isEmpty() && civModel == searchNorm) {
            civitaiMatch = item;
        }
        if (allowFuzzy && !fuzzyMatch &&
            (fuzzyMatches(itemName) || fuzzyMatches(itemFile) || fuzzyMatches(civModel))) {
            fuzzyMatch = item;
        }
    }

    if (civitaiMatch) return civitaiMatch;
    if (fuzzyMatch) return fuzzyMatch;

    return nullptr;
}

void MainWindow::jumpToModelOrLora(const QString &name, const QString &hash)
{
    QListWidgetItem *item = findModelItemByNameOrHash(name, hash);
    if (!item) {
        ui->statusbar->showMessage(AppText::tr("未在本地模型库中找到对应的模型/LoRA: %1").arg(name.isEmpty() ? hash : name), 3500);
        return;
    }

    onMenuSwitchToLibrary();
    if (ui->detailContentStack) {
        ui->detailContentStack->setCurrentIndex(0);
    }
    ui->modelList->setCurrentItem(item);
    item->setSelected(true);
    ui->modelList->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    onModelListClicked(item);
    if (ui->scrollArea) {
        if (ui->scrollArea->verticalScrollBar()) ui->scrollArea->verticalScrollBar()->setValue(0);
        if (ui->scrollArea->horizontalScrollBar()) ui->scrollArea->horizontalScrollBar()->setValue(0);
    }
    QTimer::singleShot(0, this, [this]() {
        if (ui && ui->scrollArea) {
            if (ui->scrollArea->verticalScrollBar()) ui->scrollArea->verticalScrollBar()->setValue(0);
            if (ui->scrollArea->horizontalScrollBar()) ui->scrollArea->horizontalScrollBar()->setValue(0);
        }
    });
    ui->statusbar->showMessage(AppText::tr("已定位并跳转至模型: %1").arg(item->text()), 3500);
}

void MainWindow::onGalleryButtonClicked()
{
    userGalleryGlobalMode = true;
    // 1. 清除侧边栏模型选中状态，表示现在不是看某个具体模型
    ui->modelList->clearSelection();

    // 2. 切换到详情页 (Page 1)
    ui->mainStack->setCurrentIndex(1);

    // 3. 强制切换到“本地返图”标签页 (Index 1)
    // 注意：这里我们手动模拟 onToggleDetailTab 的部分逻辑
    ui->detailContentStack->setCurrentIndex(1);
    fitDetailContentToCurrentPage();

    // 4. 设置 UI 状态 (伪装成一个 Model)
    clearDetailView(); // 清空之前的模型信息

    // 自定义标题
    ui->lblModelName->setText(AppText::tr("Global User Gallery / 所有用户返图"));
    ui->lblModelName->setStyleSheet(AppStyle::globalGalleryTitleStyle());

    // 隐藏/禁用一些不相关的按钮
    ui->btnForceUpdate->setVisible(false);
    ui->btnCheckModelUpdate->setVisible(false);
    ui->btnOpenUrl->setVisible(false);
    ui->btnEditMeta->setVisible(false);
    ui->btnFavorite->setVisible(false);
    ui->btnShowUserGallery->setVisible(false);
    if (ui->btnOpenModelGraph) ui->btnOpenModelGraph->setVisible(false);
    ui->btnEditMeta->setVisible(false);
    ui->btnCopyLoraTag->setVisible(false);

    // 5. 清除背景图 (或者你可以放一张默认的图库壁纸)
    currentHeroPath = "";
    transitionToImage("");

    // 6. 执行全局扫描 (传入空字符串)
    scanForUserImages("");
}

void MainWindow::initMenuBar() {
    // 1. 使用 this->menuBar() 这是一个保险措施
    // 它可以确保即便 XML 里的菜单栏丢失或层级错误，这里也能获取到窗口真正的菜单栏
    QMenuBar *bar = this->menuBar();
    bar->clear(); // 清空旧内容

    // 2. 直接添加“库”按钮 (Action)
    // 这种直接 addAction 到 bar 的方式，效果就像是点击按钮，而不是弹出下拉菜单
    QAction *actLib = new QAction(AppText::tr("📚 库 / Library"), this);
    actLib->setShortcut(QKeySequence("Ctrl+1"));
    connect(actLib, &QAction::triggered, this, &MainWindow::onMenuSwitchToLibrary);
    bar->addAction(actLib);

    // 4. 直接添加“设置”按钮 (Action)
    QAction *actSet = new QAction(AppText::tr("⚙️ 设置 / Settings"), this);
    actSet->setShortcut(QKeySequence("Ctrl+2"));
    connect(actSet, &QAction::triggered, this, &MainWindow::onMenuSwitchToSettings);
    bar->addAction(actSet);

    QAction *actDownloads = new QAction(AppText::tr("⬇️ 下载 / Downloads"), this);
    actDownloads->setShortcut(QKeySequence("Ctrl+3"));
    connect(actDownloads, &QAction::triggered, this, &MainWindow::onMenuSwitchToDownloads);
    bar->addAction(actDownloads);

    // 5. 工具页 (静态 QTabWidget 与 6 个占位页已在 mainwindow.ui 中定义)
    if (ui->toolsTabWidget) {
        ui->toolsTabWidget->setAutoFillBackground(true);
        QPalette pal = ui->toolsTabWidget->palette();
        pal.setColor(QPalette::Window, AppStyle::color("sidebarBg"));
        ui->toolsTabWidget->setPalette(pal);

        for (QLabel *label : ui->toolsTabWidget->findChildren<QLabel*>()) {
            if (label->property("isToolPlaceholderLabel").toBool()) {
                label->setStyleSheet(AppStyle::MutedLabelStyle());
            }
        }

        connect(ui->toolsTabWidget, &QTabWidget::currentChanged,
                this, &MainWindow::ensureToolTabLoaded, Qt::UniqueConnection);
    }

    QAction *actTools = new QAction(AppText::tr("🛠️ 工具箱 / Tools"), this);
    actTools->setShortcut(QKeySequence("Ctrl+4"));
    connect(actTools, &QAction::triggered, this, [this](){
        if (!ui->toolsTabWidget) return;
        ui->rootStack->setCurrentWidget(ui->toolsTabWidget);
        ensureToolTabLoaded(ui->toolsTabWidget->currentIndex());
    });
    bar->addAction(actTools);

    // 5b. 启动器页 (Launcher)
    if (!launcherWidget) {
        launcherWidget = new LauncherWidget(this);
        ui->rootStack->addWidget(launcherWidget);
    }
    QAction *actLauncher = new QAction(AppText::tr("🚀 启动器 / Launcher"), this);
    actLauncher->setShortcut(QKeySequence("Ctrl+5"));
    connect(actLauncher, &QAction::triggered, this, [this](){
        ui->rootStack->setCurrentWidget(launcherWidget);
    });
    bar->addAction(actLauncher);

    // 6. 关于按钮
    QAction *btnAbout = new QAction(AppText::tr("ℹ️ 关于 / About"));
    btnAbout->setShortcut(QKeySequence("Ctrl+6"));
    connect(btnAbout, &QAction::triggered, this, &MainWindow::onMenuSwitchToAbout);
    bar->addAction(btnAbout);

    // 7. 强制显示 (防止被 hidden 属性隐藏)
    bar->setVisible(true);
}

void MainWindow::ensureToolTabLoaded(int index)
{
    QTabWidget *toolsTab = ui ? ui->toolsTabWidget : nullptr;
    if (!toolsTab || index < 0 || index >= toolsTab->count()) return;
    QWidget *currentPage = toolsTab->widget(index);
    if (!currentPage || !currentPage->property("isToolPlaceholder").toBool()) return;
    if (pendingToolTabLoads.contains(index)) return;

    pendingToolTabLoads.insert(index);
    QTimer::singleShot(0, this, [this, index]() {
        pendingToolTabLoads.remove(index);
        QTabWidget *toolsTab = ui ? ui->toolsTabWidget : nullptr;
        if (!toolsTab || index < 0 || index >= toolsTab->count()) return;
        QWidget *currentPage = toolsTab->widget(index);
        if (!currentPage || !currentPage->property("isToolPlaceholder").toBool()) return;

        QWidget *newPage = nullptr;
        switch (index) {
        case 0:
            newPage = new SyncWidget(toolsTab);
            break;
        case 1:
            parserWidget = new PromptParserWidget(toolsTab);
            parserWidget->setTranslationMap(&translationMap);
            newPage = parserWidget;
            break;
        case 2:
            tagBrowserWidget = new TagBrowserWidget(toolsTab);
            tagBrowserWidget->setTranslationSources(
                buildTagTranslationSources(translationCsvPaths, disabledTranslationCsvPaths));
            tagBrowserWidget->setMergedTranslationMap(&translationMap);
            connect(tagBrowserWidget, &TagBrowserWidget::csvSaved, this, [this](const QString &path){
                Q_UNUSED(path);
                reloadTranslationMaps();
            });
            newPage = tagBrowserWidget;
            break;
        case 3:
            llmPromptWidget = new LlmPromptWidget(toolsTab);
            llmPromptWidget->setLibraryPaths(
                PathUtils::collectEnabledPaths(loraPaths, disabledLoraPaths),
                PathUtils::collectEnabledPaths(galleryPaths, disabledGalleryPaths)
            );
            newPage = llmPromptWidget;
            break;
        case 4:
            usageAnalysisWidget = new UsageAnalysisWidget(toolsTab);
            usageAnalysisWidget->setTranslationMap(&translationMap);
            connect(usageAnalysisWidget, &UsageAnalysisWidget::requestRefresh,
                    this, &MainWindow::refreshUsageAnalysisWidget);
            connect(usageAnalysisWidget, &UsageAnalysisWidget::requestOpenModel,
                    this, &MainWindow::jumpToDownloadSource);
            connect(usageAnalysisWidget, &UsageAnalysisWidget::requestJumpToModel,
                    this, [this](const QString &name) {
                        jumpToModelOrLora(name);
                    });
            connect(usageAnalysisWidget, &UsageAnalysisWidget::requestFilterInGallery,
                    this, [this](const QString &tagOrLora, bool isLora) {
                        onMenuSwitchToLibrary();
                        if (!userGalleryPage || !userGalleryPage->isVisible()) {
                            if (userGalleryPage && userGalleryPage->imageCount() > 0) {
                                ui->mainStack->setCurrentIndex(1);
                                ui->detailContentStack->setCurrentIndex(1);
                                fitDetailContentToCurrentPage();
                            } else {
                                onGalleryButtonClicked();
                            }
                        }
                        if (userGalleryPage) {
                            userGalleryPage->filterBySingleTagOrLora(tagOrLora, isLora);
                        }
                    });
            connect(usageAnalysisWidget, &UsageAnalysisWidget::requestFilterMultiInGallery,
                    this, [this](const QStringList &tags, const QStringList &loras) {
                        onMenuSwitchToLibrary();
                        if (!userGalleryPage || !userGalleryPage->isVisible()) {
                            if (userGalleryPage && userGalleryPage->imageCount() > 0) {
                                ui->mainStack->setCurrentIndex(1);
                                ui->detailContentStack->setCurrentIndex(1);
                                fitDetailContentToCurrentPage();
                            } else {
                                onGalleryButtonClicked();
                            }
                        }
                        if (userGalleryPage) {
                            userGalleryPage->filterByMultiTagsAndLoras(tags, loras);
                        }
                    });
            refreshUsageAnalysisWidget();
            newPage = usageAnalysisWidget;
            break;
        case 5:
            promptTemplateLibraryWidget = new PromptTemplateLibraryWidget(toolsTab);
            promptTemplateLibraryWidget->setTranslationMap(&translationMap);
            connect(promptTemplateLibraryWidget, &PromptTemplateLibraryWidget::modelTriggerRowsRequested,
                    this, &MainWindow::refreshPromptTemplateModelTriggerRows);
            newPage = promptTemplateLibraryWidget;
            break;
        default:
            break;
        }

        if (!newPage) return;
        applyToolPageTheme(newPage);
        const QString label = toolsTab->tabText(index);
        QSignalBlocker blocker(toolsTab);
        toolsTab->removeTab(index);
        currentPage->setParent(nullptr);
        currentPage->deleteLater();
        toolsTab->insertTab(index, newPage, label);
        toolsTab->setCurrentIndex(index);
    });
}

void MainWindow::onMenuSwitchToLibrary() {
    ui->rootStack->setCurrentIndex(0);
}

void MainWindow::onMenuSwitchToSettings() {
    ui->rootStack->setCurrentWidget(settingsPage);
}

void MainWindow::onMenuSwitchToDownloads()
{
    ui->rootStack->setCurrentWidget(downloadsPage);
    if (downloadManager) downloadManager->ensureCacheLoaded();
}

void MainWindow::onTestCivitaiApiKeyClicked()
{
    if (settingsPage) optCivitaiApiKey = settingsPage->state().civitaiApiKey;
    saveGlobalConfig();
    if (optCivitaiApiKey.isEmpty()) {
        if (settingsPage) settingsPage->setCivitaiApiStatus(AppText::tr("请先输入 Civitai API Key"));
        return;
    }
    if (settingsPage) {
        settingsPage->setCivitaiApiStatus(AppText::tr("正在测试 API Key..."));
        settingsPage->setCivitaiApiTesting(true);
    }

    QNetworkRequest request = makeNetworkRequest(QUrl("https://civitai.com/api/v1/models?limit=1&favorites=true"));
    QNetworkReply *reply = netManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (settingsPage) settingsPage->setCivitaiApiTesting(false);
        if (reply->error() != QNetworkReply::NoError) {
            if (settingsPage) settingsPage->setCivitaiApiStatus(AppText::tr("API Key 测试失败: ") + civitaiNetworkErrorMessage(reply));
            return;
        }
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            if (settingsPage) settingsPage->setCivitaiApiStatus(AppText::tr("API Key 测试失败: 返回不是有效 JSON"));
            return;
        }
        if (settingsPage) settingsPage->setCivitaiApiStatus(AppText::tr("API Key 可用"));
    });
}

void MainWindow::initDownloadsPage()
{
    downloadsPage->initializeAppearance();
    downloadManager = new DownloadManager(downloadsPage, netManager, backgroundThreadPool, this);
    downloadManager->setNetworkCallbacks(
        [this](const QUrl &url, bool allowCivitaiAuth) {
            return makeNetworkRequest(url, allowCivitaiAuth);
        },
        [this](QNetworkReply *reply) {
            return civitaiNetworkErrorMessage(reply);
        },
        [this](const QUrl &url) {
            return civitaiUrlWithToken(url);
        },
        [this]() {
            return civitaiApiKey();
        },
        [](const QString &filePath) {
            return FileUtils::calculateSha256Hex(filePath);
        });
    downloadManager->setTargetPathCallback([this](const ModelUpdateInfo &info, bool *overwrite) {
        return chooseModelDownloadTarget(info, overwrite);
    });
    downloadManager->setPreviewPathCallback([this](const ModelUpdateInfo &info) {
        return resolveDownloadPreviewPath(info);
    });
    connect(downloadManager, &DownloadManager::statusMessageChanged,
            downloadsPage, &DownloadsPage::setStatusText);
    connect(downloadManager, &DownloadManager::modelFileReady,
            this, &MainWindow::finishModelDownload);

    connect(downloadsPage->checkSelectedButton(), &QPushButton::clicked, this, [this]() {
        QList<QListWidgetItem*> items;
        const QStringList filePaths = downloadsPage->selectedFilePaths();
        for (const QString &filePath : filePaths) {
            if (QListWidgetItem *item = findModelItemByFilePath(filePath)) items << item;
        }
        checkUpdatesForItems(items);
    });
    connect(downloadsPage->checkAllButton(), &QPushButton::clicked, this, [this]() {
        QList<QListWidgetItem*> items;
        for (int i = 0; i < ui->modelList->count(); ++i) {
            QListWidgetItem *item = ui->modelList->item(i);
            if (isModelListItem(item)) items << item;
        }
        checkUpdatesForItems(items);
    });
    connect(downloadsPage->downloadSelectedButton(), &QPushButton::clicked,
            downloadManager, &DownloadManager::startSelectedDownloads);
    connect(downloadsPage->ignoreSelectedButton(), &QPushButton::clicked,
            downloadManager, &DownloadManager::ignoreSelectedUpdates);
    connect(downloadsPage->retryButton(), &QPushButton::clicked, this, [this]() {
        QList<QListWidgetItem*> failedCheckItems;
        for (const QString &filePath : downloadsPage->failedUpdateCheckFilePaths()) {
            if (QListWidgetItem *item = findModelItemByFilePath(filePath)) failedCheckItems << item;
        }
        if (!failedCheckItems.isEmpty()) {
            checkUpdatesForItems(failedCheckItems);
            return;
        }
        if (downloadManager) downloadManager->retryFailedDownloads();
    });
    connect(downloadsPage->openFolderButton(), &QPushButton::clicked, this, [this]() {
        const QStringList filePaths = downloadsPage->selectedFilePaths();
        if (filePaths.isEmpty()) return;
        const QString path = downloadsPage->cardTargetPath(filePaths.first());
        showFileInFolder(path);
    });
    connect(downloadsPage->clearCompletedButton(), &QPushButton::clicked,
            downloadManager, &DownloadManager::clearCompleted);
    connect(downloadsPage, &DownloadsPage::sourceRequested,
            this, &MainWindow::jumpToDownloadSource);
    connect(downloadsPage, &DownloadsPage::civitaiRequested,
            this, &MainWindow::openDownloadCivitaiPage);
    connect(downloadsPage, &DownloadsPage::downloadRequested, this, [this](const QString &filePath) {
        if (!downloadManager || !downloadManager->containsInfo(filePath)) {
            downloadsPage->setStatusText(AppText::tr("该模型还没有可下载的更新信息。"));
            return;
        }
        const ModelUpdateInfo info = downloadManager->info(filePath);
        const QString status = downloadsPage->cardStatusText(filePath);
        const auto action = DownloadStatus::cardAction(status, info.hasUpdate);
        if (action == DownloadStatus::CardAction::Disabled) return;
        if (action == DownloadStatus::CardAction::Check) {
            if (QListWidgetItem *item = findModelItemByFilePath(filePath)) {
                QList<QListWidgetItem*> items;
                items << item;
                checkUpdatesForItems(items);
            } else {
                downloadsPage->setStatusText(AppText::tr("模型已不在当前列表中，无法重新检测。"));
            }
            return;
        }
        if (downloadManager) downloadManager->enqueueModelDownload(info);
    });
    connect(downloadsPage, &DownloadsPage::ignoreToggled,
            downloadManager, &DownloadManager::toggleIgnore);
    connect(downloadsPage, &DownloadsPage::metadataScanRequested,
            this, &MainWindow::startMetadataScan);
    connect(downloadsPage, &DownloadsPage::metadataUpdateRequested,
            this, [this](const QStringList &paths) { startMetadataSyncForPaths(paths, true); });
    connect(downloadsPage, &DownloadsPage::metadataCivArchiveRequested,
            this, [this](const QStringList &paths) { startMetadataSyncForPaths(paths, true, true); });
    connect(downloadsPage, &DownloadsPage::metadataOpenModelRequested,
            this, &MainWindow::jumpToDownloadSource);
    connect(downloadsPage, &DownloadsPage::metadataOpenFolderRequested,
            this, &MainWindow::showFileInFolder);
    connect(downloadsPage, &DownloadsPage::healthCheckRequested,
            this, &MainWindow::runMetadataHealthCheck);
    connect(downloadsPage, &DownloadsPage::healthOpenModelRequested,
            this, &MainWindow::jumpToDownloadSource);
    connect(downloadsPage, &DownloadsPage::healthOpenFolderRequested,
            this, &MainWindow::showFileInFolder);
    downloadsPage->updateSelectionSummary();
}

void MainWindow::checkUpdatesForItems(const QList<QListWidgetItem*> &items, bool switchToDownloads, bool detailPrompt)
{
    if (downloadManager && !downloadManager->cacheLoaded() && !items.isEmpty()) {
        QStringList paths;
        for (QListWidgetItem *item : items) {
            if (isModelListItem(item)) paths.append(item->data(ROLE_FILE_PATH).toString());
        }
        disconnect(pendingDownloadChecksAfterRestore);
        pendingDownloadChecksAfterRestore = connect(downloadManager, &DownloadManager::cacheReady,
            this, [this, paths, switchToDownloads, detailPrompt]() {
                QList<QListWidgetItem*> restoredItems;
                for (const QString &path : paths) {
                    if (QListWidgetItem *item = findModelItemByFilePath(path)) restoredItems.append(item);
                }
                checkUpdatesForItems(restoredItems, switchToDownloads, detailPrompt);
            }, Qt::SingleShotConnection);
        if (switchToDownloads) ui->rootStack->setCurrentWidget(downloadsPage);
        downloadManager->ensureCacheLoaded();
        return;
    }
    if (items.isEmpty()) {
        if (downloadsPage) downloadsPage->setStatusText(AppText::tr("没有可检查的模型。请先选择模型，或使用检查全部。"));
        if (switchToDownloads) onMenuSwitchToDownloads();
        return;
    }

    if (switchToDownloads) onMenuSwitchToDownloads();
    QList<UpdateCheckSnapshot> snapshots;
    snapshots.reserve(items.size());
    for (QListWidgetItem *item : items) {
        if (!isModelListItem(item)) continue;
        snapshots.append(snapshotForModelItem(item));
    }
    if (modelUpdateCoordinator) {
        modelUpdateCoordinator->startChecks(snapshots, detailPrompt);
    }
}

void MainWindow::handleModelUpdateRootReceived(const QString &filePath,
                                               int matchedVersionId,
                                               const QJsonObject &modelRoot,
                                               const ModelUpdateInfo &fallbackInfo)
{
    QListWidgetItem *sourceItem = findModelItemByFilePath(filePath);
    if (!sourceItem) {
        ModelUpdateInfo fallback = fallbackInfo;
        fallback.modelId = modelRoot.value("id").toInt();
        addOrUpdateDownloadCard(fallback, "模型列表项不存在，无法判断");
        return;
    }

    sourceItem->setData(ROLE_CIVITAI_MODEL_ID, modelRoot.value("id").toInt());
    if (sourceItem->data(ROLE_CIVITAI_VERSION_ID).toInt() <= 0 && matchedVersionId > 0) {
        sourceItem->setData(ROLE_CIVITAI_VERSION_ID, matchedVersionId);
    }
    sourceItem->setData(ROLE_MODEL_TYPE, modelRoot.value("type").toString());
    applyCivitaiAttributionToItem(sourceItem,
                                  ModelMetadata::jsonModelCreator(modelRoot),
                                  ModelMetadata::jsonModelTags(modelRoot));
    const ModelUpdateInfo info = parseModelUpdateInfo(sourceItem, modelRoot);
    if (downloadManager) downloadManager->setInfo(info);
    const QString status = info.latestFileExistsLocally
        ? QStringLiteral("旧版共存：本地已存在新版本")
        : (info.hasUpdate ? QStringLiteral("发现新版本") : QStringLiteral("已是最新"));
    addOrUpdateDownloadCard(info, status);
}

void MainWindow::handleAllModelUpdateChecksCompleted(int, bool detailPrompt, const QString &detailFilePath)
{
    if (downloadManager) downloadManager->saveCache();
    if (!detailPrompt) return;

    const QString status = downloadsPage ? downloadsPage->cardStatusText(detailFilePath) : QString();
    const bool hasUpdate = downloadManager && downloadManager->containsInfo(detailFilePath)
        && downloadManager->info(detailFilePath).hasUpdate;
    const bool coexisting = status.contains("旧版共存");
    if (hasUpdate || coexisting) {
        const QMessageBox::StandardButton choice = QMessageBox::question(
            this,
            AppText::tr("检测到模型更新"),
            AppText::tr("检测到该模型有可用更新。\n\n状态: %1\n\n是否跳转到下载任务列表？")
                .arg(status.isEmpty() ? "发现新版本" : status),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::Yes);
        if (choice == QMessageBox::Yes) {
            onMenuSwitchToDownloads();
            if (downloadsPage) downloadsPage->setCardSelected(detailFilePath, true);
            downloadsPage->updateSelectionSummary();
        }
    } else {
        QMessageBox::information(
            this,
            AppText::tr("检查更新"),
            status.isEmpty() ? AppText::tr("当前模型没有检测到可用更新。") : AppText::tr("当前模型状态: %1").arg(status));
    }
}

ModelUpdateInfo MainWindow::parseModelUpdateInfo(QListWidgetItem *item, const QJsonObject &modelRoot) const
{
    const UpdateCheckSnapshot snapshot = snapshotForModelItem(item);
    return ModelUpdateCoordinator::parseModelUpdateInfo(
        snapshot,
        modelRoot,
        [this, item](int modelId, int versionId) {
            for (int i = 0; i < ui->modelList->count(); ++i) {
                QListWidgetItem *localItem = ui->modelList->item(i);
                if (!isModelListItem(localItem) || localItem == item) continue;
                if (localItem->data(ROLE_CIVITAI_MODEL_ID).toInt() != modelId) continue;
                if (localItem->data(ROLE_CIVITAI_VERSION_ID).toInt() != versionId) continue;
                if (QFileInfo::exists(localItem->data(ROLE_FILE_PATH).toString())) {
                    return true;
                }
            }
            return false;
        });
}

void MainWindow::addOrUpdateDownloadCard(const ModelUpdateInfo &info, const QString &status)
{
    if (info.filePath.isEmpty()) return;
    if (downloadManager) {
        downloadManager->addOrUpdateCard(info,
                                         status,
                                         findModelItemByFilePath(info.filePath) != nullptr || QFile::exists(info.filePath));
    }
}

QListWidgetItem *MainWindow::findModelItemByFilePath(const QString &filePath) const
{
    if (QListWidgetItem *direct = modelItemByPath.value(filePath, nullptr)) {
        return direct;
    }
    const QString normalized = QFileInfo(filePath).absoluteFilePath();
    if (QListWidgetItem *directNorm = modelItemByPath.value(normalized, nullptr)) {
        return directNorm;
    }
    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!isModelListItem(item)) continue;
        if (QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath() == normalized) {
            return item;
        }
    }
    return nullptr;
}

void MainWindow::jumpToDownloadSource(const QString &filePath)
{
    QListWidgetItem *item = findModelItemByFilePath(filePath);
    if (!item) {
        downloadsPage->setStatusText(AppText::tr("模型已不在当前列表中，无法跳转。"));
        return;
    }
    onMenuSwitchToLibrary();
    ui->modelList->setCurrentItem(item);
    item->setSelected(true);
    ui->modelList->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    onModelListClicked(item);
}

void MainWindow::openDownloadCivitaiPage(const QString &filePath)
{
    const QString browserHost = settingsPage ? settingsPage->state().civitaiBrowserHost : QStringLiteral("civitai.red");
    ModelUpdateInfo info = downloadManager ? downloadManager->info(filePath) : ModelUpdateInfo{};
    int modelId = info.modelId;
    QString sourceUrl = info.sourceUrl.trimmed();
    QString sha256 = info.sha256.trimmed();
    QString modelDir = info.modelDir;
    QString baseName = info.baseName;
    QJsonObject metadataRoot;

    if (QListWidgetItem *item = findModelItemByFilePath(filePath)) {
        if (modelId <= 0) modelId = item->data(ROLE_CIVITAI_MODEL_ID).toInt();
        if (sha256.isEmpty()) sha256 = item->data(ROLE_CIVITAI_SHA256).toString().trimmed();
        if (modelDir.isEmpty()) modelDir = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absolutePath();
        if (baseName.isEmpty()) baseName = item->data(ROLE_MODEL_NAME).toString();
    }

    if (!modelDir.isEmpty() && !baseName.isEmpty()) {
        QFile file(QDir(modelDir).filePath(baseName + ".json"));
        if (file.open(QIODevice::ReadOnly)) {
            metadataRoot = QJsonDocument::fromJson(file.readAll()).object();
            if (sourceUrl.isEmpty()) sourceUrl = ModelMetadata::metadataBrowserUrlFromRoot(metadataRoot);
            if (sha256.isEmpty()) sha256 = ModelMetadata::metadataShaFromRoot(metadataRoot);
            if (modelId <= 0) modelId = metadataRoot.value("model").toObject().value("id").toInt(metadataRoot.value("modelId").toInt());
        }
    }

    if (sourceUrl.contains("civarchive.com", Qt::CaseInsensitive)) {
        QDesktopServices::openUrl(QUrl(sourceUrl));
        return;
    }
    if (sourceUrl.isEmpty()
        && info.metadataSource.compare("civarchive", Qt::CaseInsensitive) == 0
        && !sha256.isEmpty()) {
        QString normalized = sha256;
        normalized.remove(QRegularExpression("[^A-Fa-f0-9]"));
        if (!normalized.isEmpty()) {
            QDesktopServices::openUrl(QUrl(QString("https://civarchive.com/sha256/%1").arg(normalized.toLower())));
            return;
        }
    }
    if (!sourceUrl.isEmpty() && (modelId <= 0 || ModelBrowser::isCivitaiModelPage(QUrl(sourceUrl)))) {
        QDesktopServices::openUrl(ModelBrowser::preferredUrl(QUrl(sourceUrl), browserHost));
        return;
    }

    if (modelId <= 0) {
        downloadsPage->setStatusText(AppText::tr("该任务没有可打开的 Civitai 模型 ID。"));
        return;
    }
    QDesktopServices::openUrl(ModelBrowser::preferredUrl(QUrl(QString("https://civitai.com/models/%1").arg(modelId)), browserHost));
}

void MainWindow::showFileInFolder(const QString &filePath)
{
    FileUtils::showFileInFolder(filePath, this);
}

QString MainWindow::resolveDownloadPreviewPath(const ModelUpdateInfo &info) const
{
    if (QListWidgetItem *item = findModelItemByFilePath(info.filePath)) {
        const QString itemPreview = item->data(ROLE_PREVIEW_PATH).toString();
        if (!itemPreview.isEmpty() && QFile::exists(itemPreview)) return itemPreview;
    }
    const QString fallback = ModelMetadata::previewPath(info.modelDir, info.baseName, 0);
    if (!fallback.isEmpty() && QFile::exists(fallback)) return fallback;
    return QString();
}

UpdateCheckSnapshot MainWindow::snapshotForModelItem(QListWidgetItem *item) const
{
    UpdateCheckSnapshot snapshot;
    if (!isModelListItem(item)) return snapshot;
    snapshot.filePath = QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath();
    snapshot.baseName = item->data(ROLE_MODEL_NAME).toString();
    snapshot.modelDir = QFileInfo(snapshot.filePath).absolutePath();
    snapshot.modelId = item->data(ROLE_CIVITAI_MODEL_ID).toInt();
    snapshot.displayName = item->text();
    snapshot.currentVersionId = item->data(ROLE_CIVITAI_VERSION_ID).toInt();
    snapshot.currentSha256 = item->data(ROLE_CIVITAI_SHA256).toString();
    snapshot.localEdited = item->data(ROLE_LOCAL_EDITED).toBool();
    snapshot.previewState = static_cast<ModelPreviewState>(
        item->data(ROLE_MODEL_PREVIEW_STATE).toInt());
    return snapshot;
}

QVector<MetadataScanItem> MainWindow::collectMetadataScanSeeds() const
{
    QVector<MetadataScanItem> items;
    items.reserve(ui->modelList->count());
    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *modelItem = ui->modelList->item(i);
        if (!isModelListItem(modelItem)) continue;
        const UpdateCheckSnapshot snapshot = snapshotForModelItem(modelItem);
        if (snapshot.filePath.isEmpty()) continue;

        MetadataScanItem item;
        item.filePath = snapshot.filePath;
        item.displayName = snapshot.displayName;
        item.jsonPath = QDir(snapshot.modelDir).filePath(snapshot.baseName + ".json");
        item.previewPath = modelItem->data(ROLE_PREVIEW_PATH).toString();
        item.modelIdText = snapshot.modelId > 0 ? QString::number(snapshot.modelId) : QString();
        item.versionIdText = snapshot.currentVersionId > 0 ? QString::number(snapshot.currentVersionId) : QString();
        item.sha256 = snapshot.currentSha256;
        item.localEdited = snapshot.localEdited;
        item.syncFailure = modelSyncFailureMessage(snapshot.filePath);
        items.append(item);
    }
    return items;
}

void MainWindow::startMetadataScan()
{
    if (!downloadsPage) return;
    if (!metadataScanWatcher) metadataScanWatcher = new QFutureWatcher<QVector<MetadataScanItem>>(this);
    if (metadataScanWatcher->isRunning()) return;

    const QVector<MetadataScanItem> seeds = collectMetadataScanSeeds();
    downloadsPage->setMetadataScanRunning(true);
    disconnect(metadataScanWatcher, nullptr, this, nullptr);
    connect(metadataScanWatcher, &QFutureWatcherBase::finished, this, [this]() {
        const QVector<MetadataScanItem> items = metadataScanWatcher->result();
        downloadsPage->setMetadataScanItems(items);
        downloadsPage->setMetadataScanRunning(false);
        downloadsPage->setStatusText(AppText::tr("元信息扫描完成，共 %1 个模型。").arg(items.size()));
    });
    metadataScanWatcher->setFuture(QtConcurrent::run(backgroundThreadPool, [seeds]() {
        return MetadataInspection::scan(seeds);
    }));
}

void MainWindow::runMetadataHealthCheck()
{
    if (!downloadsPage) return;
    if (!metadataHealthWatcher) metadataHealthWatcher = new QFutureWatcher<QVector<MetadataHealthIssue>>(this);
    if (metadataHealthWatcher->isRunning()) return;

    const QVector<MetadataScanItem> seeds = collectMetadataScanSeeds();
    downloadsPage->setHealthCheckRunning(true);
    disconnect(metadataHealthWatcher, nullptr, this, nullptr);
    connect(metadataHealthWatcher, &QFutureWatcherBase::finished, this, [this]() {
        const QVector<MetadataHealthIssue> issues = metadataHealthWatcher->result();
        downloadsPage->setHealthIssues(issues);
        downloadsPage->setHealthCheckRunning(false);
        downloadsPage->setStatusText(AppText::tr("元数据健康检查完成，共 %1 条问题/提示。").arg(issues.size()));
    });
    metadataHealthWatcher->setFuture(QtConcurrent::run(backgroundThreadPool, [seeds]() {
        return MetadataInspection::healthCheck(seeds);
    }));
}

void MainWindow::startMetadataSyncForPaths(const QStringList &filePaths, bool updateExisting, bool archiveOnly)
{
    if (!downloadsPage || filePaths.isEmpty()) {
        if (downloadsPage) downloadsPage->setStatusText(AppText::tr("请先勾选要处理的模型。"));
        return;
    }
    if (metadataSyncService && metadataSyncService->isRunning()) {
        downloadsPage->setStatusText(AppText::tr("已有元信息同步任务正在运行。"));
        return;
    }

    const QString source = archiveOnly ? QStringLiteral("CivArchive") : QStringLiteral("Civitai");
    bool hasLocalEdited = false;
    for (const QString &path : filePaths) {
        if (QListWidgetItem *item = findModelItemByFilePath(path)) {
            if (item->data(ROLE_LOCAL_EDITED).toBool()) {
                hasLocalEdited = true;
                break;
            }
        }
    }
    if (hasLocalEdited) {
        const auto ret = QMessageBox::warning(this,
                                              AppText::tr("覆盖本地元信息"),
                                              AppText::tr("选中的模型包含本地/已编辑 metadata。\n继续从 %1 同步会覆盖元信息，但会保留本地保护字段和用户私有数据。\n是否继续？").arg(source),
                                              QMessageBox::Yes | QMessageBox::Cancel,
                                              QMessageBox::Cancel);
        if (ret != QMessageBox::Yes) return;
    }

    QMessageBox previewMsg(this);
    previewMsg.setWindowTitle(AppText::tr("同步预览图"));
    previewMsg.setText(AppText::tr("本次元信息同步是否同时同步 %1 预览图？\n\n"
                               "选择“是”会同步来源中可用的预览图及图片元信息。\n"
                               "选择“仅元数据”只更新 JSON，不下载或覆盖图片。").arg(source));
    QPushButton *btnYes = previewMsg.addButton(AppText::tr("是"), QMessageBox::AcceptRole);
    QPushButton *btnMetaOnly = previewMsg.addButton(AppText::tr("仅元数据"), QMessageBox::ActionRole);
    previewMsg.addButton(AppText::tr("取消同步"), QMessageBox::RejectRole);
    previewMsg.setDefaultButton(btnMetaOnly);
    previewMsg.exec();
    if (previewMsg.clickedButton() != btnYes && previewMsg.clickedButton() != btnMetaOnly) {
        downloadsPage->setStatusText(AppText::tr("已取消元信息同步。"));
        return;
    }
    const bool syncPreviewImages = (previewMsg.clickedButton() == btnYes);

    QQueue<MetadataSyncJob> jobs;
    for (const QString &path : filePaths) {
        QListWidgetItem *item = findModelItemByFilePath(path);
        if (!item) continue;
        MetadataSyncJob job;
        job.snapshot = snapshotForModelItem(item);
        job.updateExisting = updateExisting;
        job.civArchiveOnly = archiveOnly;
        if (!job.snapshot.filePath.isEmpty()) jobs.enqueue(job);
    }
    if (metadataSyncService) {
        metadataSyncService->startBatch(jobs, syncPreviewImages, source);
    }
}

bool MainWindow::startDetailCivArchiveFallback(const QString &filePath, const QString &baseName, const QString &modelDir, const QString &reason, const QString &currentSha256)
{
    if (!optTryCivArchiveOnMetadataFail || filePath.isEmpty() || !metadataSyncService) return false;
    MetadataSyncJob job;
    if (QListWidgetItem *item = findModelItemByFilePath(filePath)) {
        job.snapshot = snapshotForModelItem(item);
    }
    job.snapshot.filePath = filePath;
    job.snapshot.baseName = baseName;
    job.snapshot.modelDir = modelDir.isEmpty() ? QFileInfo(filePath).absolutePath() : modelDir;
    if (!currentSha256.trimmed().isEmpty()) job.snapshot.currentSha256 = currentSha256.trimmed();
    if (job.snapshot.displayName.isEmpty()) job.snapshot.displayName = QFileInfo(filePath).completeBaseName();
    job.updateExisting = true;
    job.detailFallback = true;
    metadataSyncService->startDetailCivArchiveFallback(job, reason, !m_skipPreviewSync);
    return true;
}

void MainWindow::handleMetadataSyncJobFailed(const MetadataSyncJob &job, const QString &message, const QString &)
{
    recordModelSyncFailure(job.snapshot.filePath, job.snapshot.baseName, message);
    if (job.detailFallback) {
        setModelTitleError(message);
        ui->textDescription->setPlainText(QString("上次同步失败，请点击刷新模型详情重新同步。\n%1").arg(message));
        transitionToImage("");
        ui->statusbar->showMessage(AppText::tr("元数据获取失败: ") + message, 4000);
        m_forceResyncPreview = false;
        m_skipPreviewSync = false;
    }
}

bool MainWindow::saveMetadataFromModelRoot(const MetadataSyncJob &job,
                                           const QJsonObject &modelRoot,
                                           const QJsonObject &versionHint,
                                           bool syncPreviewImages,
                                           bool batchRunning)
{
    if (modelRoot.isEmpty()) return false;
    QJsonObject selectedVersion = versionHint;
    const QJsonArray versions = modelRoot.value("modelVersions").toArray();
    if (selectedVersion.isEmpty()) {
        for (const QJsonValue &val : versions) {
            const QJsonObject version = val.toObject();
            bool match = false;
            if (job.snapshot.currentVersionId > 0 && version.value("id").toInt() == job.snapshot.currentVersionId) {
                match = true;
            }
            if (!match && !job.snapshot.currentSha256.isEmpty()) {
                for (const QJsonValue &fileVal : version.value("files").toArray()) {
                    const QString sha = fileVal.toObject().value("hashes").toObject().value("SHA256").toString();
                    if (!sha.isEmpty() && sha.compare(job.snapshot.currentSha256, Qt::CaseInsensitive) == 0) {
                        match = true;
                        break;
                    }
                }
            }
            if (match) {
                selectedVersion = version;
                break;
            }
        }
    }
    if (selectedVersion.isEmpty() && !job.updateExisting && !versions.isEmpty()) selectedVersion = versions.first().toObject();
    if (selectedVersion.isEmpty()) return false;

    QJsonObject root = ModelMetadata::mergeCivitaiModelIntoVersion(selectedVersion, modelRoot);
    root["syncedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    if (modelRoot.value("metadataSource").toString() == "civarchive") {
        root["metadataSource"] = QStringLiteral("civarchive");
        if (modelRoot.contains("sourceUrl")) root["sourceUrl"] = modelRoot.value("sourceUrl");
    } else if (!root.contains("metadataSource")) {
        root["metadataSource"] = QStringLiteral("civitai");
    }
    QFile existing(QDir(job.snapshot.modelDir).filePath(job.snapshot.baseName + ".json"));
    if (existing.exists() && existing.open(QIODevice::ReadOnly)) {
        const QJsonObject oldRoot = QJsonDocument::fromJson(existing.readAll()).object();
        const QStringList localKeys = {"localEdited", "localEditedAt", "localOnly", "modelUrl"};
        for (const QString &key : localKeys) {
            if (oldRoot.contains(key)) root[key] = oldRoot.value(key);
        }
    } else {
        root["localOnly"] = false;
    }
    saveLocalMetadata(job.snapshot.modelDir, job.snapshot.baseName, root);
    if (syncPreviewImages) {
        syncPreviewImagesFromMetadata(job.snapshot.modelDir,
                                      job.snapshot.baseName,
                                      ModelMetadata::imageInfosFromVersionJson(root),
                                      true,
                                      true);
    }

    if (QListWidgetItem *item = findModelItemByFilePath(job.snapshot.filePath)) {
        preloadItemMetadata(item, QDir(job.snapshot.modelDir).filePath(job.snapshot.baseName + ".json"));
        if (optUseCivitaiName) {
            const QString civitaiName = item->data(ROLE_CIVITAI_NAME).toString();
            if (!civitaiName.isEmpty()) item->setText(civitaiName);
        }
        applyModelUserNoteData(item);
        applyModelHighlightColor(item);
        refreshPromptTemplateModelTriggerRows();
    }
    clearModelSyncFailure(job.snapshot.filePath);
    if (!batchRunning) {
        if (QListWidgetItem *current = ui->modelList->currentItem()) {
            if (current && QFileInfo(current->data(ROLE_FILE_PATH).toString()).absoluteFilePath() == job.snapshot.filePath) {
                ModelMeta meta;
                meta.filePath = job.snapshot.filePath;
                if (ModelMetadata::readLocalJson(job.snapshot.modelDir, job.snapshot.baseName, meta)) {
                    currentMeta = meta;
                    updateDetailView(meta);
                }
            }
        }
        refreshHomeGallery();
        refreshCollectionTreeView();
    }
    return true;
}

QString MainWindow::chooseModelDownloadTarget(const ModelUpdateInfo &info, bool *overwrite)
{
    if (overwrite) *overwrite = false;
    int policy = optModelUpdateDownloadPolicy;
    if (policy == 0) {
        QMessageBox msg(this);
        msg.setWindowTitle(AppText::tr("下载新版本"));
        msg.setText(AppText::tr("如何保存 %1 的新版本？").arg(info.displayName));
        QPushButton *keepBtn = msg.addButton(AppText::tr("保留旧版"), QMessageBox::AcceptRole);
        QPushButton *replaceBtn = msg.addButton(AppText::tr("覆盖当前文件"), QMessageBox::DestructiveRole);
        msg.addButton(AppText::tr("取消"), QMessageBox::RejectRole);
        msg.exec();
        if (msg.clickedButton() == keepBtn) policy = 1;
        else if (msg.clickedButton() == replaceBtn) policy = 2;
        else return QString();
    }
    QString fileName = info.downloadFileName;
    if (fileName.isEmpty()) fileName = QFileInfo(info.filePath).fileName();
    if (policy == 2) {
        if (overwrite) *overwrite = true;
        return QFileInfo(info.filePath).absoluteFilePath();
    }
    return FileUtils::uniqueFilePath(info.modelDir, fileName);
}

void MainWindow::finishModelDownload(const ModelFileDownloadTask &task)
{
    QString baseName = QFileInfo(task.targetPath).completeBaseName();
    QJsonObject versionJson = task.info.latestVersionJson;
    if (!versionJson.isEmpty()) {
        versionJson["syncedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        saveLocalMetadata(QFileInfo(task.targetPath).absolutePath(), baseName, versionJson);
        const QJsonArray images = versionJson["images"].toArray();
        for (const QJsonValue &val : images) {
            const QJsonObject img = val.toObject();
            const QString type = img["type"].toString();
            const QString url = img["url"].toString();
            if (url.isEmpty() || type == "video" || url.endsWith(".mp4", Qt::CaseInsensitive) || url.endsWith(".webm", Qt::CaseInsensitive)) {
                continue;
            }
            const QString previewPath = QDir(QFileInfo(task.targetPath).absolutePath()).filePath(baseName + ".preview.png");
            if (!QFile::exists(previewPath)) downloadThumbnail(url, previewPath, nullptr);
            break;
        }
    }
    if (downloadManager) downloadManager->saveCache();
    downloadsPage->setStatusText(AppText::tr("模型下载完成: ") + QFileInfo(task.targetPath).fileName());

    const QStringList activeLoraPaths = PathUtils::collectEnabledPaths(loraPaths, disabledLoraPaths);
    if (!activeLoraPaths.isEmpty()) scanModels(activeLoraPaths);
}

// === 全局配置加载与保存 (JSON) ===
void MainWindow::loadGlobalConfig() {
    QString configPath = qApp->applicationDirPath() + "/config/settings.json";
    QFile file(configPath);
    SettingsState settings;
    settings.filterTagsText = DEFAULT_FILTER_TAGS;
    settings.galleryNsfwRulesText = DEFAULT_GALLERY_NSFW_RULES;
    if (file.open(QIODevice::ReadOnly)) {
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        QJsonObject root;
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            root = doc.object();
        } else {
            qWarning() << "Invalid global settings JSON:" << configPath << parseError.errorString();
        }

        settings = SettingsState::fromJson(root, DEFAULT_FILTER_TAGS, DEFAULT_GALLERY_NSFW_RULES);
        optUiScale = settings.uiScale;
        optFilterNSFW = settings.filterNSFW;
        optNSFWMode = settings.nsfwMode;
        optNSFWLevel = settings.nsfwLevel;
        optFilterGalleryNSFW = settings.filterGalleryNSFW;
        optGalleryNSFWMode = settings.galleryNsfwMode;
        optGalleryNSFWRules = settings.galleryNsfwRules();
        optLoraRecursive = settings.loraRecursive;
        optGalleryRecursive = settings.galleryRecursive;
        optBlurRadius = settings.blurRadius;
        optDownscaleBlur = settings.downscaleBlur;
        optBlurProcessWidth = settings.blurProcessWidth;
        optRenderThreadCount = settings.renderThreadCount;
        optRestoreTreeState = settings.restoreTreeState;
        optRestoreFilterSortState = settings.restoreFilterSortState;
        optSplitOnNewline = settings.splitOnNewline;
        optFilterTags = settings.filterTags();
        optShowEmptyCollections = settings.showEmptyCollections;
        optCollectionFolderTopLevel = settings.collectionFolderTopLevel;
        optCollectionFolderSecondLevel = settings.collectionFolderSecondLevel;
        optModelListFolderGrouping = settings.modelListFolderGrouping;
        optUseArrangedUA = settings.useCustomUserAgent;
        optSavedUAString = settings.customUserAgent;
        optCivitaiApiKey = settings.civitaiApiKey;
        optUseCivitaiName = settings.useCivitaiName;
        optSuppressLocalWarnings = settings.suppressLocalWarnings;
        optUserGalleryMatchMode = settings.userGalleryMatchMode;
        optComfyModelNameFallback = settings.comfyModelNameFallback;
        optRecalculateKnownMetadataHash = settings.recalculateKnownMetadataHash;
        optTryCivArchiveOnMetadataFail = settings.tryCivArchiveOnMetadataFail;
        optConfirmCloseWhileLauncherRunning = settings.confirmCloseWhileLauncherRunning;
        optModelUpdateDownloadPolicy = settings.modelUpdateDownloadPolicy;
        optAutoCheckUpdatesOnStartup = settings.autoCheckUpdatesOnStartup;
        optThemeId = settings.themeId;
        optCustomThemePath = settings.customThemePath;

        qDebug() << "Loaded User-Agent:" << currentUserAgent;

        auto readPathList = [](const QJsonObject &obj, const QString &arrayKey, const QStringList &fallbackKeys) {
            QStringList out;
            if (obj.contains(arrayKey) && obj[arrayKey].isArray()) {
                QJsonArray arr = obj[arrayKey].toArray();
                for (const auto &val : arr) {
                    QString path = val.toString().trimmed();
                    if (!path.isEmpty()) out.append(path);
                }
                return out;
            }
            for (const QString &key : fallbackKeys) {
                QString val = obj.value(key).toString().trimmed();
                if (!val.isEmpty()) {
                    out.append(val);
                    break;
                }
            }
            return out;
        };
        auto readPathSet = [](const QJsonObject &obj, const QString &arrayKey) {
            QSet<QString> out;
            if (!obj.contains(arrayKey) || !obj[arrayKey].isArray()) return out;
            const QJsonArray arr = obj[arrayKey].toArray();
            for (const auto &val : arr) {
                const QString path = val.toString().trimmed();
                if (!path.isEmpty()) out.insert(path);
            }
            return out;
        };

        loraPaths = PathUtils::normalizePathList(readPathList(root, "lora_paths", {"lora_path"}));
        galleryPaths = PathUtils::normalizePathList(readPathList(root, "gallery_paths", {"gallery_path", "sd_folder"}));
        translationCsvPaths = PathUtils::normalizePathList(readPathList(root, "translation_paths", {"translation_path"}));
        disabledLoraPaths = PathUtils::normalizePathSet(readPathSet(root, "lora_paths_disabled"));
        disabledGalleryPaths = PathUtils::normalizePathSet(readPathSet(root, "gallery_paths_disabled"));
        disabledTranslationCsvPaths = PathUtils::normalizePathSet(readPathSet(root, "translation_paths_disabled"));
        collapsedModelFolders = PathUtils::normalizePathSet(readPathSet(root, "model_list_collapsed_folders"));
        {
            QSet<QString> filtered;
            for (const QString &path : loraPaths) {
                if (disabledLoraPaths.contains(path)) filtered.insert(path);
            }
            disabledLoraPaths = filtered;
        }
        {
            QSet<QString> enabledLoraPathSet;
            for (const QString &path : loraPaths) {
                if (!disabledLoraPaths.contains(path)) enabledLoraPathSet.insert(path);
            }
            QSet<QString> filtered;
            for (const QString &path : collapsedModelFolders) {
                if (enabledLoraPathSet.contains(path)) filtered.insert(path);
            }
            collapsedModelFolders = filtered;
        }
        {
            QSet<QString> filtered;
            for (const QString &path : galleryPaths) {
                if (disabledGalleryPaths.contains(path)) filtered.insert(path);
            }
            disabledGalleryPaths = filtered;
        }
        {
            QSet<QString> filtered;
            for (const QString &path : translationCsvPaths) {
                if (disabledTranslationCsvPaths.contains(path)) filtered.insert(path);
            }
            disabledTranslationCsvPaths = filtered;
        }
        translationCsvPath = PathUtils::collectEnabledPaths(translationCsvPaths, disabledTranslationCsvPaths).value(0);

        // 读取树状菜单状态
        if (optRestoreTreeState && root.contains("tree_state")) {
            QJsonObject treeState = root["tree_state"].toObject();
            startupTreeScrollPos = treeState["scroll_pos"].toInt(0);
            QJsonArray arr = treeState["expanded_items"].toArray();
            for (const auto &val : arr) {
                startupExpandedCollections.insert(val.toString());
            }
        }

        // 读取上次退出时的排序与筛选状态
        if (optRestoreFilterSortState && root.contains("filter_sort_state")) {
            const QJsonObject fs = root["filter_sort_state"].toObject();
            startupSortIndex = fs.value("sort_index").toInt(0);
            startupFilterBaseModel = fs.value("base_model").toString(QStringLiteral("All"));
            startupFilterModelType = fs.value("model_type").toString(QStringLiteral("All"));
            startupSearchText = fs.value("search_text").toString();
            startupSidebarTab = fs.value("sidebar_tab").toInt(1);
            startupCollectionFilter = fs.value("collection_filter").toString();
            startupHomeAuthorFilter = fs.value("home_author_filter").toString();
            startupHomeFilterExpanded = fs.value("home_filter_expanded").toBool(false);
            startupHomeFilterTagsSortByCount = fs.value("home_tag_sort_by_count").toBool(true);
            startupHomeTagFilters.clear();
            const QJsonArray tagArr = fs.value("home_tag_filters").toArray();
            for (const QJsonValue &v : tagArr) {
                const QString clean = v.toString().trimmed();
                if (!clean.isEmpty()) startupHomeTagFilters.insert(clean);
            }
            startupUserTagSortMode = fs.value("user_tag_sort_mode").toInt(0);
            startupUserTagLoraOnly = fs.value("user_tag_lora_only").toBool(false);
            startupUserTagIncludeNegative = fs.value("user_tag_include_negative").toBool(false);
            startupGalleryPromptSortMode = fs.value("gallery_prompt_sort_mode").toInt(0);
            startupGalleryTagViewActive = fs.value("gallery_tag_view_active").toBool(false);
        }

        // 设置UA
        if (optUseArrangedUA && !optSavedUAString.isEmpty())currentUserAgent = optSavedUAString;
        else currentUserAgent = getRandomUserAgent();
    }

    applyPathListsToUi();

    settingsPage->setState(settings);
    settingsPage->setCivitaiApiStatus(optCivitaiApiKey.isEmpty() ? AppText::tr("API Key 未配置") : AppText::tr("API Key 未测试"));
    applyApplicationTheme(optThemeId, optCustomThemePath, true);
    initSettingsPage();
}

void MainWindow::initSettingsPage()
{
    if (!settingsPage || settingsPageConnectionsInitialized) return;
    settingsPageConnectionsInitialized = true;
    connect(settingsPage, &SettingsPage::loraPathsEditRequested, this, &MainWindow::onBrowseLoraPath);
    connect(settingsPage, &SettingsPage::galleryPathsEditRequested, this, &MainWindow::onBrowseGalleryPath);
    connect(settingsPage, &SettingsPage::translationPathsEditRequested, this, &MainWindow::onBrowseTranslationPath);
    connect(settingsPage, &SettingsPage::downloadAiTagTranslateRequested,
            this, &MainWindow::downloadAiTagTranslateDictionary);
    connect(settingsPage, &SettingsPage::clearGalleryCacheRequested, this, &MainWindow::onClearUserGalleryCacheClicked);
    connect(settingsPage, &SettingsPage::testCivitaiApiKeyRequested, this, [this](const QString &key) {
        optCivitaiApiKey = key.trimmed();
        saveGlobalConfig();
        onTestCivitaiApiKeyClicked();
    });
    connect(settingsPage, &SettingsPage::blurChanged, this, [this](int value, bool finalSave) {
        optBlurRadius = value;
        if (settingsPage) settingsPage->setBlurValue(value);
        updateBackgroundImage();
        if (finalSave) saveGlobalConfig();
    });
    connect(settingsPage, &SettingsPage::resetFilterTagsRequested, this, &MainWindow::resetFilterTagsToDefault);
    connect(settingsPage, &SettingsPage::resetGalleryNsfwRulesRequested, this, &MainWindow::resetGalleryNsfwRulesToDefault);
    connect(settingsPage, &SettingsPage::randomUserAgentRequested, this, &MainWindow::applyRandomUserAgent);
    connect(settingsPage, &SettingsPage::stateChanged, this, &MainWindow::applySettingsState);
    connect(settingsPage, &SettingsPage::editThemeRequested, this, &MainWindow::onEditThemeRequested);
    connect(settingsPage, &SettingsPage::deleteThemeRequested, this, &MainWindow::onThemeDeleteRequested);
}

void MainWindow::downloadAiTagTranslateDictionary()
{
    const QString directory = QDir(qApp->applicationDirPath()).filePath("config/translations");
    const QString targetPath = QFileInfo(QDir(directory).filePath("AITagTranslate_autocomplete.txt")).absoluteFilePath();
    if (QFileInfo::exists(targetPath)) {
        const auto answer = QMessageBox::question(
            this, AppText::tr("更新 AITagTranslate 词表"),
            AppText::tr("本地已经存在 AITagTranslate 词表。是否下载最新版本并替换？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
    }

    if (!QDir().mkpath(directory)) {
        QMessageBox::warning(this, AppText::tr("下载失败"), AppText::tr("无法创建词表目录：\n") + directory);
        return;
    }

    if (settingsPage) settingsPage->setAiTagTranslateDownloadState(true, AppText::tr("正在下载 AITagTranslate 词表..."));
    QNetworkReply *reply = netManager->get(makeNetworkRequest(
        QUrl("https://raw.githubusercontent.com/hanbinhsh/AITagTranslate/main/autocomplete.txt"), false));
    connect(reply, &QNetworkReply::finished, this, [this, reply, targetPath]() {
        const QByteArray data = reply->readAll();
        const QString errorText = reply->errorString();
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();

        if (!networkOk) {
            if (settingsPage) settingsPage->setAiTagTranslateDownloadState(false, AppText::tr("下载失败：") + errorText);
            QMessageBox::warning(this, AppText::tr("下载失败"), AppText::tr("无法下载 AITagTranslate 词表：\n") + errorText);
            return;
        }

        if (data.trimmed().startsWith('<')) {
            if (settingsPage) settingsPage->setAiTagTranslateDownloadState(false, AppText::tr("下载内容不是有效的 Tag 词表。"));
            QMessageBox::warning(this, AppText::tr("下载失败"), AppText::tr("GitHub 返回的内容不是有效的 AITagTranslate 词表，旧文件未被修改。"));
            return;
        }

        if (settingsPage) settingsPage->setAiTagTranslateDownloadState(true, AppText::tr("下载完成，正在后台验证词表..."));
        auto *watcher = new QFutureWatcher<int>(this);
        connect(watcher, &QFutureWatcher<int>::finished, this, [this, watcher, data, targetPath]() {
            const int entryCount = watcher->result();
            watcher->deleteLater();
            if (entryCount < 10) {
                if (settingsPage) settingsPage->setAiTagTranslateDownloadState(false, AppText::tr("下载内容不是有效的 Tag 词表。"));
                QMessageBox::warning(this, AppText::tr("下载失败"), AppText::tr("GitHub 返回的内容不是有效的 AITagTranslate 词表，旧文件未被修改。"));
                return;
            }

            QSaveFile output(targetPath);
            if (!output.open(QIODevice::WriteOnly)
                || output.write(data) != data.size()
                || !output.commit()) {
                const QString message = output.errorString();
                if (settingsPage) settingsPage->setAiTagTranslateDownloadState(false, AppText::tr("保存失败：") + message);
                QMessageBox::warning(this, AppText::tr("保存失败"), AppText::tr("无法保存 AITagTranslate 词表：\n") + message);
                return;
            }

            for (int i = translationCsvPaths.size() - 1; i >= 0; --i) {
                if (QFileInfo(translationCsvPaths.at(i)).absoluteFilePath().compare(targetPath, Qt::CaseInsensitive) == 0) {
                    translationCsvPaths.removeAt(i);
                }
            }
            for (auto it = disabledTranslationCsvPaths.begin(); it != disabledTranslationCsvPaths.end();) {
                const QString disabledPath = QFileInfo(*it).absoluteFilePath();
                if (disabledPath.compare(targetPath, Qt::CaseInsensitive) == 0) {
                    it = disabledTranslationCsvPaths.erase(it);
                } else {
                    ++it;
                }
            }
            translationCsvPaths.append(targetPath);
            translationCsvPaths = PathUtils::normalizePathList(translationCsvPaths);
            disabledTranslationCsvPaths = PathUtils::normalizePathSet(disabledTranslationCsvPaths);

            applyPathListsToUi();
            reloadTranslationMaps();
            saveGlobalConfig();
            if (settingsPage) {
                settingsPage->setAiTagTranslateDownloadState(
                    false, AppText::tr("已加载 %1 条记录，词表优先级位于最底部。").arg(entryCount));
            }
            ui->statusbar->showMessage(AppText::tr("AITagTranslate 词表已更新：%1 条记录").arg(entryCount), 5000);
        });
        watcher->setFuture(QtConcurrent::run(backgroundThreadPool, [data]() {
            return static_cast<int>(TranslationCsv::parseUtf8(data).size());
        }));
    });
}

void MainWindow::applySettingsState(SettingsState state)
{
    state.normalize();

    const bool recursiveChanged = optLoraRecursive != state.loraRecursive || optGalleryRecursive != state.galleryRecursive;
    const bool renderThreadsChanged = optRenderThreadCount != state.renderThreadCount;
    const bool collectionTreeChanged = optShowEmptyCollections != state.showEmptyCollections
        || optCollectionFolderTopLevel != state.collectionFolderTopLevel
        || optCollectionFolderSecondLevel != state.collectionFolderSecondLevel;
    const bool modelGroupingChanged = optModelListFolderGrouping != state.modelListFolderGrouping;
    const bool civitaiNameChanged = optUseCivitaiName != state.useCivitaiName;
    const bool galleryMatchChanged = optUserGalleryMatchMode != state.userGalleryMatchMode
        || optComfyModelNameFallback != state.comfyModelNameFallback;
    const QStringList newGalleryNsfwRules = state.galleryNsfwRules();
    const bool galleryNsfwChanged = optFilterGalleryNSFW != state.filterGalleryNSFW
        || optGalleryNSFWMode != state.galleryNsfwMode
        || optGalleryNSFWRules != newGalleryNsfwRules
        || (state.filterGalleryNSFW && state.galleryNsfwMode == 1 &&
            (optBlurRadius != state.blurRadius || optDownscaleBlur != state.downscaleBlur || optBlurProcessWidth != state.blurProcessWidth));
    const bool iconVisualChanged = optFilterNSFW != state.filterNSFW
        || optNSFWMode != state.nsfwMode
        || optNSFWLevel != state.nsfwLevel
        || optBlurRadius != state.blurRadius
        || optDownscaleBlur != state.downscaleBlur
        || optBlurProcessWidth != state.blurProcessWidth;
    const bool uiScaleChanged = !qFuzzyCompare(optUiScale, state.uiScale);
    const bool customUaModeChanged = optUseArrangedUA != state.useCustomUserAgent;
    const bool themeChanged = optThemeId != state.themeId || optCustomThemePath != state.customThemePath;

    optLoraRecursive = state.loraRecursive;
    optGalleryRecursive = state.galleryRecursive;
    optBlurRadius = state.blurRadius;
    optDownscaleBlur = state.downscaleBlur;
    optBlurProcessWidth = state.blurProcessWidth;
    optFilterNSFW = state.filterNSFW;
    optNSFWMode = state.nsfwMode;
    optNSFWLevel = state.nsfwLevel;
    optFilterGalleryNSFW = state.filterGalleryNSFW;
    optGalleryNSFWMode = state.galleryNsfwMode;
    optGalleryNSFWRules = newGalleryNsfwRules;
    optRenderThreadCount = qMax(1, state.renderThreadCount);
    optRestoreTreeState = state.restoreTreeState;
    optRestoreFilterSortState = state.restoreFilterSortState;
    optSplitOnNewline = state.splitOnNewline;
    optFilterTags = state.filterTags();
    optShowEmptyCollections = state.showEmptyCollections;
    optCollectionFolderTopLevel = state.collectionFolderTopLevel;
    optCollectionFolderSecondLevel = state.collectionFolderSecondLevel;
    optModelListFolderGrouping = state.modelListFolderGrouping;
    optUseArrangedUA = state.useCustomUserAgent;
    optSavedUAString = state.customUserAgent;
    optCivitaiApiKey = state.civitaiApiKey;
    optModelUpdateDownloadPolicy = state.modelUpdateDownloadPolicy;
    optAutoCheckUpdatesOnStartup = state.autoCheckUpdatesOnStartup;
    optUseCivitaiName = state.useCivitaiName;
    optSuppressLocalWarnings = state.suppressLocalWarnings;
    optUserGalleryMatchMode = state.userGalleryMatchMode;
    optComfyModelNameFallback = state.comfyModelNameFallback;
    optRecalculateKnownMetadataHash = state.recalculateKnownMetadataHash;
    optTryCivArchiveOnMetadataFail = state.tryCivArchiveOnMetadataFail;
    optConfirmCloseWhileLauncherRunning = state.confirmCloseWhileLauncherRunning;
    optUiScale = state.uiScale;
    optThemeId = state.themeId;
    optCustomThemePath = state.customThemePath;

    if (renderThreadsChanged) {
        threadPool->setMaxThreadCount(optRenderThreadCount);
        backgroundThreadPool->setMaxThreadCount(optRenderThreadCount);
    }
    if (customUaModeChanged) {
        currentUserAgent = optUseArrangedUA && !optSavedUAString.isEmpty() ? optSavedUAString : getRandomUserAgent();
        qDebug() << "UA Changed to:" << currentUserAgent;
    } else if (optUseArrangedUA) {
        currentUserAgent = optSavedUAString;
    }

    saveGlobalConfig();

    if (recursiveChanged) {
        // 递归扫描设置在下一次扫描时生效。
    }
    if (collectionTreeChanged) refreshCollectionTreeView();
    if (modelGroupingChanged) {
        executeSort();
        refreshHomeGallery();
        refreshCollectionTreeView();
    }
    if (civitaiNameChanged) {
        updateModelListNames();
        executeSort();
    }
    if (galleryMatchChanged) refreshModelUsageStatsAsync();
    if (uiScaleChanged) {
        ui->statusbar->showMessage(AppText::tr("缩放比例已设置为 %1x，重启后生效").arg(optUiScale), 3000);
    }
    if (themeChanged) {
        applyApplicationTheme(optThemeId, optCustomThemePath, true);
    }
    if (iconVisualChanged) {
        sidebarIconCache.clear();
        if (homeGalleryPage) homeGalleryPage->clearIconCache();
    }
    syncUserGalleryOptions();
    syncHomeGalleryOptions();
    if (galleryNsfwChanged) {
        refreshGalleryNsfwDisplay();
    }
}

void MainWindow::resetFilterTagsToDefault()
{
    const QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        AppText::tr("确认重置 / Confirm Reset"),
        AppText::tr("确定要将过滤提示词重置为默认值吗？\n此操作将覆盖当前的自定义设置。\n\n"
        "Are you sure you want to reset filter tags to default?"),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    settingsPage->setFilterTagsText(DEFAULT_FILTER_TAGS);
    optFilterTags = DEFAULT_FILTER_TAGS.split(',', Qt::SkipEmptyParts);
    for (QString &s : optFilterTags) s = s.trimmed();
    syncUserGalleryOptions();
    saveGlobalConfig();
    ui->statusbar->showMessage(AppText::tr("过滤词已重置"), 2000);
}

void MainWindow::resetGalleryNsfwRulesToDefault()
{
    const QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        AppText::tr("确认重置 / Confirm Reset"),
        AppText::tr("确定要将返图 NSFW 匹配规则重置为默认值吗？\n此操作将覆盖当前的自定义设置。\n\n"
        "Are you sure you want to reset gallery NSFW rules to default?"),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    settingsPage->setGalleryNsfwRulesText(DEFAULT_GALLERY_NSFW_RULES);
    optGalleryNSFWRules = GalleryMetadata::parseNsfwRules(DEFAULT_GALLERY_NSFW_RULES);
    syncUserGalleryOptions();
    saveGlobalConfig();
    if (optFilterGalleryNSFW) refreshGalleryNsfwDisplay();
    ui->statusbar->showMessage(AppText::tr("返图 NSFW 规则已重置"), 2000);
}

void MainWindow::applyRandomUserAgent()
{
    const QString newUA = getRandomUserAgent();
    settingsPage->setUserAgentText(newUA);
    optUseArrangedUA = settingsPage->state().useCustomUserAgent;
    optSavedUAString = newUA;
    if (optUseArrangedUA) {
        currentUserAgent = newUA;
    }
    saveGlobalConfig();
}

void MainWindow::applyApplicationTheme(const QString &themeId, const QString &customPath, bool updateStatus)
{
    // 先切系统配色方案：决定原生标题栏明暗、状态栏、以及未显式设色控件(标签空白区/复选框/未样式标签)的调色板。
    if (qApp->styleHints()) {
        const QString scheme = (themeId == "custom_qss") ? QStringLiteral("dark")
                                                         : AppStyle::themeColorScheme(themeId);
        qApp->styleHints()->setColorScheme(scheme == QLatin1String("light") ? Qt::ColorScheme::Light
                                                                            : Qt::ColorScheme::Dark);
    }

    const AppStyle::ThemeBundle bundle = AppStyle::loadThemeBundle(themeId, customPath);
    qApp->setStyleSheet(bundle.dialogQss);
    setStyleSheet(bundle.mainQss);
    currentToolPageQss = bundle.toolQss;
    refreshLoadedToolPageThemes();
    recolorPlaceholderItems(); // 重染"加载失败X"占位（列表项 + 下载卡）
    updateBackgroundImage(); // 重新生成 Hero 模糊背景上的渐变叠加（steamBackground 已随主题变）
    if (ui && ui->heroFrame) ui->heroFrame->update();
    if (settingsPage && updateStatus) {
        settingsPage->setThemeStatus(bundle.status);
    }
    if (updateStatus && ui && ui->statusbar) {
        ui->statusbar->showMessage(bundle.ok ? AppText::tr("已应用主题：%1").arg(AppStyle::themeDisplayName(themeId)) : bundle.status, 2500);
    }
}

void MainWindow::applyActivePaletteToUi()
{
    // 用「当前活动调色板」重建所有 QSS 并刷新（不切 colorScheme，供编辑器实时预览）。
    // 活动调色板已由调用方通过 AppStyle::setActivePalette() 指定。
    qApp->setStyleSheet(AppStyle::loadQss(":/styles/dialog.qss"));
    setStyleSheet(AppStyle::loadQss(":/styles/mainwindow.qss"));
    currentToolPageQss = AppStyle::loadToolPageQss();
    refreshLoadedToolPageThemes();
    recolorPlaceholderItems(); // 编辑器预览时也重染占位X
    updateBackgroundImage();
    if (ui && ui->heroFrame) ui->heroFrame->update();
}

void MainWindow::onEditThemeRequested(const QString &baseThemeId)
{
    ThemeEditorDialog dlg(baseThemeId, this);
    connect(&dlg, &ThemeEditorDialog::previewRequested, this,
            [this](const QHash<QString, QString> &palette, bool lightScheme) {
                // 预览也切 colorScheme，避免亮色主题下原生控件/未样式文字仍是深色。
                if (qApp->styleHints())
                    qApp->styleHints()->setColorScheme(lightScheme ? Qt::ColorScheme::Light
                                                                   : Qt::ColorScheme::Dark);
                AppStyle::setActivePalette(palette);
                applyActivePaletteToUi();
            });
    connect(&dlg, &ThemeEditorDialog::saved, this, &MainWindow::onThemeSaved);
    connect(&dlg, &ThemeEditorDialog::canceled, this, [this]() {
        // 取消/关闭：还原到当前持久主题（完整走一遍，含 colorScheme 与 custom_qss）。
        applyApplicationTheme(optThemeId, optCustomThemePath, false);
    });
    dlg.exec();
}

void MainWindow::onThemeSaved(const QString &themeId)
{
    AppStyle::reloadUserThemes();                 // 让新存的主题进入注册表
    if (settingsPage) settingsPage->refreshThemeList(themeId);
    optThemeId = themeId;
    // 不动 optCustomThemePath：调色板主题不使用它，但保留以便日后切回 Custom QSS。
    applyApplicationTheme(themeId, optCustomThemePath, true); // 应用注册表里的正式主题
    saveGlobalConfig();
}

void MainWindow::onThemeDeleteRequested(const QString &themeId)
{
    const QString name = AppStyle::themeDisplayName(themeId);
    if (QMessageBox::question(this, AppText::tr("删除主题 / Delete Theme"),
                              AppText::tr("确定删除自定义主题“%1”吗？此操作不可撤销。").arg(name))
        != QMessageBox::Yes)
        return;
    if (!AppStyle::deleteUserTheme(themeId)) {
        QMessageBox::warning(this, AppText::tr("删除主题 / Delete Theme"),
                             AppText::tr("删除失败：内置主题不可删，或文件无法移除。"));
        return;
    }
    AppStyle::reloadUserThemes();
    QString newId = optThemeId;
    if (optThemeId == themeId) newId = QStringLiteral("steam_dark"); // 当前主题被删 -> 回退默认
    if (settingsPage) settingsPage->refreshThemeList(newId);
    optThemeId = newId;
    applyApplicationTheme(newId, optCustomThemePath, true);
    saveGlobalConfig();
}

void MainWindow::applyToolPageTheme(QWidget *page)
{
    if (!page) return;
    page->setStyleSheet(currentToolPageQss.isEmpty() ? AppStyle::loadToolPageQss() : currentToolPageQss);
}

void MainWindow::refreshLoadedToolPageThemes()
{
    applyToolPageTheme(tagBrowserWidget);
    applyToolPageTheme(llmPromptWidget);
    if (llmPromptWidget) llmPromptWidget->applyTheme(); // 重绘对话气泡内联样式
    applyToolPageTheme(parserWidget);
    applyToolPageTheme(usageAnalysisWidget);
    applyToolPageTheme(promptTemplateLibraryWidget);
    if (promptTemplateLibraryWidget) promptTemplateLibraryWidget->applyTheme(); // 重建占位符/收藏卡片换色
    if (launcherWidget) launcherWidget->applyTheme();
    if (settingsPage) settingsPage->applyTheme();
    if (downloadsPage) downloadsPage->applyTheme();
    if (ui->toolsTabWidget) {
        // West 标签条空白区背景靠 palette 决定，随主题重新着色。
        QPalette pal = ui->toolsTabWidget->palette();
        pal.setColor(QPalette::Window, AppStyle::color("sidebarBg"));
        ui->toolsTabWidget->setPalette(pal);
        for (int i = 0; i < ui->toolsTabWidget->count(); ++i) {
            QWidget *page = ui->toolsTabWidget->widget(i);
            if (!page) continue;
            if (page->property("isToolPlaceholder").toBool()) {
                for (QLabel *label : page->findChildren<QLabel*>()) {
                    if (label->property("isToolPlaceholderLabel").toBool()) {
                        label->setStyleSheet(AppStyle::MutedLabelStyle());
                    }
                }
            } else {
                applyToolPageTheme(page);
            }
        }
    }
}

void MainWindow::saveGlobalConfig() {
    QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);

    QString configPath = configDir + "/settings.json";
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
            return;
        }
        readFile.close();
    }
    SettingsState settings = settingsPage ? settingsPage->state() : SettingsState{};
    if (!settingsPage) {
        settings.loraRecursive = optLoraRecursive;
        settings.galleryRecursive = optGalleryRecursive;
        settings.blurRadius = optBlurRadius;
        settings.downscaleBlur = optDownscaleBlur;
        settings.blurProcessWidth = optBlurProcessWidth;
        settings.filterNSFW = optFilterNSFW;
        settings.nsfwMode = optNSFWMode;
        settings.nsfwLevel = optNSFWLevel;
        settings.filterGalleryNSFW = optFilterGalleryNSFW;
        settings.galleryNsfwMode = optGalleryNSFWMode;
        settings.galleryNsfwRulesText = optGalleryNSFWRules.join(", ");
        settings.renderThreadCount = optRenderThreadCount;
        settings.restoreTreeState = optRestoreTreeState;
        settings.restoreFilterSortState = optRestoreFilterSortState;
        settings.splitOnNewline = optSplitOnNewline;
        settings.filterTagsText = optFilterTags.join(", ");
        settings.showEmptyCollections = optShowEmptyCollections;
        settings.collectionFolderTopLevel = optCollectionFolderTopLevel;
        settings.collectionFolderSecondLevel = optCollectionFolderSecondLevel;
        settings.modelListFolderGrouping = optModelListFolderGrouping;
        settings.useCustomUserAgent = optUseArrangedUA;
        settings.customUserAgent = optSavedUAString;
        settings.civitaiApiKey = optCivitaiApiKey;
        settings.modelUpdateDownloadPolicy = optModelUpdateDownloadPolicy;
        settings.autoCheckUpdatesOnStartup = optAutoCheckUpdatesOnStartup;
        settings.useCivitaiName = optUseCivitaiName;
        settings.suppressLocalWarnings = optSuppressLocalWarnings;
        settings.userGalleryMatchMode = optUserGalleryMatchMode;
        settings.comfyModelNameFallback = optComfyModelNameFallback;
        settings.recalculateKnownMetadataHash = optRecalculateKnownMetadataHash;
        settings.tryCivArchiveOnMetadataFail = optTryCivArchiveOnMetadataFail;
        settings.confirmCloseWhileLauncherRunning = optConfirmCloseWhileLauncherRunning;
        settings.uiScale = optUiScale;
        settings.themeId = optThemeId;
        settings.customThemePath = optCustomThemePath;
    }

    QJsonArray loraArr;
    for (const QString &path : loraPaths) loraArr.append(path);
    QJsonArray galleryArr;
    for (const QString &path : galleryPaths) galleryArr.append(path);
    QJsonArray translationArr;
    for (const QString &path : translationCsvPaths) translationArr.append(path);
    QJsonArray loraDisabledArr;
    for (const QString &path : disabledLoraPaths) loraDisabledArr.append(path);
    QJsonArray galleryDisabledArr;
    for (const QString &path : disabledGalleryPaths) galleryDisabledArr.append(path);
    QJsonArray translationDisabledArr;
    for (const QString &path : disabledTranslationCsvPaths) translationDisabledArr.append(path);
    QJsonArray collapsedModelFoldersArr;
    for (const QString &path : collapsedModelFolders) collapsedModelFoldersArr.append(path);
    root["lora_paths"]                 = loraArr;
    root["gallery_paths"]              = galleryArr;
    root["translation_paths"]          = translationArr;
    root["lora_paths_disabled"]        = loraDisabledArr;
    root["gallery_paths_disabled"]     = galleryDisabledArr;
    root["translation_paths_disabled"] = translationDisabledArr;
    root["model_list_collapsed_folders"] = collapsedModelFoldersArr;
    root["translation_path"]           = PathUtils::collectEnabledPaths(translationCsvPaths, disabledTranslationCsvPaths).value(0);
    settings.writeToJson(root);
    root.remove("model_switch_delay_ms");

    // 记录当前窗口的实际大小（这样用户拉伸窗口后，下次启动会记住大小）
    // 只有当窗口没有最大化/全屏时才记录
    if (!this->isMaximized() && !this->isFullScreen()) {
        root["window_width"] = this->width();
        root["window_height"] = this->height();
    } else {
        // 如果处于最大化状态，可以保存正常的几何尺寸
        root["window_width"] = this->normalGeometry().width();
        root["window_height"] = this->normalGeometry().height();
    }

    if (optRestoreTreeState) {
        QJsonObject treeState;

        // 1. 获取当前展开的项
        QJsonArray expandedArr;
        // 如果 UI 还没初始化完(比如刚启动就关闭)，尽量保留读取到的旧状态，防止清空
        // 但这里我们主要处理运行时保存：
        if (ui->collectionTree->topLevelItemCount() > 0) {
            std::function<void(QTreeWidgetItem*)> collectExpanded = [&](QTreeWidgetItem *item) {
                if (!item) return;
                if (item->isExpanded()) {
                    const QString key = item->data(0, ROLE_COLLECTION_EXPAND_KEY).toString();
                    expandedArr.append(key.isEmpty() ? item->data(0, ROLE_COLLECTION_NAME).toString() : key);
                }
                for (int i = 0; i < item->childCount(); ++i) collectExpanded(item->child(i));
            };
            for (int i = 0; i < ui->collectionTree->topLevelItemCount(); ++i) {
                collectExpanded(ui->collectionTree->topLevelItem(i));
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

    if (optRestoreFilterSortState && ui) {
        QJsonObject fs;
        if (ui->comboSort) fs["sort_index"] = ui->comboSort->currentIndex();
        if (ui->comboBaseModel) {
            fs["base_model"] = ui->comboBaseModel->currentIndex() <= 0
                ? QStringLiteral("All") : ui->comboBaseModel->currentText();
        }
        if (ui->comboModelType) {
            fs["model_type"] = ui->comboModelType->currentIndex() <= 0
                ? QStringLiteral("All") : ui->comboModelType->currentText();
        }
        if (ui->searchEdit) fs["search_text"] = ui->searchEdit->text();
        if (ui->sidebarStack) fs["sidebar_tab"] = ui->sidebarStack->currentIndex();
        fs["collection_filter"] = currentCollectionFilter;
        if (homeGalleryPage) {
            const HomeGalleryFilterState hs = homeGalleryPage->filterState();
            fs["home_author_filter"] = hs.authorFilter;
            fs["home_filter_expanded"] = hs.filterExpanded;
            fs["home_tag_sort_by_count"] = hs.tagsSortByCount;
            QJsonArray homeTagsArr;
            for (const QString &t : hs.tagFilters) {
                if (!t.trimmed().isEmpty()) homeTagsArr.append(t.trimmed());
            }
            fs["home_tag_filters"] = homeTagsArr;
        }
        if (userGalleryPage) {
            const UserGalleryFilterState gs = userGalleryPage->filterState();
            fs["user_tag_sort_mode"] = gs.userTagSortMode;
            fs["user_tag_lora_only"] = gs.userTagLoraOnly;
            fs["user_tag_include_negative"] = gs.userTagIncludeNegative;
            fs["gallery_prompt_sort_mode"] = gs.galleryPromptSortMode;
            fs["gallery_tag_view_active"] = gs.galleryTagViewActive;
        }
        root["filter_sort_state"] = fs;
    } else if (!optRestoreFilterSortState) {
        root.remove("filter_sort_state");
    }

    QSaveFile file(configPath);
    const QByteArray payload = QJsonDocument(root).toJson();
    if (!file.open(QIODevice::WriteOnly)
        || file.write(payload) != payload.size()
        || !file.commit()) {
        qWarning() << "Unable to save global settings atomically:" << file.errorString();
    }
}

void MainWindow::applyPathListsToUi()
{
    const QStringList activeLoraPaths = PathUtils::collectEnabledPaths(loraPaths, disabledLoraPaths);
    const QStringList activeGalleryPaths = PathUtils::collectEnabledPaths(galleryPaths, disabledGalleryPaths);
    const QStringList activeTranslationPaths = PathUtils::collectEnabledPaths(translationCsvPaths, disabledTranslationCsvPaths);
    currentLoraPath = activeLoraPaths.value(0);
    sdOutputFolder = activeGalleryPaths.value(0);
    translationCsvPath = activeTranslationPaths.value(0);

    if (settingsPage) {
        settingsPage->setPathSummaries(
            activeLoraPaths.join("; "),
            activeGalleryPaths.join("; "),
            activeTranslationPaths.join("; "));
    }
    if (tagBrowserWidget) {
        tagBrowserWidget->setTranslationSources(
            buildTagTranslationSources(translationCsvPaths, disabledTranslationCsvPaths));
        tagBrowserWidget->setMergedTranslationMap(&translationMap);
    }
    if (llmPromptWidget) llmPromptWidget->setLibraryPaths(activeLoraPaths, activeGalleryPaths);
}

bool MainWindow::editLoraPaths(bool rescanAfter)
{
    PathListDialog dlg(this);
    dlg.setDialogTitle(AppText::tr("LoRA 路径管理"));
    dlg.setHintText(AppText::tr("可添加多个 LoRA 模型目录。通过右侧复选框控制是否启用扫描。"));
    dlg.setPathEntries(PathUtils::buildPathEntries(loraPaths, disabledLoraPaths));

    if (dlg.exec() != QDialog::Accepted) return false;

    PathUtils::applyPathEntries(dlg.pathEntries(), loraPaths, disabledLoraPaths);
    loraPaths = PathUtils::normalizePathList(loraPaths);
    disabledLoraPaths = PathUtils::normalizePathSet(disabledLoraPaths);
    collapsedModelFolders = PathUtils::normalizePathSet(collapsedModelFolders);
    {
        QSet<QString> filtered;
        for (const QString &path : loraPaths) {
            if (disabledLoraPaths.contains(path)) filtered.insert(path);
        }
        disabledLoraPaths = filtered;
    }
    {
        QSet<QString> activeRoots;
        for (const QString &path : loraPaths) {
            if (!disabledLoraPaths.contains(path)) activeRoots.insert(path);
        }
        QSet<QString> filtered;
        for (const QString &path : collapsedModelFolders) {
            if (activeRoots.contains(path)) filtered.insert(path);
        }
        collapsedModelFolders = filtered;
    }
    applyPathListsToUi();
    saveGlobalConfig();

    const QStringList activeLoraPaths = PathUtils::collectEnabledPaths(loraPaths, disabledLoraPaths);
    if (rescanAfter && !activeLoraPaths.isEmpty()) {
        scanModels(activeLoraPaths);
    }
    return true;
}

bool MainWindow::editGalleryPaths(bool rescanAfter)
{
    PathListDialog dlg(this);
    dlg.setDialogTitle(AppText::tr("图库路径管理"));
    dlg.setHintText(AppText::tr("可添加多个图库目录。通过右侧复选框控制是否启用扫描。"));
    dlg.setPathEntries(PathUtils::buildPathEntries(galleryPaths, disabledGalleryPaths));

    if (dlg.exec() != QDialog::Accepted) return false;

    PathUtils::applyPathEntries(dlg.pathEntries(), galleryPaths, disabledGalleryPaths);
    galleryPaths = PathUtils::normalizePathList(galleryPaths);
    disabledGalleryPaths = PathUtils::normalizePathSet(disabledGalleryPaths);
    {
        QSet<QString> filtered;
        for (const QString &path : galleryPaths) {
            if (disabledGalleryPaths.contains(path)) filtered.insert(path);
        }
        disabledGalleryPaths = filtered;
    }
    applyPathListsToUi();
    saveGlobalConfig();

    if (rescanAfter) {
        onRescanUserClicked();
    }
    return true;
}

// === 设置页交互 ===

void MainWindow::onBrowseLoraPath() {
    if (editLoraPaths(false)) {
        QMessageBox::information(this, AppText::tr("提示"), AppText::tr("LoRA 路径已更新，请返回库界面点击刷新按钮。"));
    }
}

void MainWindow::onBrowseGalleryPath() {
    editGalleryPaths(false);
}

QIcon MainWindow::generatePlaceholderIcon()
{
    return ImagePresentation::modelPlaceholder(false, false,
                                               QColor(AppStyle::PanelDark()), AppStyle::color("mutedText"));
}

QIcon MainWindow::generateSmallPlaceholderIcon()
{
    return ImagePresentation::modelPlaceholder(true, false,
                                               QColor(AppStyle::PanelDark()), AppStyle::color("mutedText"));
}

QIcon MainWindow::generateNoPreviewIcon(bool small) const
{
    return ImagePresentation::modelPlaceholder(small, true,
                                               QColor(AppStyle::PanelDark()), AppStyle::color("mutedText"));
}

void MainWindow::recolorPlaceholderItems()
{
    // 切主题后重建缺失叉号与明确无预览圆圈，并重染当前可见占位项。
    placeholderIcon = generatePlaceholderIcon();           // 主页大图
    smallPlaceholderIcon = generateSmallPlaceholderIcon();  // 侧边栏/收藏树/用户图
    noPreviewIcon = generateNoPreviewIcon(false);
    smallNoPreviewIcon = generateNoPreviewIcon(true);
    if (localMetaEditorPage) {
        localMetaEditorPage->setPlaceholderIcon(placeholderIcon);
        refreshCurrentDetailCacheStatus();
    }

    auto recolorList = [this](QListWidget *list, const QIcon &missingIcon, const QIcon &knownNoPreviewIcon) {
        if (!list) return;
        bool changed = false;
        for (int i = 0; i < list->count(); ++i) {
            QListWidgetItem *item = list->item(i);
            if (item && item->data(ROLE_PREVIEW_PLACEHOLDER).toBool()) {
                // 先清空再设：强制 setData 判定为"变化"并发 dataChanged（同名 QIcon 比较可能被判为相等而跳过）。
                item->setIcon(QIcon());
                const ModelPreviewState state = static_cast<ModelPreviewState>(
                    item->data(ROLE_MODEL_PREVIEW_STATE).toInt());
                item->setIcon(state == ModelPreviewState::KnownNoPreview
                                  ? knownNoPreviewIcon
                                  : missingIcon);
                changed = true;
            }
        }
        if (changed && list->viewport()) list->viewport()->update();
    };
    if (homeGalleryPage) homeGalleryPage->recolorPlaceholders(placeholderIcon, noPreviewIcon);
    recolorList(ui->modelList, smallPlaceholderIcon, smallNoPreviewIcon);
    if (userGalleryPage) userGalleryPage->recolorPlaceholders(placeholderIcon); // 非模型图片始终使用叉

    bool treeChanged = false;
    std::function<void(QTreeWidgetItem *)> recolorNode = [&](QTreeWidgetItem *node) {
        if (!node) return;
        if (node->data(0, ROLE_PREVIEW_PLACEHOLDER).toBool()) {
            node->setIcon(0, QIcon());
            const ModelPreviewState state = static_cast<ModelPreviewState>(
                node->data(0, ROLE_MODEL_PREVIEW_STATE).toInt());
            node->setIcon(0, state == ModelPreviewState::KnownNoPreview
                                 ? smallNoPreviewIcon
                                 : smallPlaceholderIcon);
            treeChanged = true;
        }
        for (int j = 0; j < node->childCount(); ++j) recolorNode(node->child(j));
    };
    for (int i = 0; i < ui->collectionTree->topLevelItemCount(); ++i)
        recolorNode(ui->collectionTree->topLevelItem(i));
    if (treeChanged && ui->collectionTree->viewport()) ui->collectionTree->viewport()->update();

    // 下载卡片的占位由 downloadsPage->applyTheme() 自行重绘。
}

// 浏览翻译文件
void MainWindow::onBrowseTranslationPath() {
    if (editTranslationCsvPaths()) {
        QMessageBox::information(this, AppText::tr("设置"), AppText::tr("翻译词表已加载。"));
    }
}

bool MainWindow::editTranslationCsvPaths()
{
    PathListDialog dlg(this);
    dlg.setDialogTitle(AppText::tr("Tag 翻译表管理"));
    dlg.setHintText(AppText::tr("可添加多个 Tag 翻译 CSV。列表越靠上优先级越高，可通过右侧复选框临时停用。"));
    dlg.setSelectionMode(PathListDialog::FileMode, "CSV Files (*.csv);;All Files (*.*)");
    dlg.setPathEntries(PathUtils::buildPathEntries(translationCsvPaths, disabledTranslationCsvPaths));

    if (dlg.exec() != QDialog::Accepted) return false;

    PathUtils::applyPathEntries(dlg.pathEntries(), translationCsvPaths, disabledTranslationCsvPaths);
    translationCsvPaths = PathUtils::normalizePathList(translationCsvPaths);
    disabledTranslationCsvPaths = PathUtils::normalizePathSet(disabledTranslationCsvPaths);
    {
        QSet<QString> filtered;
        for (const QString &path : translationCsvPaths) {
            if (disabledTranslationCsvPaths.contains(path)) filtered.insert(path);
        }
        disabledTranslationCsvPaths = filtered;
    }

    applyPathListsToUi();
    reloadTranslationMaps();
    saveGlobalConfig();
    return true;
}

void MainWindow::reloadTranslationMaps(bool notifyWidgets, bool asynchronous)
{
    const int token = ++translationLoadToken;
    const QStringList activePaths = PathUtils::collectEnabledPaths(translationCsvPaths, disabledTranslationCsvPaths);
    translationCsvPath = activePaths.value(0);
    const auto readMaps = [activePaths]() {
        QHash<QString, QString> result;
        for (int i = activePaths.size() - 1; i >= 0; --i) {
            const QVector<TranslationCsvEntry> entries = TranslationCsv::readFile(activePaths.at(i));
            for (const TranslationCsvEntry &entry : entries) {
                const QString display = entry.displayValue().trimmed();
                if (!entry.tag.isEmpty() && !display.isEmpty()) result.insert(entry.tag, display);
            }
        }
        return result;
    };
    const auto applyMaps = [this, token, notifyWidgets, activePaths](QHash<QString, QString> result) {
        if (isShuttingDown || token != translationLoadToken) return;
        translationMap = std::move(result);
        if (notifyWidgets) {
            if (parserWidget) parserWidget->setTranslationMap(&translationMap);
            if (promptTemplateLibraryWidget) promptTemplateLibraryWidget->setTranslationMap(&translationMap);
            if (tagBrowserWidget) tagBrowserWidget->setMergedTranslationMap(&translationMap);
            if (userGalleryPage) userGalleryPage->setTranslationMap(&translationMap);
            if (usageAnalysisWidget) usageAnalysisWidget->setTranslationMap(&translationMap);
        }
        qDebug() << "Loaded translation entries:" << translationMap.size() << "from" << activePaths.size() << "CSV file(s)";
    };
    if (!asynchronous) {
        applyMaps(readMaps());
        return;
    }
    auto *watcher = new QFutureWatcher<QHash<QString, QString>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [watcher, applyMaps]() {
        auto result = watcher->result();
        watcher->deleteLater();
        applyMaps(std::move(result));
    });
    watcher->setFuture(QtConcurrent::run(backgroundThreadPool, readMaps));
}

void MainWindow::showComfyWorkflowViewer(const QString &filePath)
{
    ComfyWorkflowViewerDialog dialog(filePath, this);
    dialog.exec();
}

void MainWindow::showModelDescriptionDialog()
{
    QString descriptionHtml = currentMeta.description.trimmed();
    if (descriptionHtml.isEmpty()) {
        descriptionHtml = ui->textDescription->toHtml().trimmed();
    }

    QTextDocument probe;
    probe.setHtml(descriptionHtml);
    const QString plainText = probe.toPlainText().trimmed();
    if (plainText.isEmpty() || plainText == "No description.") {
        QMessageBox::information(this, AppText::tr("模型简介"), AppText::tr("当前模型没有可显示的简介。"));
        return;
    }

    TextViewerDialog dialog(this);
    dialog.configureForHtmlDescription(
        currentMeta.name.isEmpty() ? currentMeta.fileNameServer : currentMeta.name,
        descriptionHtml);
    connect(&dialog, &TextViewerDialog::statusMessageRequested, ui->statusbar, &QStatusBar::showMessage);
    dialog.exec();
}

void MainWindow::showRawImageMetadataDialog(const QString &filePath)
{
    const QMap<QString, QString> chunks = extractImageMetadataTextChunks(filePath);

    QStringList sections;
    for (auto it = chunks.begin(); it != chunks.end(); ++it) {
        sections << QString("===== %1 =====\n%2").arg(it.key(), it.value());
    }
    const QString rawText = sections.isEmpty()
        ? QStringLiteral("未找到图片 metadata。")
        : sections.join(QStringLiteral("\n\n"));

    TextViewerDialog dialog(this);
    dialog.configureForRawMetadata(QFileInfo(filePath).fileName(), rawText);
    dialog.exec();
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
        QString displayName;
        if (item->data(0, ROLE_IS_FOLDER_HEADER).toBool()) {
            displayName = item->data(0, ROLE_MODEL_ROOT_NAME).toString();
            if (displayName.isEmpty()) displayName = collectionName;
        } else {
            displayName = (collectionName == FILTER_UNCATEGORIZED) ? AppText::tr("未分类 / Uncategorized") : collectionName;
        }
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
        if (clickedItem->data(0, ROLE_IS_FOLDER_HEADER).toBool()) {
            QList<QListWidgetItem*> targetListItems;
            std::function<void(QTreeWidgetItem*)> collectModelItems = [&](QTreeWidgetItem *treeItem) {
                if (!treeItem) return;
                const QString baseName = treeItem->data(0, ROLE_MODEL_NAME).toString();
                if (!baseName.isEmpty()) {
                    for (int i = 0; i < ui->modelList->count(); ++i) {
                        QListWidgetItem *listItem = ui->modelList->item(i);
                        if (listItem->data(ROLE_MODEL_NAME).toString() == baseName) {
                            targetListItems.append(listItem);
                            break;
                        }
                    }
                }
                for (int i = 0; i < treeItem->childCount(); ++i) collectModelItems(treeItem->child(i));
            };
            collectModelItems(clickedItem);
            if (!targetListItems.isEmpty()) {
                showCollectionMenu(targetListItems, ui->collectionTree->mapToGlobal(pos));
            }
            return;
        }
        // --- 收藏夹节点右键菜单 ---
        QString collectionName = clickedItem->data(0, ROLE_COLLECTION_NAME).toString();

        QMenu menu(this);
        QAction *title = menu.addAction(AppText::tr("管理收藏夹: %1").arg(collectionName));
        title->setEnabled(false);
        menu.addSeparator();

        if (collectionName == FILTER_UNCATEGORIZED) {
            // "未分类"特殊处理，不能重命名和删除
            QAction *dummy = menu.addAction(AppText::tr("无法操作此项"));
            dummy->setEnabled(false);
        } else {
            QAction *actRename = menu.addAction(AppText::tr("重命名 / Rename Collection"));
            QAction *actDelete = menu.addAction(AppText::tr("删除 / Delete Collection"));

            QAction *selected = menu.exec(ui->collectionTree->mapToGlobal(pos));

            if (selected == actRename) {
                bool ok;
                QString newName = QInputDialog::getText(this, AppText::tr("重命名收藏夹"), AppText::tr("新名称:"), QLineEdit::Normal, collectionName, &ok);
                if (ok && !newName.trimmed().isEmpty() && newName != collectionName) {
                    if (collections.contains(newName)) {
                        QMessageBox::warning(this, AppText::tr("错误"), AppText::tr("该名称已存在！"));
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
                auto reply = QMessageBox::question(this, AppText::tr("确认删除"),
                                                   AppText::tr("确定要删除收藏夹 \"%1\" 吗？\n(里面的模型不会被删除，仅删除分类)").arg(collectionName),
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
        std::function<void(QTreeWidgetItem*)> collectExpanded = [&](QTreeWidgetItem *item) {
            if (!item) return;
            if (item->isExpanded()) {
                const QString key = item->data(0, ROLE_COLLECTION_EXPAND_KEY).toString();
                expandedCollections.insert(key.isEmpty() ? item->data(0, ROLE_COLLECTION_NAME).toString() : key);
            }
            for (int i = 0; i < item->childCount(); ++i) collectExpanded(item->child(i));
        };
        for (int i = 0; i < ui->collectionTree->topLevelItemCount(); ++i) {
            collectExpanded(ui->collectionTree->topLevelItem(i));
        }
        scrollPos = ui->collectionTree->verticalScrollBar()->value();
    }

    // 清理和生成
    ui->collectionTree->setUpdatesEnabled(false);
    ui->collectionTree->setAnimated(false);
    collectionTreeItemsByPath.clear();
    ui->collectionTree->clear();
    ui->collectionTree->setIconSize(QSize(32, 32));
    ui->collectionTree->setRootIsDecorated(false);
    ui->collectionTree->setIndentation(10); // 子节点的缩进保持正常
    ui->collectionTree->setExpandsOnDoubleClick(false);

    QFont categoryFont = ui->collectionTree->font();
    categoryFont.setBold(true);
    categoryFont.setPointSize(10);

    const QString PRE_OPEN   = " - "; // 展开时：减号 + 空格
    const QString PRE_CLOSED = " + "; // 折叠时：加号 + 空格

    QHash<QString, QListWidgetItem*> visibleItemMap; // BaseName -> Item
    QHash<QString, int> visibleItemRank;             // BaseName -> SortIndex (排名)
    visibleItemMap.reserve(ui->modelList->count());
    visibleItemRank.reserve(ui->modelList->count());

    for (int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!isModelListItem(item)) continue;

        // 收藏夹树只关心搜索/底模过滤结果，不受 Models 文件夹折叠影响。
        if (!item->data(ROLE_MODEL_FILTER_VISIBLE).toBool()) continue;

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
            if (QListWidgetItem *sourceItem = visibleItemMap.value(baseName, nullptr)) {
                QTreeWidgetItem *child = new QTreeWidgetItem(parent);
                child->setText(0, sourceItem->text());
                copyCommonModelRoles(sourceItem, [child](int role, const QVariant &val) {
                    child->setData(0, role, val);
                });
                child->setData(0, ROLE_PREVIEW_PLACEHOLDER, sourceItem->data(ROLE_PREVIEW_PLACEHOLDER));
                child->setIcon(0, sourceItem->icon());
                applyModelHighlightColor(child);
                applyModelUserNoteData(child);
                const QString fp = sourceItem->data(ROLE_FILE_PATH).toString();
                if (!fp.isEmpty()) collectionTreeItemsByPath.insert(fp, child);
            }
        }
    };

    // =========================================================
    // 3. 生成节点 (使用上面的可见数据源)
    // =========================================================

    QSet<QString> categorizedSet;
    for (auto it = collections.begin(); it != collections.end(); ++it) {
        for (const QString &m : it.value()) categorizedSet.insert(m);
    }

    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setIgnorePunctuation(false);

    QList<QString> collectionNames = collections.keys();
    collectionNames.removeAll(FILTER_UNCATEGORIZED);
    std::sort(collectionNames.begin(), collectionNames.end(), [&](const QString &s1, const QString &s2){
        return collator.compare(s1, s2) < 0;
    });

    auto makeTreeParent = [&](QTreeWidgetItem *parent, const QString &expandKey, const QString &collectionName, const QString &displayName, int count) {
        QTreeWidgetItem *node = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(ui->collectionTree);
        node->setData(0, ROLE_IS_COLLECTION_NODE, true);
        node->setData(0, ROLE_COLLECTION_NAME, collectionName);
        node->setData(0, ROLE_COLLECTION_EXPAND_KEY, expandKey);
        node->setData(0, ROLE_ITEM_COUNT, count);
        node->setFont(0, categoryFont);

        const bool expanded = expandedCollections.contains(expandKey);
        node->setExpanded(expanded);
        node->setText(0, (expanded ? PRE_OPEN : PRE_CLOSED) + displayName + " (" + QString::number(count) + ")");
        return node;
    };

    auto modelBelongsToFolder = [&](const QString &baseName, const QString &folderPath) {
        QListWidgetItem *item = visibleItemMap.value(baseName, nullptr);
        if (!item) return false;
        return item->data(ROLE_MODEL_ROOT_PATH).toString() == folderPath;
    };
    auto addFolderChildrenForCollection = [&](QTreeWidgetItem *collectionNode, const QString &collectionKeyPrefix, QStringList models) {
        QHash<QString, QStringList> modelsByFolder;
        QHash<QString, QString> folderNames;
        for (const QString &baseName : models) {
            QListWidgetItem *item = visibleItemMap.value(baseName, nullptr);
            if (!item) continue;
            QString folderPath = item->data(ROLE_MODEL_ROOT_PATH).toString();
            if (folderPath.isEmpty()) folderPath = QStringLiteral("__unknown__");
            modelsByFolder[folderPath].append(baseName);
            if (!folderNames.contains(folderPath)) {
                QString folderName = item->data(ROLE_MODEL_ROOT_NAME).toString();
                if (folderName.isEmpty()) {
                    folderName = (folderPath == QStringLiteral("__unknown__")) ? QStringLiteral("未指定文件夹") : folderPath;
                }
                folderNames.insert(folderPath, folderName);
            }
        }

        QStringList folderPaths = modelsByFolder.keys();
        std::sort(folderPaths.begin(), folderPaths.end(), [&](const QString &a, const QString &b) {
            return collator.compare(folderNames.value(a), folderNames.value(b)) < 0;
        });

        for (const QString &folderPath : folderPaths) {
            QStringList folderModels = modelsByFolder.value(folderPath);
            const int visibleCount = folderModels.count();
            if (visibleCount <= 0 && !optShowEmptyCollections) continue;
            const QString key = QString("%1:folder:%2").arg(collectionKeyPrefix, folderPath);
            QTreeWidgetItem *folderNode = makeTreeParent(collectionNode, key, key, folderNames.value(folderPath), visibleCount);
            folderNode->setData(0, ROLE_IS_FOLDER_HEADER, true);
            folderNode->setData(0, ROLE_MODEL_ROOT_NAME, folderNames.value(folderPath));
            addModelChildren(folderNode, folderModels);
        }
    };

    auto addCollectionGroup = [&](QTreeWidgetItem *parent, const QString &folderPath) {
        QStringList uncatModels;
        for (auto it = visibleItemMap.begin(); it != visibleItemMap.end(); ++it) {
            if (optCollectionFolderTopLevel && !modelBelongsToFolder(it.key(), folderPath)) continue;
            if (!categorizedSet.contains(it.key())) uncatModels.append(it.key());
        }

        const int uncatCount = uncatModels.count();
        if (uncatCount > 0 || optShowEmptyCollections) {
            const QString key = optCollectionFolderTopLevel ? QString("folder:%1:%2").arg(folderPath, FILTER_UNCATEGORIZED)
                                                            : FILTER_UNCATEGORIZED;
            QTreeWidgetItem *uncategorizedNode = makeTreeParent(parent, key, FILTER_UNCATEGORIZED, AppText::tr("未分类 / Uncategorized"), uncatCount);
            std::sort(uncatModels.begin(), uncatModels.end(), rankSort);
            if (optCollectionFolderSecondLevel && !optCollectionFolderTopLevel) {
                addFolderChildrenForCollection(uncategorizedNode, key, uncatModels);
            } else {
                addModelChildren(uncategorizedNode, uncatModels);
            }
        }

        for (const QString &colName : collectionNames) {
            QStringList models = collections.value(colName);
            if (optCollectionFolderTopLevel) {
                QMutableStringListIterator it(models);
                while (it.hasNext()) {
                    if (!modelBelongsToFolder(it.next(), folderPath)) it.remove();
                }
            }

            int visibleCount = 0;
            for(const QString &m : models) {
                if(visibleItemMap.contains(m)) visibleCount++;
            }

            if (visibleCount > 0 || optShowEmptyCollections) {
                const QString key = optCollectionFolderTopLevel ? QString("folder:%1:%2").arg(folderPath, colName)
                                                                : colName;
                QTreeWidgetItem *collectionNode = makeTreeParent(parent, key, colName, colName, visibleCount);
                if (optCollectionFolderSecondLevel && !optCollectionFolderTopLevel) {
                    addFolderChildrenForCollection(collectionNode, key, models);
                } else {
                    addModelChildren(collectionNode, models);
                }
            }
        }
    };

    if (optCollectionFolderTopLevel) {
        QMap<QString, QString> folderNames;
        QHash<QString, int> folderCounts;
        for (auto it = visibleItemMap.cbegin(); it != visibleItemMap.cend(); ++it) {
            QListWidgetItem *item = it.value();
            const QString folderPath = item->data(ROLE_MODEL_ROOT_PATH).toString();
            if (folderPath.isEmpty()) continue;
            folderCounts[folderPath]++;
            if (!folderNames.contains(folderPath)) {
                QString folderName = item->data(ROLE_MODEL_ROOT_NAME).toString();
                if (folderName.isEmpty()) folderName = folderPath;
                folderNames.insert(folderPath, folderName);
            }
        }

        QStringList folderPaths = folderNames.keys();
        std::sort(folderPaths.begin(), folderPaths.end(), [&](const QString &a, const QString &b) {
            return collator.compare(folderNames.value(a), folderNames.value(b)) < 0;
        });

        for (const QString &folderPath : folderPaths) {
            const int folderVisibleCount = folderCounts.value(folderPath, 0);
            if (folderVisibleCount <= 0 && !optShowEmptyCollections) continue;

            const QString folderKey = "folder:" + folderPath;
            QTreeWidgetItem *folderNode = makeTreeParent(nullptr, folderKey, folderKey, folderNames.value(folderPath), folderVisibleCount);
            folderNode->setData(0, ROLE_IS_FOLDER_HEADER, true);
            folderNode->setData(0, ROLE_MODEL_ROOT_NAME, folderNames.value(folderPath));
            addCollectionGroup(folderNode, folderPath);
        }
    } else {
        addCollectionGroup(nullptr, QString());
    }

    ui->collectionTree->setAnimated(true);
    ui->collectionTree->setUpdatesEnabled(true);

    // 恢复滚动条
    // 使用 QTimer 0 延时，确保 UI 布局完成后再滚动，否则可能滚不到位
    if (scrollPos > 0) {
        QTimer::singleShot(0, this, [this, scrollPos](){
            ui->collectionTree->verticalScrollBar()->setValue(scrollPos);
        });
    }
}

void MainWindow::cancelPendingTasks()
{
    // QThreadPool::clear() 会移除所有尚未开始的任务
    // 正在运行的任务无法强制停止，但它们很快就会结束。
    // 注意：backgroundThreadPool 用于侧边栏/收藏夹等静默缩略图加载，
    // 普通页面刷新不应清掉它，否则模型列表会留下占位叉号。
    if (threadPool) threadPool->clear();

    // 可选：如果之前的逻辑有正在下载的队列，也可以在这里清空
    // downloadQueue.clear();
    // isDownloading = false;
}

void MainWindow::syncTreeSelection(const QString &filePath)
{
    if (filePath.isEmpty()) return;

    bool found = false;

    std::function<bool(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem *node) {
        if (!node) return false;
        if (node->data(0, ROLE_FILE_PATH).toString() == filePath) {
            QTreeWidgetItem *parent = node->parent();
            while (parent) {
                parent->setExpanded(true);
                parent = parent->parent();
            }
            ui->collectionTree->setCurrentItem(node);
            node->setSelected(true);
            ui->collectionTree->scrollToItem(node, QAbstractItemView::PositionAtCenter);
            return true;
        }
        for (int i = 0; i < node->childCount(); ++i) {
            if (visit(node->child(i))) return true;
        }
        return false;
    };

    for (int i = 0; i < ui->collectionTree->topLevelItemCount(); ++i) {
        if (visit(ui->collectionTree->topLevelItem(i))) {
            found = true;
            break;
        }
    }
    Q_UNUSED(found);
}

void MainWindow::onMenuSwitchToAbout()
{
    ui->rootStack->setCurrentWidget(aboutPage);
}

void MainWindow::onCheckUpdateClicked()
{
    startAppUpdateCheck(false);
}

void MainWindow::startAppUpdateCheck(bool silentIfLatest)
{
    if (appUpdateChecker) {
        appUpdateChecker->checkForUpdates(GITHUB_REPO_API, CURRENT_VERSION, currentUserAgent, silentIfLatest);
    }
}

QString MainWindow::getRandomUserAgent()
{
    return CivitaiClient::randomUserAgent();
}

// 加载缓存
void MainWindow::resumePendingUserGalleryScan()
{
    if (!pendingUserGalleryScan || userGalleryCacheLoading) return;
    pendingUserGalleryScan = false;
    const QListWidgetItem *item = ui->modelList->currentItem();
    const QString path = isModelListItem(item)
                             ? item->data(ROLE_FILE_PATH).toString() : currentMeta.filePath;
    if (!pendingUserGalleryModel.isEmpty() && path != pendingUserGalleryModelPath) return;
    scanForUserImages(pendingUserGalleryModel);
}

void MainWindow::loadUserGalleryCache() {
    userGalleryCacheLoading = true;
    const int token = ++userGalleryCacheLoadToken;
    const QString cachePath = qApp->applicationDirPath() + "/config/user_gallery_cache.json";
    const bool splitOnNewline = optSplitOnNewline;
    const QStringList filterTags = optFilterTags;
    auto *watcher = new QFutureWatcher<QMap<QString, UserImageInfo>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, token, splitOnNewline, filterTags]() {
        auto result = watcher->result();
        watcher->deleteLater();
        if (isShuttingDown || token != userGalleryCacheLoadToken) return;
        if (splitOnNewline != optSplitOnNewline || filterTags != optFilterTags) {
            loadUserGalleryCache();
            return;
        }
        imageCache = std::move(result);
        userGalleryCacheLoading = false;
        refreshModelUsageStatsAsync();
        resumePendingUserGalleryScan();
    });
    watcher->setFuture(QtConcurrent::run(backgroundThreadPool, [cachePath, splitOnNewline, filterTags]() {
        return UserGalleryCache::load(cachePath, splitOnNewline, filterTags);
    }));
}

// 保存缓存
void MainWindow::saveUserGalleryCache() {
    UserGalleryCache::save(qApp->applicationDirPath() + "/config/user_gallery_cache.json", imageCache);
}

void MainWindow::updateModelListNames()
{
    // 暂时关闭排序，防止修改文本时列表乱跳
    ui->modelList->setSortingEnabled(false);

    for(int i = 0; i < ui->modelList->count(); ++i) {
        QListWidgetItem *item = ui->modelList->item(i);
        if (!isModelListItem(item)) continue;

        // 获取文件名 (BaseName)
        QString baseName = item->data(ROLE_MODEL_NAME).toString();
        // 获取 Civitai 名
        QString civitName = item->data(ROLE_CIVITAI_NAME).toString();

        if (optUseCivitaiName && !civitName.isEmpty()) {
            item->setText(civitName);
        } else {
            item->setText(baseName);
        }
        applyModelUserNoteData(item);
    }

    // 恢复排序 (executeSort 会处理)
}
