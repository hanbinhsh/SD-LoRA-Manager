#include "styleconstants.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

// 调色板的构造/注册/读盘实现。从 styleconstants.h 移出，避免 QJson 进入大量 TU。
// 预设调色板存于 :/themes/*.json（编译进资源，只读）；用户主题存于
// 程序目录/config/themes/<id>.json（便携，与 settings.json 同级）。

namespace AppStyle {

namespace {

// 解析一份调色板 JSON：`{ "_name": "...", "token": "#hex", ... }`。
// `_` 前缀字段是元数据（如 _name），不作为 token。
QHash<QString, QString> parsePaletteBytes(const QByteArray &bytes,
                                          QString *displayName = nullptr,
                                          QString *scheme = nullptr)
{
    QHash<QString, QString> map;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return map;
    const QJsonObject obj = doc.object();
    if (displayName)
        *displayName = obj.value(QStringLiteral("_name")).toString();
    if (scheme)
        *scheme = obj.value(QStringLiteral("_scheme")).toString();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const QString key = it.key();
        if (key.startsWith(QLatin1Char('_')))
            continue;
        if (it.value().isString())
            map.insert(key, it.value().toString());
    }
    return map;
}

QHash<QString, QString> loadPaletteResource(const QString &path, QString *scheme = nullptr)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return parsePaletteBytes(f.readAll(), nullptr, scheme);
}

struct ThemeEntry {
    QString id;
    QString displayName;
    bool builtin = false;
    QString scheme = QStringLiteral("dark"); // 窗口基调 "light"/"dark"
    QHash<QString, QString> tokens;          // 已与 steam_dark 合并为全量
};

struct BuiltinDef {
    const char *id;
    const char *file;
    const char *name;
};
const BuiltinDef kBuiltins[] = {
    { "steam_dark", ":/themes/steam_dark.json", "Steam Dark" },
    { "midnight_blue", ":/themes/midnight_blue.json", "Midnight Blue" },
    { "light", ":/themes/light.json", "Light" },
    { "high_contrast", ":/themes/high_contrast.json", "High Contrast" },
};

// 注册表：值用堆指针，保证扩容 / 增删其它项时，已取出的 tokens 引用 / 指针稳定
// （setActiveTheme 会缓存 &entry->tokens，reloadUserThemes 又会增删用户项）。
QHash<QString, ThemeEntry *> &registry()
{
    static QHash<QString, ThemeEntry *> reg;
    return reg;
}
QStringList &themeOrder()
{
    static QStringList order;
    return order;
}

const QHash<QString, QString> &baseDefault()
{
    static const QHash<QString, QString> base =
        loadPaletteResource(QStringLiteral(":/themes/steam_dark.json"));
    return base;
}

void mergeOver(QHash<QString, QString> &dst, const QHash<QString, QString> &over)
{
    for (auto it = over.begin(); it != over.end(); ++it)
        dst.insert(it.key(), it.value());
}

void loadUserThemes()
{
    QDir dir(userThemesDir());
    if (!dir.exists())
        return;
    const QStringList files =
        dir.entryList(QStringList{ QStringLiteral("*.json") }, QDir::Files, QDir::Name);
    for (const QString &fn : files) {
        const QString id = QFileInfo(fn).completeBaseName();
        if (id.isEmpty())
            continue;
        if (registry().contains(id) && registry().value(id)->builtin)
            continue; // 不允许用户主题覆盖内置 id
        QFile f(dir.filePath(fn));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        QString display, sch;
        const QHash<QString, QString> over = parsePaletteBytes(f.readAll(), &display, &sch);
        if (over.isEmpty() && display.isEmpty())
            continue; // 解析失败 / 空文件 -> 跳过（容错，不崩）
        auto *e = new ThemeEntry;
        e->id = id;
        e->displayName = display.isEmpty() ? id : display;
        e->builtin = false;
        e->scheme = (sch == QLatin1String("light")) ? QStringLiteral("light") : QStringLiteral("dark");
        e->tokens = baseDefault();
        mergeOver(e->tokens, over);
        if (!registry().contains(id))
            themeOrder().append(id);
        registry().insert(id, e);
    }
}

