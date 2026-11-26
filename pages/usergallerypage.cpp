#include "pages/usergallerypage.h"
#include "ui_usergallerypage.h"

#include "core/modelitemroles.h"
#include "utils/applanguage.h"
#include "utils/gallerymetadata.h"
#include "utils/imageloader.h"
#include "utils/imagepresentation.h"
#include "utils/styleconstants.h"
#include "utils/tagutils.h"
#include "widgets/flowlayout.h"
#include "widgets/tagflowwidget.h"

#include <QAbstractScrollArea>
#include <QClipboard>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QImageReader>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QPair>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSharedPointer>
#include <QSignalBlocker>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <atomic>
#include <utility>

namespace {
FlowChipButton *setupChipButton(FlowChipButton *button,
                                const QString &tooltip,
                                const QString &tagSource = QString())
{
    button->setProperty("class", "filterChip");
    if (!tagSource.isEmpty()) {
        button->setProperty("tagSource", tagSource);
    }
    button->setCursor(Qt::PointingHandCursor);
    if (!tooltip.isEmpty()) {
        button->setToolTip(tooltip);
    }
    return button;
}

void clearFlowLayout(QLayout *layout)
{
    if (!layout) return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }
}
} // namespace

UserGalleryPage::UserGalleryPage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::UserGalleryPage)
{
    ui->setupUi(this);

    // 1. 左侧图库列表视口与滚动配置
    ui->listUserImages->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    ui->listUserImages->viewport()->setObjectName(QStringLiteral("userGalleryViewport"));
    ui->listUserImages->viewport()->setAutoFillBackground(false);
    ui->listUserImages->viewport()->setAttribute(Qt::WA_StyledBackground, true);
    ui->listUserImages->verticalScrollBar()->setSingleStep(40);
    ui->listUserImages->setContextMenuPolicy(Qt::CustomContextMenu);

    m_thumbLoadTimer = new QTimer(this);
    m_thumbLoadTimer->setSingleShot(true);
    connect(m_thumbLoadTimer, &QTimer::timeout, this, &UserGalleryPage::dispatchVisibleThumbLoad);
    connect(ui->listUserImages->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
        scheduleVisibleThumbLoad();
    });
    ui->listUserImages->viewport()->installEventFilter(this);

    // 2. 初始化右侧上方 Tag 流式控件
    m_tagFlowWidget = new TagFlowWidget();
    m_tagFlowWidget->setObjectName("tagFlowContainer");
    m_tagFlowWidget->setAttribute(Qt::WA_TranslucentBackground);
    ui->scrollAreaTags->viewport()->setAutoFillBackground(false);
    ui->scrollAreaTags->setWidget(m_tagFlowWidget);
    ui->tagFlowContainer = nullptr; // setWidget() deletes the Designer placeholder widget
    ui->scrollAreaTags->viewport()->setAutoFillBackground(false);
    connect(ui->scrollAreaTags->verticalScrollBar(), &QScrollBar::valueChanged,
            ui->scrollAreaTags->viewport(), [this]() {
                ui->scrollAreaTags->viewport()->update();
            });

    // 3. 初始化提示词展示工具栏与标签视图
    setupPromptToolbar();

    // 4. 列表交互信号
    connect(ui->listUserImages, &QListWidget::customContextMenuRequested,
            this, &UserGalleryPage::onContextMenuRequested);
    connect(ui->listUserImages, &QListWidget::itemClicked,
            this, &UserGalleryPage::onUserImageClicked);
    connect(ui->listUserImages, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (!item) return;
        const QString path = item->data(ROLE_USER_IMAGE_PATH).toString();
        if (!path.isEmpty()) {
            emit openImageViewerRequested(visibleImagePaths(), path);
        }
    });

    // 5. 顶栏按钮与 Tag 过滤控件
    connect(ui->btnSetSdFolder, &QPushButton::clicked, this, &UserGalleryPage::setSdFolderRequested);
    connect(ui->btnRescanUser, &QPushButton::clicked, this, &UserGalleryPage::rescanRequested);
    connect(ui->btnTranslate, &QPushButton::toggled, this, [this](bool checked) {
        if (checked && (!m_translationMap || m_translationMap->isEmpty())) {
            {
                const QSignalBlocker blocker(ui->btnTranslate);
                ui->btnTranslate->setChecked(false);
            }
            const QMessageBox::StandardButton reply = QMessageBox::question(
                this,
                AppText::tr("未加载翻译"),
                AppText::tr("尚未加载翻译词表 (CSV)。\n是否现在前往设置页面进行设置？\n\n(格式: 英文,中文)"),
                QMessageBox::Yes | QMessageBox::No
            );
            if (reply == QMessageBox::Yes) {
                emit openSettingsForTranslationRequested();
            }
            return;
        }
        if (m_tagFlowWidget) m_tagFlowWidget->setShowTranslation(checked);
    });
    connect(ui->btnClearUserTagSelection, &QPushButton::clicked, this, [this]() {
        m_extraLoraFilters.clear();
        if (m_tagFlowWidget) m_tagFlowWidget->clearSelectedTags();
    });
    if (ui->btnOpenGalleryGraph) {
        connect(ui->btnOpenGalleryGraph, &QPushButton::clicked, this, [this]() {
            QString focusQuery;
            if (m_tagFlowWidget && !m_tagFlowWidget->getSelectedTags().isEmpty()) {
                focusQuery = *m_tagFlowWidget->getSelectedTags().cbegin();
                static const QRegularExpression loraCleanup("^<\\s*(?:lora|lyco)\\s*:\\s*([^:>]+).*>$",
                                                            QRegularExpression::CaseInsensitiveOption);
                const QRegularExpressionMatch m = loraCleanup.match(focusQuery.trimmed());
                if (m.hasMatch()) focusQuery = m.captured(1).trimmed();
            }
            const int scope = (ui->listUserImages && ui->listUserImages->count() > 0) ? 1 : 0;
            emit openGraphRequested(scope, focusQuery);
        });
    }
    connect(ui->comboUserTagSortMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
                TagFlowWidget::SortMode mode = TagFlowWidget::SortByCount;
                if (index == 1) mode = TagFlowWidget::SortAlphabetically;
                else if (index == 2) mode = TagFlowWidget::SortByGivenOrder;
                else if (index == 3) mode = TagFlowWidget::SortByCategory;
                if (m_tagFlowWidget) m_tagFlowWidget->setSortMode(mode);
                if (index == 2) refreshTagFlowStats(false);
            });

    m_tagSearchDebounceTimer = new QTimer(this);
    m_tagSearchDebounceTimer->setSingleShot(true);
    m_tagSearchDebounceTimer->setInterval(120);
    connect(m_tagSearchDebounceTimer, &QTimer::timeout, this, [this]() {
        if (m_tagFlowWidget && ui && ui->editUserTagSearch) {
            m_tagFlowWidget->setSearchText(ui->editUserTagSearch->text());
        }
    });
    connect(ui->editUserTagSearch, &QLineEdit::textChanged, this, [this](const QString &) {
        if (m_tagSearchDebounceTimer) m_tagSearchDebounceTimer->start();
    });
    connect(ui->chkUserTagLoraOnly, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_tagFlowWidget) m_tagFlowWidget->setLoraOnly(checked);
    });
    connect(ui->chkUserTagCurrentImageOnly, &QCheckBox::toggled, this, [this](bool) {
        refreshTagFlowStats(false);
    });
    connect(ui->chkUserTagIncludeNegative, &QCheckBox::toggled, this, [this](bool) {
        refreshTagFlowStats(true, false);
    });
    connect(m_tagFlowWidget, &TagFlowWidget::filterChanged, this, &UserGalleryPage::onTagFilterChanged);

    // 6. 默认分割器比例
    ui->splitterUser->setSizes(QList<int>() << 480 << 435);
    if (ui->splitterParameter) {
        ui->splitterParameter->setStretchFactor(0, 0);
        ui->splitterParameter->setStretchFactor(1, 1);
        ui->splitterParameter->setSizes(QList<int>() << 185 << 380);
    }
}

UserGalleryPage::~UserGalleryPage()
{
    shutdown();
    delete ui;
}

void UserGalleryPage::setThreadPools(QThreadPool *thumbPool, QThreadPool *backgroundPool)
{
    m_threadPool = thumbPool;
    m_backgroundThreadPool = backgroundPool;
}

void UserGalleryPage::setTranslationMap(const QHash<QString, QString> *map)
{
    m_translationMap = map;
    if (m_tagFlowWidget) m_tagFlowWidget->setTranslationMap(map);
    if (m_posTagWidget) m_posTagWidget->setTranslationMap(map);
    if (m_negTagWidget) m_negTagWidget->setTranslationMap(map);
}

