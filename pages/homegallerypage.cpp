#include "pages/homegallerypage.h"
#include "ui_homegallerypage.h"

#include "core/modelitemroles.h"
#include "utils/applanguage.h"
#include "utils/imageloader.h"
#include "utils/imagepresentation.h"
#include "utils/styleconstants.h"
#include "widgets/flowlayout.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QListWidgetItem>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QTextBoundaryFinder>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <utility>

namespace {
QString normalizedTagKey(const QString &tag)
{
    return tag.trimmed().toCaseFolded();
}

QString forceWrapText(const QString &text)
{
    if (text.isEmpty()) return text;
    QString result;
    result.reserve(text.size() * 2);
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    int start = 0;
    int end;
    while ((end = finder.toNextBoundary()) != -1) {
        result += QStringView(text).mid(start, end - start);
        result += QChar(0x200B);
        start = end;
    }
    return result;
}

void clearLayoutWidgets(QLayout *layout)
{
    if (!layout) return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }
}

void copyCommonModelRoles(const QListWidgetItem *source, QListWidgetItem *target)
{
    static constexpr int kCopiedRoles[] = {
        ROLE_FILE_PATH,
        ROLE_PREVIEW_PATH,
        ROLE_NSFW_LEVEL,
        ROLE_MODEL_NAME,
        ROLE_CIVITAI_NAME,
        ROLE_LOCAL_EDITED,
        ROLE_CIVITAI_MODEL_ID,
        ROLE_CIVITAI_VERSION_ID,
        ROLE_CIVITAI_SHA256,
        ROLE_MODEL_CREATOR,
        ROLE_MODEL_TAGS,
        ROLE_MODEL_TYPE,
        ROLE_MODEL_TRAINED_WORDS,
        ROLE_USER_RATING,
        ROLE_USER_NOTE,
        ROLE_USER_TAGS,
        ROLE_USER_CUSTOM_TRIGGERS,
        ROLE_MODEL_PREVIEW_STATE,
    };
    for (int role : kCopiedRoles) {
        target->setData(role, source->data(role));
    }
}
} // namespace

HomeGalleryPage::HomeGalleryPage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::HomeGalleryPage)
{
    ui->setupUi(this);

    ui->homeGalleryList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    ui->homeGalleryList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->homeGalleryList->verticalScrollBar()->setSingleStep(40);
    ui->homeGalleryList->setIconSize(QSize(180, 180));
    ui->homeGalleryList->setGridSize(QSize(200, 200));
    ui->homeGalleryList->setViewMode(QListWidget::IconMode);
    ui->homeGalleryList->setResizeMode(QListWidget::Adjust);
    ui->homeGalleryList->setSpacing(10);
    ui->homeGalleryList->setMovement(QListView::Static);
    ui->homeGalleryList->setContextMenuPolicy(Qt::CustomContextMenu);

    m_thumbLoadTimer = new QTimer(this);
    m_thumbLoadTimer->setSingleShot(true);
    connect(m_thumbLoadTimer, &QTimer::timeout, this, &HomeGalleryPage::dispatchVisibleThumbLoad);
    connect(ui->homeGalleryList->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
        scheduleVisibleThumbLoad();
    });
    ui->homeGalleryList->viewport()->installEventFilter(this);

    connect(ui->homeGalleryList, &QListWidget::itemClicked, this, &HomeGalleryPage::onGalleryItemClicked);
    connect(ui->homeGalleryList, &QListWidget::customContextMenuRequested,
            this, &HomeGalleryPage::onGalleryContextMenu);
    connect(ui->btnAddCollection, &QPushButton::clicked, this, &HomeGalleryPage::createCollectionRequested);

    m_authorFilterDebounceTimer = new QTimer(this);
    m_authorFilterDebounceTimer->setSingleShot(true);
    m_authorFilterDebounceTimer->setInterval(120);
    connect(m_authorFilterDebounceTimer, &QTimer::timeout, this, [this]() {
        if (!ui || !ui->editHomeAuthorFilter) return;
        m_authorFilter = ui->editHomeAuthorFilter->text().trimmed();
        emit filtersChanged();
    });
    connect(ui->editHomeAuthorFilter, &QLineEdit::textChanged, this, [this](const QString &) {
        if (m_authorFilterDebounceTimer) m_authorFilterDebounceTimer->start();
    });
    connect(ui->editHomeTagFilter, &QLineEdit::returnPressed, this, [this]() {
        addTagFilter(ui->editHomeTagFilter->text());
    });
    connect(ui->btnHomeAddTagFilter, &QPushButton::clicked, this, [this]() {
        addTagFilter(ui->editHomeTagFilter->text());
    });
    connect(ui->btnHomeTagSortMode, &QPushButton::clicked, this, [this]() {
        m_tagsSortByCount = !m_tagsSortByCount;
        emit filterChipsRefreshRequested();
    });
    connect(ui->btnHomeClearFilters, &QPushButton::clicked, this, &HomeGalleryPage::clearFilters);
    connect(ui->btnHomeFilterToggle, &QPushButton::clicked, this, [this](bool checked) {
        m_filterExpanded = checked;
        emit filterChipsRefreshRequested();
    });
}

