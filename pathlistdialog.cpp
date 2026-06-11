#include "pathlistdialog.h"
#include "tableviewstylehelper.h"
#include "ui_pathlistdialog.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QHeaderView>
#include <QSet>
#include <QTableWidgetItem>
#include <algorithm>

PathListDialog::PathListDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::PathListDialog)
{
    ui->setupUi(this);
    applyUnifiedTableRowStyle(ui->tablePaths);
    ui->tablePaths->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->tablePaths->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tablePaths->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->tablePaths->setShowGrid(false);
    ui->tablePaths->setFocusPolicy(Qt::NoFocus);
    ui->tablePaths->horizontalHeader()->setStretchLastSection(false);
    ui->tablePaths->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->tablePaths->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ui->tablePaths->verticalHeader()->setVisible(false);

    connect(ui->btnAddPath, &QPushButton::clicked, this, &PathListDialog::onAddClicked);
    connect(ui->btnRemovePath, &QPushButton::clicked, this, &PathListDialog::onRemoveClicked);
    connect(ui->btnMoveUp, &QPushButton::clicked, this, &PathListDialog::onMoveUpClicked);
    connect(ui->btnMoveDown, &QPushButton::clicked, this, &PathListDialog::onMoveDownClicked);
}

PathListDialog::~PathListDialog()
{
    delete ui;
}

void PathListDialog::setPathEntries(const QList<ManagedPathEntry> &entries)
{
    ui->tablePaths->setRowCount(0);
    for (const ManagedPathEntry &entry : entries) {
        const QString path = entry.path.trimmed();
        if (!path.isEmpty()) {
            addPathRow(path, entry.enabled);
        }
    }
}

void PathListDialog::setSelectionMode(SelectionMode mode, const QString &fileFilter)
{
    m_selectionMode = mode;
    m_fileFilter = fileFilter;
}

QList<ManagedPathEntry> PathListDialog::pathEntries() const
{
    QList<ManagedPathEntry> result;
    for (int i = 0; i < ui->tablePaths->rowCount(); ++i) {
        QTableWidgetItem *pathItem = ui->tablePaths->item(i, 0);
        if (!pathItem) continue;

        const QString path = pathItem->text().trimmed();
        if (path.isEmpty()) continue;

        QTableWidgetItem *enabledItem = ui->tablePaths->item(i, 1);
        const bool enabled = (enabledItem && enabledItem->checkState() == Qt::Checked);
        result.append({path, enabled});
    }
    return result;
}

void PathListDialog::setPaths(const QStringList &paths)
{
    QList<ManagedPathEntry> entries;
    entries.reserve(paths.size());
    for (const QString &path : paths) {
        if (!path.trimmed().isEmpty()) {
            entries.append({path, true});
        }
    }
    setPathEntries(entries);
}

QStringList PathListDialog::paths() const
{
    QStringList result;
    const QList<ManagedPathEntry> entries = pathEntries();
    for (const ManagedPathEntry &entry : entries) {
        if (!entry.path.isEmpty()) result.append(entry.path);
    }
    return result;
}

void PathListDialog::setHintText(const QString &text)
{
    ui->labelHint->setText(text);
}

void PathListDialog::setDialogTitle(const QString &title)
{
    setWindowTitle(title);
}

void PathListDialog::onAddClicked()
{
    QString startDir;
    const int currentRow = ui->tablePaths->currentRow();
    if (currentRow >= 0) {
        QTableWidgetItem *currentPathItem = ui->tablePaths->item(currentRow, 0);
        if (currentPathItem) startDir = currentPathItem->text();
    }
    if (startDir.isEmpty() && ui->tablePaths->rowCount() > 0) {
        QTableWidgetItem *firstPathItem = ui->tablePaths->item(0, 0);
        if (firstPathItem) startDir = firstPathItem->text();
    }
    if (startDir.isEmpty()) startDir = QDir::homePath();

    QString path;
    if (m_selectionMode == FileMode) {
        QFileInfo startInfo(startDir);
        const QString startPath = startInfo.isDir() ? startDir : startInfo.absolutePath();
        path = QFileDialog::getOpenFileName(this,
                                            "选择文件",
                                            startPath,
                                            m_fileFilter.isEmpty() ? "All Files (*.*)" : m_fileFilter);
    } else {
        path = QFileDialog::getExistingDirectory(this, "选择文件夹", startDir);
    }
    if (path.isEmpty()) return;
    const QString normalizedPath = QFileInfo(path).absoluteFilePath();

    for (int i = 0; i < ui->tablePaths->rowCount(); ++i) {
        QTableWidgetItem *pathItem = ui->tablePaths->item(i, 0);
        if (!pathItem) continue;
        if (QFileInfo(pathItem->text()).absoluteFilePath() == normalizedPath) {
            return;
        }
    }

    addPathRow(normalizedPath, true);
}

void PathListDialog::addPathRow(const QString &path, bool enabled)
{
    const QString normalizedPath = QFileInfo(path).absoluteFilePath();
    if (normalizedPath.isEmpty()) return;

    const int row = ui->tablePaths->rowCount();
    ui->tablePaths->insertRow(row);

    QTableWidgetItem *pathItem = new QTableWidgetItem(normalizedPath);
    pathItem->setToolTip(normalizedPath);
    ui->tablePaths->setItem(row, 0, pathItem);

    QTableWidgetItem *enabledItem = new QTableWidgetItem();
    enabledItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    enabledItem->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
    ui->tablePaths->setItem(row, 1, enabledItem);
}

void PathListDialog::onRemoveClicked()
{
    QList<int> selectedRows;
    const QModelIndexList selected = ui->tablePaths->selectionModel()->selectedRows();
    selectedRows.reserve(selected.size());
    for (const QModelIndex &index : selected) selectedRows.append(index.row());

    std::sort(selectedRows.begin(), selectedRows.end(), std::greater<int>());
    for (const int row : selectedRows) {
        ui->tablePaths->removeRow(row);
    }
}

void PathListDialog::onMoveUpClicked()
{
    moveSelectedRows(-1);
}

void PathListDialog::onMoveDownClicked()
{
    moveSelectedRows(1);
}

void PathListDialog::moveSelectedRows(int direction)
{
    if (direction == 0) return;
    QModelIndexList selected = ui->tablePaths->selectionModel()->selectedRows();
    if (selected.isEmpty()) return;

    QList<int> rows;
    rows.reserve(selected.size());
    for (const QModelIndex &index : selected) rows.append(index.row());
    std::sort(rows.begin(), rows.end());
    if (direction > 0) std::reverse(rows.begin(), rows.end());

    const int rowCount = ui->tablePaths->rowCount();
    QSet<int> selectedSet;
    for (const int row : rows) selectedSet.insert(row);
    for (const int row : rows) {
        const int target = row + direction;
        if (target < 0 || target >= rowCount || selectedSet.contains(target)) continue;

        QList<QTableWidgetItem*> currentItems;
        QList<QTableWidgetItem*> targetItems;
        for (int col = 0; col < ui->tablePaths->columnCount(); ++col) {
            currentItems.append(ui->tablePaths->takeItem(row, col));
            targetItems.append(ui->tablePaths->takeItem(target, col));
        }
        for (int col = 0; col < ui->tablePaths->columnCount(); ++col) {
            ui->tablePaths->setItem(row, col, targetItems.value(col));
            ui->tablePaths->setItem(target, col, currentItems.value(col));
        }
        selectedSet.remove(row);
        selectedSet.insert(target);
    }

    ui->tablePaths->clearSelection();
    for (const int row : selectedSet) {
        ui->tablePaths->selectRow(row);
    }
}