void UserGalleryPage::setImageCache(const QMap<QString, UserImageInfo> *cache)
{
    m_imageCache = cache;
}

void UserGalleryPage::setOptions(const UserGalleryOptions &options)
{
    m_options = options;
}

void UserGalleryPage::setPlaceholderIcon(const QIcon &icon)
{
    m_placeholderIcon = icon;
}

void UserGalleryPage::recolorPlaceholders(const QIcon &icon)
{
    m_placeholderIcon = icon;
    if (!ui || !ui->listUserImages) return;
    bool changed = false;
    for (int i = 0; i < ui->listUserImages->count(); ++i) {
        QListWidgetItem *item = ui->listUserImages->item(i);
        if (item && item->data(ROLE_PREVIEW_PLACEHOLDER).toBool()) {
            item->setIcon(QIcon());
            item->setIcon(m_placeholderIcon);
            changed = true;
        }
    }
    if (changed && ui->listUserImages->viewport()) {
        ui->listUserImages->viewport()->update();
    }
}

void UserGalleryPage::restoreFilterState(const UserGalleryFilterState &state)
{
    if (ui->comboUserTagSortMode) {
        const QSignalBlocker blocker(ui->comboUserTagSortMode);
        const int maxIdx = qMax(0, ui->comboUserTagSortMode->count() - 1);
        const int idx = qBound(0, state.userTagSortMode, maxIdx);
        ui->comboUserTagSortMode->setCurrentIndex(idx);
        if (m_tagFlowWidget) {
            TagFlowWidget::SortMode mode = TagFlowWidget::SortByCount;
            if (idx == 1) mode = TagFlowWidget::SortAlphabetically;
            else if (idx == 2) mode = TagFlowWidget::SortByGivenOrder;
            else if (idx == 3) mode = TagFlowWidget::SortByCategory;
            m_tagFlowWidget->setSortMode(mode);
        }
    }
    if (ui->chkUserTagLoraOnly) {
        const QSignalBlocker blocker(ui->chkUserTagLoraOnly);
        ui->chkUserTagLoraOnly->setChecked(state.userTagLoraOnly);
        if (m_tagFlowWidget) m_tagFlowWidget->setLoraOnly(state.userTagLoraOnly);
    }
    if (ui->chkUserTagIncludeNegative) {
        const QSignalBlocker blocker(ui->chkUserTagIncludeNegative);
        ui->chkUserTagIncludeNegative->setChecked(state.userTagIncludeNegative);
    }
    if (ui->comboGalleryPromptSortMode) {
        const QSignalBlocker blocker(ui->comboGalleryPromptSortMode);
        const int maxIdx = qMax(0, ui->comboGalleryPromptSortMode->count() - 1);
        ui->comboGalleryPromptSortMode->setCurrentIndex(qBound(0, state.galleryPromptSortMode, maxIdx));
    }
    if (ui->btnGalleryPromptToggle) {
        const QSignalBlocker blocker(ui->btnGalleryPromptToggle);
        ui->btnGalleryPromptToggle->setChecked(state.galleryTagViewActive);
        setTagViewActive(state.galleryTagViewActive);
    }
}

UserGalleryFilterState UserGalleryPage::filterState() const
{
    UserGalleryFilterState state;
    if (ui->comboUserTagSortMode) state.userTagSortMode = ui->comboUserTagSortMode->currentIndex();
    if (ui->chkUserTagLoraOnly) state.userTagLoraOnly = ui->chkUserTagLoraOnly->isChecked();
    if (ui->chkUserTagIncludeNegative) state.userTagIncludeNegative = ui->chkUserTagIncludeNegative->isChecked();
    if (ui->comboGalleryPromptSortMode) state.galleryPromptSortMode = ui->comboGalleryPromptSortMode->currentIndex();
    state.galleryTagViewActive = m_tagViewActive;
    return state;
}

void UserGalleryPage::invalidateAndReset()
{
    ++m_generation;
    ++m_layoutGeneration;
    resetThumbLoading();
}

void UserGalleryPage::shutdown()
{
    m_isShuttingDown = true;
    if (m_tagSearchDebounceTimer) m_tagSearchDebounceTimer->stop();
    if (m_thumbLoadTimer) m_thumbLoadTimer->stop();
}

void UserGalleryPage::clearGalleryView()
{
    m_itemByPath.clear();
    if (ui && ui->listUserImages) {
        ui->listUserImages->clear();
    }
    clearPromptView();
    if (m_tagFlowWidget) {
        m_tagFlowWidget->setData({});
    }
    resetThumbLoading();
}

void UserGalleryPage::showInvalidFolderHint()
{
    if (ui && ui->textUserPrompt) {
        ui->textUserPrompt->setText(
            AppText::tr("<span style='color:orange'>请先点击右上方按钮设置并启用 Stable Diffusion 图片输出目录。</span>"));
    }
}

int UserGalleryPage::imageCount() const
{
    return (ui && ui->listUserImages) ? ui->listUserImages->count() : 0;
}

QString UserGalleryPage::currentImagePath() const
{
    return m_currentImagePath;
}

QStringList UserGalleryPage::visibleImagePaths() const
{
    QStringList result;
    QSet<QString> seen;
    if (!ui || !ui->listUserImages) return result;

    for (int index = 0; index < ui->listUserImages->count(); ++index) {
        QListWidgetItem *item = ui->listUserImages->item(index);
        if (!item || item->isHidden()) continue;
        const QFileInfo info(item->data(ROLE_USER_IMAGE_PATH).toString());
        if (!info.exists() || !info.isFile()) continue;
        const QString absolutePath = info.absoluteFilePath();
        const QString key = QDir::cleanPath(absolutePath).toCaseFolded();
        if (seen.contains(key)) continue;
        seen.insert(key);
        result.append(absolutePath);
    }
    return result;
}

QSet<QString> UserGalleryPage::allImagePaths() const
{
    QSet<QString> paths;
    if (!ui || !ui->listUserImages) return paths;
    for (int i = 0; i < ui->listUserImages->count(); ++i) {
        if (QListWidgetItem *it = ui->listUserImages->item(i)) {
            const QString p = it->data(ROLE_USER_IMAGE_PATH).toString();
            if (!p.isEmpty()) paths.insert(p);
        }
    }
    return paths;
}

bool UserGalleryPage::isImageNsfw(const QString &prompt, const QStringList &cleanTags) const
{
    return GalleryMetadata::matchesPositivePromptNsfwRules(prompt, cleanTags, m_options.nsfwRules);
}

bool UserGalleryPage::isImageNsfwByPath(const QString &imagePath) const
{
    if (imagePath.isEmpty() || m_options.nsfwRules.isEmpty()) return false;

    if (QListWidgetItem *directItem = m_itemByPath.value(imagePath, nullptr)) {
        return isImageNsfw(directItem->data(ROLE_USER_IMAGE_PROMPT).toString(),
                           directItem->data(ROLE_USER_IMAGE_TAGS).toStringList());
    }

    if (ui && ui->listUserImages) {
        const QString cleanTarget = QDir::cleanPath(QFileInfo(imagePath).absoluteFilePath());
        for (int i = 0; i < ui->listUserImages->count(); ++i) {
            QListWidgetItem *item = ui->listUserImages->item(i);
            if (!item) continue;
            const QString itemPath = item->data(ROLE_USER_IMAGE_PATH).toString();
            if (itemPath == imagePath ||
                QDir::cleanPath(QFileInfo(itemPath).absoluteFilePath()).compare(cleanTarget, Qt::CaseInsensitive) == 0) {
                return isImageNsfw(item->data(ROLE_USER_IMAGE_PROMPT).toString(),
                                   item->data(ROLE_USER_IMAGE_TAGS).toStringList());
            }
        }
    }
    return false;
}

void UserGalleryPage::refreshNsfwDisplay()
{
    if (!ui || !ui->listUserImages || ui->listUserImages->count() == 0) return;

    resetThumbLoading();
    ui->listUserImages->setUpdatesEnabled(false);
    for (int i = 0; i < ui->listUserImages->count(); ++i) {
        QListWidgetItem *item = ui->listUserImages->item(i);
        if (!item) continue;
        const bool isNsfw = isImageNsfw(item->data(ROLE_USER_IMAGE_PROMPT).toString(),
                                        item->data(ROLE_USER_IMAGE_TAGS).toStringList());
        item->setData(ROLE_USER_IMAGE_NSFW, isNsfw);
        item->setIcon(m_placeholderIcon);
        item->setData(ROLE_PREVIEW_PLACEHOLDER, true);
    }
    ui->listUserImages->setUpdatesEnabled(true);
    refreshTagFlowStats(true, false);
    scheduleVisibleThumbLoad();
}

