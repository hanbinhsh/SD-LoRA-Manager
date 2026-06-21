#include "themeeditordialog.h"

#include "styleconstants.h"

#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMap>
#include <QMessageBox>
#include <QPair>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>

namespace {

// 常用色：带中文标签，放在最上面。其余 token 进入「高级」分组（全量）。
const QList<QPair<QString, QString>> &curatedTokens()
{
    static const QList<QPair<QString, QString>> list = {
        { QStringLiteral("accentBlue"), QStringLiteral("强调色") },
        { QStringLiteral("windowBg"), QStringLiteral("窗口背景") },
        { QStringLiteral("sidebarBg"), QStringLiteral("侧栏背景") },
        { QStringLiteral("panelBg"), QStringLiteral("面板背景") },
        { QStringLiteral("headerBg"), QStringLiteral("表头/标签背景") },
        { QStringLiteral("inputBg"), QStringLiteral("输入框背景") },
        { QStringLiteral("bodyText"), QStringLiteral("正文文字") },
        { QStringLiteral("primaryText"), QStringLiteral("主前景文字") },
        { QStringLiteral("mutedText"), QStringLiteral("次要文字") },
        { QStringLiteral("placeholderText"), QStringLiteral("占位文字") },
        { QStringLiteral("inputBorder"), QStringLiteral("输入框边框") },
        { QStringLiteral("groupBorder"), QStringLiteral("分组边框") },
        { QStringLiteral("buttonBg"), QStringLiteral("按钮背景") },
        { QStringLiteral("selectionBg"), QStringLiteral("选中背景") },
        { QStringLiteral("hoverBg"), QStringLiteral("悬停背景") },
        { QStringLiteral("successGreen"), QStringLiteral("成功色") },
        { QStringLiteral("warningYellow"), QStringLiteral("警告色") },
        { QStringLiteral("errorRed"), QStringLiteral("错误色") },
    };
    return list;
}

// 高级区按 token 名前缀归组（启发式）。
QString groupOf(const QString &token)
{
    if (token.startsWith(QLatin1String("chat")))
        return QStringLiteral("对话 Chat");
    if (token.startsWith(QLatin1String("workflow")) || token.startsWith(QLatin1String("wf")))
        return QStringLiteral("工作流 Workflow");
    if (token.startsWith(QLatin1String("chart")))
        return QStringLiteral("图表 Chart");
    if (token.startsWith(QLatin1String("download")) || token.startsWith(QLatin1String("progress")))
        return QStringLiteral("下载 Download");
    if (token.startsWith(QLatin1String("tag")) || token.startsWith(QLatin1String("chip"))
        || token.startsWith(QLatin1String("userTag")))
        return QStringLiteral("标签 Tag/Chip");
    if (token.startsWith(QLatin1String("html")))
        return QStringLiteral("富文本 HTML");
    if (token.startsWith(QLatin1String("scroll")))
        return QStringLiteral("滚动条 Scroll");
    return QStringLiteral("其他 Other");
}

// 高级分组的固定展示顺序。
const QStringList &groupOrder()
{
    static const QStringList order = {
        QStringLiteral("其他 Other"),
        QStringLiteral("滚动条 Scroll"),
        QStringLiteral("下载 Download"),
        QStringLiteral("标签 Tag/Chip"),
        QStringLiteral("对话 Chat"),
        QStringLiteral("工作流 Workflow"),
        QStringLiteral("图表 Chart"),
        QStringLiteral("富文本 HTML"),
    };
    return order;
}

} // namespace

ThemeEditorDialog::ThemeEditorDialog(const QString &baseThemeId, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("主题编辑器 / Theme Editor"));
    resize(560, 680);
    buildUi(baseThemeId);

    // 取消 / 关闭 -> 请求 MainWindow 还原到当前持久主题。
    connect(this, &QDialog::rejected, this, [this]() { emit canceled(); });
}

