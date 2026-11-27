#include "dialogs/foldersyncdeletedialog.h"
#include "ui_foldersyncdeletedialog.h"

#include "utils/applanguage.h"
#include "utils/tableviewstylehelper.h"

#include <QDir>
#include <QHeaderView>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QTableWidgetItem>

namespace {
QString formatDeleteByteSize(qint64 bytes)
{
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024LL * 1024LL) return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024LL * 1024LL) return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
    return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}
} // namespace

FolderSyncDeleteDialog::FolderSyncDeleteDialog(const FolderCompareResult &compareResult, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::FolderSyncDeleteDialog)
{
    ui->setupUi(this);
    applyUnifiedTableRowStyle(ui->tableDeleteCandidates);

    for (const FolderCompareEntry &entry : compareResult.entries) {
        if (entry.kind == FolderCompareEntryKind::OnlyInA
            || entry.kind == FolderCompareEntryKind::OnlyInB
            || entry.kind == FolderCompareEntryKind::ExcludedInA
            || entry.kind == FolderCompareEntryKind::ExcludedInB) {
            m_candidates.append(entry);
        }
    }

    ui->lblDeleteSummary->setText(
        AppText::tr("可清理候选文件共 %1 个（仅在 B：%2，仅在 A：%3，A 中排除项：%4，B 中排除项：%5）。请勾选要移入回收站的文件：")
            .arg(m_candidates.size())
            .arg(compareResult.onlyInBCount)
            .arg(compareResult.onlyInACount)
            .arg(compareResult.excludedInACount)
            .arg(compareResult.excludedInBCount));

    ui->btnSelectOnlyInB->setEnabled(compareResult.onlyInBCount > 0);
    ui->btnSelectOnlyInA->setEnabled(compareResult.onlyInACount > 0);
    ui->btnSelectExcludedA->setEnabled(compareResult.excludedInACount > 0);
    ui->btnSelectExcludedB->setEnabled(compareResult.excludedInBCount > 0);

    connect(ui->btnSelectOnlyInB, &QPushButton::clicked, this, [this]() {
        onSelectByCategory(FolderCompareEntryKind::OnlyInB);
    });
    connect(ui->btnSelectOnlyInA, &QPushButton::clicked, this, [this]() {
        onSelectByCategory(FolderCompareEntryKind::OnlyInA);
    });
    connect(ui->btnSelectExcludedA, &QPushButton::clicked, this, [this]() {
        onSelectByCategory(FolderCompareEntryKind::ExcludedInA);
    });
    connect(ui->btnSelectExcludedB, &QPushButton::clicked, this, [this]() {
        onSelectByCategory(FolderCompareEntryKind::ExcludedInB);
    });
    connect(ui->btnSelectAll, &QPushButton::clicked, this, &FolderSyncDeleteDialog::onSelectAll);
    connect(ui->btnSelectNone, &QPushButton::clicked, this, &FolderSyncDeleteDialog::onSelectNone);
    connect(ui->btnMoveToTrash, &QPushButton::clicked, this, &FolderSyncDeleteDialog::onConfirmMoveToTrash);
    connect(ui->btnClose, &QPushButton::clicked, this, &QDialog::reject);
    connect(ui->tableDeleteCandidates, &QTableWidget::itemChanged,
            this, [this](QTableWidgetItem *) { updateSelectionSummary(); });

    populateTable();
    updateSelectionSummary();
}

FolderSyncDeleteDialog::~FolderSyncDeleteDialog()
{
    delete ui;
}

void FolderSyncDeleteDialog::populateTable()
{
    const QSignalBlocker blocker(ui->tableDeleteCandidates);
    ui->tableDeleteCandidates->setRowCount(m_candidates.size());

    auto *header = ui->tableDeleteCandidates->horizontalHeader();
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    header->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(2, QHeaderView::Stretch);
    header->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(4, QHeaderView::ResizeToContents);

    for (int row = 0; row < m_candidates.size(); ++row) {
        const FolderCompareEntry &entry = m_candidates.at(row);

        auto *fileItem = new QTableWidgetItem(entry.relativePath);
        fileItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        // Default pre-check: OnlyInB or Excluded items so user can quickly review & confirm.
        const bool defaultChecked = (entry.kind == FolderCompareEntryKind::OnlyInB
                                     || entry.kind == FolderCompareEntryKind::ExcludedInB);
        fileItem->setCheckState(defaultChecked ? Qt::Checked : Qt::Unchecked);
        fileItem->setData(Qt::UserRole, entry.absolutePath);
        fileItem->setData(Qt::UserRole + 1, static_cast<int>(entry.kind));
        fileItem->setToolTip(QDir::toNativeSeparators(entry.absolutePath));

        auto *sideItem = new QTableWidgetItem(AppText::display(entry.sideLabel));
        sideItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

        const QString nativeDir = QDir::toNativeSeparators(entry.parentDir);
        auto *dirItem = new QTableWidgetItem(nativeDir);
        dirItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        dirItem->setToolTip(nativeDir);

        auto *sizeItem = new QTableWidgetItem(formatDeleteByteSize(entry.fileSize));
        sizeItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

        auto *reasonItem = new QTableWidgetItem(AppText::display(entry.reason));
        reasonItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        reasonItem->setToolTip(reasonItem->text());

        ui->tableDeleteCandidates->setItem(row, 0, fileItem);
        ui->tableDeleteCandidates->setItem(row, 1, sideItem);
        ui->tableDeleteCandidates->setItem(row, 2, dirItem);
        ui->tableDeleteCandidates->setItem(row, 3, sizeItem);
        ui->tableDeleteCandidates->setItem(row, 4, reasonItem);
    }
}