HomeGalleryPage::~HomeGalleryPage()
{
    shutdown();
    delete ui;
}

void HomeGalleryPage::setThreadPools(QThreadPool *thumbPool, QThreadPool *backgroundPool)
{
    m_thumbPool = thumbPool;
    m_backgroundPool = backgroundPool;
}

void HomeGalleryPage::setOptions(const HomeGalleryOptions &options)
{
    m_options = options;
}

void HomeGalleryPage::setPlaceholderIcons(const QIcon &placeholderIcon, const QIcon &noPreviewIcon)
{
    m_placeholderIcon = placeholderIcon;
    m_noPreviewIcon = noPreviewIcon;
}

void HomeGalleryPage::recolorPlaceholders(const QIcon &placeholderIcon, const QIcon &noPreviewIcon)
{
    m_placeholderIcon = placeholderIcon;
    m_noPreviewIcon = noPreviewIcon;
    if (!ui || !ui->homeGalleryList) return;

    bool changed = false;
    for (int i = 0; i < ui->homeGalleryList->count(); ++i) {
        QListWidgetItem *item = ui->homeGalleryList->item(i);
        if (item && item->data(ROLE_PREVIEW_PLACEHOLDER).toBool()) {
            item->setIcon(QIcon());
            const ModelPreviewState state = static_cast<ModelPreviewState>(
                item->data(ROLE_MODEL_PREVIEW_STATE).toInt());
            item->setIcon(state == ModelPreviewState::KnownNoPreview ? m_noPreviewIcon : m_placeholderIcon);
            changed = true;
        }
    }
    if (changed && ui->homeGalleryList->viewport()) {
        ui->homeGalleryList->viewport()->update();
    }
}

void HomeGalleryPage::restoreFilterState(const HomeGalleryFilterState &state)
{
    m_authorFilter = state.authorFilter;
    m_tagFilters = state.tagFilters;
    m_filterExpanded = state.filterExpanded;
    m_tagsSortByCount = state.tagsSortByCount;

    if (ui->editHomeAuthorFilter) {
        const QSignalBlocker blocker(ui->editHomeAuthorFilter);
        ui->editHomeAuthorFilter->setText(m_authorFilter);
    }
    if (ui->btnHomeFilterToggle) {
        const QSignalBlocker blocker(ui->btnHomeFilterToggle);
        ui->btnHomeFilterToggle->setChecked(m_filterExpanded);
    }
}

HomeGalleryFilterState HomeGalleryPage::filterState() const
{
    HomeGalleryFilterState state;
    state.authorFilter = m_authorFilter;
    state.tagFilters = m_tagFilters;
    state.filterExpanded = m_filterExpanded;
    state.tagsSortByCount = m_tagsSortByCount;
    return state;
}

QString HomeGalleryPage::authorFilter() const
{
    return m_authorFilter;
}

QSet<QString> HomeGalleryPage::tagFilters() const
{
    return m_tagFilters;
}

bool HomeGalleryPage::isTagsSortByCount() const
{
    return m_tagsSortByCount;
}

void HomeGalleryPage::setAuthorFilter(const QString &author)
{
    m_authorFilter = author.trimmed();
    if (ui && ui->editHomeAuthorFilter && ui->editHomeAuthorFilter->text() != m_authorFilter) {
        const QSignalBlocker blocker(ui->editHomeAuthorFilter);
        ui->editHomeAuthorFilter->setText(m_authorFilter);
    }
    emit filtersChanged();
}

void HomeGalleryPage::addTagFilter(const QString &tag)
{
    const QString clean = tag.trimmed();
    if (clean.isEmpty()) return;

    QString existing;
    for (const QString &value : std::as_const(m_tagFilters)) {
        if (value.compare(clean, Qt::CaseInsensitive) == 0) {
            existing = value;
            break;
        }
    }
    if (existing.isEmpty()) m_tagFilters.insert(clean);
    if (ui && ui->editHomeTagFilter) ui->editHomeTagFilter->clear();
    emit filtersChanged();
}

void HomeGalleryPage::clearFilters()
{
    m_authorFilter.clear();
    m_tagFilters.clear();
    if (ui && ui->editHomeAuthorFilter) {
        const QSignalBlocker blocker(ui->editHomeAuthorFilter);
        ui->editHomeAuthorFilter->clear();
    }
    if (ui && ui->editHomeTagFilter) ui->editHomeTagFilter->clear();
    emit filtersChanged();
}

