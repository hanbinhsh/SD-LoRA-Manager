#pragma once

#include <QStringList>

namespace ModelFilter {

struct Record {
    QStringList searchableText;
    QString baseModel;
    QString modelType;
};

QString normalizeModelType(const QString &raw);
bool matches(const Record &record, const QString &query,
             const QString &baseModel, const QString &modelType);

}
