#include "translationcsv.h"

#include <QBuffer>
#include <QFile>
#include <QSet>
#include <QStringConverter>
#include <QTextStream>

namespace {

bool isIntegerText(const QString &text)
{
    if (text.isEmpty()) return false;
    for (const QChar ch : text) {
        if (!ch.isDigit()) return false;
    }
    return true;
}

QString removeLeadingTag(const QString &tag, QString display)
{
    display = display.trimmed();
    for (const QString &prefix : {tag + " ", tag + "\t"}) {
        if (display.startsWith(prefix, Qt::CaseSensitive)) {
            return display.mid(prefix.size()).trimmed();
        }
    }
    return display;
}

void splitCategoryAndTranslation(const QString &text, QString &category, QString &translation)
{
    const QString value = text.trimmed();
    category.clear();
    translation.clear();
    if (value.isEmpty()) return;

    int dash = value.indexOf('-');
    if (dash < 0) dash = value.indexOf(QChar(0xFF0D));
    if (dash < 0) dash = value.indexOf(QChar(0x2014));
    if (dash < 0) dash = value.indexOf(QChar(0x2013));
    if (dash > 0) {
        category = value.left(dash).trimmed();
        translation = value.mid(dash + 1).trimmed();
    } else {
        translation = value;
    }
}

QVector<TranslationCsvEntry> readStream(QTextStream &stream)
{
    QVector<TranslationCsvEntry> rows;
    int rowIndex = 0;
    while (!stream.atEnd()) {
        QString line = stream.readLine();
        if (line.trimmed().isEmpty()) continue;

        QStringList parts = TranslationCsv::parseLine(line);
        if (parts.isEmpty()) continue;
        QString first = parts.value(0).trimmed();
        if (rowIndex == 0 && first.startsWith(QChar(0xFEFF))) {
            first.remove(0, 1);
            parts[0] = first;
        }

        if (rowIndex == 0) {
            const QString second = parts.value(1).trimmed().toLower();
            const QString lowerFirst = first.toLower();
            if ((lowerFirst == "tag" || lowerFirst == "name")
                && (second == "translation" || second == "count"
                    || second == "post_count" || second == "postcount")) {
                ++rowIndex;
                continue;
            }
        }

        TranslationCsvEntry entry = TranslationCsv::parseEntry(parts);
        if (!entry.tag.isEmpty()) rows.append(entry);
        ++rowIndex;
    }
    return rows;
}

}

QString TranslationCsvEntry::displayValue() const
{
    if (category.isEmpty()) return translation;
    if (translation.isEmpty()) return category;
    return category + '-' + translation;
}

QStringList TranslationCsv::parseLine(const QString &line)
{
    QStringList parts;
    QString current;
    bool inQuotes = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (ch == '"') {
            if (inQuotes && i + 1 < line.size() && line.at(i + 1) == '"') {
                current += '"';
                ++i;
            } else {
                inQuotes = !inQuotes;
            }
        } else if (ch == ',' && !inQuotes) {
            parts.append(current.trimmed());
            current.clear();
        } else {
            current += ch;
        }
    }
    parts.append(current.trimmed());
    return parts;
}

TranslationCsvEntry TranslationCsv::parseEntry(const QStringList &parts)
{
    TranslationCsvEntry entry;
    entry.tag = parts.value(0).trimmed();

    QString display;
    if (parts.size() >= 3 && isIntegerText(parts.last().trimmed())) {
        entry.priority = parts.last().trimmed();
        display = parts.mid(1, parts.size() - 2).join(',').trimmed();
    } else {
        display = parts.mid(1).join(',').trimmed();
    }

    display = removeLeadingTag(entry.tag, display);
    splitCategoryAndTranslation(display, entry.category, entry.translation);
    return entry;
}

QVector<TranslationCsvEntry> TranslationCsv::readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    return readStream(stream);
}

QVector<TranslationCsvEntry> TranslationCsv::parseUtf8(const QByteArray &data)
{
    QBuffer buffer;
    buffer.setData(data);
    if (!buffer.open(QIODevice::ReadOnly)) return {};
    QTextStream stream(&buffer);
    stream.setEncoding(QStringConverter::Utf8);
    return readStream(stream);
}
