#include "downloadspage.h"
#include "ui_downloadspage.h"
#include "styleconstants.h"
#include "tableviewstylehelper.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QProgressBar>
#include <QPixmap>
#include <QPushButton>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollArea>
#include <QSaveFile>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <algorithm>

namespace {
constexpr int RoleFilePath = Qt::UserRole;
constexpr int RoleModelIdText = Qt::UserRole + 1;
constexpr int RoleVersionIdText = Qt::UserRole + 2;
constexpr int RoleSha256 = Qt::UserRole + 3;

QString metadataCategoryDisplayName(const QString &category)
{
    if (category == "missing") return QStringLiteral("缺失元信息");
    if (category == "existing") return QStringLiteral("已有元信息");
    if (category == "local") return QStringLiteral("本地/已编辑");
    if (category == "failed") return QStringLiteral("同步失败");
    if (category == "invalid") return QStringLiteral("JSON 异常");
    if (category == "no_ids") return QStringLiteral("缺少识别字段");
    return QStringLiteral("全部");
}

QJsonObject metadataScanItemToJson(const MetadataScanItem &item)
{
    QJsonObject obj;
    obj["filePath"] = item.filePath;
    obj["displayName"] = item.displayName;
    obj["jsonPath"] = item.jsonPath;
    obj["previewPath"] = item.previewPath;
    obj["modelIdText"] = item.modelIdText;
    obj["versionIdText"] = item.versionIdText;
    obj["sha256"] = item.sha256;
    obj["status"] = item.status;
    obj["category"] = item.category;
    obj["lastSyncedAt"] = item.lastSyncedAt;
    obj["lastSyncedSource"] = item.lastSyncedSource;
    obj["syncFailure"] = item.syncFailure;
    obj["errorText"] = item.errorText;
    obj["localEdited"] = item.localEdited;
    obj["checked"] = item.checked;
    return obj;
}

MetadataScanItem metadataScanItemFromJson(const QJsonObject &obj)
{
    MetadataScanItem item;
    item.filePath = obj.value("filePath").toString();
    item.displayName = obj.value("displayName").toString(QFileInfo(item.filePath).completeBaseName());
    item.jsonPath = obj.value("jsonPath").toString();
    item.previewPath = obj.value("previewPath").toString();
    item.modelIdText = obj.value("modelIdText").toString();
    item.versionIdText = obj.value("versionIdText").toString();
    item.sha256 = obj.value("sha256").toString();
    item.status = obj.value("status").toString();
    item.category = obj.value("category").toString("all");
    item.lastSyncedAt = obj.value("lastSyncedAt").toString();
    item.lastSyncedSource = obj.value("lastSyncedSource").toString();
    item.syncFailure = obj.value("syncFailure").toString();
    item.errorText = obj.value("errorText").toString();
    item.localEdited = obj.value("localEdited").toBool(false);
    item.checked = obj.value("checked").toBool(false);
    return item;
}

QJsonObject metadataHealthIssueToJson(const MetadataHealthIssue &issue)
{
    QJsonObject obj;
    obj["severity"] = issue.severity;
    obj["modelName"] = issue.modelName;
    obj["issue"] = issue.issue;
    obj["suggestion"] = issue.suggestion;
    obj["filePath"] = issue.filePath;
    return obj;
}

MetadataHealthIssue metadataHealthIssueFromJson(const QJsonObject &obj)
{
    MetadataHealthIssue issue;
    issue.severity = obj.value("severity").toString();
    issue.modelName = obj.value("modelName").toString();
    issue.issue = obj.value("issue").toString();
    issue.suggestion = obj.value("suggestion").toString();
    issue.filePath = obj.value("filePath").toString();
    return issue;
}
}

