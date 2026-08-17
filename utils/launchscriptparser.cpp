#include "launchscriptparser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>

namespace {

QString quoteArgument(const QString &value)
{
    if (!value.contains(QRegularExpression("[\\s\"]"))) return value;
    QString escaped = value;
    escaped.replace('"', "\\\"");
    return QStringLiteral("\"") + escaped + QStringLiteral("\"");
}

QString joinArguments(const QStringList &values)
{
    QStringList quoted;
    quoted.reserve(values.size());
    for (const QString &value : values) quoted.append(quoteArgument(value));
    return quoted.join(' ');
}

QString decodeBatch(const QByteArray &bytes)
{
    QString text = QString::fromUtf8(bytes);
    if (text.contains(QChar::ReplacementCharacter)) text = QString::fromLocal8Bit(bytes);
    return text;
}

QString expandBatchVariables(QString value, const QString &scriptDir, const QMap<QString, QString> &variables)
{
    value.replace(QRegularExpression("%~dp0", QRegularExpression::CaseInsensitiveOption),
                  QDir::toNativeSeparators(scriptDir + '/'));
    static const QRegularExpression variablePattern("%([^%]+)%");
    auto match = variablePattern.match(value);
    int guard = 0;
    while (match.hasMatch() && guard++ < 20) {
        const QString key = match.captured(1).trimmed().toUpper();
        if (!variables.contains(key)) break;
        value.replace(match.capturedStart(), match.capturedLength(), variables.value(key));
        match = variablePattern.match(value);
    }
    return value.trimmed();
}

QStringList logicalBatchLines(const QString &text)
{
    QStringList result;
    QString pending;
    const QStringList physical = text.split(QRegularExpression("\\r?\\n"));
    for (QString line : physical) {
        line = line.trimmed();
        if (!pending.isEmpty()) line = pending + line;
        if (line.endsWith('^')) {
            line.chop(1);
            pending = line + ' ';
            continue;
        }
        pending.clear();
        if (!line.isEmpty()) result.append(line);
    }
    if (!pending.trimmed().isEmpty()) result.append(pending.trimmed());
    return result;
}

void readBatchRecursive(const QString &path, int depth, QSet<QString> &visited,
                        QStringList &lines, QMap<QString, QString> &variables,
                        QStringList &notes)
{
    if (depth > 4) {
        notes.append("已达到批处理引用解析深度上限。 ");
        return;
    }
    const QString absolute = QFileInfo(path).absoluteFilePath();
    const QString key = absolute.toCaseFolded();
    if (visited.contains(key)) return;
    visited.insert(key);

    QFile file(absolute);
    if (!file.open(QIODevice::ReadOnly)) {
        notes.append("无法读取引用脚本：" + absolute);
        return;
    }
    const QString scriptDir = QFileInfo(absolute).absolutePath();
    const QStringList fileLines = logicalBatchLines(decodeBatch(file.readAll()));
    static const QRegularExpression setPattern(
        "^@?\\s*set\\s+\"?([^=\"]+)=(.*?)\"?\\s*$", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression callPattern(
        "^@?\\s*call\\s+(.+?\\.(?:bat|cmd))(?:\\s|$)", QRegularExpression::CaseInsensitiveOption);

    for (const QString &raw : fileLines) {
        const QString trimmed = raw.trimmed();
        if (trimmed.startsWith("rem ", Qt::CaseInsensitive) || trimmed.startsWith("::")) continue;
        const QRegularExpressionMatch setMatch = setPattern.match(trimmed);
        if (setMatch.hasMatch()) {
            const QString name = setMatch.captured(1).trimmed().toUpper();
            const QString value = expandBatchVariables(setMatch.captured(2).trimmed(), scriptDir, variables);
            variables.insert(name, value);
        }
        lines.append(expandBatchVariables(trimmed, scriptDir, variables));

        const QRegularExpressionMatch callMatch = callPattern.match(trimmed);
        if (!callMatch.hasMatch()) continue;
        QString called = expandBatchVariables(callMatch.captured(1).trimmed(), scriptDir, variables);
        if (called.startsWith('"') && called.endsWith('"')) called = called.mid(1, called.size() - 2);
        if (QDir::isRelativePath(called)) called = QDir(scriptDir).filePath(called);
        if (QFileInfo::exists(called)) readBatchRecursive(called, depth + 1, visited, lines, variables, notes);
    }
}

bool findComfyDirectInvocation(const QStringList &lines, const QString &baseDirectory,
                               QString &program, QString &arguments)
{
    for (QString line : lines) {
        if (!line.contains("main.py", Qt::CaseInsensitive)) continue;
        line.remove(QRegularExpression("^@?\\s*(?:call\\s+)?", QRegularExpression::CaseInsensitiveOption));
        QStringList tokens = QProcess::splitCommand(line);
        if (tokens.size() < 2) continue;
        int programIndex = 0;
        if (tokens.value(0).compare("start", Qt::CaseInsensitive) == 0) {
            programIndex = 1;
            if (tokens.value(programIndex).isEmpty()) ++programIndex;
        }
        if (programIndex >= tokens.size()) continue;
        QString candidate = tokens.at(programIndex);
        if (candidate.compare("python", Qt::CaseInsensitive) == 0
            || candidate.compare("python.exe", Qt::CaseInsensitive) == 0) continue;
        if (QDir::isRelativePath(candidate)) candidate = QDir(baseDirectory).filePath(candidate);
        candidate = QDir::cleanPath(candidate);
        if (!candidate.endsWith(".exe", Qt::CaseInsensitive) || !QFileInfo::exists(candidate)) continue;
        program = QFileInfo(candidate).absoluteFilePath();
        arguments = joinArguments(tokens.mid(programIndex + 1));
        return true;
    }
    return false;
}

}

LaunchScriptImportResult LaunchScriptParser::parse(const QString &path, LaunchScriptTarget target)
{
    LaunchScriptImportResult result;
    const QFileInfo info(path);
    result.sourcePath = info.absoluteFilePath();
    result.workingDirectory = info.absolutePath();
    result.workingDirectorySafe = info.exists();
    if (!info.exists() || !info.isFile()) {
        result.notes.append("选择的启动脚本不存在。");
        return result;
    }

    const QString suffix = info.suffix().toLower();
    if (suffix == "py" || suffix == "exe") {
        result.scriptPath = result.sourcePath;
        result.scriptSafe = true;
        result.argumentsSafe = true;
        result.notes.append("已识别为可直接启动的程序或 Python 脚本。");
        return result;
    }
    if (suffix != "bat" && suffix != "cmd") {
        result.notes.append("第一版只解析 BAT、CMD、PY 和 EXE 启动入口。");
        return result;
    }

    QStringList lines;
    QMap<QString, QString> variables;
    QSet<QString> visited;
    readBatchRecursive(result.sourcePath, 0, visited, lines, variables, result.notes);
    result.internalArguments = variables.value("COMMANDLINE_ARGS").trimmed();

    static const QSet<QString> scriptControlVariables = {
        "COMMANDLINE_ARGS", "PYTHON", "VENV_DIR", "GIT", "PATH"
    };
    QStringList environment;
    for (auto it = variables.cbegin(); it != variables.cend(); ++it) {
        if (!scriptControlVariables.contains(it.key()) && !it.value().isEmpty()) {
            environment.append(it.key() + '=' + it.value());
        }
    }
    result.environmentText = environment.join('\n');
    result.environmentSafe = !result.environmentText.isEmpty();

    if (target == LaunchScriptTarget::ComfyUI
        && findComfyDirectInvocation(lines, result.workingDirectory, result.scriptPath, result.arguments)) {
        result.scriptSafe = true;
        result.argumentsSafe = true;
        result.notes.append("已识别 ComfyUI 的直接 Python 运行命令，可安全导入程序与参数。");
    } else {
        result.scriptPath = result.sourcePath;
        result.scriptSafe = true;
        result.argumentsSafe = false;
        if (!result.internalArguments.isEmpty()) {
            result.notes.append("检测到脚本内部参数；为避免重复传参，仅展示，不追加到启动命令。");
        } else {
            result.notes.append("保留原批处理作为启动入口，未强制展开复杂脚本逻辑。");
        }
    }
    return result;
}
