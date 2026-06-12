#include "downloadspage.h"
#include "ui_downloadspage.h"

#include <QColor>
#include <QComboBox>
#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QProgressBar>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStyle>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>

DownloadsPage::DownloadsPage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::DownloadsPage)
{
    ui->setupUi(this);
    m_statusLabel = ui->lblDownloadsStatus;
    m_selectedLabel = ui->lblDownloadsSelected;
    m_filterLabel = ui->lblDownloadsFilter;
    m_filterCombo = ui->comboDownloadsFilter;
    m_statusTabs = ui->tabDownloadsStatus;

    m_checkCurrentButton = ui->btnDownloadsCheckCurrent;
    m_checkSelectedButton = ui->btnDownloadsCheckSelected;
    m_checkAllButton = ui->btnDownloadsCheckAll;
    m_downloadSelectedButton = ui->btnDownloadsDownloadSelected;
    m_cancelButton = ui->btnDownloadsCancel;
    m_retryButton = ui->btnDownloadsRetry;
    m_openFolderButton = ui->btnDownloadsOpenFolder;
    m_clearCompletedButton = ui->btnDownloadsClearCompleted;
    m_toggleCurrentTabButton = ui->btnDownloadsToggleCurrentTab;

    m_defaultCardsContainer = ui->downloadCardsContainer;
    m_scrollUpdates = ui->scrollDownloadsCards;
    m_scrollCoexisting = ui->scrollDownloadsCoexisting;
    m_scrollIgnored = ui->scrollDownloadsIgnored;
    m_scrollLatest = ui->scrollDownloadsLatest;
    m_scrollErrors = ui->scrollDownloadsErrors;
    m_scrollLocal = ui->scrollDownloadsLocal;

    m_layoutUpdates = ui->downloadCardsLayout;
    m_layoutCoexisting = ui->downloadCardsLayoutCoexisting;
    m_layoutIgnored = ui->downloadCardsLayoutIgnored;
    m_layoutLatest = ui->downloadCardsLayoutLatest;
    m_layoutErrors = ui->downloadCardsLayoutErrors;
    m_layoutLocal = ui->downloadCardsLayoutLocal;
}

DownloadsPage::~DownloadsPage()
{
    delete ui;
}

QVBoxLayout *DownloadsPage::cardsLayout(const QString &category) const
{
    if (category == "coexisting") return m_layoutCoexisting;
    if (category == "ignored") return m_layoutIgnored;
    if (category == "latest") return m_layoutLatest;
    if (category == "errors") return m_layoutErrors;
    if (category == "local") return m_layoutLocal;
    return m_layoutUpdates;
}

QString DownloadsPage::currentCategory() const
{
    if (!m_statusTabs) return "updates";
    switch (m_statusTabs->currentIndex()) {
    case 1: return "coexisting";
    case 2: return "ignored";
    case 3: return "latest";
    case 4: return "errors";
    case 5: return "local";
    default: return "updates";
    }
}

void DownloadsPage::setStatusText(const QString &text)
{
    if (m_statusLabel) m_statusLabel->setText(text);
}

void DownloadsPage::setUpdateCheckButtonsEnabled(bool enabled)
{
    if (m_checkCurrentButton) m_checkCurrentButton->setEnabled(enabled);
    if (m_checkSelectedButton) m_checkSelectedButton->setEnabled(enabled);
    if (m_checkAllButton) m_checkAllButton->setEnabled(enabled);
}

void DownloadsPage::updateSelectionSummary(int selectedCurrent, int currentTotal, int selectedTotal)
{
    if (m_selectedLabel) {
        m_selectedLabel->setText(QString("当前 Tab 已选择 %1/%2 个模型\n全部已选择 %3 个模型")
                                     .arg(selectedCurrent)
                                     .arg(currentTotal)
                                     .arg(selectedTotal));
    }

    const bool allChecked = currentTotal > 0 && selectedCurrent == currentTotal;
    if (m_toggleCurrentTabButton) {
        m_toggleCurrentTabButton->setText(allChecked ? "取消全选当前 Tab" : "全选当前 Tab");
        m_toggleCurrentTabButton->setEnabled(currentTotal > 0);
    }
}

QStringList DownloadsPage::selectedFilePaths() const
{
    QStringList filePaths;
    for (auto it = m_cards.cbegin(); it != m_cards.cend(); ++it) {
        if (it.value().selected) filePaths << it.key();
    }
    return filePaths;
}