DownloadsPage::DownloadsPage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::DownloadsPage)
{
    ui->setupUi(this);
    applyUnifiedTableRowStyle(ui->tableMetadataScan);
    applyUnifiedTableRowStyle(ui->tableHealth);
    for (QTableWidget *table : {ui->tableMetadataScan, ui->tableHealth}) {
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        table->horizontalHeader()->setStretchLastSection(false);
        table->horizontalHeader()->setSectionsClickable(true);
        table->setSortingEnabled(true);
        table->setAlternatingRowColors(false);
        table->setShowGrid(false);
        table->setFocusPolicy(Qt::NoFocus);
        table->verticalHeader()->setVisible(false);
    }
    applyMetadataTableColumnLayout();
    applyHealthTableColumnLayout();
    updateSelectedMetadataIdentityLabel();

    connect(ui->btnMetadataScan, &QPushButton::clicked, this, &DownloadsPage::metadataScanRequested);
    connect(ui->btnMetadataSyncSelected, &QPushButton::clicked, this, [this]() {
        emit metadataSyncRequested(checkedMetadataScanFilePaths());
    });
    connect(ui->btnMetadataUpdateSelected, &QPushButton::clicked, this, [this]() {
        emit metadataUpdateRequested(checkedMetadataScanFilePaths());
    });
    connect(ui->btnMetadataToggleCurrent, &QPushButton::clicked, this, [this]() {
        const QString category = currentMetadataCategory();
        bool allChecked = true;
        bool hasRow = false;
        for (const MetadataScanItem &item : std::as_const(m_metadataScanItems)) {
            if (category != "all" && item.category != category) continue;
            hasRow = true;
            if (!item.checked) {
                allChecked = false;
                break;
            }
        }
        if (hasRow) setCurrentMetadataCategoryChecked(!allChecked);
    });
    connect(ui->btnMetadataOpenModel, &QPushButton::clicked, this, [this]() {
        const int row = ui->tableMetadataScan->currentRow();
        if (row < 0) return;
        QTableWidgetItem *item = ui->tableMetadataScan->item(row, 1);
        if (!item) return;
        emit metadataOpenModelRequested(item->data(RoleFilePath).toString());
    });
    connect(ui->btnMetadataOpenFolder, &QPushButton::clicked, this, [this]() {
        const int row = ui->tableMetadataScan->currentRow();
        if (row < 0) return;
        QTableWidgetItem *item = ui->tableMetadataScan->item(row, 1);
        if (!item) return;
        emit metadataOpenFolderRequested(item->data(RoleFilePath).toString());
    });
    connect(ui->tabMetadataScanStatus, &QTabWidget::currentChanged, this, [this]() {
        refreshMetadataScanTable();
    });
    connect(ui->tableMetadataScan, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (!item || item->column() != 0) return;
        const QString filePath = item->data(RoleFilePath).toString();
        for (MetadataScanItem &scanItem : m_metadataScanItems) {
            if (scanItem.filePath == filePath) {
                scanItem.checked = item->checkState() == Qt::Checked;
                break;
            }
        }
        updateMetadataSelectionSummary();
        saveMetadataResultCache();
        emit metadataSelectionChanged();
    });
    connect(ui->tableMetadataScan, &QTableWidget::itemSelectionChanged, this, [this]() {
        updateSelectedMetadataIdentityLabel();
        updateMetadataActionButtons();
    });

    connect(ui->btnRunHealthCheck, &QPushButton::clicked, this, &DownloadsPage::healthCheckRequested);
    connect(ui->btnCopyHealth, &QPushButton::clicked, this, &DownloadsPage::copySelectedHealthIssues);
    connect(ui->btnOpenHealthModel, &QPushButton::clicked, this, [this]() {
        const int row = ui->tableHealth->currentRow();
        if (row < 0) return;
        QTableWidgetItem *item = ui->tableHealth->item(row, 0);
        if (!item) return;
        emit healthOpenModelRequested(item->data(RoleFilePath).toString());
    });
    connect(ui->btnOpenHealthFolder, &QPushButton::clicked, this, [this]() {
        const int row = ui->tableHealth->currentRow();
        if (row < 0) return;
        QTableWidgetItem *item = ui->tableHealth->item(row, 0);
        if (!item) return;
        emit healthOpenFolderRequested(item->data(RoleFilePath).toString());
    });
    connect(ui->tableHealth, &QTableWidget::itemSelectionChanged, this, [this]() {
        updateHealthActionButtons();
    });

    loadMetadataResultCache();
    updateVersionActionButtons();
    updateMetadataActionButtons();
    updateHealthActionButtons();
}

