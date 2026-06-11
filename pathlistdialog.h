#ifndef PATHLISTDIALOG_H
#define PATHLISTDIALOG_H

#include <QDialog>
#include <QStringList>

namespace Ui { class PathListDialog; }

struct ManagedPathEntry
{
    QString path;
    bool enabled = true;
};

class PathListDialog : public QDialog
{
    Q_OBJECT

public:
    enum SelectionMode {
        DirectoryMode,
        FileMode
    };

    explicit PathListDialog(QWidget *parent = nullptr);
    ~PathListDialog();

    void setSelectionMode(SelectionMode mode, const QString &fileFilter = QString());
    void setPathEntries(const QList<ManagedPathEntry> &entries);
    QList<ManagedPathEntry> pathEntries() const;

    void setPaths(const QStringList &paths);
    QStringList paths() const;
    void setHintText(const QString &text);
    void setDialogTitle(const QString &title);

private slots:
    void onAddClicked();
    void onRemoveClicked();
    void onMoveUpClicked();
    void onMoveDownClicked();

private:
    void addPathRow(const QString &path, bool enabled);
    void moveSelectedRows(int direction);

    Ui::PathListDialog *ui;
    SelectionMode m_selectionMode = DirectoryMode;
    QString m_fileFilter;
};

#endif // PATHLISTDIALOG_H
