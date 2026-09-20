#ifndef MODELLISTCACHE_H
#define MODELLISTCACHE_H

#include <QFileInfo>
#include <QJsonObject>
#include <QString>

// Disposable list summaries only; never writes the original model metadata.
// Each scan owns an instance and uses it exclusively on its worker thread.
class ModelListCache
{
public:
    explicit ModelListCache(const QString &path);
    static QJsonObject fingerprint(const QFileInfo &model, const QFileInfo &metadata);
    bool lookup(const QString &path, const QJsonObject &stamp, QJsonObject *summary);
    void insert(const QString &path, const QJsonObject &stamp, const QJsonObject &summary);
    bool save() const;

private:
    QString m_path;
    QJsonObject m_previous;
    QJsonObject m_current;
};

#endif