QStringList DownloadsPage::filePathsForCategory(const QString &category) const
{
    QStringList filePaths;
    for (auto it = m_cards.cbegin(); it != m_cards.cend(); ++it) {
        if (it.value().category == category) filePaths << it.key();
    }
    return filePaths;
}

QStringList DownloadsPage::sortedFilePathsForCategory(const QString &category) const
{
    QStringList paths = filePathsForCategory(category);
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(paths.begin(), paths.end(), [this, &collator](const QString &a, const QString &b) {
        QString nameA = m_cards.value(a).displayName;
        QString nameB = m_cards.value(b).displayName;
        if (nameA.isEmpty()) nameA = QFileInfo(a).completeBaseName();
        if (nameB.isEmpty()) nameB = QFileInfo(b).completeBaseName();
        const int cmp = collator.compare(nameA, nameB);
        if (cmp != 0) return cmp < 0;
        return collator.compare(a, b) < 0;
    });
    return paths;
}

QString DownloadsPage::cardStatusText(const QString &filePath) const
{
    return m_cards.value(filePath).statusText;
}

QString DownloadsPage::cardTargetPath(const QString &filePath) const
{
    return m_cards.value(filePath).targetPath;
}

bool DownloadsPage::containsCard(const QString &filePath) const
{
    return m_cards.contains(filePath);
}

void DownloadsPage::updateSelectionSummary()
{
    const QStringList currentPaths = filePathsForCategory(currentCategory());
    int selectedCurrent = 0;
    for (const QString &filePath : currentPaths) {
        if (m_cards.value(filePath).selected) ++selectedCurrent;
    }
    updateSelectionSummary(selectedCurrent, currentPaths.size(), selectedFilePaths().size());
}

