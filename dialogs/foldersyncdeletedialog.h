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
    FolderDeleteMode deleteMode() const { return m_deleteMode; }

private slots:
    void onSelectByCategory(FolderCompareEntryKind kind);
    void onSelectAll();
    void onSelectNone();
    void onConfirmMoveToTrash();
    void onConfirmPermanentDelete();
    void updateSelectionSummary();

private:
    void populateTable();
    void confirmDeleteWithMode(FolderDeleteMode mode);

    Ui::FolderSyncDeleteDialog *ui = nullptr;
    QVector<FolderCompareEntry> m_candidates;
    FolderDeleteMode m_deleteMode = FolderDeleteMode::MoveToTrash;
};

#endif // FOLDERSYNCDELETEDIALOG_H