void ThemeEditorDialog::buildUi(const QString &baseThemeId)
{
    auto *outer = new QVBoxLayout(this);

    // —— 顶部：基于 + 名称 ——
    auto *form = new QFormLayout;
    m_comboBase = new QComboBox(this);
    const QList<AppStyle::ThemeInfo> themes = AppStyle::availableThemes();
    for (const AppStyle::ThemeInfo &t : themes) {
        const QString label = t.builtin ? t.displayName
                                        : QStringLiteral("%1（自定义）").arg(t.displayName);
        m_comboBase->addItem(label, t.id);
    }
    int baseIdx = m_comboBase->findData(baseThemeId);
    if (baseIdx < 0)
        baseIdx = m_comboBase->findData(QStringLiteral("steam_dark"));
    if (baseIdx >= 0)
        m_comboBase->setCurrentIndex(baseIdx);
    form->addRow(tr("基于 / Base:"), m_comboBase);

    // 窗口基调：决定原生 colorScheme（标题栏明暗、复选框、未样式控件的系统调色板）。
    m_comboScheme = new QComboBox(this);
    m_comboScheme->addItem(tr("暗色 / Dark"), QStringLiteral("dark"));
    m_comboScheme->addItem(tr("亮色 / Light"), QStringLiteral("light"));
    QString baseScheme = QStringLiteral("dark");
    for (const AppStyle::ThemeInfo &t : themes) {
        if (t.id == baseThemeId) {
            baseScheme = t.scheme;
            break;
        }
    }
    m_comboScheme->setCurrentIndex(baseScheme == QLatin1String("light") ? 1 : 0);
    form->addRow(tr("窗口基调 / Window base:"), m_comboScheme);

    m_editName = new QLineEdit(this);
    m_editName->setPlaceholderText(tr("自定义主题名称（将保存为 config/themes 下的 JSON）"));
    // 若以用户主题为起点，预填其名称，方便就地修改并覆盖。
    for (const AppStyle::ThemeInfo &t : themes) {
        if (t.id == baseThemeId && !t.builtin) {
            m_editName->setText(t.displayName);
            break;
        }
    }
    form->addRow(tr("名称 / Name:"), m_editName);
    outer->addLayout(form);

    // —— 颜色区（可滚动）——
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto *content = new QWidget;
    auto *colLayout = new QVBoxLayout(content);
    colLayout->setContentsMargins(4, 4, 4, 4);
    colLayout->setSpacing(2);

    // 常用色
    addSectionHeader(colLayout, tr("常用色 / Key Colors"));
    QSet<QString> curatedSet;
    for (const auto &pair : curatedTokens()) {
        addTokenRow(colLayout, pair.first, pair.second);
        curatedSet.insert(pair.first);
    }

    // 高级：其余 token 按分组（全量）。用可勾选按钮当折叠头，避免 QGroupBox 边框/空框残留。
    auto *advToggle = new QPushButton(content);
    advToggle->setCheckable(true);
    advToggle->setChecked(false);
    advToggle->setCursor(Qt::PointingHandCursor);
    advToggle->setStyleSheet(QStringLiteral("text-align:left;padding:6px 10px;"));
    const QString advTextOn = tr("▾ 高级（全部 token）/ Advanced");
    const QString advTextOff = tr("▸ 高级（全部 token）/ Advanced");
    advToggle->setText(advTextOff);

    auto *advInner = new QWidget(content);
    auto *advLayout = new QVBoxLayout(advInner);
    advLayout->setContentsMargins(0, 0, 0, 0);
    advLayout->setSpacing(2);

    QMap<QString, QStringList> byGroup;
    const QList<QString> allTokens = AppStyle::defaultPalette().keys();
    for (const QString &token : allTokens) {
        if (curatedSet.contains(token))
            continue;
        byGroup[groupOf(token)].append(token);
    }
    for (const QString &group : groupOrder()) {
        QStringList tokens = byGroup.value(group);
        if (tokens.isEmpty())
            continue;
        tokens.sort();
        addSectionHeader(advLayout, group);
        for (const QString &token : tokens)
            addTokenRow(advLayout, token, token);
    }
    advInner->setVisible(false);
    connect(advToggle, &QPushButton::toggled, advInner, &QWidget::setVisible);
    connect(advToggle, &QPushButton::toggled, advToggle,
            [advToggle, advTextOn, advTextOff](bool on) {
                advToggle->setText(on ? advTextOn : advTextOff);
            });

    colLayout->addWidget(advToggle);
    colLayout->addWidget(advInner);
    colLayout->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    // —— 底部按钮 ——
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *btnSave = new QPushButton(tr("保存并应用"), this);
    auto *btnClose = new QPushButton(tr("取消"), this);
    btnSave->setDefault(true);
    buttons->addWidget(btnSave);
    buttons->addWidget(btnClose);
    outer->addLayout(buttons);

    connect(btnSave, &QPushButton::clicked, this, &ThemeEditorDialog::onSaveClicked);
    connect(btnClose, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_comboBase, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ThemeEditorDialog::onBaseChanged);
    connect(m_comboScheme, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this]() { emitPreview(); }); // 改基调即重设 colorScheme 预览

    // 初始载入基底调色板（顺带触发一次预览）。
    loadPalette(AppStyle::paletteForTheme(baseThemeId));
}

void ThemeEditorDialog::addSectionHeader(QVBoxLayout *layout, const QString &title)
{
    auto *lbl = new QLabel(title);
    lbl->setStyleSheet(QStringLiteral("font-weight:bold;margin-top:6px;"));
    layout->addWidget(lbl);
}

