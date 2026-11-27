#ifndef FOLDERSYNCDELETEDIALOG_H
#define FOLDERSYNCDELETEDIALOG_H

#include "utils/foldersyncengine.h"

#include <QDialog>
#include <QStringList>
#include <QVector>

namespace Ui {
class FolderSyncDeleteDialog;
}

class FolderSyncDeleteDialog : public QDialog
{
    Q_OBJECT

public:
    explicit FolderSyncDeleteDialog(const FolderCompareResult &compareResult, QWidget *parent = nullptr);
    ~FolderSyncDeleteDialog() override;

    QStringList selectedFilePaths() const;

private slots:
    void onSelectByCategory(FolderCompareEntryKind kind);
    void onSelectAll();
    void onSelectNone();
    void onConfirmMoveToTrash();
    void updateSelectionSummary();

private:
    void populateTable();

    Ui::FolderSyncDeleteDialog *ui = nullptr;
    QVector<FolderCompareEntry> m_candidates;
};

#endif // FOLDERSYNCDELETEDIALOG_H