void HomeGalleryPage::refreshCollectionsBar(const QStringList &collectionNames,
                                            const QString &currentFilter,
                                            const QString &uncategorizedKey)
{
    QLayout *layout = ui->scrollAreaCollections->widget()->layout();
    while (layout->count() > 1) {
        QLayoutItem *item = layout->takeAt(1);
        if (item->widget()) delete item->widget();
        delete item;
    }

    ui->btnAddCollection->setProperty("class", "collectionBtn");

    auto *btnAll = new QPushButton(AppText::tr("ALL\n全部"));
    btnAll->setFixedSize(90, 90);
    btnAll->setProperty("class", "collectionBtn");
    btnAll->setCheckable(true);
    btnAll->setChecked(currentFilter.isEmpty());
    btnAll->setCursor(Qt::PointingHandCursor);
    connect(btnAll, &QPushButton::clicked, this, [this]() {
        emit collectionFilterRequested(QString());
    });
    layout->addWidget(btnAll);

    auto *btnUncat = new QPushButton(AppText::tr("📦\n未分类"));
    btnUncat->setFixedSize(90, 90);
    btnUncat->setProperty("class", "collectionBtn");
    btnUncat->setCheckable(true);
    btnUncat->setChecked(currentFilter == uncategorizedKey);
    btnUncat->setCursor(Qt::PointingHandCursor);
    connect(btnUncat, &QPushButton::clicked, this, [this, uncategorizedKey]() {
        emit collectionFilterRequested(uncategorizedKey);
    });
    layout->addWidget(btnUncat);

    for (const QString &name : collectionNames) {
        if (name == uncategorizedKey) continue;

        QString displayName = name;
        if (displayName.length() > 20) displayName = displayName.left(18) + "..";

        auto *btn = new QPushButton(displayName);
        btn->setFixedSize(90, 90);
        btn->setProperty("class", "collectionBtn");
        btn->setCheckable(true);
        btn->setChecked(currentFilter == name);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setToolTip(name);

        connect(btn, &QPushButton::clicked, this, [this, name]() {
            emit collectionFilterRequested(name);
        });

        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(btn, &QPushButton::customContextMenuRequested, this, [this, btn, name](const QPoint &pos) {
            QMenu menu;
            QAction *title = menu.addAction(AppText::tr("管理: %1").arg(name));
            title->setEnabled(false);
            menu.addSeparator();

            QAction *actRename = menu.addAction(AppText::tr("重命名 / Rename"));
            QAction *actDelete = menu.addAction(AppText::tr("删除 / Delete"));

            QAction *selected = menu.exec(btn->mapToGlobal(pos));
            if (selected == actRename) {
                emit renameCollectionRequested(name);
            } else if (selected == actDelete) {
                emit deleteCollectionRequested(name);
            }
        });

        layout->addWidget(btn);
    }

    static_cast<QHBoxLayout*>(layout)->addStretch();
}

