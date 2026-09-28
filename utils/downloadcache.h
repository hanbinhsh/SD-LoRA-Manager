#pragma once

#include "core/modelupdateinfo.h"
#include <QVector>

namespace DownloadCache {

struct Entry {
    ModelUpdateInfo info;
    QString status;
    bool sourceAvailable = false;
};

struct LoadResult {
    QVector<Entry> entries;
    QString error;
};

ModelUpdateInfo infoFromJson(const QJsonObject &object);
QJsonObject entryToJson(const ModelUpdateInfo &info, const QString &status);
LoadResult load(const QString &path);
bool save(const QString &path, const QVector<Entry> &entries, QString *error = nullptr);

}
