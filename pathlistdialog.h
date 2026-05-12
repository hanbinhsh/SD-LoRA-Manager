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
    explicit PathListDialog(QWidget *parent = nullptr);
    ~PathListDialog();

    void setPathEntries(const QList<ManagedPathEntry> &entries);
    QList<ManagedPathEntry> pathEntries() const;

    void setPaths(const QStringList &paths);
    QStringList paths() const;
    void setHintText(const QString &text);
    void setDialogTitle(const QString &title);

private slots:
    void onAddClicked();
    void onRemoveClicked();

private:
    Ui::PathListDialog *ui;
};

#endif // PATHLISTDIALOG_H