void HomeGalleryPage::refreshFilterChips(const QList<HomeFilterTagInfo> &availableTagInfos)
{
    if (!ui || !ui->layoutHomeFilterChips || !ui->layoutHomeFilterSummaryChips) return;

    constexpr int kSummaryChipHeight = 24;
    constexpr int kSummaryAreaHeight = 36;
    constexpr int kSummarySpacing = 5;
    constexpr int kTagsMaxHeight = 170;
    const int previousTagScrollValue = ui->scrollHomeFilterTags
        ? ui->scrollHomeFilterTags->verticalScrollBar()->value()
        : 0;

    clearLayoutWidgets(ui->layoutHomeFilterSummaryChips);
    clearLayoutWidgets(ui->layoutHomeFilterChips);

    // 1. 第一行：已选作者 / Tag 摘要区
    ui->scrollHomeFilterSummary->setWidgetResizable(false);
    ui->scrollHomeFilterSummary->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ui->scrollHomeFilterSummary->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->scrollHomeFilterSummary->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    ui->scrollHomeFilterSummary->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui->scrollHomeFilterSummary->setMinimumWidth(320);
    ui->scrollHomeFilterSummary->setFixedHeight(kSummaryAreaHeight);

    ui->homeFilterSummaryContents->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    ui->homeFilterSummaryContents->setFixedHeight(kSummaryChipHeight);

    ui->layoutHomeFilterSummaryChips->setContentsMargins(0, 0, 0, 0);
    ui->layoutHomeFilterSummaryChips->setSpacing(kSummarySpacing);
    ui->layoutHomeFilterSummaryChips->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    int summaryContentWidth = 0;

    auto appendSummaryWidth = [&](int itemWidth) {
        if (summaryContentWidth > 0) summaryContentWidth += kSummarySpacing;
        summaryContentWidth += itemWidth;
    };

    auto fixSummaryWidgetSize = [&](QWidget *widget) {
        if (!widget) return;

        widget->ensurePolished();
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);

        int width = 0;
        if (auto *btn = qobject_cast<QPushButton*>(widget)) {
            width = btn->fontMetrics().horizontalAdvance(btn->text()) + 22;
        } else if (auto *label = qobject_cast<QLabel*>(widget)) {
            width = label->fontMetrics().horizontalAdvance(label->text()) + 6;
        }
        width = qMax(width, 18);

        widget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        widget->setMinimumSize(width, kSummaryChipHeight);
        widget->setMaximumSize(width, kSummaryChipHeight);
        widget->resize(width, kSummaryChipHeight);

        appendSummaryWidth(width);
    };

    QHash<QString, HomeFilterTagInfo> availableTagInfoByKey;
    for (const HomeFilterTagInfo &info : availableTagInfos) {
        availableTagInfoByKey.insert(normalizedTagKey(info.tag), info);
    }

    auto tagSourceFor = [&availableTagInfoByKey](const QString &tag) {
        return availableTagInfoByKey.value(normalizedTagKey(tag)).sourceName();
    };

    auto displayTagWithCount = [&availableTagInfoByKey](const QString &tag) {
        const HomeFilterTagInfo info = availableTagInfoByKey.value(normalizedTagKey(tag));
        if (info.count > 0) return QString("%1(%2)").arg(tag).arg(info.count);
        return tag;
    };

    auto addSummaryChip = [this, &fixSummaryWidgetSize](const QString &text,
                                                        const QString &tooltip,
                                                        const QString &source,
                                                        const std::function<void()> &removeFn) {
        auto *chip = new QPushButton(text);
        chip->setProperty("class", "filterChip");
        if (!source.isEmpty()) chip->setProperty("tagSource", source);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip(tooltip);
        chip->setCheckable(false);
        chip->setAutoDefault(false);
        chip->setDefault(false);

        fixSummaryWidgetSize(chip);

        connect(chip, &QPushButton::clicked, this, removeFn);
        ui->layoutHomeFilterSummaryChips->addWidget(chip);
    };

    QStringList selectedTags = m_tagFilters.values();
    std::sort(selectedTags.begin(), selectedTags.end(), [](const QString &a, const QString &b) {
        return QString::localeAwareCompare(a, b) < 0;
    });

    if (!m_authorFilter.isEmpty()) {
        addSummaryChip("作者: " + m_authorFilter + " ×",
                       "点击移除作者筛选",
                       QString(),
                       [this]() {
                           setAuthorFilter(QString());
                       });
    }

    for (const QString &tag : selectedTags) {
        addSummaryChip(displayTagWithCount(tag) + " ×",
                       "点击移除此 Tag 筛选",
                       tagSourceFor(tag),
                       [this, tag]() {
                           m_tagFilters.remove(tag);
                           emit filtersChanged();
                       });
    }

    if (m_authorFilter.isEmpty() && m_tagFilters.isEmpty()) {
        auto *emptySummary = new QLabel(AppText::tr("未启用主页作者/Tag 筛选"));
        emptySummary->setStyleSheet(AppStyle::MutedBoldLabelStyle());
        emptySummary->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        fixSummaryWidgetSize(emptySummary);
        ui->layoutHomeFilterSummaryChips->addWidget(emptySummary);
    }

    const int summaryViewportWidth = qMax(1, ui->scrollHomeFilterSummary->viewport()->width());
    const int finalSummaryWidth = qMax(summaryContentWidth + 2, summaryViewportWidth);

    ui->homeFilterSummaryContents->setFixedSize(finalSummaryWidth, kSummaryChipHeight);
    ui->homeFilterSummaryContents->updateGeometry();

    ui->layoutHomeFilterSummaryChips->invalidate();
    ui->layoutHomeFilterSummaryChips->activate();

    ui->scrollHomeFilterSummary->horizontalScrollBar()->setSingleStep(40);
    ui->scrollHomeFilterSummary->horizontalScrollBar()->setPageStep(summaryViewportWidth);

    // 2. 展开 / 收起按钮和输入区
    ui->btnHomeFilterToggle->setChecked(m_filterExpanded);
    ui->btnHomeFilterToggle->setText(m_filterExpanded ? AppText::tr("收起筛选") : AppText::tr("展开筛选"));

    auto setLayoutVisible = [](QLayout *layout, bool visible) {
        if (!layout) return;
        for (int i = 0; i < layout->count(); ++i) {
            QLayoutItem *item = layout->itemAt(i);
            if (!item) continue;
            if (QWidget *widget = item->widget()) widget->setVisible(visible);
            if (QLayout *childLayout = item->layout()) {
                for (int j = 0; j < childLayout->count(); ++j) {
                    if (QWidget *childWidget = childLayout->itemAt(j)->widget()) {
                        childWidget->setVisible(visible);
                    }
                }
            }
        }
    };

    setLayoutVisible(ui->horizontalLayout_HomeFilterInputs, m_filterExpanded);

    // 3. 第二行：展开后的全部 Tag 区域
    ui->homeFilterChipsContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui->layoutHomeFilterChips->setContentsMargins(0, 0, 0, 0);
    ui->layoutHomeFilterChips->setSpacing(0);
    ui->layoutHomeFilterChips->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    if (ui->scrollHomeFilterTags) {
        ui->scrollHomeFilterTags->setWidgetResizable(true);
        ui->scrollHomeFilterTags->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        ui->scrollHomeFilterTags->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        ui->scrollHomeFilterTags->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        ui->scrollHomeFilterTags->setVisible(m_filterExpanded);
    } else {
        ui->homeFilterChipsContainer->setVisible(m_filterExpanded);
    }

    if (!m_filterExpanded) {
        if (ui->scrollHomeFilterTags) {
            ui->scrollHomeFilterTags->setFixedHeight(0);
            ui->scrollHomeFilterTags->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        }
        ui->homeFilterChipsContainer->setMinimumHeight(0);
        ui->homeFilterChipsContainer->setMaximumHeight(0);
        return;
    }

    auto *availableTagsWidget = new QWidget();
    availableTagsWidget->setStyleSheet("background:transparent;");
    availableTagsWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *availableTagsLayout = new FlowLayout(availableTagsWidget, 0, 6, 6);

    if (ui->btnHomeTagSortMode) {
        ui->btnHomeTagSortMode->setText(m_tagsSortByCount ? AppText::tr("按次数") : AppText::tr("按字母"));
        ui->btnHomeTagSortMode->setToolTip(m_tagsSortByCount
            ? AppText::tr("当前按出现次数排序，点击切换为字母序")
            : AppText::tr("当前按字母序排序，点击切换为出现次数"));
    }

    for (const HomeFilterTagInfo &info : availableTagInfos) {
        const QString tag = info.tag;
        bool tagSelected = false;
        for (const QString &selected : std::as_const(m_tagFilters)) {
            if (selected.compare(tag, Qt::CaseInsensitive) == 0) {
                tagSelected = true;
                break;
            }
        }

        auto *tagButton = new QPushButton(forceWrapText(QString("%1(%2)").arg(tag).arg(info.count)));
        tagButton->setProperty("class", "filterChip");
        tagButton->setProperty("tagSource", info.sourceName());
        tagButton->setCheckable(true);
        tagButton->setChecked(tagSelected);
        tagButton->setCursor(Qt::PointingHandCursor);
        const QString sourceText = info.fromModel && info.fromUser
            ? "模型 Tag + 用户 Tag"
            : (info.fromUser ? "用户 Tag" : "模型 Tag");
        tagButton->setToolTip(AppText::tr("%1，出现于 %2 个模型。点击添加/移除此 Tag 筛选。")
                                  .arg(sourceText)
                                  .arg(info.count));
        tagButton->setMaximumWidth(180);
        tagButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

        connect(tagButton, &QPushButton::clicked, this, [this, tag]() {
            QString existing;
            for (const QString &value : std::as_const(m_tagFilters)) {
                if (value.compare(tag, Qt::CaseInsensitive) == 0) {
                    existing = value;
                    break;
                }
            }
            if (existing.isEmpty()) {
                m_tagFilters.insert(tag);
            } else {
                m_tagFilters.remove(existing);
            }
            emit filtersChanged();
        });

        availableTagsLayout->addWidget(tagButton);
    }

    if (availableTagInfos.isEmpty()) {
        auto *emptyTags = new QLabel(AppText::tr("当前模型库没有可用模型 Tag"));
        emptyTags->setStyleSheet(AppStyle::MutedLabelStyle());
        emptyTags->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        availableTagsLayout->addWidget(emptyTags);
    }

    ui->layoutHomeFilterChips->addWidget(availableTagsWidget);
    ui->layoutHomeFilterChips->invalidate();
    ui->layoutHomeFilterChips->activate();

    if (ui->scrollHomeFilterTags) {
        const int tagViewportWidth = qMax(1, ui->scrollHomeFilterTags->viewport()->width());

        int tagContentHeight = availableTagsLayout->heightForWidth(tagViewportWidth);
        tagContentHeight = qMax(tagContentHeight, availableTagsLayout->minimumSize().height());
        tagContentHeight = qMax(tagContentHeight, kSummaryChipHeight + 4);

        availableTagsWidget->setMinimumHeight(tagContentHeight);
        availableTagsWidget->setMaximumHeight(tagContentHeight);

        ui->homeFilterChipsContainer->setMinimumHeight(tagContentHeight);
        ui->homeFilterChipsContainer->setMaximumHeight(tagContentHeight);

        const int visibleHeight = qMin(tagContentHeight, kTagsMaxHeight);

        ui->scrollHomeFilterTags->setFixedHeight(visibleHeight);
        ui->scrollHomeFilterTags->setVerticalScrollBarPolicy(
            tagContentHeight > visibleHeight ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff
        );

        ui->scrollHomeFilterTags->verticalScrollBar()->setSingleStep(40);
        QScrollBar *tagScrollBar = ui->scrollHomeFilterTags->verticalScrollBar();
        tagScrollBar->setValue(qBound(0, previousTagScrollValue, tagScrollBar->maximum()));
        QTimer::singleShot(0, this, [this, previousTagScrollValue]() {
            if (!ui || !ui->scrollHomeFilterTags || !m_filterExpanded) return;
            QScrollBar *bar = ui->scrollHomeFilterTags->verticalScrollBar();
            bar->setValue(qBound(0, previousTagScrollValue, bar->maximum()));
        });
    }
}