void ThemeEditorDialog::addTokenRow(QVBoxLayout *layout, const QString &token, const QString &label)
{
    auto *row = new QWidget;
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 1, 0, 1);

    auto *lbl = new QLabel(label, row);
    lbl->setMinimumWidth(150);
    lbl->setToolTip(token);

    auto *swatch = new QPushButton(row);
    swatch->setFixedSize(30, 20);
    swatch->setCursor(Qt::PointingHandCursor);
    swatch->setToolTip(tr("点击取色"));

    auto *hex = new QLineEdit(row);
    hex->setMaximumWidth(90);
    hex->setPlaceholderText(QStringLiteral("#RRGGBB"));

    h->addWidget(lbl);
    h->addWidget(swatch);
    h->addWidget(hex);
    h->addStretch();
    layout->addWidget(row);

    m_rows.insert(token, TokenRow{ hex, swatch });

    connect(swatch, &QPushButton::clicked, this, [this, token]() {
        const QColor cur(m_palette.value(token));
        const QColor picked =
            QColorDialog::getColor(cur.isValid() ? cur : QColor(Qt::black), this, tr("选择颜色"));
        if (picked.isValid())
            setTokenColor(token, picked.name(QColor::HexRgb), true);
    });
    connect(hex, &QLineEdit::editingFinished, this, [this, token, hex]() {
        if (m_loading)
            return;
        const QColor c(hex->text().trimmed());
        if (c.isValid())
            setTokenColor(token, c.name(QColor::HexRgb), true);
        else {
            const QSignalBlocker b(hex);
            hex->setText(m_palette.value(token)); // 非法值还原
        }
    });
}

void ThemeEditorDialog::updateSwatch(const QString &token, const QString &hex)
{
    const TokenRow tr = m_rows.value(token);
    if (tr.swatch)
        tr.swatch->setStyleSheet(
            QStringLiteral("QPushButton{background:%1;border:1px solid #888888;border-radius:3px;}")
                .arg(hex));
}

void ThemeEditorDialog::setTokenColor(const QString &token, const QString &hex, bool doEmit)
{
    m_palette.insert(token, hex);
    const TokenRow tr = m_rows.value(token);
    if (tr.hex) {
        const QSignalBlocker b(tr.hex);
        tr.hex->setText(hex);
    }
    updateSwatch(token, hex);
    if (doEmit && !m_loading)
        emitPreview();
}

bool ThemeEditorDialog::isLightSelected() const
{
    return m_comboScheme && m_comboScheme->currentData().toString() == QLatin1String("light");
}

void ThemeEditorDialog::emitPreview()
{
    emit previewRequested(m_palette, isLightSelected());
}

void ThemeEditorDialog::loadPalette(const QHash<QString, QString> &palette)
{
    m_loading = true;
    m_palette = palette;
    for (auto it = m_rows.begin(); it != m_rows.end(); ++it) {
        const QString hex = m_palette.value(it.key());
        if (it->hex) {
            const QSignalBlocker b(it->hex);
            it->hex->setText(hex);
        }
        updateSwatch(it.key(), hex);
    }
    m_loading = false;
    emitPreview();
}

void ThemeEditorDialog::onBaseChanged()
{
    if (!m_comboBase)
        return;
    const QString id = m_comboBase->currentData().toString();
    // 切基底时把窗口基调也同步到该主题的 scheme（用户仍可手动改）。
    if (m_comboScheme) {
        const QString sch = AppStyle::themeColorScheme(id);
        const QSignalBlocker b(m_comboScheme);
        m_comboScheme->setCurrentIndex(sch == QLatin1String("light") ? 1 : 0);
    }
    loadPalette(AppStyle::paletteForTheme(id));
}

void ThemeEditorDialog::onSaveClicked()
{
    const QString name = m_editName ? m_editName->text().trimmed() : QString();
    if (name.isEmpty()) {
        QMessageBox::warning(this, tr("提示"), tr("请先填写主题名称。"));
        return;
    }
    const QString id = sanitizeId(name);
    if (id.isEmpty()) {
        QMessageBox::warning(this, tr("提示"), tr("主题名称无法转换为有效文件名，请换一个。"));
        return;
    }
    if (AppStyle::themeExists(id)) {
        bool builtin = false;
        for (const AppStyle::ThemeInfo &t : AppStyle::availableThemes()) {
            if (t.id == id) {
                builtin = t.builtin;
                break;
            }
        }
        if (builtin) {
            QMessageBox::warning(this, tr("提示"), tr("该名称与内置主题冲突，请换一个名称。"));
            return;
        }
        if (QMessageBox::question(this, tr("覆盖确认"),
                                  tr("已存在同名用户主题“%1”，是否覆盖？").arg(name))
            != QMessageBox::Yes)
            return;
    }
    QString err;
    const QString scheme = isLightSelected() ? QStringLiteral("light") : QStringLiteral("dark");
    if (!AppStyle::saveUserTheme(id, name, scheme, m_palette, &err)) {
        QMessageBox::warning(this, tr("保存失败"), err);
        return;
    }
    emit saved(id);
    accept();
}

QString ThemeEditorDialog::sanitizeId(const QString &name)
{
    QString id = name.trimmed();
    static const QString invalid = QStringLiteral("\\/:*?\"<>|");
    for (QChar &c : id) {
        if (invalid.contains(c) || c < QChar(' '))
            c = QLatin1Char('_');
    }
    id = id.simplified();
    id.replace(QLatin1Char(' '), QLatin1Char('_'));
    return id;
}
