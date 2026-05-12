#include "pathlistdialog.h"
#include "ui_pathlistdialog.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QHeaderView>
#include <QTableWidgetItem>
#include <algorithm>

PathListDialog::PathListDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::PathListDialog)
{
    ui->setupUi(this);
    ui->tablePaths->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->tablePaths->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tablePaths->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->tablePaths->horizontalHeader()->setStretchLastSection(false);
    ui->tablePaths->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->tablePaths->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ui->tablePaths->verticalHeader()->setVisible(false);

    connect(ui->btnAddPath, &QPushButton::clicked, this, &PathListDialog::onAddClicked);
    connect(ui->btnRemovePath, &QPushButton::clicked, this, &PathListDialog::onRemoveClicked);
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
            const int row = ui->tablePaths->rowCount();
            ui->tablePaths->insertRow(row);

            QTableWidgetItem *pathItem = new QTableWidgetItem(path);
            pathItem->setToolTip(path);
            ui->tablePaths->setItem(row, 0, pathItem);

            QTableWidgetItem *enabledItem = new QTableWidgetItem();
            enabledItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            enabledItem->setCheckState(entry.enabled ? Qt::Checked : Qt::Unchecked);
            ui->tablePaths->setItem(row, 1, enabledItem);
        }
    }
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

    QString dir = QFileDialog::getExistingDirectory(this, "选择文件夹", startDir);
    if (dir.isEmpty()) return;
    const QString normalizedDir = QFileInfo(dir).absoluteFilePath();

    for (int i = 0; i < ui->tablePaths->rowCount(); ++i) {
        QTableWidgetItem *pathItem = ui->tablePaths->item(i, 0);
        if (!pathItem) continue;
        if (QFileInfo(pathItem->text()).absoluteFilePath() == normalizedDir) {
            return;
        }
    }

    const int row = ui->tablePaths->rowCount();
    ui->tablePaths->insertRow(row);

    QTableWidgetItem *pathItem = new QTableWidgetItem(normalizedDir);
    pathItem->setToolTip(normalizedDir);
    ui->tablePaths->setItem(row, 0, pathItem);

    QTableWidgetItem *enabledItem = new QTableWidgetItem();
    enabledItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    enabledItem->setCheckState(Qt::Checked);
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
