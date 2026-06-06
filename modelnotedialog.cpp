#include "modelnotedialog.h"
#include "ui_modelnotedialog.h"

#include <QSet>

static QStringList normalizeModelNoteTags(const QString &text)
{
    QStringList result;
    QSet<QString> seen;
    QString normalizedText = text;
    normalizedText.replace('\n', ',');
    normalizedText.replace('\r', ',');
    for (const QString &raw : normalizedText.split(',', Qt::SkipEmptyParts)) {
        const QString tag = raw.trimmed();
        if (tag.isEmpty()) continue;
        const QString key = tag.toCaseFolded();
        if (seen.contains(key)) continue;
        seen.insert(key);
        result.append(tag);
    }
    return result;
}

ModelNoteDialog::ModelNoteDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::ModelNoteDialog)
{
    ui->setupUi(this);
}

ModelNoteDialog::~ModelNoteDialog()
{
    delete ui;
}

void ModelNoteDialog::setModelName(const QString &name)
{
    ui->lblModelName->setText(name.isEmpty() ? "当前模型" : name);
}

void ModelNoteDialog::setRating(double rating)
{
    if (rating < 0.5) rating = 0.0;
    if (rating > 5.0) rating = 5.0;
    ui->spinRating->setValue(rating);
}

double ModelNoteDialog::rating() const
{
    return ui->spinRating->value();
}

void ModelNoteDialog::setNote(const QString &note)
{
    ui->textNote->setPlainText(note);
}

QString ModelNoteDialog::note() const
{
    return ui->textNote->toPlainText().trimmed();
}

void ModelNoteDialog::setTags(const QStringList &tags)
{
    ui->editTags->setPlainText(tags.join(", "));
}

QStringList ModelNoteDialog::tags() const
{
    return normalizeModelNoteTags(ui->editTags->toPlainText());
}
