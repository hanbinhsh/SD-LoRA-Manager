#ifndef MODELNOTEDIALOG_H
#define MODELNOTEDIALOG_H

#include <QDialog>
#include <QStringList>

namespace Ui {
class ModelNoteDialog;
}

class ModelNoteDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ModelNoteDialog(QWidget *parent = nullptr);
    ~ModelNoteDialog();

    void setModelName(const QString &name);
    void setRating(double rating);
    double rating() const;
    void setNote(const QString &note);
    QString note() const;
    void setTags(const QStringList &tags);
    QStringList tags() const;

private:
    Ui::ModelNoteDialog *ui;
};

#endif // MODELNOTEDIALOG_H