void UserGalleryPage::resetThumbLoading()
{
    m_queuedThumbPaths.clear();
    m_loadedThumbPaths.clear();
    m_failedThumbLoads.clear();
    if (m_thumbLoadTimer) {
        m_thumbLoadTimer->stop();
    }
}

void UserGalleryPage::scheduleVisibleThumbLoad()
{
    if (!m_thumbLoadTimer) return;
    m_thumbLoadTimer->start(30);
}

void UserGalleryPage::dispatchVisibleThumbLoad()
{
    if (!ui->listUserImages || ui->listUserImages->count() == 0 || !m_threadPool) return;

    const QRect visibleRect = ui->listUserImages->viewport()->rect();
    if (visibleRect.isEmpty()) return;

    const QRect prefetchRect = visibleRect.adjusted(-visibleRect.width(),
                                                    -visibleRect.height(),
                                                    visibleRect.width(),
                                                    visibleRect.height());
    const QPoint centerPoint = visibleRect.center();

    struct ThumbCandidate {
        int priority = 2;
        int distance = 0;
        QString path;
    };

    QList<ThumbCandidate> candidates;
    candidates.reserve(ui->listUserImages->count());

    for (int i = 0; i < ui->listUserImages->count(); ++i) {
        QListWidgetItem *item = ui->listUserImages->item(i);
        if (!item || item->isHidden()) continue;

        const QString path = item->data(ROLE_USER_IMAGE_PATH).toString();
        if (path.isEmpty()) continue;
        if (m_loadedThumbPaths.contains(path) || m_queuedThumbPaths.contains(path)) continue;
        if (m_failedThumbLoads.value(path) >= 3) continue;

        const QRect itemRect = ui->listUserImages->visualItemRect(item);
        if (!itemRect.isValid() || itemRect.isEmpty()) continue;

        ThumbCandidate candidate;
        candidate.path = path;
        candidate.distance = qAbs(itemRect.center().y() - centerPoint.y())
                           + qAbs(itemRect.center().x() - centerPoint.x());
        if (itemRect.intersects(visibleRect)) {
            candidate.priority = 0;
        } else if (itemRect.intersects(prefetchRect)) {
            candidate.priority = 1;
        }
        candidates.append(candidate);
    }

    if (candidates.isEmpty()) return;

    const int threadCount = qMax(1, m_threadPool->maxThreadCount());
    const int maxLaunchPerPass = qMax(8, threadCount * 2);
    const int topCount = qMin(candidates.size(), maxLaunchPerPass);
    std::partial_sort(candidates.begin(), candidates.begin() + topCount, candidates.end(),
                      [](const ThumbCandidate &a, const ThumbCandidate &b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.distance < b.distance;
    });

    int launched = 0;
    for (int idx = 0; idx < topCount; ++idx) {
        const ThumbCandidate &candidate = candidates.at(idx);
        if (candidate.priority > 1 && launched >= threadCount) break;

        const QString taskId = QStringLiteral("USERGALLERY|%1|%2")
                                   .arg(m_generation)
                                   .arg(candidate.path);
        auto *task = new IconLoaderTask(candidate.path, 140, 4, this, taskId);
        task->setAutoDelete(true);
        m_threadPool->start(task);
        m_queuedThumbPaths.insert(candidate.path);
        ++launched;
    }
}

void UserGalleryPage::onIconLoaded(const QString &id, const QImage &image)
{
    if (m_isShuttingDown) return;
    const QString userGalleryPrefix = QStringLiteral("USERGALLERY|");
    if (!id.startsWith(userGalleryPrefix)) return;

    const int separator = id.indexOf('|', userGalleryPrefix.size());
    bool tokenOk = false;
    const quint64 token = separator > userGalleryPrefix.size()
                              ? id.mid(userGalleryPrefix.size(), separator - userGalleryPrefix.size()).toULongLong(&tokenOk)
                              : 0;
    const QString filePath = separator >= 0 ? id.mid(separator + 1) : QString();
    if (!tokenOk || token != m_generation || filePath.isEmpty()) return;

    m_queuedThumbPaths.remove(filePath);
    if (image.isNull()) {
        const int failureCount = m_failedThumbLoads.value(filePath) + 1;
        m_failedThumbLoads.insert(filePath, failureCount);
        if (failureCount < 3) scheduleVisibleThumbLoad();
        return;
    }

    QListWidgetItem *item = m_itemByPath.value(filePath, nullptr);
    if (item && item->data(ROLE_USER_IMAGE_PATH).toString() == filePath) {
        const bool blurNsfw = m_options.filterNsfw && m_options.nsfwMode == 1
                           && item->data(ROLE_USER_IMAGE_NSFW).toBool();
        if (blurNsfw) {
            const QPixmap pix = QPixmap::fromImage(image);
            const QPixmap blurred = ImagePresentation::applyNSFWBlur(pix);
            item->setIcon(QIcon(ImagePresentation::applyRoundedMask(blurred, 4)));
        } else {
            item->setIcon(QIcon(QPixmap::fromImage(image)));
        }
        item->setData(ROLE_PREVIEW_PLACEHOLDER, false);
    }
    m_failedThumbLoads.remove(filePath);
    m_loadedThumbPaths.insert(filePath);
    scheduleVisibleThumbLoad();
}

void UserGalleryPage::applyExternalImageUpdate(const QString &filePath,
                                               const QPixmap &originalPix,
                                               const QIcon &originalIcon)
{
    if (!ui || !ui->listUserImages) return;
    bool updated = false;
    QPixmap blurredPix;
    for (int i = 0; i < ui->listUserImages->count(); ++i) {
        QListWidgetItem *item = ui->listUserImages->item(i);
        if (item && item->data(ROLE_USER_IMAGE_PATH).toString() == filePath) {
            const bool blurNsfw = m_options.filterNsfw && m_options.nsfwMode == 1
                               && item->data(ROLE_USER_IMAGE_NSFW).toBool();
            if (blurNsfw) {
                if (blurredPix.isNull()) blurredPix = ImagePresentation::applyNSFWBlur(originalPix);
                item->setIcon(QIcon(ImagePresentation::applyRoundedMask(blurredPix, 4)));
            } else {
                item->setIcon(originalIcon);
            }
            item->setData(ROLE_PREVIEW_PLACEHOLDER, false);
            updated = true;
        }
    }
    if (updated) {
        m_queuedThumbPaths.remove(filePath);
        m_loadedThumbPaths.insert(filePath);
        scheduleVisibleThumbLoad();
    }
}

