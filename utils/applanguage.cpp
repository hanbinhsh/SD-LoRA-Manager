#include "applanguage.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QTranslator>
#include <algorithm>

static void initTranslationResources()
{
    static const bool registered = []() {
        Q_INIT_RESOURCE(app_translations);
        Q_INIT_RESOURCE(app_translation_catalog);
        return true;
    }();
    Q_UNUSED(registered);
}

namespace {
QString activeLanguage = QStringLiteral("zh_CN");
QPointer<QTranslator> activeTranslator;

class ApplicationTranslator final : public QTranslator {
public:
    using QTranslator::QTranslator;
    QString translate(const char *context, const char *source, const char *comment = nullptr,
                      int n = -1) const override
    {
        QString translated = QTranslator::translate(context, source, comment, n);
        if (translated.isNull()) translated = QTranslator::translate("AppText", source, nullptr, n);
        return translated;
    }
};

struct StatusPattern {
    QString source;
    QString prefix;
    QRegularExpression expression;
    QHash<QString, int> captures;
};

const QList<StatusPattern> &statusPatterns()
{
    static const QList<StatusPattern> patterns = []() {
        QList<StatusPattern> result;
        QFile file(":/i18n/catalog.json");
        if (!file.open(QIODevice::ReadOnly)) return result;
        const QJsonObject catalog = QJsonDocument::fromJson(file.readAll()).object();
        static const QRegularExpression placeholder("%[1-9][0-9]?");
        for (auto it = catalog.begin(); it != catalog.end(); ++it) {
            const QString source = it.key();
            auto matches = placeholder.globalMatch(source);
            if (!matches.hasNext()) continue;
            StatusPattern pattern;
            pattern.source = source;
            pattern.prefix = source.left(placeholder.match(source).capturedStart());
            QString regex = "\\A";
            qsizetype offset = 0;
            while (matches.hasNext()) {
                const auto match = matches.next();
                regex += QRegularExpression::escape(source.mid(offset, match.capturedStart() - offset));
                if (pattern.captures.contains(match.captured())) {
                    regex += QString("\\g{%1}").arg(pattern.captures.value(match.captured()));
                } else {
                    pattern.captures.insert(match.captured(), pattern.captures.size() + 1);
                    regex += "(.*?)";
                }
                offset = match.capturedEnd();
            }
            regex += QRegularExpression::escape(source.mid(offset)) + "\\z";
            pattern.expression = QRegularExpression(regex, QRegularExpression::DotMatchesEverythingOption);
            result.append(pattern);
        }
        // Prefer specific messages over generic patterns such as "Error: %1".
        std::stable_sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
            return a.source.size() > b.source.size();
        });
        return result;
    }();
    return patterns;
}
}

QString AppText::display(const QString &source)
{
    const QByteArray utf8 = source.toUtf8();
    const QString exact = tr(utf8.constData());
    if (exact != source) return exact;
    if (AppLanguage::current() != "en_US") return source;
    static const QRegularExpression chinese("[\\x{3400}-\\x{9fff}]");
    if (!source.contains(chinese)) return source;
    static const QRegularExpression placeholder("%[1-9][0-9]?");
    for (const auto &pattern : statusPatterns()) {
        if (!source.startsWith(pattern.prefix)) continue;
        const auto match = pattern.expression.match(source);
        if (!match.hasMatch()) continue;
        const QByteArray key = pattern.source.toUtf8();
        const QString translated = tr(key.constData());
        if (translated == pattern.source) continue;
        QString output;
        qsizetype offset = 0;
        auto tokens = placeholder.globalMatch(translated);
        while (tokens.hasNext()) {
            const auto token = tokens.next();
            output += translated.mid(offset, token.capturedStart() - offset);
            output += match.captured(pattern.captures.value(token.captured()));
            offset = token.capturedEnd();
        }
        return output + translated.mid(offset);
    }
    return source;
}

QString AppLanguage::normalized(QString language)
{
    language = language.trimmed().replace('-', '_');
    return language.compare("en", Qt::CaseInsensitive) == 0
        || language.compare("en_US", Qt::CaseInsensitive) == 0 ? "en_US" : "zh_CN";
}

QString AppLanguage::current() { return activeLanguage; }

bool AppLanguage::initialize(QCoreApplication &application, const QString &language)
{
    initTranslationResources();
    const QString selected = normalized(language);
    auto *translator = new ApplicationTranslator(&application);
    if (!translator->load(":/i18n/app_" + selected + ".qm")) {
        delete translator;
        return false;
    }
    if (activeTranslator) {
        application.removeTranslator(activeTranslator);
        delete activeTranslator;
    }
    activeTranslator = translator;
    activeLanguage = selected;
    application.installTranslator(translator);
    return true;
}