void DownloadsPage::addOrUpdateCard(const ModelUpdateInfo &info, const QString &status, bool sourceAvailable)
{
    if (info.filePath.isEmpty()) return;
    DownloadCardWidgets card = m_cards.value(info.filePath);
    QString effectiveStatus = status;
    if (card.statusText.contains("已忽略") &&
        !status.contains("下载") &&
        !status.contains("完成") &&
        !status.contains("失败")) {
        effectiveStatus = "已忽略更新";
    }

    if (!card.card) {
        auto *frame = new QFrame(m_defaultCardsContainer);
        frame->setProperty("class", "downloadCard");
        frame->setProperty("selected", false);
        frame->setProperty("downloadFilePath", info.filePath);
        frame->setCursor(Qt::PointingHandCursor);
        frame->installEventFilter(this);
        frame->setFixedHeight(156);
        frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        auto *root = new QHBoxLayout(frame);
        root->setContentsMargins(14, 14, 14, 14);
        root->setSpacing(14);

        auto *preview = new QLabel(frame);
        preview->setProperty("downloadFilePath", info.filePath);
        preview->setCursor(Qt::PointingHandCursor);
        preview->installEventFilter(this);
        preview->setFixedSize(96, 128);
        preview->setAlignment(Qt::AlignCenter);
        preview->setProperty("class", "downloadPreview");
        root->addWidget(preview);

        auto *content = new QVBoxLayout();
        content->setSpacing(7);
        root->addLayout(content, 1);

        auto *title = new QLabel(frame);
        title->setProperty("downloadFilePath", info.filePath);
        title->setCursor(Qt::PointingHandCursor);
        title->installEventFilter(this);
        title->setProperty("class", "downloadTitle");
        title->setWordWrap(true);
        title->setMinimumWidth(0);
        title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        content->addWidget(title);

        auto *metaGrid = new QGridLayout();
        metaGrid->setHorizontalSpacing(16);
        metaGrid->setVerticalSpacing(4);
        content->addLayout(metaGrid);

        auto *version = new QLabel(frame);
        auto *size = new QLabel(frame);
        auto *target = new QLabel(frame);
        for (QLabel *label : {version, size, target}) {
            label->setProperty("downloadFilePath", info.filePath);
            label->setCursor(Qt::PointingHandCursor);
            label->installEventFilter(this);
        }
        version->setProperty("class", "downloadMeta");
        size->setProperty("class", "downloadMeta");
        target->setProperty("class", "downloadMeta");
        target->setWordWrap(false);
        version->setMinimumWidth(0);
        target->setMinimumWidth(0);
        version->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        target->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        metaGrid->addWidget(version, 0, 0);
        metaGrid->addWidget(size, 0, 1);
        metaGrid->addWidget(target, 1, 0, 1, 2);

        auto *progress = new QProgressBar(frame);
        progress->setProperty("downloadFilePath", info.filePath);
        progress->setCursor(Qt::PointingHandCursor);
        progress->installEventFilter(this);
        progress->setRange(0, 100);
        progress->setValue(0);
        progress->setTextVisible(true);
        content->addWidget(progress);

        auto *footer = new QHBoxLayout();
        footer->setSpacing(12);
        content->addLayout(footer);
        auto *statusLabel = new QLabel(frame);
        auto *speed = new QLabel(frame);
        for (QLabel *label : {statusLabel, speed}) {
            label->setProperty("downloadFilePath", info.filePath);
            label->setCursor(Qt::PointingHandCursor);
            label->installEventFilter(this);
        }
        statusLabel->setProperty("class", "downloadStatus");
        statusLabel->setWordWrap(true);
        statusLabel->setMinimumWidth(0);
        statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        speed->setProperty("class", "downloadMeta");
        footer->addWidget(statusLabel, 1);
        footer->addWidget(speed);

        auto *buttonLayout = new QVBoxLayout();
        buttonLayout->setSpacing(5);
        auto *sourceButton = new QPushButton(frame);
        sourceButton->setCursor(Qt::PointingHandCursor);
        sourceButton->setMinimumWidth(110);
        auto *civitaiButton = new QPushButton(frame);
        civitaiButton->setCursor(Qt::PointingHandCursor);
        civitaiButton->setMinimumWidth(110);
        auto *downloadButton = new QPushButton(frame);
        downloadButton->setCursor(Qt::PointingHandCursor);
        downloadButton->setMinimumWidth(110);
        auto *ignoreButton = new QPushButton(frame);
        ignoreButton->setCursor(Qt::PointingHandCursor);
        ignoreButton->setMinimumWidth(110);
        for (QPushButton *button : {sourceButton, civitaiButton, downloadButton, ignoreButton}) {
            button->setFixedHeight(26);
        }
        buttonLayout->addWidget(sourceButton);
        buttonLayout->addWidget(civitaiButton);
        buttonLayout->addWidget(downloadButton);
        buttonLayout->addWidget(ignoreButton);
        root->addLayout(buttonLayout);

        card.card = frame;
        card.previewLabel = preview;
        card.titleLabel = title;
        card.versionLabel = version;
        card.sizeLabel = size;
        card.speedLabel = speed;
        card.statusLabel = statusLabel;
        card.targetLabel = target;
        card.progressBar = progress;
        card.sourceButton = sourceButton;
        card.civitaiButton = civitaiButton;
        card.downloadButton = downloadButton;
        card.ignoreButton = ignoreButton;
        m_cards.insert(info.filePath, card);

        const QString initialCategory = categoryForStatus(effectiveStatus);
        QVBoxLayout *targetLayout = cardsLayout(initialCategory);
        const int insertIndex = targetLayout ? qMax(0, targetLayout->count() - 1) : 0;
        if (targetLayout) targetLayout->insertWidget(insertIndex, frame);
        card.category = initialCategory;
        m_cards[info.filePath] = card;

        connect(sourceButton, &QPushButton::clicked, this, [this, filePath = info.filePath]() {
            emit sourceRequested(filePath);
        });
        connect(civitaiButton, &QPushButton::clicked, this, [this, filePath = info.filePath]() {
            emit civitaiRequested(filePath);
        });
        connect(downloadButton, &QPushButton::clicked, this, [this, filePath = info.filePath]() {
            emit downloadRequested(filePath);
        });
        connect(ignoreButton, &QPushButton::clicked, this, [this, filePath = info.filePath]() {
            emit ignoreToggled(filePath);
        });
    }

    card = m_cards.value(info.filePath);
    card.displayName = info.displayName.isEmpty() ? QFileInfo(info.filePath).completeBaseName() : info.displayName;
    card.hasUpdate = info.hasUpdate;
    const QString sizeText = info.sizeMB > 0 ? QString::number(info.sizeMB, 'f', 1) + " MB" : "--";
    const QString targetPath = info.downloadFileName.isEmpty() ? info.filePath : QDir(info.modelDir).filePath(info.downloadFileName);

    if (card.titleLabel) card.titleLabel->setText(card.displayName);
    if (card.versionLabel) {
        card.versionLabel->setText(QString("当前: %1    最新: %2")
                                       .arg(info.currentVersion.isEmpty() ? "--" : info.currentVersion,
                                            info.latestVersion.isEmpty() ? "--" : info.latestVersion));
    }
    if (card.sizeLabel) card.sizeLabel->setText("大小: " + sizeText);
    if (card.targetLabel) {
        card.targetLabel->setText("目标: " + targetPath);
        card.targetLabel->setToolTip(targetPath);
    }
    if (card.sourceButton) {
        card.sourceButton->setText("模型详情");
        card.sourceButton->setEnabled(sourceAvailable);
    }
    if (card.civitaiButton) {
        card.civitaiButton->setText(info.modelId > 0 ? QString("Civitai #%1").arg(info.modelId) : "Civitai");
        card.civitaiButton->setEnabled(info.modelId > 0);
    }
    if (card.downloadButton) {
        card.downloadButton->setText("下载更新");
        card.downloadButton->setEnabled(info.hasUpdate);
    }
    if (card.ignoreButton) {
        card.ignoreButton->setText(effectiveStatus.contains("已忽略") ? "取消忽略" : "忽略更新");
        card.ignoreButton->setEnabled(info.hasUpdate || effectiveStatus.contains("已忽略"));
    }
    card.targetPath = targetPath;
    m_cards[info.filePath] = card;

    updateCardStatus(info.filePath, effectiveStatus);
    if (card.progressBar) card.progressBar->setValue(effectiveStatus.contains("完成") ? 100 : 0);
}