void HomeGalleryPage::setModelEntries(const QVector<HomeGalleryEntry> &entries)
{
    if (m_thumbLoadTimer) m_thumbLoadTimer->stop();
    m_queuedThumbPaths.clear();
    m_loadedThumbPaths.clear();
    m_itemByPath.clear();

    ui->homeGalleryList->setUpdatesEnabled(false);
    ui->homeGalleryList->clear();

    for (const HomeGalleryEntry &entry : entries) {
        if (!entry.sourceItem) continue;
        const QString filePath = entry.sourceItem->data(ROLE_FILE_PATH).toString();
        const QString previewPath = entry.sourceItem->data(ROLE_PREVIEW_PATH).toString();
        const int nsfwLevel = entry.sourceItem->data(ROLE_NSFW_LEVEL).toInt();
        const bool isNSFW = nsfwLevel > m_options.nsfwLevel;
        const ModelPreviewState previewState = static_cast<ModelPreviewState>(
            entry.sourceItem->data(ROLE_MODEL_PREVIEW_STATE).toInt());

        auto *item = new QListWidgetItem();
        copyCommonModelRoles(entry.sourceItem, item);
        item->setToolTip(entry.tooltip);

        const bool blurItem = (m_options.filterNsfw && isNSFW && m_options.nsfwMode == 1);
        const QString cacheKey = (blurItem ? QStringLiteral("blur|") : QStringLiteral("norm|")) + previewPath;
        if (!previewPath.isEmpty() && m_iconCache.contains(cacheKey)) {
            item->setIcon(m_iconCache.value(cacheKey));
            item->setData(ROLE_PREVIEW_PLACEHOLDER, false);
            if (!filePath.isEmpty()) m_loadedThumbPaths.insert(filePath);
        } else {
            item->setIcon(previewState == ModelPreviewState::KnownNoPreview ? m_noPreviewIcon : m_placeholderIcon);
            item->setData(ROLE_PREVIEW_PLACEHOLDER, true);
        }
        ui->homeGalleryList->addItem(item);
        if (!filePath.isEmpty()) m_itemByPath.insert(filePath, item);
    }

    ui->homeGalleryList->setUpdatesEnabled(true);
    ui->homeGalleryList->doItemsLayout();
    scheduleVisibleThumbLoad();
}