void ensureInit()
{
    static bool inited = false;
    if (inited)
        return;
    inited = true;
    for (const BuiltinDef &b : kBuiltins) {
        auto *e = new ThemeEntry;
        e->id = QString::fromLatin1(b.id);
        e->displayName = QString::fromLatin1(b.name);
        e->builtin = true;
        e->tokens = baseDefault();
        QString sch;
        const QHash<QString, QString> over = loadPaletteResource(QString::fromLatin1(b.file), &sch);
        if (e->id != QLatin1String("steam_dark"))
            mergeOver(e->tokens, over);
        e->scheme = (sch == QLatin1String("light")) ? QStringLiteral("light") : QStringLiteral("dark");
        registry().insert(e->id, e);
        themeOrder().append(e->id);
    }
    loadUserThemes();
}

// 实时预览用：进程持有的副本 + 活动指针。
QHash<QString, QString> g_override;
const QHash<QString, QString> *g_active = nullptr;

} // namespace

const QHash<QString, QString> &defaultPalette()
{
    return baseDefault();
}

const QHash<QString, QString> &paletteForTheme(const QString &themeId)
{
    ensureInit();
    auto it = registry().constFind(themeId);
    if (it != registry().constEnd())
        return (*it)->tokens;
    return baseDefault(); // steam_dark / custom_qss / 未知 -> 回退默认
}

const QHash<QString, QString> &activePalette()
{
    return g_active ? *g_active : baseDefault();
}

void setActiveTheme(const QString &themeId)
{
    g_active = &paletteForTheme(themeId);
}

void setActivePalette(const QHash<QString, QString> &palette)
{
    g_override = palette;
    g_active = &g_override;
}

QList<ThemeInfo> availableThemes()
{
    ensureInit();
    QList<ThemeInfo> list;
    list.reserve(themeOrder().size());
    for (const QString &id : themeOrder()) {
        const ThemeEntry *e = registry().value(id, nullptr);
        if (!e)
            continue;
        list.append(ThemeInfo{ e->id, e->displayName, e->builtin, e->scheme });
    }
    return list;
}

QString themeDisplayName(const QString &themeId)
{
    ensureInit();
    const ThemeEntry *e = registry().value(themeId, nullptr);
    return e ? e->displayName : themeId;
}

QString themeColorScheme(const QString &themeId)
{
    ensureInit();
    const ThemeEntry *e = registry().value(themeId, nullptr);
    return (e && !e->scheme.isEmpty()) ? e->scheme : QStringLiteral("dark");
}

bool themeExists(const QString &themeId)
{
    ensureInit();
    return registry().contains(themeId);
}

QString userThemesDir()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/config/themes");
}

bool saveUserTheme(const QString &id, const QString &displayName, const QString &scheme,
                   const QHash<QString, QString> &tokens, QString *errorOut)
{
    if (id.trimmed().isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("主题 id 为空");
        return false;
    }
    if (!QDir().mkpath(userThemesDir())) {
        if (errorOut)
            *errorOut = QStringLiteral("无法创建主题目录：%1").arg(userThemesDir());
        return false;
    }
    // 只写与 steam_dark 默认不同的 token：文件精简、可读、可手改；缺省项加载时回退默认。
    const QHash<QString, QString> &base = baseDefault();
    QJsonObject obj;
    obj.insert(QStringLiteral("_name"), displayName.isEmpty() ? id : displayName);
    obj.insert(QStringLiteral("_scheme"),
               scheme == QLatin1String("light") ? QStringLiteral("light") : QStringLiteral("dark"));
    for (auto it = tokens.begin(); it != tokens.end(); ++it) {
        if (base.value(it.key()) != it.value())
            obj.insert(it.key(), it.value());
    }
    QSaveFile f(userThemesDir() + QLatin1Char('/') + id + QStringLiteral(".json"));
    if (!f.open(QIODevice::WriteOnly)) {
        if (errorOut)
            *errorOut = f.errorString();
        return false;
    }
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (errorOut)
            *errorOut = f.errorString();
        return false;
    }
    return true;
}

bool deleteUserTheme(const QString &id)
{
    ensureInit();
    const ThemeEntry *e = registry().value(id, nullptr);
    if (e && e->builtin)
        return false; // 不删内置
    const QString path = userThemesDir() + QLatin1Char('/') + id + QStringLiteral(".json");
    return QFile::exists(path) ? QFile::remove(path) : true;
}

void reloadUserThemes()
{
    ensureInit();
    for (auto it = themeOrder().begin(); it != themeOrder().end();) {
        ThemeEntry *e = registry().value(*it, nullptr);
        if (e && !e->builtin) {
            registry().remove(*it);
            delete e;
            it = themeOrder().erase(it);
        } else {
            ++it;
        }
    }
    loadUserThemes();
}

} // namespace AppStyle