void UserGalleryPage::startScan(const QStringList &validGalleryPaths,
                                bool isGlobalMode,
                                const QString &displayName,
                                const ModelImageMatcher::ModelUsageInput &modelInput)
{
    m_extraLoraFilters.clear();
    const quint64 scanGeneration = ++m_generation;
    ++m_layoutGeneration;
    clearGalleryView();

    const bool wantSummaryHashMatch = (!isGlobalMode && (m_options.matchMode == 1 || m_options.matchMode == 2));
    const bool strictSummaryHashMatch = (!isGlobalMode && m_options.matchMode == 2);

    QString scanPrefix;
    if (isGlobalMode) {
        scanPrefix = "正在扫描所有本地图片";
    } else if (strictSummaryHashMatch) {
        scanPrefix = QString("正在扫描使用 '%1' 的图片 (严格摘要值匹配)").arg(displayName);
    } else if (wantSummaryHashMatch) {
        scanPrefix = QString("正在扫描使用 '%1' 的图片 (摘要值匹配)").arg(displayName);
    } else {
        scanPrefix = QString("正在扫描使用 '%1' 的图片").arg(displayName);
    }

    emit statusMessage(scanPrefix + "...");

    const QMap<QString, UserImageInfo> currentCacheCopy = m_imageCache ? *m_imageCache : QMap<QString, UserImageInfo>{};
    const int matchMode = m_options.matchMode;
    const bool recursive = m_options.recursive;
    const bool splitOnNewline = m_options.splitOnNewline;
    const QStringList filterTags = m_options.filterTags;
    const bool comfyModelNameFallback = m_options.comfyModelNameFallback;
    auto scannedCount = QSharedPointer<std::atomic<int>>::create(0);
    auto matchedCount = QSharedPointer<std::atomic<int>>::create(0);

    QThreadPool *workerPool = m_backgroundThreadPool ? m_backgroundThreadPool : QThreadPool::globalInstance();
    auto future = QtConcurrent::run(
        workerPool,
        [modelInput, matchMode, isGlobalMode, recursive, splitOnNewline, filterTags,
         currentCacheCopy, validGalleryPaths, scannedCount, matchedCount, comfyModelNameFallback]() {
            const auto candidate = isGlobalMode ? ModelImageMatcher::ModelUsageCandidate{}
                                               : ModelImageMatcher::buildCandidate(modelInput);
            const ModelImageMatcher::MatchOptions options{matchMode, comfyModelNameFallback};
            QList<UserImageInfo> results;
            QMap<QString, UserImageInfo> newCacheUpdates;

            const QDirIterator::IteratorFlag iterFlag = recursive
                ? QDirIterator::Subdirectories
                : QDirIterator::NoIteratorFlags;
            QSet<QString> visited;

            for (const QString &root : validGalleryPaths) {
                QDirIterator it(root, QStringList() << "*.png" << "*.jpg" << "*.jpeg" << "*.webp", QDir::Files, iterFlag);
                while (it.hasNext()) {
                    const QString path = it.next();
                    if (visited.contains(path)) continue;
                    visited.insert(path);
                    scannedCount->fetch_add(1, std::memory_order_relaxed);

                    const QFileInfo fi = it.fileInfo();
                    const qint64 currentModified = fi.lastModified().toMSecsSinceEpoch();

                    UserImageInfo info;
                    bool needParse = true;

                    if (currentCacheCopy.contains(path)) {
                        const UserImageInfo &cachedInfo = currentCacheCopy.value(path);
                        if (cachedInfo.lastModified == currentModified
                            && cachedInfo.parserVersion >= GalleryMetadata::ParserVersion) {
                            info = cachedInfo;
                            needParse = false;
                        }
                    }

                    if (needParse) {
                        info.path = path;
                        info.lastModified = currentModified;
                        GalleryMetadata::parseImage(path, info, splitOnNewline, filterTags);
                        newCacheUpdates.insert(path, info);
                    }

                    if (info.prompt.isEmpty() && info.parameters.isEmpty()) continue;

                    const bool matched = isGlobalMode || ModelImageMatcher::matches(
                        candidate, ModelImageMatcher::imageUsage(info), options);

                    if (matched) {
                        matchedCount->fetch_add(1, std::memory_order_relaxed);
                        results.append(info);
                    }
                }
            }

            std::sort(results.begin(), results.end(), [](const UserImageInfo &a, const UserImageInfo &b) {
                return a.lastModified > b.lastModified;
            });

            return qMakePair(results, newCacheUpdates);
        });

    auto *watcher = new QFutureWatcher<QPair<QList<UserImageInfo>, QMap<QString, UserImageInfo>>>(this);

    auto *scanProgressTimer = new QTimer(this);
    connect(scanProgressTimer, &QTimer::timeout, this,
            [this, scanPrefix, scannedCount, matchedCount, scanGeneration, lastShown = 0]() mutable {
        if (scanGeneration != m_generation) return;
        const int scanned = scannedCount->load(std::memory_order_relaxed);
        if (scanned <= 0) return;
        if (scanned < lastShown + 50) return;
        lastShown = (scanned / 50) * 50;
        const int matched = matchedCount->load(std::memory_order_relaxed);
        emit statusMessage(AppText::tr("%1... 已扫描 %2 张，匹配 %3 张")
                               .arg(scanPrefix)
                               .arg(scanned)
                               .arg(matched));
    });
    scanProgressTimer->start(200);

    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, scanProgressTimer, scanGeneration]() {
        if (scanProgressTimer) {
            scanProgressTimer->stop();
            scanProgressTimer->deleteLater();
        }
        if (scanGeneration != m_generation || m_isShuttingDown) {
            watcher->deleteLater();
            return;
        }

        const auto resultPair = watcher->result();
        QList<UserImageInfo> results = resultPair.first;
        const QMap<QString, UserImageInfo> &newUpdates = resultPair.second;

        if (!newUpdates.isEmpty()) {
            emit cacheEntriesParsed(newUpdates);
        }

        auto pendingResults = QSharedPointer<QList<UserImageInfo>>::create(std::move(results));
        auto nextResultIndex = QSharedPointer<int>::create(0);
        const int resultCount = pendingResults->size();
        auto *batchTimer = new QTimer(this);
        batchTimer->setInterval(0);
        connect(batchTimer, &QTimer::timeout, this,
                [this, pendingResults, nextResultIndex, batchTimer, scanGeneration, resultCount]() {
            if (scanGeneration != m_generation || m_isShuttingDown) {
                batchTimer->stop();
                batchTimer->deleteLater();
                return;
            }

            constexpr int batchSize = 100;
            const int end = qMin(*nextResultIndex + batchSize, resultCount);
            ui->listUserImages->setUpdatesEnabled(false);
            for (; *nextResultIndex < end; ++(*nextResultIndex)) {
                const UserImageInfo &info = pendingResults->at(*nextResultIndex);
                auto *item = new QListWidgetItem();
                item->setData(ROLE_USER_IMAGE_PATH, info.path);
                item->setData(ROLE_USER_IMAGE_PROMPT, info.prompt);
                item->setData(ROLE_USER_IMAGE_NEG, info.negativePrompt);
                item->setData(ROLE_USER_IMAGE_PARAMS, info.parameters);
                item->setData(ROLE_USER_IMAGE_TAGS, info.cleanTags);
                item->setData(ROLE_USER_IMAGE_NEG_TAGS, info.negativeCleanTags);
                item->setData(ROLE_USER_IMAGE_TAG_KEYS,
                              TagUtils::normalizedGalleryTagKeys(info.cleanTags));
                item->setData(ROLE_USER_IMAGE_NEG_KEYS,
                              TagUtils::normalizedGalleryTagKeys(info.negativeCleanTags));
                const bool isNsfw = isImageNsfw(info.prompt, info.cleanTags);
                item->setData(ROLE_USER_IMAGE_NSFW, isNsfw);
                if (m_options.filterNsfw && m_options.nsfwMode == 0 && isNsfw) {
                    item->setHidden(true);
                }
                item->setIcon(m_placeholderIcon);
                item->setData(ROLE_PREVIEW_PLACEHOLDER, true);
                ui->listUserImages->addItem(item);
                if (!info.path.isEmpty()) {
                    m_itemByPath.insert(info.path, item);
                }
            }
            ui->listUserImages->setUpdatesEnabled(true);

            if (*nextResultIndex < resultCount) {
                emit statusMessage(AppText::tr("正在显示扫描结果... %1/%2")
                                       .arg(*nextResultIndex)
                                       .arg(resultCount));
                return;
            }

            batchTimer->stop();
            batchTimer->deleteLater();
            ui->listUserImages->doItemsLayout();
            ui->listUserImages->viewport()->update();
            scheduleVisibleThumbLoad();
            refreshTagFlowStats();
            emit statusMessage(AppText::tr("扫描完成，共 %1 张").arg(resultCount), 3000);
        });
        batchTimer->start();
        watcher->deleteLater();
    });

    watcher->setFuture(future);
}

