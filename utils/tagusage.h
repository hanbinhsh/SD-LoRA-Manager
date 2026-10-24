#pragma once

#include <QHash>
#include <QMap>
#include <QStringList>
#include <QVector>

namespace TagUsage {

enum Scope { Positive = 0, Negative = 1, Both = 2, Combined = 3 };

struct Row {
    QString tag;
    QString kind;
    int count = 0;
};

// Counts images containing a tag, not occurrences within each prompt.
struct Counts {
    QMap<QString, int> values;
    QHash<QString, QString> displayTags;
    void addImage(const QStringList &tags);
};

struct Statistics {
    Counts positive;
    Counts negative;
    Counts combined;
    QVector<Row> rows(int scope = 2) const; // 0: positive, 1: negative, 2: both
};

QStringList promptTags(const QString &prompt);
Statistics readCache(const QString &path, Scope scope = Both);

}