void FolderSyncDeleteDialog::onSelectByCategory(FolderCompareEntryKind kind)
{
    const QSignalBlocker blocker(ui->tableDeleteCandidates);
    for (int row = 0; row < ui->tableDeleteCandidates->rowCount(); ++row) {
        if (QTableWidgetItem *item = ui->tableDeleteCandidates->item(row, 0)) {
            const auto itemKind = static_cast<FolderCompareEntryKind>(item->data(Qt::UserRole + 1).toInt());
            item->setCheckState(itemKind == kind ? Qt::Checked : Qt::Unchecked);
        }
    }
    updateSelectionSummary();
}

void FolderSyncDeleteDialog::onSelectAll()
{
    const QSignalBlocker blocker(ui->tableDeleteCandidates);
    for (int row = 0; row < ui->tableDeleteCandidates->rowCount(); ++row) {
        if (QTableWidgetItem *item = ui->tableDeleteCandidates->item(row, 0)) {
            item->setCheckState(Qt::Checked);
        }
    }
    updateSelectionSummary();
}

void FolderSyncDeleteDialog::onSelectNone()
{
    const QSignalBlocker blocker(ui->tableDeleteCandidates);
    for (int row = 0; row < ui->tableDeleteCandidates->rowCount(); ++row) {
        if (QTableWidgetItem *item = ui->tableDeleteCandidates->item(row, 0)) {
            item->setCheckState(Qt::Unchecked);
        }
    }
    updateSelectionSummary();
}

void FolderSyncDeleteDialog::updateSelectionSummary()
{
    int count = 0;
    qint64 totalBytes = 0;
    for (int row = 0; row < ui->tableDeleteCandidates->rowCount(); ++row) {
        if (QTableWidgetItem *item = ui->tableDeleteCandidates->item(row, 0)) {
            if (item->checkState() == Qt::Checked) {
                ++count;
                if (row < m_candidates.size()) {
                    totalBytes += m_candidates.at(row).fileSize;
                }
            }
        }
    }
    ui->lblSelectedCount->setText(
        AppText::tr("已勾选 %1 个文件（共 %2）").arg(count).arg(formatDeleteByteSize(totalBytes)));
    ui->btnMoveToTrash->setEnabled(count > 0);
}

QStringList FolderSyncDeleteDialog::selectedFilePaths() const
{
    QStringList paths;
    for (int row = 0; row < ui->tableDeleteCandidates->rowCount(); ++row) {
        if (QTableWidgetItem *item = ui->tableDeleteCandidates->item(row, 0)) {
            if (item->checkState() == Qt::Checked) {
                paths.append(item->data(Qt::UserRole).toString());
            }
        }
    }
    return paths;
}

void FolderSyncDeleteDialog::onConfirmMoveToTrash()
{
    const QStringList paths = selectedFilePaths();
    if (paths.isEmpty()) return;

    int countA = 0;
    int countB = 0;
    for (int row = 0; row < ui->tableDeleteCandidates->rowCount(); ++row) {
        if (QTableWidgetItem *item = ui->tableDeleteCandidates->item(row, 0)) {
            if (item->checkState() != Qt::Checked) continue;
            const auto kind = static_cast<FolderCompareEntryKind>(item->data(Qt::UserRole + 1).toInt());
            if (kind == FolderCompareEntryKind::OnlyInA || kind == FolderCompareEntryKind::ExcludedInA) {
                ++countA;
            } else {
                ++countB;
            }
        }
    }

    const auto reply = QMessageBox::question(
        this,
        AppText::tr("二次确认删除"),
        AppText::tr("确定将选中的 %1 个文件移至系统回收站吗？\n（文件夹 A：%2 个，文件夹 B：%3 个）")
            .arg(paths.size())
            .arg(countA)
            .arg(countB),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        accept();
    }
}