void UserGalleryPage::refreshTagFlowStats(bool applyGalleryFilter, bool resetGalleryScrollToTop)
{
    QHash<QString, int> tagCountsByKey;
    QHash<QString, QString> tagDisplayText;
    QSet<QString> positiveTagKeys;
    QSet<QString> negativeTagKeys;
    QSet<QString> perImageTagKeys;
    const bool currentOnly = ui->chkUserTagCurrentImageOnly && ui->chkUserTagCurrentImageOnly->isChecked();
    const bool includeNegative = ui->chkUserTagIncludeNegative && ui->chkUserTagIncludeNegative->isChecked();
    const bool hideNsfwItems = m_options.filterNsfw && m_options.nsfwMode == 0;

    auto addTagsFromItem = [&](QListWidgetItem *item) {
        if (!item) return;
        if (hideNsfwItems && item->data(ROLE_USER_IMAGE_NSFW).toBool()) return;
        perImageTagKeys.clear();
        const QStringList positiveTags = item->data(ROLE_USER_IMAGE_TAGS).toStringList();
        for (const QString &tag : positiveTags) {
            if (tag.compare("BREAK", Qt::CaseInsensitive) == 0) continue;
            const QString key = TagUtils::normalizedGalleryTagKey(tag);
            if (key.isEmpty()) continue;
            perImageTagKeys.insert(key);
            positiveTagKeys.insert(key);
            if (!tagDisplayText.contains(key)) tagDisplayText.insert(key, tag);
        }
        if (includeNegative) {
            const QStringList negativeTags = item->data(ROLE_USER_IMAGE_NEG_TAGS).toStringList();
            for (const QString &tag : negativeTags) {
                if (tag.compare("BREAK", Qt::CaseInsensitive) == 0) continue;
                const QString key = TagUtils::normalizedGalleryTagKey(tag);
                if (key.isEmpty()) continue;
                perImageTagKeys.insert(key);
                negativeTagKeys.insert(key);
                if (!tagDisplayText.contains(key)) tagDisplayText.insert(key, tag);
            }
        }
        for (const QString &key : std::as_const(perImageTagKeys)) {
            tagCountsByKey[key]++;
        }
    };

    if (currentOnly) {
        addTagsFromItem(ui->listUserImages->currentItem());
    } else {
        const int count = ui->listUserImages->count();
        tagCountsByKey.reserve(qMin(count * 4, 16384));
        tagDisplayText.reserve(qMin(count * 4, 16384));
        for (int i = 0; i < count; ++i) {
            addTagsFromItem(ui->listUserImages->item(i));
        }
    }

    QMap<QString, int> tagCounts;
    for (auto it = tagCountsByKey.cbegin(); it != tagCountsByKey.cend(); ++it) {
        tagCounts.insert(tagDisplayText.value(it.key(), it.key()), it.value());
    }
    m_tagFlowWidget->setData(tagCounts);

    QHash<QString, TagFlowWidget::VisualRole> visualRoles;
    if (includeNegative) {
        for (const QString &key : std::as_const(negativeTagKeys)) {
            if (!positiveTagKeys.contains(key)) {
                visualRoles.insert(tagDisplayText.value(key, key), TagFlowWidget::VisualNegative);
            }
        }
    }
    m_tagFlowWidget->setTagVisualRoles(visualRoles);

    QStringList promptOrder;
    if (QListWidgetItem *cur = ui->listUserImages->currentItem()) {
        promptOrder = cur->data(ROLE_USER_IMAGE_TAGS).toStringList();
        if (includeNegative) promptOrder += cur->data(ROLE_USER_IMAGE_NEG_TAGS).toStringList();
    }
    m_tagFlowWidget->setGivenOrder(promptOrder);

    if (applyGalleryFilter) {
        applyTagFilter(m_tagFlowWidget->getSelectedTags(), resetGalleryScrollToTop);
    }
}

void UserGalleryPage::onUserImageClicked(QListWidgetItem *item)
{
    if (!item) return;

    const QString path = item->data(ROLE_USER_IMAGE_PATH).toString();
    const QString prompt = item->data(ROLE_USER_IMAGE_PROMPT).toString();
    const QString neg = item->data(ROLE_USER_IMAGE_NEG).toString();
    const QString params = item->data(ROLE_USER_IMAGE_PARAMS).toString();

    m_currentImagePath = path;
    m_currentPrompt = prompt;
    m_currentNegative = neg;
    m_currentParams = params;

    const QString safePrompt = prompt.toHtmlEscaped();
    const QString safeNeg = neg.toHtmlEscaped();
    QStringList paramLines;
    for (const QString &line : GalleryMetadata::splitParameterLines(params)) {
        paramLines << line.toHtmlEscaped();
    }
    const QString paramsHtml = paramLines.join(QStringLiteral("<br>"));

    QString resolutionHtml;
    {
        QImageReader reader(path);
        const QSize sz = reader.size();
        if (sz.isValid()) {
            resolutionHtml = QString("<b>Resolution / 分辨率:</b> %1 × %2<br>")
                                 .arg(sz.width()).arg(sz.height());
        }
    }

    const QString html = QString(
                             "<style>"
                             "  .content { white-space: pre-wrap; }"
                             "</style>"
                             "<p><b><span style='color:%4'>Positive:</span></b><br>"
                             "<span class='content'>%1</span></p>"
                             "<p><b><span style='color:%5'>Negative:</span></b><br>"
                             "<span class='content'>%2</span></p>"
                             "<hr style='background-color:#444; height:1px; border:none;'>"
                             "<p><b><span style='color:%6'>Parameters:</span></b><br>"
                             "<span style='color:%7; font-size:11px; font-family:Consolas, monospace;'>%8%3</span></p>"
                             ).arg(safePrompt, safeNeg, paramsHtml,
                                   AppStyle::AccentBlue(), AppStyle::HtmlNegative(),
                                   AppStyle::HtmlSubtle(), AppStyle::HtmlDim(), resolutionHtml);

    ui->textUserPrompt->setHtml(html);

    if (m_tagViewActive) {
        updatePromptTagView();
    }

    emit imageSelected(path);

    const bool promptOrderSort = ui->comboUserTagSortMode && ui->comboUserTagSortMode->currentIndex() == 2;
    if ((ui->chkUserTagCurrentImageOnly && ui->chkUserTagCurrentImageOnly->isChecked()) || promptOrderSort) {
        refreshTagFlowStats(false);
    }
}

void UserGalleryPage::setupPromptToolbar()
{
    if (m_posTagWidget) return;

    m_posTagWidget = new TagFlowWidget(ui->galleryPosTagContainer);
    m_posTagWidget->setObjectName("galleryPosTagWidget");
    m_posTagWidget->setAttribute(Qt::WA_TranslucentBackground);
    m_posTagWidget->setEditingEnabled(false);
    ui->layoutGalleryPosTags->addWidget(m_posTagWidget);

    m_negTagWidget = new TagFlowWidget(ui->galleryNegTagContainer);
    m_negTagWidget->setObjectName("galleryNegTagWidget");
    m_negTagWidget->setAttribute(Qt::WA_TranslucentBackground);
    m_negTagWidget->setDefaultVisualRole(TagFlowWidget::VisualNegative);
    m_negTagWidget->setEditingEnabled(false);
    ui->layoutGalleryNegTags->addWidget(m_negTagWidget);

    if (ui->galleryParamsChipContainer) {
        m_paramsChipLayout = new FlowLayout(ui->galleryParamsChipContainer, 0, 6, 6);
    }

    if (ui->comboGalleryPromptSortMode) {
        connect(ui->comboGalleryPromptSortMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) {
                    applyPromptSortMode();
                });
    }

    if (ui->btnGalleryPromptTranslate) {
        connect(ui->btnGalleryPromptTranslate, &QPushButton::toggled, this, [this](bool checked) {
            if (checked && (!m_translationMap || m_translationMap->isEmpty())) {
                QMessageBox::warning(this, AppText::tr("提示"), AppText::tr("未加载翻译词表，请在设置中配置 CSV 文件。"));
                const QSignalBlocker blocker(ui->btnGalleryPromptTranslate);
                ui->btnGalleryPromptTranslate->setChecked(false);
                return;
            }
            if (m_posTagWidget) m_posTagWidget->setShowTranslation(checked);
            if (m_negTagWidget) m_negTagWidget->setShowTranslation(checked);
        });
    }

    if (ui->btnGalleryPromptSelectAll) {
        connect(ui->btnGalleryPromptSelectAll, &QPushButton::clicked, this, [this]() {
            if (m_posTagWidget) m_posTagWidget->selectAllVisibleTags();
            if (m_negTagWidget) m_negTagWidget->selectAllVisibleTags();
        });
    }

    if (ui->btnGalleryPromptDeselect) {
        connect(ui->btnGalleryPromptDeselect, &QPushButton::clicked, this, [this]() {
            if (m_posTagWidget) m_posTagWidget->clearSelectedTags();
            if (m_negTagWidget) m_negTagWidget->clearSelectedTags();
        });
    }

    if (ui->btnGalleryPromptToggle) {
        connect(ui->btnGalleryPromptToggle, &QPushButton::toggled, this, [this](bool tagView) {
            setTagViewActive(tagView);
        });
    }

    auto installAntiScrollLeak = [this](QWidget *w) {
        if (!w) return;
        w->installEventFilter(this);
        if (auto *scrollArea = qobject_cast<QAbstractScrollArea*>(w)) {
            if (scrollArea->viewport()) scrollArea->viewport()->installEventFilter(this);
        }
    };

    installAntiScrollLeak(ui->listUserImages);
    installAntiScrollLeak(ui->scrollAreaTags);
    installAntiScrollLeak(ui->scrollAreaGalleryPromptTags);
    installAntiScrollLeak(ui->textUserPrompt);
    installAntiScrollLeak(m_posTagWidget);
    installAntiScrollLeak(m_negTagWidget);
    installAntiScrollLeak(m_tagFlowWidget);
    if (ui->galleryPromptTagsContent) ui->galleryPromptTagsContent->installEventFilter(this);
    if (ui->galleryParamsChipContainer) ui->galleryParamsChipContainer->installEventFilter(this);
    if (ui->lblGalleryParamsPlain) ui->lblGalleryParamsPlain->installEventFilter(this);

    setTagViewActive(false);
}

