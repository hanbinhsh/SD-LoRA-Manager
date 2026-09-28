#ifndef FILEUTILS_H
#define FILEUTILS_H

#include <QString>

class QObject;

namespace FileUtils {
QString uniqueFilePath(const QString &dirPath, const QString &fileName);

QString calculateSha256Hex(const QString &filePath, bool uppercase = true);
bool showFileInFolder(const QString &filePath, QObject *processParent = nullptr);

} // namespace FileUtils

#endif // FILEUTILS_H