void DownloadsPage::updateCardStatus(const QString &filePath, const QString &status)
{
    if (!m_cards.contains(filePath)) return;
    DownloadCardWidgets card = m_cards.value(filePath);
    card.statusText = status;
    if (card.statusLabel) {
        QString displayStatus = status;
        if (displayStatus.size() > 220) {
            displayStatus = displayStatus.left(217) + "...";
        }
        card.statusLabel->setText(displayStatus);
        card.statusLabel->setToolTip(status);
    }
    if (card.downloadButton) {
        const bool busy = status.contains("下载中") || status.contains("认证重试") || status.contains("队列");
        card.downloadButton->setEnabled(card.hasUpdate && !busy && !status.contains("完成") && !status.contains("已忽略"));
    }
    if (card.ignoreButton) {
        card.ignoreButton->setText(status.contains("已忽略") ? "取消忽略" : "忽略更新");
        card.ignoreButton->setEnabled(card.hasUpdate || status.contains("已忽略"));
    }
    m_cards[filePath] = card;
    placeCardInCategory(filePath, categoryForStatus(status));
    updateSelectionSummary();
}

void DownloadsPage::updateCardProgress(const QString &filePath, int percent, const QString &speedText)
{
    const DownloadCardWidgets card = m_cards.value(filePath);
    if (card.progressBar) card.progressBar->setValue(qBound(0, percent, 100));
    if (card.speedLabel) card.speedLabel->setText(speedText);
}

void DownloadsPage::updateCardTargetPath(const QString &filePath, const QString &targetPath)
{
    if (!m_cards.contains(filePath)) return;
    DownloadCardWidgets card = m_cards.value(filePath);
    card.targetPath = targetPath;
    if (card.targetLabel) {
        card.targetLabel->setText("目标: " + targetPath);
        card.targetLabel->setToolTip(targetPath);
    }
    m_cards[filePath] = card;
}

void DownloadsPage::setCardPreview(const QString &filePath, const QPixmap &pixmap)
{
    const DownloadCardWidgets card = m_cards.value(filePath);
    if (!card.previewLabel || pixmap.isNull()) return;

    QPixmap scaled = pixmap.scaled(card.previewLabel->size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const int x = qMax(0, (scaled.width() - card.previewLabel->width()) / 2);
    const int y = qMax(0, (scaled.height() - card.previewLabel->height()) / 2);
    scaled = scaled.copy(x, y, card.previewLabel->width(), card.previewLabel->height());

    QPixmap rounded(scaled.size());
    rounded.fill(Qt::transparent);
    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(QPointF(0, 0), QSizeF(scaled.size())), 6, 6);
    painter.setClipPath(path);
    painter.drawPixmap(0, 0, scaled);
    painter.end();
    card.previewLabel->setPixmap(rounded);
}

void DownloadsPage::setCardSelected(const QString &filePath, bool selected)
{
    if (!m_cards.contains(filePath)) return;
    DownloadCardWidgets card = m_cards.value(filePath);
    card.selected = selected;
    if (card.card) {
        card.card->setProperty("selected", selected);
        card.card->style()->unpolish(card.card);
        card.card->style()->polish(card.card);
        card.card->update();
    }
    m_cards[filePath] = card;
}