void UserGalleryPage::setTagViewActive(bool tagView)
{
    m_tagViewActive = tagView;
    if (ui->galleryPromptStack) {
        ui->galleryPromptStack->setCurrentIndex(tagView ? 1 : 0);
    }
    if (ui->comboGalleryPromptSortMode) ui->comboGalleryPromptSortMode->setVisible(tagView);
    if (ui->btnGalleryPromptTranslate) ui->btnGalleryPromptTranslate->setVisible(tagView);
    if (ui->btnGalleryPromptSelectAll) ui->btnGalleryPromptSelectAll->setVisible(tagView);
    if (ui->btnGalleryPromptDeselect) ui->btnGalleryPromptDeselect->setVisible(tagView);

    if (ui->btnGalleryPromptToggle) {
        ui->btnGalleryPromptToggle->setText(tagView ? AppText::tr("文本") : AppText::tr("标签"));
        ui->btnGalleryPromptToggle->setToolTip(tagView ? AppText::tr("切换为纯文本视图") : AppText::tr("切换为标签视图"));
    }

    if (tagView) {
        updatePromptTagView();
    }
}

void UserGalleryPage::updatePromptTagView()
{
    if (!m_posTagWidget || !m_negTagWidget) return;

    QMap<QString, int> posCounts;
    TagUtils::collectPromptTagStats(m_currentPrompt, m_options.splitOnNewline, posCounts, &m_posTagOrder);

    QMap<QString, int> negCounts;
    TagUtils::collectPromptTagStats(m_currentNegative, m_options.splitOnNewline, negCounts, &m_negTagOrder);

    m_posTagWidget->setData(posCounts);
    m_negTagWidget->setData(negCounts);

    applyPromptSortMode();

    clearFlowLayout(m_paramsChipLayout);

    UserImageInfo parsedInfo;
    if (!m_currentImagePath.isEmpty() && m_imageCache && m_imageCache->contains(m_currentImagePath)) {
        parsedInfo = m_imageCache->value(m_currentImagePath);
    }
    if (!parsedInfo.parsedModelsReady ||
        parsedInfo.prompt != m_currentPrompt ||
        parsedInfo.parameters != m_currentParams) {
        parsedInfo.prompt = m_currentPrompt;
        parsedInfo.parameters = m_currentParams;
        GalleryMetadata::populateParsedModelFields(parsedInfo);
    }
    const QString modelName = parsedInfo.checkpointName;
    const QString modelHash = parsedInfo.checkpointHash;
    const QList<ParsedLoraEntry> &loras = parsedInfo.parsedLoras;

    if (m_paramsChipLayout && ui->galleryParamsChipContainer) {
        if (!modelName.isEmpty() || !modelHash.isEmpty()) {
            const QString displayName = modelName.isEmpty() ? modelHash : modelName;
            QString labelText = QString("📦 %1").arg(displayName);
            if (!modelHash.isEmpty() && !modelName.isEmpty()) {
                labelText += QString(" (%1)").arg(modelHash);
            }
            FlowChipButton *chipBtn = setupChipButton(
                new FlowChipButton(labelText, ui->galleryParamsChipContainer),
                AppText::tr("底模: %1 (点击跳转)").arg(displayName),
                QStringLiteral("model"));
            connect(chipBtn, &QPushButton::clicked, this, [this, modelName, modelHash]() {
                emit jumpToModelRequested(modelName, modelHash);
            });
            m_paramsChipLayout->addWidget(chipBtn);
        }

        for (const ParsedLoraEntry &lora : loras) {
            QString labelText = QString("🏷️ %1").arg(lora.name);
            if (!lora.weight.isEmpty()) {
                labelText += QString(" [%1]").arg(lora.weight);
            }
            if (!lora.hash.isEmpty()) {
                labelText += QString(" (%1)").arg(lora.hash);
            }
            FlowChipButton *chipBtn = setupChipButton(
                new FlowChipButton(labelText, ui->galleryParamsChipContainer),
                AppText::tr("LoRA: %1 (点击跳转)").arg(lora.name));
            connect(chipBtn, &QPushButton::clicked, this, [this, name = lora.name, hash = lora.hash]() {
                emit jumpToModelRequested(name, hash);
            });
            m_paramsChipLayout->addWidget(chipBtn);
        }
    }

    if (ui->lblGalleryParamsPlain) {
        QString plainHtml;

        if (!m_currentImagePath.isEmpty()) {
            QImageReader reader(m_currentImagePath);
            const QSize sz = reader.size();
            if (sz.isValid()) {
                plainHtml += QString("<div style='margin: 3px 0; word-break: break-all;'><b>Resolution / 分辨率:</b> %1 × %2</div>")
                                 .arg(sz.width()).arg(sz.height());
            }
        }

        for (const QString &t : GalleryMetadata::splitParameterLines(m_currentParams)) {
            if (t.startsWith("Lora hashes:", Qt::CaseInsensitive)) continue;
            if (t.startsWith("ComfyUI LoRAs:", Qt::CaseInsensitive)) continue;
            if (t.startsWith("ComfyUI Lora hashes:", Qt::CaseInsensitive)) continue;
            if (t.startsWith("Model:", Qt::CaseInsensitive)) continue;
            if (t.startsWith("Model hash:", Qt::CaseInsensitive)) continue;
            if (t.startsWith("Checkpoint:", Qt::CaseInsensitive)) continue;
            plainHtml += QString("<div style='margin: 2px 0; word-break: break-all;'>%1</div>").arg(t.toHtmlEscaped());
        }

        ui->lblGalleryParamsPlain->setText(plainHtml);
        ui->lblGalleryParamsPlain->setVisible(!plainHtml.trimmed().isEmpty());
    }

    if (ui->galleryParamsChipContainer) {
        const bool hasChips = (m_paramsChipLayout && m_paramsChipLayout->count() > 0);
        ui->galleryParamsChipContainer->setVisible(hasChips);
        ui->galleryParamsChipContainer->updateGeometry();
    }

    if (ui->lblGalleryParamsTitle) {
        const bool hasParams = (!m_currentParams.trimmed().isEmpty() ||
                                (m_paramsChipLayout && m_paramsChipLayout->count() > 0));
        ui->lblGalleryParamsTitle->setVisible(hasParams);
    }

    if (ui->lblGalleryPosTitle) ui->lblGalleryPosTitle->setVisible(!posCounts.isEmpty());
    if (ui->galleryPosTagContainer) ui->galleryPosTagContainer->setVisible(!posCounts.isEmpty());
    if (ui->lblGalleryNegTitle) ui->lblGalleryNegTitle->setVisible(!negCounts.isEmpty());
    if (ui->galleryNegTagContainer) ui->galleryNegTagContainer->setVisible(!negCounts.isEmpty());
}

void UserGalleryPage::applyPromptSortMode()
{
    if (!m_posTagWidget || !m_negTagWidget || !ui->comboGalleryPromptSortMode) return;

    TagFlowWidget::SortMode mode = TagFlowWidget::SortByGivenOrder;
    const int index = ui->comboGalleryPromptSortMode->currentIndex();
    if (index == 1) mode = TagFlowWidget::SortByCount;
    else if (index == 2) mode = TagFlowWidget::SortAlphabetically;
    else if (index == 3) mode = TagFlowWidget::SortByCategory;

    m_posTagWidget->setGivenOrder(m_posTagOrder);
    m_negTagWidget->setGivenOrder(m_negTagOrder);
    m_posTagWidget->setSortMode(mode);
    m_negTagWidget->setSortMode(mode);
}

void UserGalleryPage::clearPromptView()
{
    m_currentPrompt.clear();
    m_currentNegative.clear();
    m_currentParams.clear();
    m_currentImagePath.clear();
    m_posTagOrder.clear();
    m_negTagOrder.clear();

    if (ui->textUserPrompt) ui->textUserPrompt->clear();
    if (m_posTagWidget) m_posTagWidget->setData({});
    if (m_negTagWidget) m_negTagWidget->setData({});
    clearFlowLayout(m_paramsChipLayout);
    if (ui->lblGalleryParamsPlain) {
        ui->lblGalleryParamsPlain->clear();
    }
}

bool UserGalleryPage::isCursorInsideScrollAreas() const
{
    const QPoint globalPos = QCursor::pos();
    auto isInside = [&globalPos](QWidget *w) -> bool {
        if (!w || !w->isVisible()) return false;
        return w->rect().contains(w->mapFromGlobal(globalPos));
    };

    if (isInside(ui->listUserImages)) return true;
    if (isInside(ui->scrollAreaTags)) return true;
    if (isInside(ui->textUserPrompt)) return true;
    if (isInside(ui->scrollAreaGalleryPromptTags)) return true;

    return false;
}