DownloadsPage::~DownloadsPage()
{
    delete ui;
}

QComboBox *DownloadsPage::filterCombo() const { return ui->comboDownloadsFilter; }
QTabWidget *DownloadsPage::statusTabs() const { return ui->tabDownloadsStatus; }

QPushButton *DownloadsPage::checkCurrentButton() const { return ui->btnDownloadsCheckCurrent; }
QPushButton *DownloadsPage::checkSelectedButton() const { return ui->btnDownloadsCheckSelected; }
QPushButton *DownloadsPage::checkAllButton() const { return ui->btnDownloadsCheckAll; }
QPushButton *DownloadsPage::downloadSelectedButton() const { return ui->btnDownloadsDownloadSelected; }
QPushButton *DownloadsPage::cancelButton() const { return ui->btnDownloadsCancel; }
QPushButton *DownloadsPage::retryButton() const { return ui->btnDownloadsRetry; }
QPushButton *DownloadsPage::openFolderButton() const { return ui->btnDownloadsOpenFolder; }
QPushButton *DownloadsPage::clearCompletedButton() const { return ui->btnDownloadsClearCompleted; }
QPushButton *DownloadsPage::toggleCurrentTabButton() const { return ui->btnDownloadsToggleCurrentTab; }

QVBoxLayout *DownloadsPage::cardsLayout(const QString &category) const
{
    if (category == "coexisting") return ui->downloadCardsLayoutCoexisting;
    if (category == "ignored") return ui->downloadCardsLayoutIgnored;
    if (category == "latest") return ui->downloadCardsLayoutLatest;
    if (category == "errors") return ui->downloadCardsLayoutErrors;
    if (category == "local") return ui->downloadCardsLayoutLocal;
    return ui->downloadCardsLayout;
}