void DownloadsPage::setCurrentTabSelection(bool checked)
{
    for (const QString &filePath : filePathsForCategory(currentCategory())) {
        setCardSelected(filePath, checked);
    }
    updateSelectionSummary();
    emit cardSelectionChanged();
}

void DownloadsPage::toggleCurrentTabSelection()
{
    const QStringList paths = filePathsForCategory(currentCategory());
    bool allChecked = !paths.isEmpty();
    for (const QString &filePath : paths) {
        if (!m_cards.value(filePath).selected) {
            allChecked = false;
            break;
        }
    }
    setCurrentTabSelection(!allChecked);
}

void DownloadsPage::placeCardInCategory(const QString &filePath, const QString &category, bool deferSort)
{
    if (!m_cards.contains(filePath)) return;
    DownloadCardWidgets card = m_cards.value(filePath);
    if (!card.card) return;
    if (card.category == category) {
        if (!deferSort) sortCardsInCategory(category);
        return;
    }

    if (QLayout *oldLayout = card.card->parentWidget() ? card.card->parentWidget()->layout() : nullptr) {
        oldLayout->removeWidget(card.card);
    }
    QVBoxLayout *targetLayout = cardsLayout(category);
    if (targetLayout) {
        const int insertIndex = qMax(0, targetLayout->count() - 1);
        targetLayout->insertWidget(insertIndex, card.card);
    }
    card.category = category;
    m_cards[filePath] = card;
    if (!deferSort) sortCardsInCategory(category);
    updateSelectionSummary();
}

void DownloadsPage::sortCardsInCategory(const QString &category)
{
    QVBoxLayout *layout = cardsLayout(category);
    if (!layout) return;
    const QStringList paths = sortedFilePathsForCategory(category);
    for (const QString &filePath : paths) {
        const DownloadCardWidgets card = m_cards.value(filePath);
        if (!card.card) continue;
        layout->removeWidget(card.card);
        const int insertIndex = qMax(0, layout->count() - 1);
        layout->insertWidget(insertIndex, card.card);
    }
}

void DownloadsPage::sortAllCards()
{
    for (const QString &category : {"updates", "coexisting", "ignored", "latest", "errors", "local"}) {
        sortCardsInCategory(category);
    }
}

void DownloadsPage::removeCard(const QString &filePath)
{
    DownloadCardWidgets card = m_cards.take(filePath);
    if (card.card) {
        card.card->removeEventFilter(this);
        if (QLayout *layout = card.card->parentWidget() ? card.card->parentWidget()->layout() : nullptr) {
            layout->removeWidget(card.card);
        }
        card.card->deleteLater();
    }
    updateSelectionSummary();
}

void DownloadsPage::initializeAppearance()
{
    if (m_filterLabel) m_filterLabel->hide();
    if (m_filterCombo) m_filterCombo->hide();

    const QColor downloadBg("#131922");
    const QList<QScrollArea*> areas = {
        m_scrollUpdates,
        m_scrollCoexisting,
        m_scrollIgnored,
        m_scrollLatest,
        m_scrollErrors,
        m_scrollLocal,
    };
    for (QScrollArea *area : areas) {
        if (!area) continue;
        QWidget *vp = area->viewport();
        if (!vp) continue;
        vp->setAutoFillBackground(true);
        QPalette pal = vp->palette();
        pal.setColor(QPalette::Window, downloadBg);
        vp->setPalette(pal);
    }
}

QString DownloadsPage::categoryForStatus(const QString &status) const
{
    if (status.contains("已忽略")) return "ignored";
    if (status.contains("旧版共存")) return "coexisting";
    if (status.contains("本地") || status.contains("跳过")) return "local";
    if (status.contains("失败") || status.contains("无法") || status.contains("错误") || status.contains("出错")) return "errors";
    if (status.contains("已是最新")) return "latest";
    return "updates";
}

bool DownloadsPage::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        QWidget *widget = qobject_cast<QWidget*>(watched);
        const QString filePath = widget ? widget->property("downloadFilePath").toString() : QString();
        if (!filePath.isEmpty() && m_cards.contains(filePath)) {
            auto *mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                const bool nextSelected = !m_cards.value(filePath).selected;
                setCardSelected(filePath, nextSelected);
                updateSelectionSummary();
                emit cardSelectionChanged();
                return true;
            }
        }
    }
    return QObject::eventFilter(watched, event);
}