void HomeGalleryPage::clearItems()
{
    if (m_thumbLoadTimer) m_thumbLoadTimer->stop();
    m_queuedThumbPaths.clear();
    m_loadedThumbPaths.clear();
    m_itemByPath.clear();
    if (ui && ui->homeGalleryList) {
        ui->homeGalleryList->clear();
    }
}

QListWidgetItem *HomeGalleryPage::itemForPath(const QString &filePath) const
{
    return m_itemByPath.value(filePath, nullptr);
}

void HomeGalleryPage::updateDownloadedCoverPreview(const QString &localBaseName,
                                                   const QString &downloadedDirectory,
                                                   const QString &savePath)
{
    if (!ui || !ui->homeGalleryList) return;
    QSet<QString> matchedPaths;
    for (int i = 0; i < ui->homeGalleryList->count(); ++i) {
        QListWidgetItem *item = ui->homeGalleryList->item(i);
        if (!item) continue;
        const QString itemPath = item->data(ROLE_FILE_PATH).toString();
        if (itemPath.isEmpty()) continue;
        const QFileInfo modelInfo(itemPath);
        if (modelInfo.completeBaseName().compare(localBaseName, Qt::CaseSensitive) == 0 &&
            QDir::cleanPath(modelInfo.absolutePath()).compare(downloadedDirectory, Qt::CaseInsensitive) == 0) {
            item->setData(ROLE_PREVIEW_PATH, savePath);
            item->setData(ROLE_MODEL_PREVIEW_STATE, static_cast<int>(ModelPreviewState::RealPreview));
            matchedPaths.insert(itemPath);
        }
    }
    QThreadPool *pool = m_backgroundPool ? m_backgroundPool : m_thumbPool;
    if (!pool) return;
    for (const QString &modelPath : std::as_const(matchedPaths)) {
        auto *task = new IconLoaderTask(savePath, 180, 12, this, QStringLiteral("HOME:") + modelPath);
        task->setAutoDelete(true);
        pool->start(task);
    }
}