QString DownloadsPage::currentCategory() const
{
    switch (ui->tabDownloadsStatus->currentIndex()) {
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
    ui->lblDownloadsStatus->setText(text);
}

void DownloadsPage::setModelSelectionAvailability(bool hasCurrentModel, bool hasSelectedModels)
{
    m_hasCurrentModel = hasCurrentModel;
    m_hasSelectedModels = hasSelectedModels;
    updateVersionActionButtons();
}

void DownloadsPage::setUpdateCheckButtonsEnabled(bool enabled)
{
    m_updateCheckBusy = !enabled;
    updateVersionActionButtons();
}

void DownloadsPage::updateVersionActionButtons()
{
    const bool hasSelectedCards = !selectedFilePaths().isEmpty();
    ui->btnDownloadsCheckCurrent->setEnabled(!m_updateCheckBusy && m_hasCurrentModel);
    ui->btnDownloadsCheckSelected->setEnabled(!m_updateCheckBusy && m_hasSelectedModels);
    ui->btnDownloadsCheckAll->setEnabled(!m_updateCheckBusy);
    ui->btnDownloadsDownloadSelected->setEnabled(hasSelectedCards);
    ui->btnDownloadsCancel->setEnabled(hasSelectedCards);
    ui->btnDownloadsRetry->setEnabled(hasSelectedCards);
    ui->btnDownloadsOpenFolder->setEnabled(hasSelectedCards);
}

void DownloadsPage::updateSelectionSummary(int selectedCurrent, int currentTotal, int selectedTotal)
{
    ui->lblDownloadsSelected->setText(QString("当前 Tab 已选择 %1/%2 个模型\n全部已选择 %3 个模型")
                                          .arg(selectedCurrent)
                                          .arg(currentTotal)
                                          .arg(selectedTotal));

    const bool allChecked = currentTotal > 0 && selectedCurrent == currentTotal;
    ui->btnDownloadsToggleCurrentTab->setText(allChecked ? "取消全选当前 Tab" : "全选当前 Tab");
    ui->btnDownloadsToggleCurrentTab->setEnabled(currentTotal > 0);
    updateVersionActionButtons();
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
        auto *frame = new QFrame(ui->downloadCardsContainer);
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
    ui->lblDownloadsFilter->hide();
    ui->comboDownloadsFilter->hide();

    const QColor downloadBg(AppStyle::DownloadCardBackground);
    const QList<QScrollArea*> areas = {
        ui->scrollDownloadsCards,
        ui->scrollDownloadsCoexisting,
        ui->scrollDownloadsIgnored,
        ui->scrollDownloadsLatest,
        ui->scrollDownloadsErrors,
        ui->scrollDownloadsLocal,
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

QString DownloadsPage::currentMetadataCategory() const
{
    switch (ui->tabMetadataScanStatus->currentIndex()) {
    case 1: return "missing";
    case 2: return "existing";
    case 3: return "local";
    case 4: return "failed";
    case 5: return "invalid";
    case 6: return "no_ids";
    default: return "all";
    }
}

bool DownloadsPage::metadataItemMatchesCurrentCategory(const MetadataScanItem &item) const
{
    const QString category = currentMetadataCategory();
    return category == "all" || item.category == category;
}

void DownloadsPage::applyMetadataTableColumnLayout()
{
    QHeaderView *header = ui->tableMetadataScan->horizontalHeader();
    header->setMinimumSectionSize(56);
    header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    header->setSectionResizeMode(2, QHeaderView::Interactive);
    header->setSectionResizeMode(3, QHeaderView::Interactive);
    header->setSectionResizeMode(4, QHeaderView::Interactive);
    header->resizeSection(2, 120);
    header->resizeSection(3, 180);
    header->resizeSection(4, 190);
}

void DownloadsPage::applyHealthTableColumnLayout()
{
    QHeaderView *header = ui->tableHealth->horizontalHeader();
    header->setMinimumSectionSize(64);
    header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(1, QHeaderView::Interactive);
    header->setSectionResizeMode(2, QHeaderView::Stretch);
    header->setSectionResizeMode(3, QHeaderView::Stretch);
    header->resizeSection(1, 260);
}

void DownloadsPage::updateSelectedMetadataIdentityLabel()
{
    const int row = ui->tableMetadataScan->currentRow();
    QTableWidgetItem *item = row >= 0 ? ui->tableMetadataScan->item(row, 1) : nullptr;
    if (!item) {
        ui->lblMetadataSelectedIdentity->setText("未选择模型");
        ui->lblMetadataSelectedIdentity->setToolTip(QString());
        return;
    }

    const QString modelId = item->data(RoleModelIdText).toString().trimmed();
    const QString versionId = item->data(RoleVersionIdText).toString().trimmed();
    const QString sha256 = item->data(RoleSha256).toString().trimmed();
    const QString shortSha = sha256.isEmpty() ? QStringLiteral("--") : sha256.left(12);
    ui->lblMetadataSelectedIdentity->setText(QString("Model ID: %1 | Version ID: %2 | SHA256: %3")
                                                 .arg(modelId.isEmpty() ? QStringLiteral("--") : modelId,
                                                      versionId.isEmpty() ? QStringLiteral("--") : versionId,
                                                      shortSha));
    ui->lblMetadataSelectedIdentity->setToolTip(QString("模型路径: %1\nModel ID: %2\nVersion ID: %3\nSHA256: %4")
                                                    .arg(item->data(RoleFilePath).toString(),
                                                         modelId.isEmpty() ? QStringLiteral("--") : modelId,
                                                         versionId.isEmpty() ? QStringLiteral("--") : versionId,
                                                         sha256.isEmpty() ? QStringLiteral("--") : sha256));
}

void DownloadsPage::setMetadataScanRunning(bool running)
{
    m_metadataScanRunning = running;
    ui->btnMetadataScan->setEnabled(!running);
    ui->lblMetadataScanStatus->setText(running ? "正在后台扫描模型元信息..." : "元信息扫描完成。");
    updateMetadataActionButtons();
}

void DownloadsPage::setMetadataScanItems(const QVector<MetadataScanItem> &items)
{
    m_metadataScanItems = items;
    refreshMetadataScanTable();
    saveMetadataResultCache();
}

void DownloadsPage::refreshMetadataScanTable()
{
    if (QWidget *currentPage = ui->tabMetadataScanStatus->currentWidget()) {
        if (QLayout *oldLayout = ui->tableMetadataScan->parentWidget() ? ui->tableMetadataScan->parentWidget()->layout() : nullptr) {
            oldLayout->removeWidget(ui->tableMetadataScan);
        }
        if (QLayout *newLayout = currentPage->layout()) {
            newLayout->addWidget(ui->tableMetadataScan);
        }
    }

    QSignalBlocker blocker(ui->tableMetadataScan);
    ui->tableMetadataScan->setSortingEnabled(false);
    ui->tableMetadataScan->setRowCount(0);

    int row = 0;
    for (const MetadataScanItem &item : std::as_const(m_metadataScanItems)) {
        if (!metadataItemMatchesCurrentCategory(item)) continue;
        ui->tableMetadataScan->insertRow(row);

        auto *checkItem = new QTableWidgetItem();
        checkItem->setFlags((checkItem->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        checkItem->setCheckState(item.checked ? Qt::Checked : Qt::Unchecked);
        checkItem->setData(RoleFilePath, item.filePath);
        ui->tableMetadataScan->setItem(row, 0, checkItem);

        auto *nameItem = new QTableWidgetItem(item.displayName);
        nameItem->setData(RoleFilePath, item.filePath);
        nameItem->setData(RoleModelIdText, item.modelIdText);
        nameItem->setData(RoleVersionIdText, item.versionIdText);
        nameItem->setData(RoleSha256, item.sha256);
        nameItem->setToolTip(QString("路径: %1\nModel ID: %2\nVersion ID: %3\nSHA256: %4")
                                 .arg(item.filePath,
                                      item.modelIdText.isEmpty() ? QStringLiteral("--") : item.modelIdText,
                                      item.versionIdText.isEmpty() ? QStringLiteral("--") : item.versionIdText,
                                      item.sha256.isEmpty() ? QStringLiteral("--") : item.sha256));
        ui->tableMetadataScan->setItem(row, 1, nameItem);
        ui->tableMetadataScan->setItem(row, 2, new QTableWidgetItem(metadataCategoryDisplayName(item.category)));
        ui->tableMetadataScan->setItem(row, 3, new QTableWidgetItem(item.status));
        const QString syncedText = item.lastSyncedAt.isEmpty()
            ? QStringLiteral("-")
            : QString("%1 (%2)").arg(item.lastSyncedAt, item.lastSyncedSource);
        ui->tableMetadataScan->setItem(row, 4, new QTableWidgetItem(syncedText));
        ++row;
    }

    ui->tableMetadataScan->setSortingEnabled(true);
    updateMetadataSelectionSummary();
    updateSelectedMetadataIdentityLabel();
    updateMetadataActionButtons();
}

QStringList DownloadsPage::checkedMetadataScanFilePaths() const
{
    QStringList paths;
    for (const MetadataScanItem &item : m_metadataScanItems) {
        if (item.checked) paths << item.filePath;
    }
    return paths;
}

void DownloadsPage::setCurrentMetadataCategoryChecked(bool checked)
{
    const QString category = currentMetadataCategory();
    for (MetadataScanItem &item : m_metadataScanItems) {
        if (category == "all" || item.category == category) item.checked = checked;
    }
    refreshMetadataScanTable();
    saveMetadataResultCache();
    emit metadataSelectionChanged();
}

void DownloadsPage::updateMetadataSelectionSummary()
{
    const QString category = currentMetadataCategory();
    int currentTotal = 0;
    int currentChecked = 0;
    int allChecked = 0;
    for (const MetadataScanItem &item : std::as_const(m_metadataScanItems)) {
        if (item.checked) ++allChecked;
        if (category == "all" || item.category == category) {
            ++currentTotal;
            if (item.checked) ++currentChecked;
        }
    }
    const bool allCurrentChecked = currentTotal > 0 && currentChecked == currentTotal;
    ui->btnMetadataToggleCurrent->setText(allCurrentChecked ? "取消全选当前分类" : "全选当前分类");
    ui->btnMetadataToggleCurrent->setEnabled(!m_metadataScanRunning && currentTotal > 0);
    ui->lblMetadataScanStatus->setText(QString("当前分类 %1/%2，全部已选择 %3 个模型。")
                                           .arg(currentChecked)
                                           .arg(currentTotal)
                                           .arg(allChecked));
    updateMetadataActionButtons();
}

void DownloadsPage::updateMetadataActionButtons()
{
    const bool hasChecked = !checkedMetadataScanFilePaths().isEmpty();
    const int row = ui->tableMetadataScan->currentRow();
    const bool hasCurrentRow = row >= 0 && ui->tableMetadataScan->item(row, 1);
    ui->btnMetadataSyncSelected->setEnabled(!m_metadataScanRunning && hasChecked);
    ui->btnMetadataUpdateSelected->setEnabled(!m_metadataScanRunning && hasChecked);
    ui->btnMetadataOpenModel->setEnabled(hasCurrentRow);
    ui->btnMetadataOpenFolder->setEnabled(hasCurrentRow);
}

void DownloadsPage::updateMetadataScanItemStatus(const QString &filePath, const QString &status, const QString &category, const QString &lastSyncedAt, const QString &lastSyncedSource)
{
    for (MetadataScanItem &item : m_metadataScanItems) {
        if (item.filePath != filePath) continue;
        item.status = status;
        if (!category.isEmpty()) item.category = category;
        if (!lastSyncedAt.isEmpty()) item.lastSyncedAt = lastSyncedAt;
        if (!lastSyncedSource.isEmpty()) item.lastSyncedSource = lastSyncedSource;
        break;
    }
    refreshMetadataScanTable();
    saveMetadataResultCache();
}

void DownloadsPage::setHealthCheckRunning(bool running)
{
    m_healthCheckRunning = running;
    ui->btnRunHealthCheck->setEnabled(!running);
    ui->lblHealthStatus->setText(running ? "正在后台检查元数据..." : "元数据健康检查完成。");
    updateHealthActionButtons();
}

void DownloadsPage::setHealthIssues(const QVector<MetadataHealthIssue> &issues)
{
    m_healthIssues = issues;
    ui->tableHealth->setSortingEnabled(false);
    ui->tableHealth->setRowCount(issues.size());
    for (int row = 0; row < issues.size(); ++row) {
        const MetadataHealthIssue &issue = issues[row];
        auto *severityItem = new QTableWidgetItem(issue.severity);
        severityItem->setData(RoleFilePath, issue.filePath);
        ui->tableHealth->setItem(row, 0, severityItem);
        ui->tableHealth->setItem(row, 1, new QTableWidgetItem(issue.modelName));
        ui->tableHealth->setItem(row, 2, new QTableWidgetItem(issue.issue));
        ui->tableHealth->setItem(row, 3, new QTableWidgetItem(issue.suggestion));
    }
    ui->tableHealth->setSortingEnabled(true);
    ui->lblHealthStatus->setText(QString("检查完成，共发现 %1 条问题/提示。").arg(issues.size()));
    saveMetadataResultCache();
    updateHealthActionButtons();
}

void DownloadsPage::updateHealthActionButtons()
{
    const int row = ui->tableHealth->currentRow();
    const bool hasCurrentRow = row >= 0 && ui->tableHealth->item(row, 0);
    const bool hasSelection = !ui->tableHealth->selectedRanges().isEmpty();
    ui->btnCopyHealth->setEnabled(!m_healthCheckRunning && hasSelection);
    ui->btnOpenHealthModel->setEnabled(!m_healthCheckRunning && hasCurrentRow);
    ui->btnOpenHealthFolder->setEnabled(!m_healthCheckRunning && hasCurrentRow);
}

void DownloadsPage::copySelectedHealthIssues() const
{
    QStringList lines;
    const auto ranges = ui->tableHealth->selectedRanges();
    for (const QTableWidgetSelectionRange &range : ranges) {
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) {
            QStringList cols;
            for (int col = 0; col < ui->tableHealth->columnCount(); ++col) {
                if (QTableWidgetItem *item = ui->tableHealth->item(row, col)) cols << item->text();
            }
            lines << cols.join('\t');
        }
    }
    if (!lines.isEmpty()) QApplication::clipboard()->setText(lines.join('\n'));
}

QString DownloadsPage::metadataResultCachePath() const
{
    return qApp->applicationDirPath() + "/config/metadata_scan_cache.json";
}

void DownloadsPage::loadMetadataResultCache()
{
    QFile file(metadataResultCachePath());
    if (!file.open(QIODevice::ReadOnly)) return;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return;
    const QJsonObject root = doc.object();

    QVector<MetadataScanItem> scanItems;
    const QJsonArray scanArray = root.value("metadataScanItems").toArray();
    scanItems.reserve(scanArray.size());
    for (const QJsonValue &value : scanArray) {
        const MetadataScanItem item = metadataScanItemFromJson(value.toObject());
        if (!item.filePath.isEmpty()) scanItems.append(item);
    }

    QVector<MetadataHealthIssue> healthIssues;
    const QJsonArray healthArray = root.value("healthIssues").toArray();
    healthIssues.reserve(healthArray.size());
    for (const QJsonValue &value : healthArray) {
        const MetadataHealthIssue issue = metadataHealthIssueFromJson(value.toObject());
        if (!issue.filePath.isEmpty() || !issue.issue.isEmpty()) healthIssues.append(issue);
    }

    if (!scanItems.isEmpty()) {
        m_metadataScanItems = scanItems;
        refreshMetadataScanTable();
        ui->lblMetadataScanStatus->setText(QString("已恢复上次元信息扫描结果，共 %1 个模型。").arg(scanItems.size()));
    }
    if (!healthIssues.isEmpty()) {
        setHealthIssues(healthIssues);
        ui->lblHealthStatus->setText(QString("已恢复上次健康检查结果，共 %1 条问题/提示。").arg(healthIssues.size()));
    }
}

void DownloadsPage::saveMetadataResultCache() const
{
    const QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);

    QJsonArray scanArray;
    for (const MetadataScanItem &item : m_metadataScanItems) {
        if (!item.filePath.isEmpty()) scanArray.append(metadataScanItemToJson(item));
    }

    QJsonArray healthArray;
    for (const MetadataHealthIssue &issue : m_healthIssues) {
        healthArray.append(metadataHealthIssueToJson(issue));
    }

    QSaveFile file(metadataResultCachePath());
    if (!file.open(QIODevice::WriteOnly)) return;
    QJsonObject root;
    root["version"] = 1;
    root["savedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    root["metadataScanItems"] = scanArray;
    root["healthIssues"] = healthArray;
    file.write(QJsonDocument(root).toJson());
    file.commit();
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
