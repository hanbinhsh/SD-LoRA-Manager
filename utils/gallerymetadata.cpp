#include "gallerymetadata.h"
#include "imagemetadataparser.h"
#include "tagutils.h"
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QSet>

namespace GalleryMetadata {

QString cleanModelOrLoraBaseName(QString raw)
{
    raw = raw.trimmed();
    raw.replace('\\', '/');
    if (raw.contains('/')) raw = raw.section('/', -1);
    if (raw.endsWith(".safetensors", Qt::CaseInsensitive) ||
        raw.endsWith(".ckpt", Qt::CaseInsensitive) ||
        raw.endsWith(".pt", Qt::CaseInsensitive)) {
        raw = QFileInfo(raw).completeBaseName();
    }
    return raw.trimmed();
}

void populateParsedModelFields(UserImageInfo &info)
{
    info.checkpointName.clear();
    info.checkpointHash.clear();
    info.parsedLoras.clear();
    info.extraLoraHashes.clear();
    info.isComfy = false;
    info.parsedModelsReady = true;

    const QString &params = info.parameters;
    const QString &prompt = info.prompt;

    if (!params.isEmpty()) {
        static const QRegularExpression sourceRegex("(?:^|[\\n\\r])\\s*Source\\s*:\\s*ComfyUI\\b",
                                                    QRegularExpression::CaseInsensitiveOption);
        info.isComfy = sourceRegex.match(params).hasMatch();

        static const QRegularExpression modelRegex("(?:^|[,\\n\\r])\\s*Model\\s*:\\s*([^,\\n\\r]+)",
                                                   QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch modelMatch = modelRegex.match(params);
        if (modelMatch.hasMatch()) {
            info.checkpointName = modelMatch.captured(1).trimmed();
        }

        if (info.checkpointName.isEmpty()) {
            static const QRegularExpression checkpointRegex("(?:^|[,\\n\\r])\\s*Checkpoint\\s*:\\s*([^,\\n\\r]+)",
                                                            QRegularExpression::CaseInsensitiveOption);
            const QRegularExpressionMatch ckptMatch = checkpointRegex.match(params);
            if (ckptMatch.hasMatch()) {
                info.checkpointName = ckptMatch.captured(1).trimmed();
            }
        }

        static const QRegularExpression modelHashRegex("(?:^|[,\\n\\r])\\s*Model\\s+hash\\s*:\\s*([A-Fa-f0-9]{6,128})",
                                                       QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch hashMatch = modelHashRegex.match(params);
        if (hashMatch.hasMatch()) {
            info.checkpointHash = hashMatch.captured(1).trimmed().toLower();
        }
    }

    QSet<QString> seenLoraNames;
    QSet<QString> seenExtraHashes;

    // 1. 从正向提示词中提取 <lora:name:weight> / <lyco:name:weight>
    if (!prompt.isEmpty()) {
        static const QRegularExpression promptLoraRegex("<\\s*(?:lora|lyco)\\s*:\\s*([^:>]+)(?::([^:>]+))?",
                                                        QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatchIterator promptIt = promptLoraRegex.globalMatch(prompt);
        while (promptIt.hasNext()) {
            const QRegularExpressionMatch m = promptIt.next();
            const QString name = m.captured(1).trimmed();
            const QString weight = m.captured(2).trimmed();
            if (!name.isEmpty()) {
                const QString norm = name.toLower();
                if (!seenLoraNames.contains(norm)) {
                    seenLoraNames.insert(norm);
                    info.parsedLoras.append({name, QString(), weight});
                }
            }
        }
    }

    if (!params.isEmpty()) {
        // 2. 从参数中的 Lora hashes / ComfyUI Lora hashes 提取
        static const QRegularExpression loraHashesBlockRegex(
            "(?:^|[,\\n\\r])\\s*(?:ComfyUI\\s+)?Lora\\s+hashes\\s*:\\s*(\"[^\"]*\"|[^\\n\\r]*)",
            QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatchIterator blockIt = loraHashesBlockRegex.globalMatch(params);
        while (blockIt.hasNext()) {
            QString block = blockIt.next().captured(1).trimmed();
            if (block.startsWith('"') && block.endsWith('"') && block.size() >= 2) {
                block = block.mid(1, block.size() - 2);
            }
            static const QRegularExpression pairRegex("([^:,]+?)\\s*:\\s*([A-Fa-f0-9]{6,128})");
            QRegularExpressionMatchIterator pairIt = pairRegex.globalMatch(block);
            while (pairIt.hasNext()) {
                const QRegularExpressionMatch m = pairIt.next();
                const QString name = m.captured(1).trimmed();
                const QString hash = m.captured(2).trimmed().toLower();
                if (!name.isEmpty()) {
                    const QString norm = name.toLower();
                    bool found = false;
                    for (ParsedLoraEntry &entry : info.parsedLoras) {
                        const QString entryNorm = entry.name.toLower();
                        if (entryNorm == norm || entryNorm.endsWith("/" + norm) || entryNorm.endsWith("\\" + norm) ||
                            norm.endsWith("/" + entryNorm) || norm.endsWith("\\" + entryNorm)) {
                            if (entry.hash.isEmpty()) entry.hash = hash;
                            found = true;
                            break;
                        }
                    }
                    if (!found && !seenLoraNames.contains(norm)) {
                        seenLoraNames.insert(norm);
                        info.parsedLoras.append({name, hash, QString()});
                    } else if (!hash.isEmpty() && !seenExtraHashes.contains(hash)) {
                        seenExtraHashes.insert(hash);
                        info.extraLoraHashes.append(hash);
                    }
                }
            }
        }

        // 3. 从 ComfyUI LoRAs 提取
        static const QRegularExpression comfyLoraRegex("(?:^|[,\\n\\r])\\s*ComfyUI\\s+LoRAs\\s*:\\s*([^\\n\\r]*)",
                                                       QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatchIterator comfyIt = comfyLoraRegex.globalMatch(params);
        while (comfyIt.hasNext()) {
            const QStringList entries = comfyIt.next().captured(1).split(',', Qt::SkipEmptyParts);
            for (QString entry : entries) {
                entry = entry.trimmed();
                QString weight;
                const int colon = entry.indexOf(':');
                if (colon > 0) {
                    weight = entry.mid(colon + 1).trimmed();
                    entry = entry.left(colon).trimmed();
                }
                if (!entry.isEmpty()) {
                    const QString norm = entry.toLower();
                    if (!seenLoraNames.contains(norm)) {
                        seenLoraNames.insert(norm);
                        info.parsedLoras.append({entry, QString(), weight});
                    }
                }
            }
        }

        // 4. 从 AddNet Model 提取
        static const QRegularExpression addNetRegex("(?:^|[,\\n\\r])\\s*AddNet\\s+Model\\s+\\d+\\s*:\\s*([^,\\n\\r]+)",
                                                    QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatchIterator addNetIt = addNetRegex.globalMatch(params);
        while (addNetIt.hasNext()) {
            const QString name = addNetIt.next().captured(1).trimmed();
            if (!name.isEmpty()) {
                const QString norm = name.toLower();
                if (!seenLoraNames.contains(norm)) {
                    seenLoraNames.insert(norm);
                    info.parsedLoras.append({name, QString(), QString()});
                }
            }
        }

        // 5. 从 AddNet Model hash 提取
        static const QRegularExpression addNetHashRegex(
            "(?:^|[,\\n\\r])\\s*AddNet\\s+Model\\s+hash\\s+\\d+\\s*:\\s*([A-Fa-f0-9]{6,128})",
            QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatchIterator addNetHashIt = addNetHashRegex.globalMatch(params);
        while (addNetHashIt.hasNext()) {
            const QString hash = addNetHashIt.next().captured(1).trimmed().toLower();
            if (!hash.isEmpty() && !seenExtraHashes.contains(hash)) {
                seenExtraHashes.insert(hash);
                info.extraLoraHashes.append(hash);
            }
        }
    }
}

void parseImage(const QString &path, UserImageInfo &info, bool splitOnNewline, const QStringList &filterTags)
{
    const ParsedImageMetadata parsed = parseImageMetadataFromFile(path);
    // A rescan replaces parsed fields, including a transition to no metadata.
    info.prompt.clear();
    info.negativePrompt.clear();
    info.parameters.clear();
    info.cleanTags.clear();
    info.negativeCleanTags.clear();
    info.checkpointName.clear();
    info.checkpointHash.clear();
    info.parsedLoras.clear();
    info.extraLoraHashes.clear();
    info.isComfy = false;
    info.parsedModelsReady = false;
    info.parserVersion = 0;
    if (!parsed.hasContent()) {
        // A readable image without metadata is a stable result. Incomplete,
        // locked, or damaged files stay retryable on the next scan.
        QImageReader reader(path);
        if (!reader.read().isNull()) info.parserVersion = ParserVersion;
        return;
    }

    info.parserVersion = ParserVersion;
    info.prompt = parsed.positivePrompt.trimmed();
    info.negativePrompt = parsed.negativePrompt.trimmed();
    if (info.negativePrompt.isEmpty() && !info.prompt.isEmpty()) {
        info.negativePrompt = "(empty)";
    }
    info.parameters = parsed.parametersText.trimmed();
    info.cleanTags = TagUtils::parsePromptTags(info.prompt, splitOnNewline, filterTags);
    info.negativeCleanTags = TagUtils::parsePromptTags(info.negativePrompt, splitOnNewline, filterTags);
    populateParsedModelFields(info);
}

namespace {
QString normalizeRuleText(QString s)
{
    s = s.trimmed();
    s.replace(QLatin1Char('_'), QLatin1Char(' '));
    return s.simplified().toCaseFolded();
}
} // namespace

QStringList parseNsfwRules(const QString &rawRules)
{
    QStringList rules;
    QString current;
    int braceDepth = 0;
    int parenDepth = 0;
    bool escaped = false;

    auto flushCurrent = [&]() {
        const QString clean = current.trimmed();
        current.clear();
        if (clean.isEmpty()) return;
        for (const QString &existing : std::as_const(rules)) {
            if (existing.compare(clean, Qt::CaseInsensitive) == 0) return;
        }
        rules.append(clean);
    };

    for (const QChar ch : rawRules) {
        if (escaped) {
            current.append(ch);
            escaped = false;
            continue;
        }
        if (ch == QLatin1Char('\\')) {
            current.append(ch);
            escaped = true;
            continue;
        }
        if (ch == QLatin1Char('{')) ++braceDepth;
        else if (ch == QLatin1Char('}') && braceDepth > 0) --braceDepth;
        else if (ch == QLatin1Char('(')) ++parenDepth;
        else if (ch == QLatin1Char(')') && parenDepth > 0) --parenDepth;

        const bool isLineBreak = (ch == QLatin1Char('\n') || ch == QLatin1Char('\r'));
        const bool isSeparator = isLineBreak ||
            (braceDepth == 0 && parenDepth == 0 &&
             (ch == QLatin1Char(',') || ch == QChar(0xFF0C) || ch == QLatin1Char(';') || ch == QChar(0xFF1B)));
        if (isSeparator) {
            if (isLineBreak) {
                braceDepth = 0;
                parenDepth = 0;
            }
            flushCurrent();
        } else {
            current.append(ch);
        }
    }
    flushCurrent();
    return rules;
}

bool matchesPositivePromptNsfwRules(const QString &rawPrompt,
                                    const QStringList &cleanTags,
                                    const QStringList &rules)
{
    if (rules.isEmpty()) return false;
    if (rawPrompt.trimmed().isEmpty() && cleanTags.isEmpty()) return false;

    static const QRegularExpression loraStripRegex(
        "<\\s*(?:lora|lyco)\\s*:[^>]*>",
        QRegularExpression::CaseInsensitiveOption);

    QString promptWithoutLoras = rawPrompt;
    promptWithoutLoras.remove(loraStripRegex);
    QString promptSpaced = promptWithoutLoras;
    promptSpaced.replace(QLatin1Char('_'), QLatin1Char(' '));

    QString rawPromptSpaced = rawPrompt;
    rawPromptSpaced.replace(QLatin1Char('_'), QLatin1Char(' '));

    QStringList allNormalizedTags;
    QStringList nonLoraNormalizedTags;
    const QStringList &sourceTags = cleanTags.isEmpty()
        ? TagUtils::parsePromptTags(rawPrompt, true, QStringList())
        : cleanTags;
    allNormalizedTags.reserve(sourceTags.size());
    nonLoraNormalizedTags.reserve(sourceTags.size());
    for (const QString &t : sourceTags) {
        const QString norm = normalizeRuleText(t);
        if (norm.isEmpty()) continue;
        allNormalizedTags.append(norm);
        const QString trimmed = t.trimmed();
        if (!trimmed.startsWith(QLatin1String("<lora:"), Qt::CaseInsensitive) &&
            !trimmed.startsWith(QLatin1String("<lyco:"), Qt::CaseInsensitive)) {
            nonLoraNormalizedTags.append(norm);
        }
    }

    for (const QString &rule : rules) {
        const QString cleanRule = rule.trimmed();
        if (cleanRule.isEmpty()) continue;

        const bool targetLoras = cleanRule.startsWith(QLatin1Char('<'))
                              || cleanRule.contains(QLatin1String("lora:"), Qt::CaseInsensitive)
                              || cleanRule.contains(QLatin1String("lyco:"), Qt::CaseInsensitive);
        const QStringList &tagsToCheck = targetLoras ? allNormalizedTags : nonLoraNormalizedTags;
        const QString &promptToCheck = targetLoras ? rawPrompt : promptWithoutLoras;
        const QString &promptSpacedToCheck = targetLoras ? rawPromptSpaced : promptSpaced;

        // 1. regex:<pattern>
        if (cleanRule.startsWith(QLatin1String("regex:"), Qt::CaseInsensitive)) {
            const QString pattern = cleanRule.mid(6).trimmed();
            if (!pattern.isEmpty()) {
                const QRegularExpression re(pattern, QRegularExpression::CaseInsensitiveOption);
                if (re.isValid() && re.match(promptToCheck).hasMatch()) {
                    return true;
                }
            }
            continue;
        }

        // 2. Wildcard rule (* / ?)
        if (cleanRule.contains(QLatin1Char('*')) || cleanRule.contains(QLatin1Char('?'))) {
            const QString normWildcard = normalizeRuleText(cleanRule);
            const QRegularExpression tagRe = QRegularExpression::fromWildcard(
                normWildcard, Qt::CaseInsensitive);
            for (const QString &tag : tagsToCheck) {
                if (tagRe.match(tag).hasMatch()) return true;
            }
            if (!promptToCheck.isEmpty()) {
                const QRegularExpression promptRe(
                    QRegularExpression::wildcardToRegularExpression(
                        cleanRule, QRegularExpression::UnanchoredWildcardConversion),
                    QRegularExpression::CaseInsensitiveOption);
                if (promptRe.match(promptToCheck).hasMatch()) return true;
            }
            continue;
        }

        // 3. Plain keyword / tag match
        const QString ruleNorm = normalizeRuleText(cleanRule);
        for (const QString &tag : tagsToCheck) {
            if (tag == ruleNorm) return true;
        }

        if (!promptToCheck.isEmpty()) {
            const QString escapedRaw = QRegularExpression::escape(cleanRule);
            const QString escapedNorm = QRegularExpression::escape(ruleNorm);
            const QRegularExpression boundaryRaw(
                QStringLiteral("(?<![A-Za-z0-9_])%1(?![A-Za-z0-9_])").arg(escapedRaw),
                QRegularExpression::CaseInsensitiveOption);
            if (boundaryRaw.match(promptToCheck).hasMatch()) return true;

            if (escapedNorm != escapedRaw) {
                const QRegularExpression boundaryNorm(
                    QStringLiteral("(?<![A-Za-z0-9_])%1(?![A-Za-z0-9_])").arg(escapedNorm),
                    QRegularExpression::CaseInsensitiveOption);
                if (boundaryNorm.match(promptSpacedToCheck).hasMatch()) return true;
            }
        }
    }

    return false;
}

QStringList splitTopLevelCommas(const QString &text)
{
    QStringList out;
    QString cur;
    bool inQuotes = false;
    for (const QChar c : text) {
        if (c == QLatin1Char('"')) {
            inQuotes = !inQuotes;
            cur += c;
        } else if (c == QLatin1Char(',') && !inQuotes) {
            out << cur;
            cur.clear();
        } else {
            cur += c;
        }
    }
    out << cur;
    return out;
}

QStringList splitParameterLines(const QString &params)
{
    QStringList lines;
    const QStringList rawLines = params.split(QLatin1Char('\n'));
    for (const QString &line : rawLines) {
        for (const QString &seg : splitTopLevelCommas(line)) {
            const QString trimmed = seg.trimmed();
            if (!trimmed.isEmpty()) {
                lines << trimmed;
            }
        }
    }
    return lines;
}

}
