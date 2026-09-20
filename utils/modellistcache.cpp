#include "modellistcache.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>

namespace {
// Increment when the list summary schema or its parsing rules change.
constexpr int CacheVersion = 1;

QJsonObject fileStamp(const QFileInfo &file)
{
    return {{"path", file.absoluteFilePath()},
            {"exists", file.isFile()},
            {"size", QString::number(file.size())},
            {"modified", QString::number(file.lastModified().toMSecsSinceEpoch())},
            {"changed", QString::number(file.metadataChangeTime().toMSecsSinceEpoch())},
            {"created", QString::number(file.birthTime().toMSecsSinceEpoch())}};
}
}

ModelListCache::ModelListCache(const QString &path) : m_path(path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value("version").toInt() == CacheVersion)
        m_previous = root.value("entries").toObject();
}

QJsonObject ModelListCache::fingerprint(const QFileInfo &model, const QFileInfo &metadata)
{
    return {{"model", fileStamp(model)}, {"metadata", fileStamp(metadata)}};
}

bool ModelListCache::lookup(const QString &path, const QJsonObject &stamp, QJsonObject *summary)
{
    const QJsonObject entry = m_previous.value(path).toObject();
    if (entry.value("stamp").toObject() != stamp || !entry.value("summary").isObject())
        return false;
    *summary = entry.value("summary").toObject();
    m_current.insert(path, entry);
    return true;
}

void ModelListCache::insert(const QString &path, const QJsonObject &stamp, const QJsonObject &summary)
{
    m_current.insert(path, QJsonObject{{"stamp", stamp}, {"summary", summary}});
}

bool ModelListCache::save() const
{
    if (m_current == m_previous) return true;
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) return false;
    const QByteArray bytes = QJsonDocument(QJsonObject{{"version", CacheVersion},
                                                      {"entries", m_current}})
                                 .toJson(QJsonDocument::Compact);
    QSaveFile file(m_path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
