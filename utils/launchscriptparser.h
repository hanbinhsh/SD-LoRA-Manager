#ifndef LAUNCHSCRIPTPARSER_H
#define LAUNCHSCRIPTPARSER_H

#include <QString>
#include <QStringList>

enum class LaunchScriptTarget
{
    A1111,
    ComfyUI
};

struct LaunchScriptImportResult
{
    QString sourcePath;
    QString scriptPath;
    QString arguments;
    QString workingDirectory;
    QString environmentText;
    QString internalArguments;
    QStringList notes;
    bool scriptSafe = false;
    bool argumentsSafe = false;
    bool workingDirectorySafe = false;
    bool environmentSafe = false;
};

namespace LaunchScriptParser {

LaunchScriptImportResult parse(const QString &path, LaunchScriptTarget target);

}

#endif // LAUNCHSCRIPTPARSER_H
