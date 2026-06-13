#include "fileutils.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>

namespace FileUtils {

QString calculateSha256Hex(const QString &filePath, bool uppercase)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return QString();

    QCryptographicHash hash(QCryptographicHash::Sha256);
    constexpr qsizetype bufferSize = 1024 * 1024;
    QByteArray buffer;
    buffer.resize(bufferSize);

    while (!file.atEnd()) {
        const qint64 size = file.read(buffer.data(), buffer.size());
        if (size <= 0) break;
        hash.addData(buffer.constData(), size);
    }

    QString result = QString::fromLatin1(hash.result().toHex());
    return uppercase ? result.toUpper() : result;
}

bool showFileInFolder(const QString &filePath, QObject *processParent)
{
    const QString trimmed = filePath.trimmed();
    if (trimmed.isEmpty()) return false;

    const QFileInfo fi(trimmed);
#ifdef Q_OS_WIN
    QProcess *process = new QProcess(processParent);
    process->setProgram("explorer.exe");
    if (fi.exists()) {
        process->setNativeArguments(QString("/select,\"%1\"").arg(QDir::toNativeSeparators(fi.absoluteFilePath())));
    } else {
        process->setNativeArguments(QString("\"%1\"").arg(QDir::toNativeSeparators(fi.absolutePath())));
    }
    QObject::connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     process, &QProcess::deleteLater);
    QObject::connect(process, &QProcess::errorOccurred,
                     process, &QObject::deleteLater);
    process->start();
    return true;
#else
    return QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
#endif
}

} // namespace FileUtils