void HomeGalleryPage::syncPreviewState(const QString &filePath, ModelPreviewState previewState)
{
    if (!ui || !ui->homeGalleryList || previewState == ModelPreviewState::RealPreview) return;
    for (int i = 0; i < ui->homeGalleryList->count(); ++i) {
        QListWidgetItem *item = ui->homeGalleryList->item(i);
        if (!item || QFileInfo(item->data(ROLE_FILE_PATH).toString()).absoluteFilePath() != filePath) continue;
        item->setData(ROLE_MODEL_PREVIEW_STATE, static_cast<int>(previewState));
        item->setData(ROLE_PREVIEW_PLACEHOLDER, true);
        item->setIcon(previewState == ModelPreviewState::KnownNoPreview ? m_noPreviewIcon : m_placeholderIcon);
    }
}

void HomeGalleryPage::invalidateIconCacheForPath(const QString &previewPath)
{
    if (previewPath.isEmpty()) return;
    const QString cleaned = QDir::cleanPath(previewPath);
    const QString abs = QFileInfo(previewPath).absoluteFilePath();
    for (const QString &p : {previewPath, cleaned, abs}) {
        if (p.isEmpty()) continue;
        m_iconCache.remove(QStringLiteral("norm|") + p);
        m_iconCache.remove(QStringLiteral("blur|") + p);
    }
}

void HomeGalleryPage::clearIconCache()
{
    m_iconCache.clear();
}

void HomeGalleryPage::shutdown()
{
    m_isShuttingDown = true;
    if (m_authorFilterDebounceTimer) m_authorFilterDebounceTimer->stop();
    if (m_thumbLoadTimer) m_thumbLoadTimer->stop();
}

void HomeGalleryPage::scheduleVisibleThumbLoad()
{
    if (!m_thumbLoadTimer) return;
    m_thumbLoadTimer->start(20);
}

void HomeGalleryPage::dispatchVisibleThumbLoad()
{
    if (m_isShuttingDown || !ui || !ui->homeGalleryList || ui->homeGalleryList->count() == 0 || !m_thumbPool) return;

    const QRect visibleRect = ui->homeGalleryList->viewport()->rect();
    if (visibleRect.isEmpty()) return;

    const QRect prefetchRect = visibleRect.adjusted(-visibleRect.width(),
                                                    -visibleRect.height(),
                                                    visibleRect.width(),
                                                    visibleRect.height());
    const QPoint centerPoint = visibleRect.center();

    struct ThumbCandidate {
        int priority = 2;
        int distance = 0;
        QString filePath;
        QString previewPath;
    };

    QList<ThumbCandidate> candidates;
    for (int i = 0; i < ui->homeGalleryList->count(); ++i) {
        QListWidgetItem *item = ui->homeGalleryList->item(i);
        if (!item || item->isHidden()) continue;

        const QString filePath = item->data(ROLE_FILE_PATH).toString();
        const QString previewPath = item->data(ROLE_PREVIEW_PATH).toString();
        if (filePath.isEmpty() || previewPath.isEmpty()) continue;
        if (m_loadedThumbPaths.contains(filePath) || m_queuedThumbPaths.contains(filePath)) continue;

        const QRect itemRect = ui->homeGalleryList->visualItemRect(item);
        if (!itemRect.isValid() || itemRect.isEmpty()) continue;
        if (!itemRect.intersects(prefetchRect)) continue;

        ThumbCandidate candidate;
        candidate.filePath = filePath;
        candidate.previewPath = previewPath;
        candidate.distance = qAbs(itemRect.center().y() - centerPoint.y())
                           + qAbs(itemRect.center().x() - centerPoint.x());
        candidate.priority = itemRect.intersects(visibleRect) ? 0 : 1;
        candidates.append(candidate);
    }

    if (candidates.isEmpty()) return;

    std::sort(candidates.begin(), candidates.end(), [](const ThumbCandidate &a, const ThumbCandidate &b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.distance < b.distance;
    });

    const int threadCount = qMax(1, m_thumbPool->maxThreadCount());
    const int maxLaunchPerPass = qMax(12, threadCount * 3);
    int launched = 0;
    for (const ThumbCandidate &candidate : candidates) {
        if (launched >= maxLaunchPerPass) break;
        auto *task = new IconLoaderTask(candidate.previewPath, 180, 12, this, QStringLiteral("HOME:") + candidate.filePath);
        task->setAutoDelete(true);
        m_thumbPool->start(task);
        m_queuedThumbPaths.insert(candidate.filePath);
        ++launched;
    }
}

