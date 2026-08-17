#ifndef TRANSLATIONCSV_H
#define TRANSLATIONCSV_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

struct TranslationCsvEntry
{
    QString tag;
    QString category;
    QString translation;
    QString priority;

    QString displayValue() const;
};

namespace TranslationCsv {

QStringList parseLine(const QString &line);
TranslationCsvEntry parseEntry(const QStringList &parts);
QVector<TranslationCsvEntry> readFile(const QString &path);
QVector<TranslationCsvEntry> parseUtf8(const QByteArray &data);

}

#endif // TRANSLATIONCSV_H