bool UserGalleryPage::isScrollChild(QObject *watched) const
{
    QWidget *w = qobject_cast<QWidget*>(watched);
    if (!w) return false;

    if (ui->listUserImages && (w == ui->listUserImages || ui->listUserImages->isAncestorOf(w))) return true;
    if (ui->scrollAreaTags && (w == ui->scrollAreaTags || ui->scrollAreaTags->isAncestorOf(w))) return true;
    if (ui->textUserPrompt && (w == ui->textUserPrompt || ui->textUserPrompt->isAncestorOf(w))) return true;
    if (ui->scrollAreaGalleryPromptTags && (w == ui->scrollAreaGalleryPromptTags || ui->scrollAreaGalleryPromptTags->isAncestorOf(w))) return true;

    return false;
}

void UserGalleryPage::handleWheelScroll(QObject *watched, QWheelEvent *event)
{
    if (!event) return;

    QScrollBar *vBar = nullptr;
    QWidget *w = qobject_cast<QWidget*>(watched);

    if (w) {
        if (ui->listUserImages && (w == ui->listUserImages || ui->listUserImages->isAncestorOf(w))) {
            vBar = ui->listUserImages->verticalScrollBar();
        } else if (ui->scrollAreaTags && (w == ui->scrollAreaTags || ui->scrollAreaTags->isAncestorOf(w))) {
            vBar = ui->scrollAreaTags->verticalScrollBar();
        } else if (ui->textUserPrompt && (w == ui->textUserPrompt || ui->textUserPrompt->isAncestorOf(w))) {
            vBar = ui->textUserPrompt->verticalScrollBar();
        } else if (ui->scrollAreaGalleryPromptTags && (w == ui->scrollAreaGalleryPromptTags || ui->scrollAreaGalleryPromptTags->isAncestorOf(w))) {
            vBar = ui->scrollAreaGalleryPromptTags->verticalScrollBar();
        }
    }

    if (!vBar) {
        const QPoint globalPos = QCursor::pos();
        if (ui->scrollAreaGalleryPromptTags && ui->scrollAreaGalleryPromptTags->isVisible() &&
            ui->scrollAreaGalleryPromptTags->rect().contains(ui->scrollAreaGalleryPromptTags->mapFromGlobal(globalPos))) {
            vBar = ui->scrollAreaGalleryPromptTags->verticalScrollBar();
        } else if (ui->textUserPrompt && ui->textUserPrompt->isVisible() &&
                   ui->textUserPrompt->rect().contains(ui->textUserPrompt->mapFromGlobal(globalPos))) {
            vBar = ui->textUserPrompt->verticalScrollBar();
        } else if (ui->scrollAreaTags && ui->scrollAreaTags->isVisible() &&
                   ui->scrollAreaTags->rect().contains(ui->scrollAreaTags->mapFromGlobal(globalPos))) {
            vBar = ui->scrollAreaTags->verticalScrollBar();
        } else if (ui->listUserImages && ui->listUserImages->isVisible() &&
                   ui->listUserImages->rect().contains(ui->listUserImages->mapFromGlobal(globalPos))) {
            vBar = ui->listUserImages->verticalScrollBar();
        }
    }

    if (vBar && vBar->isEnabled() && vBar->maximum() > vBar->minimum()) {
        const int delta = event->angleDelta().y();
        if (delta != 0) {
            int step = vBar->singleStep();
            if (step <= 0) step = 20;
            const double stepsToScroll = static_cast<double>(delta) / 120.0 * 3.0;
            int deltaValue = qRound(-stepsToScroll * step);
            if (deltaValue == 0) deltaValue = (delta > 0) ? -step : step;
            vBar->setValue(vBar->value() + deltaValue);
        }
    }

    event->accept();
}

bool UserGalleryPage::eventFilter(QObject *watched, QEvent *event)
{
    if (ui->listUserImages && watched == ui->listUserImages->viewport() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        scheduleVisibleThumbLoad();
    }

    if (event->type() == QEvent::Wheel && isScrollChild(watched)) {
        handleWheelScroll(watched, static_cast<QWheelEvent*>(event));
        return true;
    }

    return QWidget::eventFilter(watched, event);
}

void UserGalleryPage::onTagFilterChanged(const QSet<QString> &selectedTags)
{
    if (selectedTags.isEmpty()) {
        m_extraLoraFilters.clear();
    }
    applyTagFilter(selectedTags, true);
}

void UserGalleryPage::filterBySingleTagOrLora(const QString &tagOrLora, bool isLora)
{
    if (!m_tagFlowWidget) return;
    if (isLora) {
        m_extraLoraFilters = {tagOrLora};
        if (ui->editUserTagSearch) {
            const QSignalBlocker blocker(ui->editUserTagSearch);
            ui->editUserTagSearch->setText(tagOrLora);
        }
        if (m_tagSearchDebounceTimer) m_tagSearchDebounceTimer->stop();
        m_tagFlowWidget->setSearchText(tagOrLora);
        m_tagFlowWidget->clearSelectedTags();
        applyTagFilter({}, true);
    } else {
        m_extraLoraFilters.clear();
        if (ui->editUserTagSearch) {
            const QSignalBlocker blocker(ui->editUserTagSearch);
            ui->editUserTagSearch->clear();
        }
        if (m_tagSearchDebounceTimer) m_tagSearchDebounceTimer->stop();
        m_tagFlowWidget->setSearchText(QString());
        m_tagFlowWidget->setSelectedTags({tagOrLora});
        applyTagFilter(m_tagFlowWidget->getSelectedTags(), true);
    }
}

void UserGalleryPage::filterByMultiTagsAndLoras(const QStringList &tags, const QStringList &loras)
{
    if (!m_tagFlowWidget) return;
    m_extraLoraFilters = loras;
    if (ui->editUserTagSearch) {
        const QSignalBlocker blocker(ui->editUserTagSearch);
        ui->editUserTagSearch->clear();
    }
    if (m_tagSearchDebounceTimer) m_tagSearchDebounceTimer->stop();
    m_tagFlowWidget->setSearchText(QString());
    QStringList selTags;
    for (const QString &t : tags) {
        const QString clean = t.trimmed();
        if (!clean.isEmpty() && !selTags.contains(clean)) selTags.append(clean);
    }
    m_tagFlowWidget->setSelectedTags(selTags);
    applyTagFilter(m_tagFlowWidget->getSelectedTags(), true);
}

