#pragma once

#include "core/pathmodels.h"
#include <QList>
#include <QSet>
#include <QStringList>

namespace PathUtils {

QStringList normalizePathList(const QStringList &paths);
QSet<QString> normalizePathSet(const QSet<QString> &paths);
QStringList collectValidPaths(const QStringList &paths);
QStringList collectEnabledPaths(const QStringList &paths, const QSet<QString> &disabledPaths);
QList<ManagedPathEntry> buildPathEntries(const QStringList &paths, const QSet<QString> &disabledPaths);
void applyPathEntries(const QList<ManagedPathEntry> &entries, QStringList &paths, QSet<QString> &disabledPaths);

}
