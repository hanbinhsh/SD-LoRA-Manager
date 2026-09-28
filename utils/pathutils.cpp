#include "pathutils.h"

#include <QDir>
#include <QFileInfo>

namespace PathUtils {

QStringList normalizePathList(const QStringList &paths)
{
    QStringList result;
    QSet<QString> seen;
    for (QString path : paths) {
        path = path.trimmed();
        if (path.isEmpty()) continue;
        QString normalized = QFileInfo(path).absoluteFilePath();
        if (seen.contains(normalized)) continue;
        seen.insert(normalized);
        result.append(normalized);
    }
    return result;
}

QSet<QString> normalizePathSet(const QSet<QString> &paths)
{
    QSet<QString> result;
    for (QString path : paths) {
        path = path.trimmed();
        if (path.isEmpty()) continue;
        result.insert(QFileInfo(path).absoluteFilePath());
    }
    return result;
}

QStringList collectValidPaths(const QStringList &paths)
{
    QStringList valid;
    for (const QString &path : paths) {
        if (!path.isEmpty() && QDir(path).exists()) {
            valid.append(path);
        }
    }
    return valid;
}

QStringList collectEnabledPaths(const QStringList &paths, const QSet<QString> &disabledPaths)
{
    QStringList enabled;
    enabled.reserve(paths.size());
    for (const QString &path : paths) {
        if (path.isEmpty()) continue;
        const QString normalized = QFileInfo(path).absoluteFilePath();
        if (disabledPaths.contains(normalized)) continue;
        enabled.append(normalized);
    }
    return normalizePathList(enabled);
}

QList<ManagedPathEntry> buildPathEntries(const QStringList &paths, const QSet<QString> &disabledPaths)
{
    QList<ManagedPathEntry> entries;
    entries.reserve(paths.size());
    for (const QString &path : paths) {
        if (path.isEmpty()) continue;
        const QString normalized = QFileInfo(path).absoluteFilePath();
        entries.append({normalized, !disabledPaths.contains(normalized)});
    }
    return entries;
}

void applyPathEntries(const QList<ManagedPathEntry> &entries, QStringList &paths, QSet<QString> &disabledPaths)
{
    paths.clear();
    disabledPaths.clear();
    QSet<QString> seen;
    for (const ManagedPathEntry &entry : entries) {
        if (entry.path.trimmed().isEmpty()) continue;
        const QString normalized = QFileInfo(entry.path.trimmed()).absoluteFilePath();
        if (normalized.isEmpty() || seen.contains(normalized)) continue;
        seen.insert(normalized);
        paths.append(normalized);
        if (!entry.enabled) disabledPaths.insert(normalized);
    }
}

}