void HomeGalleryPage::applyItemIcon(QListWidgetItem *item,
                                    const QPixmap &originalPix,
                                    const QIcon &originalIcon,
                                    QPixmap &blurredPix)
{
    if (!item) return;
    const bool isNSFW = item->data(ROLE_NSFW_LEVEL).toInt() > m_options.nsfwLevel;
    const bool blurItem = (m_options.filterNsfw && isNSFW && m_options.nsfwMode == 1);
    const QString previewPath = item->data(ROLE_PREVIEW_PATH).toString();
    const QString cacheKey = (blurItem ? QStringLiteral("blur|") : QStringLiteral("norm|")) + previewPath;
    QIcon finalIcon = m_iconCache.value(cacheKey);
    if (finalIcon.isNull()) {
        if (blurItem) {
            if (blurredPix.isNull()) blurredPix = ImagePresentation::applyNSFWBlur(originalPix);
            finalIcon = QIcon(ImagePresentation::applyRoundedMask(blurredPix, 12));
        } else {
            finalIcon = originalIcon;
        }
        if (!previewPath.isEmpty()) m_iconCache.insert(cacheKey, finalIcon);
    }
    item->setIcon(finalIcon);
    item->setData(ROLE_PREVIEW_PLACEHOLDER, false);
    item->setData(ROLE_MODEL_PREVIEW_STATE, static_cast<int>(ModelPreviewState::RealPreview));
}

void HomeGalleryPage::onIconLoaded(const QString &id, const QImage &image)
{
    if (m_isShuttingDown || !id.startsWith(QLatin1String("HOME:"))) return;
    const QString filePath = id.mid(5);
    if (image.isNull()) {
        m_queuedThumbPaths.remove(filePath);
        return;
    }

    const QPixmap originalPix = QPixmap::fromImage(image);
    const QIcon originalIcon(originalPix);
    QPixmap blurredPix;

    if (QListWidgetItem *item = m_itemByPath.value(filePath, nullptr)) {
        applyItemIcon(item, originalPix, originalIcon, blurredPix);
    } else if (ui && ui->homeGalleryList) {
        for (int i = 0; i < ui->homeGalleryList->count(); ++i) {
            QListWidgetItem *item = ui->homeGalleryList->item(i);
            if (item && item->data(ROLE_FILE_PATH).toString() == filePath) {
                applyItemIcon(item, originalPix, originalIcon, blurredPix);
            }
        }
    }

    m_queuedThumbPaths.remove(filePath);
    m_loadedThumbPaths.insert(filePath);
    scheduleVisibleThumbLoad();
}

void HomeGalleryPage::applyExternalImageUpdate(const QString &filePath,
                                               const QPixmap &originalPix,
                                               const QIcon &originalIcon)
{
    QPixmap blurredPix;
    if (QListWidgetItem *item = m_itemByPath.value(filePath, nullptr)) {
        applyItemIcon(item, originalPix, originalIcon, blurredPix);
    } else if (ui && ui->homeGalleryList) {
        for (int i = 0; i < ui->homeGalleryList->count(); ++i) {
            QListWidgetItem *item = ui->homeGalleryList->item(i);
            if (item && item->data(ROLE_FILE_PATH).toString() == filePath) {
                applyItemIcon(item, originalPix, originalIcon, blurredPix);
            }
        }
    }
}

bool HomeGalleryPage::eventFilter(QObject *watched, QEvent *event)
{
    if (ui->homeGalleryList && watched == ui->homeGalleryList->viewport() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        scheduleVisibleThumbLoad();
    }
    return QWidget::eventFilter(watched, event);
}

void HomeGalleryPage::onGalleryItemClicked(QListWidgetItem *item)
{
    if (!item) return;
    if (QApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) {
        return;
    }
    const QString targetPath = item->data(ROLE_FILE_PATH).toString();
    if (!targetPath.isEmpty()) {
        emit modelClicked(targetPath);
    }
}

void HomeGalleryPage::onGalleryContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = ui->homeGalleryList->itemAt(pos);
    if (!item) return;

    QList<QListWidgetItem*> selectedItems = ui->homeGalleryList->selectedItems();
    if (!selectedItems.contains(item)) {
        selectedItems = {item};
    }

    emit contextMenuRequested(selectedItems, ui->homeGalleryList->mapToGlobal(pos));
}