void UserGalleryPage::applyTagFilter(const QSet<QString> &selectedTags, bool resetScrollToTop)
{
    const quint64 layoutGeneration = ++m_layoutGeneration;
    const int previousVerticalScroll = ui->listUserImages->verticalScrollBar()
                                           ? ui->listUserImages->verticalScrollBar()->value()
                                           : 0;
    const int previousHorizontalScroll = ui->listUserImages->horizontalScrollBar()
                                             ? ui->listUserImages->horizontalScrollBar()->value()
                                             : 0;
    int visibleCount = 0;
    const bool includeNegative = ui->chkUserTagIncludeNegative && ui->chkUserTagIncludeNegative->isChecked();
    const QString currentPath = ui->listUserImages->currentItem()
                                    ? ui->listUserImages->currentItem()->data(ROLE_USER_IMAGE_PATH).toString()
                                    : QString();

    QSet<QString> selectedTagKeys;
    for (const QString &tag : selectedTags) {
        const QString key = TagUtils::normalizedGalleryTagKey(tag);
        if (!key.isEmpty()) selectedTagKeys.insert(key);
    }

    QStringList normalizedLoraFilters;
    for (const QString &l : m_extraLoraFilters) {
        const QString clean = GalleryMetadata::cleanModelOrLoraBaseName(l).toLower();
        if (!clean.isEmpty() && !normalizedLoraFilters.contains(clean)) {
            normalizedLoraFilters.append(clean);
        }
    }

    struct FilteredGalleryItem {
        QListWidgetItem *item = nullptr;
        bool visible = false;
    };
    QVector<FilteredGalleryItem> filteredItems;
    filteredItems.reserve(ui->listUserImages->count());

    for (int i = 0; i < ui->listUserImages->count(); ++i) {
        QListWidgetItem *item = ui->listUserImages->item(i);
        auto normalizedKeys = [item](int tagRole, int cacheRole) {
            const QVariant cached = item->data(cacheRole);
            if (cached.isValid()) return cached.toStringList();

            const QStringList keys = TagUtils::normalizedGalleryTagKeys(item->data(tagRole).toStringList());
            item->setData(cacheRole, keys);
            return keys;
        };

        const QStringList positiveTagKeys = normalizedKeys(ROLE_USER_IMAGE_TAGS, ROLE_USER_IMAGE_TAG_KEYS);
        const QStringList negativeTagKeys = includeNegative
                                                ? normalizedKeys(ROLE_USER_IMAGE_NEG_TAGS, ROLE_USER_IMAGE_NEG_KEYS)
                                                : QStringList();

        bool match = true;
        if (m_options.filterNsfw && m_options.nsfwMode == 0 && item->data(ROLE_USER_IMAGE_NSFW).toBool()) {
            match = false;
        }
        for (const QString &selTag : selectedTagKeys) {
            if (!match) break;
            const bool tagFound = std::binary_search(positiveTagKeys.cbegin(), positiveTagKeys.cend(), selTag)
                                  || (includeNegative
                                      && std::binary_search(negativeTagKeys.cbegin(), negativeTagKeys.cend(), selTag));
            if (!tagFound) {
                match = false;
                break;
            }
        }

        if (match && !normalizedLoraFilters.isEmpty()) {
            const QString imgPath = item->data(ROLE_USER_IMAGE_PATH).toString();
            UserImageInfo info = (m_imageCache && m_imageCache->contains(imgPath))
                                     ? m_imageCache->value(imgPath)
                                     : UserImageInfo{};
            if (!info.parsedModelsReady) {
                info.prompt = item->data(ROLE_USER_IMAGE_PROMPT).toString();
                info.parameters = item->data(ROLE_USER_IMAGE_PARAMS).toString();
                GalleryMetadata::populateParsedModelFields(info);
            }
            QSet<QString> imageLoras;
            for (const ParsedLoraEntry &entry : info.parsedLoras) {
                const QString clean = GalleryMetadata::cleanModelOrLoraBaseName(entry.name).toLower();
                if (!clean.isEmpty()) imageLoras.insert(clean);
            }
            for (const QString &reqLora : normalizedLoraFilters) {
                if (!imageLoras.contains(reqLora)) {
                    match = false;
                    break;
                }
            }
        }

        if (match) visibleCount++;
        filteredItems.append({item, match});
    }

    ui->listUserImages->setUpdatesEnabled(false);
    QListWidgetItem *restoredCurrentItem = nullptr;
    for (const FilteredGalleryItem &entry : std::as_const(filteredItems)) {
        if (!entry.item) continue;
        const bool shouldHide = !entry.visible;
        if (entry.item->isHidden() != shouldHide) {
            entry.item->setHidden(shouldHide);
        }
        if (!currentPath.isEmpty()
            && entry.item->data(ROLE_USER_IMAGE_PATH).toString() == currentPath) {
            restoredCurrentItem = entry.item;
        }
    }
    if (restoredCurrentItem && !restoredCurrentItem->isHidden()) {
        ui->listUserImages->setCurrentItem(restoredCurrentItem, QItemSelectionModel::NoUpdate);
    } else {
        ui->listUserImages->setCurrentItem(nullptr);
    }

    const QSize galleryGridSize = ui->listUserImages->gridSize();
    ui->listUserImages->setGridSize(QSize());
    ui->listUserImages->setGridSize(galleryGridSize);
    ui->listUserImages->setUpdatesEnabled(true);
    ui->listUserImages->doItemsLayout();
    if (QScrollBar *verticalBar = ui->listUserImages->verticalScrollBar()) {
        verticalBar->setValue(resetScrollToTop
                                  ? verticalBar->minimum()
                                  : qBound(verticalBar->minimum(), previousVerticalScroll, verticalBar->maximum()));
    }
    if (QScrollBar *horizontalBar = ui->listUserImages->horizontalScrollBar()) {
        horizontalBar->setValue(resetScrollToTop
                                    ? horizontalBar->minimum()
                                    : qBound(horizontalBar->minimum(), previousHorizontalScroll, horizontalBar->maximum()));
    }
    ui->listUserImages->viewport()->repaint();

    auto resetFilteredGalleryViewport = [this, layoutGeneration, resetScrollToTop,
                                         previousVerticalScroll, previousHorizontalScroll]() {
        if (layoutGeneration != m_layoutGeneration || !ui->listUserImages) return;
        ui->listUserImages->doItemsLayout();
        if (resetScrollToTop) {
            for (int i = 0; i < ui->listUserImages->count(); ++i) {
                QListWidgetItem *item = ui->listUserImages->item(i);
                if (item && !item->isHidden()) {
                    ui->listUserImages->scrollToItem(item, QAbstractItemView::PositionAtTop);
                    break;
                }
            }
        }
        if (QScrollBar *verticalBar = ui->listUserImages->verticalScrollBar()) {
            verticalBar->setValue(resetScrollToTop
                                      ? verticalBar->minimum()
                                      : qBound(verticalBar->minimum(), previousVerticalScroll, verticalBar->maximum()));
        }
        if (QScrollBar *horizontalBar = ui->listUserImages->horizontalScrollBar()) {
            horizontalBar->setValue(resetScrollToTop
                                        ? horizontalBar->minimum()
                                        : qBound(horizontalBar->minimum(), previousHorizontalScroll, horizontalBar->maximum()));
        }
        ui->listUserImages->update();
        ui->listUserImages->viewport()->repaint();
        scheduleVisibleThumbLoad();
    };
    QTimer::singleShot(0, this, resetFilteredGalleryViewport);
    QTimer::singleShot(50, this, resetFilteredGalleryViewport);

    if (m_extraLoraFilters.isEmpty()) {
        emit statusMessage(AppText::tr("筛选: %1 张图片符合条件").arg(visibleCount));
    } else {
        emit statusMessage(AppText::tr("组合筛选 (含 LoRA %1): %2 张图片符合条件")
                               .arg(m_extraLoraFilters.join(QStringLiteral(", ")))
                               .arg(visibleCount));
    }
}

void UserGalleryPage::onContextMenuRequested(const QPoint &pos)
{
    QListWidgetItem *item = ui->listUserImages->itemAt(pos);
    if (!item) return;

    const QString filePath = item->data(ROLE_USER_IMAGE_PATH).toString();
    if (filePath.isEmpty()) return;
    const QString prompt = item->data(ROLE_USER_IMAGE_PROMPT).toString();
    const QString neg = item->data(ROLE_USER_IMAGE_NEG).toString();
    const QString params = item->data(ROLE_USER_IMAGE_PARAMS).toString();

    QMenu menu(this);

    QAction *actCopyGenParams = menu.addAction(AppText::tr("复制生成参数 / Copy Gen Params"));
    actCopyGenParams->setToolTip(AppText::tr("复制符合SD WebUI格式的完整参数，\n粘贴进提示词框后可直接点击↙️按钮读取。"));
    menu.addSeparator();
    QAction *actOpenImg = menu.addAction(AppText::tr("内置图片浏览器 / Image Viewer"));
    QAction *actOpenSystem = menu.addAction(AppText::tr("使用系统程序打开 / Open with System"));
    QAction *actOpenDir = menu.addAction(AppText::tr("打开文件位置 / Show in Folder"));
    QAction *actShowComfyWorkflow = menu.addAction(AppText::tr("查看 ComfyUI Workflow / View Workflow"));
    QAction *actShowRawMetadata = menu.addAction(AppText::tr("显示原始 Metadata / Raw Metadata"));
    menu.addSeparator();
    QAction *actCopyPath = menu.addAction(AppText::tr("复制路径 / Copy Path"));

    QAction *selected = menu.exec(ui->listUserImages->mapToGlobal(pos));

    if (selected == actCopyGenParams) {
        QStringList parts;
        if (!prompt.isEmpty()) parts.append(prompt);
        if (!neg.isEmpty() && neg != "(empty)") parts.append("Negative prompt: " + neg);
        if (!params.isEmpty()) parts.append(params);
        QGuiApplication::clipboard()->setText(parts.join("\n"));
        emit statusMessage(AppText::tr("已复制 SD 生成参数到剪贴板"), 2000);
    } else if (selected == actOpenImg) {
        emit openImageViewerRequested(visibleImagePaths(), filePath);
    } else if (selected == actOpenSystem) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
    } else if (selected == actOpenDir) {
        emit showInFolderRequested(filePath);
    } else if (selected == actShowRawMetadata) {
        emit showRawMetadataRequested(filePath);
    } else if (selected == actShowComfyWorkflow) {
        emit showComfyWorkflowRequested(filePath);
    } else if (selected == actCopyPath) {
        QGuiApplication::clipboard()->setText(QDir::toNativeSeparators(filePath));
        emit statusMessage(AppText::tr("路径已复制"), 2000);
    }
}
