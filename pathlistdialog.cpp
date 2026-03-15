#include "pathlistdialog.h"
#include "ui_pathlistdialog.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QDir>

PathListDialog::PathListDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::PathListDialog)
{
    ui->setupUi(this);
    ui->listPaths->setSelectionMode(QAbstractItemView::ExtendedSelection);

    connect(ui->btnAddPath, &QPushButton::clicked, this, &PathListDialog::onAddClicked);
    connect(ui->btnRemovePath, &QPushButton::clicked, this, &PathListDialog::onRemoveClicked);
}

PathListDialog::~PathListDialog()
{
    delete ui;
}

void PathListDialog::setPaths(const QStringList &paths)
{
    ui->listPaths->clear();
    for (const QString &path : paths) {
        if (!path.isEmpty()) {
            ui->listPaths->addItem(path);
        }
    }
}

QStringList PathListDialog::paths() const
{
    QStringList result;
    for (int i = 0; i < ui->listPaths->count(); ++i) {
        QString path = ui->listPaths->item(i)->text().trimmed();
        if (!path.isEmpty()) result.append(path);
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
    if (ui->listPaths->currentItem()) {
        startDir = ui->listPaths->currentItem()->text();
    }
    if (startDir.isEmpty() && ui->listPaths->count() > 0) {
        startDir = ui->listPaths->item(0)->text();
    }
    if (startDir.isEmpty()) startDir = QDir::homePath();

    QString dir = QFileDialog::getExistingDirectory(this, "选择文件夹", startDir);
    if (dir.isEmpty()) return;

    for (int i = 0; i < ui->listPaths->count(); ++i) {
        if (ui->listPaths->item(i)->text() == dir) {
            return;
        }
    }
    ui->listPaths->addItem(dir);
}

void PathListDialog::onRemoveClicked()
{
    const QList<QListWidgetItem*> items = ui->listPaths->selectedItems();
    for (QListWidgetItem *item : items) {
        delete ui->listPaths->takeItem(ui->listPaths->row(item));
    }
}
