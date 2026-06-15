#include "prompttemplatelibrarywidget.h"
#include "tableviewstylehelper.h"
#include "ui_prompttemplatelibrarywidget.h"

#include "imagemetadataparser.h"
#include "tagflowwidget.h"
#include "tagutils.h"

#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFutureWatcher>
#include <QBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QInputDialog>
#include <QSpinBox>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTextCursor>
#include <QTextStream>
#include <QTextOption>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <numeric>

namespace {

class ModelTriggerTreeDelegate : public QStyledItemDelegate
{
public:
    explicit ModelTriggerTreeDelegate(QTreeWidget *tree, QObject *parent = nullptr)
        : QStyledItemDelegate(parent), m_tree(tree) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const bool child = index.parent().isValid();
        const int viewportWidth = m_tree ? m_tree->viewport()->width() : option.rect.width();
        const int indent = child ? (m_tree ? m_tree->indentation() : 18) : 0;
        // 与 paint() 中文本的可用宽度保持一致，并略微取窄以确保测得行数 >= 实际绘制行数，
        // 避免最后一行因实际换行更多而被裁掉。
        const int sideInsets = child ? 60 : 100;
        const int availableWidth = qMax(80, viewportWidth - indent - sideInsets);
        QFont font = option.font;
        font.setBold(true); // 以粗体测量，行宽更保守
        QFontMetrics fm(font);
        const QRect textRect = fm.boundingRect(QRect(0, 0, availableWidth, 10000),
                                               Qt::TextWordWrap | Qt::AlignLeft,
                                               index.data(Qt::DisplayRole).toString());
        const int height = child
            ? qMax(38, textRect.height() + 20)
            : qMax(72, textRect.height() + 24);
        return QSize(viewportWidth, height);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const bool child = index.parent().isValid();
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hover = option.state.testFlag(QStyle::State_MouseOver);
        QRect card = option.rect.adjusted(child ? 26 : 4, 3, -6, -3);

        const QColor bg = selected
            ? QColor("#3D4450")
            : hover ? QColor("#2a3442")
                    : child ? QColor("#202936") : QColor("#1f2833");
        painter->setPen(Qt::NoPen);
        painter->setBrush(bg);
        painter->drawRoundedRect(card, 7, 7);

        if (selected) {
            painter->setBrush(QColor("#66c0f4"));
            painter->drawRoundedRect(QRect(card.left(), card.top(), 4, card.height()), 2, 2);
        }

        QRect textRect = card.adjusted(12, 4, -12, -4);
        if (!child) {
            const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
            if (!icon.isNull()) {
                const QRect iconRect(card.left() + 12, card.top() + qMax(8, (card.height() - 52) / 2), 52, 52);
                icon.paint(painter, iconRect, Qt::AlignCenter, QIcon::Normal, QIcon::Off);
                textRect.setLeft(iconRect.right() + 12);
            }
        }

        QFont font = option.font;
        font.setBold(!child || selected);
        painter->setFont(font);
        QColor textColor = QColor("#dcdedf");
        if (const QVariant foreground = index.data(Qt::ForegroundRole); foreground.canConvert<QBrush>()) {
            textColor = foreground.value<QBrush>().color();
        }
        if (selected) textColor = QColor("#ffffff");
        painter->setPen(textColor);

        QTextOption textOption;
        textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        textOption.setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        painter->drawText(QRectF(textRect), index.data(Qt::DisplayRole).toString(), textOption);
        painter->restore();
    }

private:
    QTreeWidget *m_tree = nullptr;
};

QString normalizeTagSearch(QString text)
{
    text = text.toCaseFolded().trimmed();
    text.replace('_', ' ');
    text.replace('-', ' ');
    static const QRegularExpression spaces("\\s+");
    text.replace(spaces, " ");
    return text;
}

bool isAsciiText(const QString &text)
{
    for (const QChar c : text) {
        if (c.unicode() > 0x7f) return false;
    }
    return true;
}

// 解析翻译词表的中文字段：去掉末尾的 ",数字"（autocomplete 词表里的优先级/使用次数），
// 把数字作为排序权重返回，剩余部分作为展示文本。
void parseTranslationValue(const QString &raw, QString &display, int &count)
{
    display = raw.trimmed();
    count = 0;
    const int lastComma = display.lastIndexOf(',');
    if (lastComma > 0) {
        const QString tail = display.mid(lastComma + 1).trimmed();
        bool numeric = !tail.isEmpty();
        for (const QChar c : tail) {
            if (!c.isDigit()) { numeric = false; break; }
        }
        if (numeric) {
            count = tail.toInt();
            display = display.left(lastComma).trimmed();
        }
    }
}

// 从翻译字段中剥离重复出现的英文 tag 前缀。
// ComfyUI 词表里中文字段形如 "1girl 人物-一个女孩"（tag 类型-翻译），需要去掉开头的 "1girl "；
// a1111 词表形如 "一个女孩"，无需处理。
QString stripLeadingTagFromTranslation(const QString &tag, QString display)
{
    display = display.trimmed();
    if (display.isEmpty() || tag.isEmpty()) return display;

    QStringList prefixes;
    prefixes << tag;
    QString spaced = tag; spaced.replace('_', ' ');
    if (spaced != tag) prefixes << spaced;
    QString underscored = tag; underscored.replace(' ', '_');
    if (underscored != tag) prefixes << underscored;

    for (const QString &prefix : prefixes) {
        if (display.size() > prefix.size() + 1 &&
            display.startsWith(prefix, Qt::CaseInsensitive) &&
            display.at(prefix.size()).isSpace()) {
            return display.mid(prefix.size()).trimmed();
        }
    }
    // 翻译字段与 tag 完全相同时视为没有翻译。
    if (display.compare(tag, Qt::CaseInsensitive) == 0) return QString();
    return display;
}

// 自动补全弹窗的条目绘制：左侧 tag（白色左对齐），右侧翻译（灰色右对齐）。
class AutocompleteItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize s = QStyledItemDelegate::sizeHint(option, index);
        s.setHeight(qMax(s.height(), option.fontMetrics.height() + 8));
        return s;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        painter->save();
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        if (selected) painter->fillRect(option.rect, QColor("#3d4450"));

        const QString tag = index.data(Qt::UserRole).toString();
        const QString translation = index.data(Qt::UserRole + 1).toString();
        const QRect r = option.rect.adjusted(8, 0, -8, 0);
        const QFontMetrics fm(option.font);

        painter->setFont(option.font);
        painter->setPen(selected ? QColor("#ffffff") : QColor("#dcdedf"));
        const QString tagText = fm.elidedText(tag, Qt::ElideRight, r.width());
        painter->drawText(r, Qt::AlignVCenter | Qt::AlignLeft, tagText);

        if (!translation.isEmpty()) {
            const int tagWidth = fm.horizontalAdvance(tagText) + 14;
            const QRect trRect = r.adjusted(tagWidth, 0, 0, 0);
            if (trRect.width() > 12) {
                painter->setPen(QColor("#8c96a0"));
                const QString trText = fm.elidedText(translation, Qt::ElideRight, trRect.width());
                painter->drawText(trRect, Qt::AlignVCenter | Qt::AlignRight, trText);
            }
        }
        painter->restore();
    }
};

bool isSupportedImagePath(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == "png" || suffix == "jpg" || suffix == "jpeg" || suffix == "webp";
}

QString imagePathFromMimeData(const QMimeData *mimeData)
{
    if (!mimeData) return QString();
    if (mimeData->hasUrls()) {
        for (const QUrl &url : mimeData->urls()) {
            if (!url.isLocalFile()) continue;
            const QString path = url.toLocalFile();
            if (QFile::exists(path) && isSupportedImagePath(path)) return path;
        }
    }
    const QString text = mimeData->text().trimmed();
    if (QFile::exists(text) && isSupportedImagePath(text)) return text;
    return QString();
}

QStringList parsePromptTags(const QString &prompt)
{
    QString normalized = prompt;
    normalized.replace("\r\n", ",");
    normalized.replace('\n', ',');
    normalized.replace('\r', ',');

    QStringList tags;
    QSet<QString> seenInImage;
    static const QSet<QString> blockedTags = {"BREAK", "ADDCOMM", "ADDBASE", "ADDCOL", "ADDROW"};
    for (const QString &part : normalized.split(',', Qt::SkipEmptyParts)) {
        const QString tag = TagUtils::cleanPromptTag(part, false);
        if (tag.isEmpty()) continue;
        bool blocked = false;
        for (const QString &blockedTag : blockedTags) {
            if (tag.compare(blockedTag, Qt::CaseInsensitive) == 0) {
                blocked = true;
                break;
            }
        }
        if (blocked || seenInImage.contains(tag)) continue;
        seenInImage.insert(tag);
        tags.append(tag);
    }
    return tags;
}

QString tagDedupKey(const QString &tag)
{
    return normalizeTagSearch(TagUtils::cleanPromptTag(tag, false));
}

QStringList newPromptTagsOnly(const QString &currentPrompt, const QStringList &tags)
{
    QSet<QString> existing;
    for (const QString &tag : parsePromptTags(currentPrompt)) {
        const QString key = tagDedupKey(tag);
        if (!key.isEmpty()) existing.insert(key);
    }

    QStringList out;
    QSet<QString> seenNew;
    for (const QString &rawTag : tags) {
        const QString tag = TagUtils::cleanPromptTag(rawTag, false);
        const QString key = tagDedupKey(tag);
        if (tag.isEmpty() || key.isEmpty()) continue;
        if (existing.contains(key) || seenNew.contains(key)) continue;
        seenNew.insert(key);
        out.append(tag);
    }
    return out;
}

void splitCategoryAndTranslationText(const QString &text, QString &category, QString &translation)
{
    QString value = text.trimmed();
    category.clear();
    translation.clear();

    if (value.isEmpty()) return;

    // 兼容新 autocomplete 格式被旧逻辑读成一整段的情况：
    // 发型-长发,2898315
    // 只在最后一段是纯数字时，把它当作优先级丢掉。
    const int lastComma = value.lastIndexOf(',');
    if (lastComma > 0) {
        const QString tail = value.mid(lastComma + 1).trimmed();

        bool isNumber = !tail.isEmpty();
        for (const QChar ch : tail) {
            if (!ch.isDigit()) {
                isNumber = false;
                break;
            }
        }

        if (isNumber) {
            value = value.left(lastComma).trimmed();
        }
    }

    int dash = value.indexOf('-');
    if (dash < 0) dash = value.indexOf(QChar(0xFF0D)); // －
    if (dash < 0) dash = value.indexOf(QChar(0x2014)); // —
    if (dash < 0) dash = value.indexOf(QChar(0x2013)); // –

    if (dash > 0) {
        category = value.left(dash).trimmed();
        translation = value.mid(dash + 1).trimmed();
    } else {
        translation = value.trimmed();
    }
}

int appendUniquePromptTags(QPlainTextEdit *target, const QStringList &tags)
{
    if (!target) return 0;

    const QString currentText = target->toPlainText();
    const QStringList newTags = newPromptTagsOnly(currentText, tags);
    if (newTags.isEmpty()) return 0;

    QString nextText = currentText;

    if (!nextText.trimmed().isEmpty()) {
        if (nextText.endsWith('\n') || nextText.endsWith('\r')) {
            // 保留模板末尾已有换行/空行，直接在后面追加
        } else if (!nextText.trimmed().endsWith(',')) {
            nextText += ", ";
        } else if (!nextText.endsWith(' ')) {
            nextText += " ";
        }
    }

    nextText += newTags.join(", ");
    target->setPlainText(nextText);
    return newTags.size();
}

QTableWidgetItem *makePromptTemplateTableItem(const QVariant &value)
{
    auto *item = new QTableWidgetItem;
    item->setData(Qt::DisplayRole, value);
    return item;
}

QTableWidgetItem *makeReadOnlyPromptTemplateItem(const QVariant &value)
{
    auto *item = makePromptTemplateTableItem(value);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QString placeholderOptionDisplayText(QString value)
{
    value.replace("\r\n", "\n");
    value.replace('\r', '\n');

    if (value.isEmpty()) return "<空选项>";

    const bool hasNewline = value.contains('\n');
    QString firstLine = value.section('\n', 0, 0);

    if (firstLine.isEmpty()) firstLine = "⏎";
    if (hasNewline) firstLine += "  ↵";

    return firstLine;
}

QTableWidgetItem *makePlaceholderOptionItem(const QString &value)
{
    auto *item = new QTableWidgetItem(placeholderOptionDisplayText(value));
    item->setData(Qt::UserRole, value);
    item->setToolTip(value.isEmpty() ? "空选项" : value);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

void swapPlaceholderOptionRows(QTableWidget *table, int rowA, int rowB)
{
    if (!table) return;
    if (rowA < 0 || rowB < 0) return;
    if (rowA >= table->rowCount() || rowB >= table->rowCount()) return;
    if (rowA == rowB) return;

    QTableWidgetItem *itemA = table->takeItem(rowA, 0);
    QTableWidgetItem *itemB = table->takeItem(rowB, 0);

    table->setItem(rowA, 0, itemB);
    table->setItem(rowB, 0, itemA);
}

QString cleanupRenderedPrompt(QString text)
{
    text.replace("\r\n", "\n");
    text.replace('\r', '\n');
    return text;
}

QStringList placeholderNamesInTemplateText(const QString &positive, const QString &negative)
{
    QStringList names;
    QSet<QString> seen;

    static const QRegularExpression regex("\\{([A-Za-z0-9_]+)\\}");

    auto collect = [&names, &seen](const QString &text) {
        auto it = regex.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const QString name = match.captured(1).trimmed();
            if (name.isEmpty() || seen.contains(name)) continue;
            seen.insert(name);
            names.append(name);
        }
    };

    collect(positive);
    collect(negative);
    return names;
}

QStringList promptTagsNotInBase(const QString &prompt, const QString &basePrompt)
{
    QSet<QString> baseKeys;
    for (const QString &tag : parsePromptTags(basePrompt)) {
        const QString key = tagDedupKey(tag);
        if (!key.isEmpty()) baseKeys.insert(key);
    }

    QStringList out;
    QSet<QString> seen;
    for (const QString &tag : parsePromptTags(prompt)) {
        const QString key = tagDedupKey(tag);
        if (key.isEmpty() || baseKeys.contains(key) || seen.contains(key)) continue;
        seen.insert(key);
        out.append(tag);
    }
    return out;
}

QString promptSummary(QString text, int maxLen = 80)
{
    text = text.simplified();
    if (text.size() <= maxLen) return text;
    return text.left(maxLen - 3) + "...";
}

// 收藏的更新时间（存储为 UTC ISO）转换为本地 yyyy-MM-dd HH:mm:ss。
QString formatFavoriteTime(const QString &iso)
{
    QDateTime dt = QDateTime::fromString(iso, Qt::ISODate);
    if (!dt.isValid()) return iso;
    dt.setTimeSpec(Qt::UTC);
    return dt.toLocalTime().toString("yyyy-MM-dd HH:mm:ss");
}

void populateModelTriggerChildren(QTreeWidgetItem *modelItem)
{
    if (!modelItem || modelItem->childCount() > 0) return;
    const QStringList metadataTriggers = modelItem->data(0, Qt::UserRole + 2).toStringList();
    const QStringList customTriggers = modelItem->data(0, Qt::UserRole + 3).toStringList();

    const QVariant loraName = modelItem->data(0, Qt::UserRole + 5);
    auto addTriggerChildren = [&](const QStringList &list, const QColor &color) {
        for (const QString &trigger : list) {
            auto *child = new QTreeWidgetItem(modelItem);
            child->setText(0, trigger);
            child->setData(0, Qt::UserRole, modelItem->data(0, Qt::UserRole));
            child->setData(0, Qt::UserRole + 1, QStringList{trigger});
            child->setData(0, Qt::UserRole + 5, loraName);
            child->setForeground(0, color);
            child->setToolTip(0, trigger);
        }
    };

    addTriggerChildren(metadataTriggers, QColor("#66c0f4"));
    addTriggerChildren(customTriggers, QColor("#7bd88f"));
}

void addTagCounts(const QString &prompt, QMap<QString, int> &counts)
{
    for (const QString &tag : parsePromptTags(prompt)) {
        counts[tag] += 1;
    }
}

QString removeLeadingTagFromDisplayText(const QString &tag, QString display)
{
    display = display.trimmed();

    if (display.isEmpty()) return display;

    QSet<QString> candidates;

    auto addCandidate = [&candidates](QString value) {
        value = value.trimmed();
        if (value.isEmpty()) return;

        candidates.insert(value);

        QString withSpaces = value;
        withSpaces.replace('_', ' ');
        candidates.insert(withSpaces.trimmed());

        QString withUnderscores = value;
        withUnderscores.replace(' ', '_');
        candidates.insert(withUnderscores.trimmed());
    };

    addCandidate(tag);
    addCandidate(TagUtils::cleanPromptTag(tag, false));

    // 兼容 display 自身开头就是英文 tag 的情况：
    // long_hair 发型-长发
    // long hair 发型-长发
    const int firstSpace = display.indexOf(QRegularExpression("\\s+"));
    if (firstSpace > 0) {
        addCandidate(display.left(firstSpace));
    }

    QStringList sortedCandidates = candidates.values();

    // 长的优先，避免 tag 是 long，display 是 long_hair 时误删 long
    std::sort(sortedCandidates.begin(), sortedCandidates.end(), [](const QString &a, const QString &b) {
        return a.size() > b.size();
    });

    for (const QString &candidate : sortedCandidates) {
        if (candidate.isEmpty()) continue;

        const QString prefixSpace = candidate + " ";
        const QString prefixTab = candidate + "\t";

        if (display.startsWith(prefixSpace, Qt::CaseSensitive)) {
            return display.mid(prefixSpace.size()).trimmed();
        }

        if (display.startsWith(prefixTab, Qt::CaseSensitive)) {
            return display.mid(prefixTab.size()).trimmed();
        }
    }

    return display;
}

QVector<PromptTemplateLibraryWidget::TagUsageRow> readTagRowsWorker(const QString &cachePath, int scope)
{
    QVector<PromptTemplateLibraryWidget::TagUsageRow> rows;
    QFile file(cachePath);
    if (!file.open(QIODevice::ReadOnly)) return rows;

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    QMap<QString, int> positiveCounts;
    QMap<QString, int> negativeCounts;
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        if (it.key().startsWith("__")) continue;
        const QJsonObject obj = it.value().toObject();
        if (scope == 0 || scope == 2) addTagCounts(obj["p"].toString(), positiveCounts);
        if (scope == 1 || scope == 2) addTagCounts(obj["np"].toString(), negativeCounts);
    }

    auto appendRows = [&rows](const QMap<QString, int> &counts, const QString &kind) {
        for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
            PromptTemplateLibraryWidget::TagUsageRow row;
            row.tag = it.key();
            row.kind = kind;
            row.count = it.value();
            rows.append(row);
        }
    };
    if (scope == 0 || scope == 2) appendRows(positiveCounts, "正面");
    if (scope == 1 || scope == 2) appendRows(negativeCounts, "负面");

    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        if (a.count != b.count) return a.count > b.count;
        if (a.tag != b.tag) return QString::compare(a.tag, b.tag, Qt::CaseInsensitive) < 0;
        return a.kind < b.kind;
    });
    return rows;
}

QString jsonString(const QJsonObject &obj, const QString &key, const QString &fallback = QString())
{
    const QString value = obj.value(key).toString();
    return value.isEmpty() ? fallback : value;
}

QString loadToolPageStyle()
{
    QFile file(":/styles/toolpage.qss");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(file.readAll());
}

QJsonObject hashToObject(const QHash<QString, QString> &hash)
{
    QJsonObject obj;
    for (auto it = hash.constBegin(); it != hash.constEnd(); ++it) {
        obj.insert(it.key(), it.value());
    }
    return obj;
}

QHash<QString, QString> objectToHash(const QJsonObject &obj)
{
    QHash<QString, QString> hash;
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        hash.insert(it.key(), it.value().toString());
    }
    return hash;
}

} // namespace

PromptTemplateLibraryWidget::PromptTemplateLibraryWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::PromptTemplateLibraryWidget)
{
    ui->setupUi(this);
    setStyleSheet(loadToolPageStyle());

    ui->splitterGenerate->setSizes({360, 760});
    ui->tableTemplateDefaults->horizontalHeader()->setStretchLastSection(true);
    ui->tableTemplateDefaults->verticalHeader()->hide();
    ui->tableTemplateDefaults->setShowGrid(false);
    ui->tableTemplateDefaults->setFocusPolicy(Qt::NoFocus);
    ui->tableTemplateDefaults->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableTemplateDefaults->setToolTip("当前模板对每个全局占位符的默认值，可按模板单独覆盖。");
    ui->tablePlaceholders->horizontalHeader()->setStretchLastSection(true);
    ui->tablePlaceholders->verticalHeader()->hide();
    ui->tablePlaceholders->setShowGrid(false);
    ui->tablePlaceholders->setFocusPolicy(Qt::NoFocus);
    ui->tablePlaceholders->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tablePlaceholders->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->tablePlaceholders->setToolTip("全局占位符定义，例如名称、显示名、文本/单选/多选类型。");
    ui->tablePlaceholderOptions->setColumnCount(1);
    ui->tablePlaceholderOptions->setHorizontalHeaderLabels({"选项"});
    ui->tablePlaceholderOptions->horizontalHeader()->setStretchLastSection(true);
    ui->tablePlaceholderOptions->verticalHeader()->hide();
    ui->tablePlaceholderOptions->setShowGrid(false);
    ui->tablePlaceholderOptions->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tablePlaceholderOptions->setSelectionMode(QAbstractItemView::SingleSelection);
    ui->tablePlaceholderOptions->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->tablePlaceholderOptions->setToolTip("单选/多选的候选项。选项内容可以包含换行，也可以为空。");
    m_generateTagPicker = { ui->tabGenerateTagPicker,
                            ui->editTagSearch,
                            ui->comboTagScope,
                            ui->btnRefreshTags,
                            ui->btnInsertTagsPositive,
                            ui->btnInsertTagsNegative,
                            ui->tableTagPicker,
                            ui->lblTagPickerStatus,
                            false };
    setupTagPickerUi(m_generateTagPicker);

    m_templateTagPicker = {
        ui->tabTemplateTagPicker,
        ui->editTemplateTagSearch,
        ui->comboTemplateTagScope,
        ui->btnRefreshTemplateTags,
        ui->btnInsertTemplateTagsPositive,
        ui->btnInsertTemplateTagsNegative,
        ui->tableTemplateTagPicker,
        ui->lblTemplateTagPickerStatus,
        true
    };

    setupTagPickerUi(m_templateTagPicker);
    m_generateModelTriggerPicker = {
        ui->tabGenerateModelTriggers,
        ui->editGenerateModelTriggerSearch,
        ui->btnAddGenerateModelTriggersPositive,
        ui->treeGenerateModelTriggers,
        ui->lblGenerateModelTriggerStatus,
        false
    };
    m_templateModelTriggerPicker = {
        ui->tabTemplateModelTriggers,
        ui->editTemplateModelTriggerSearch,
        ui->btnAddTemplateModelTriggersPositive,
        ui->treeTemplateModelTriggers,
        ui->lblTemplateModelTriggerStatus,
        true
    };
    setupModelTriggerPickerUi(m_generateModelTriggerPicker);
    setupModelTriggerPickerUi(m_templateModelTriggerPicker);
    ui->scrollFavorites->setFrameShape(QFrame::NoFrame);
    ui->favoritesContainer->setStyleSheet("background:transparent;");
    applyUnifiedTableRowStyle(this);
    ui->splitterTemplateManage->setSizes({240, 520, 520});
    ui->splitterGenerate->setSizes({560, 620});
    ui->splitterGenerateImageTags->setSizes({260, 220});
    ui->editGenerateImagePath->setAcceptDrops(true);
    ui->editGenerateImagePath->installEventFilter(this);
    ui->editTemplateImagePath->setAcceptDrops(true);
    ui->editTemplateImagePath->installEventFilter(this);

    m_generateImagePositiveTags = new TagFlowWidget(ui->generateImagePositiveTagsContainer);
    m_generateImageNegativeTags = new TagFlowWidget(ui->generateImageNegativeTagsContainer);
    m_generateImagePositiveTags->setTranslationMap(m_translationMap);
    m_generateImageNegativeTags->setTranslationMap(m_translationMap);
    ui->layoutGenerateImagePositiveTags->addWidget(m_generateImagePositiveTags);
    ui->layoutGenerateImageNegativeTags->addWidget(m_generateImageNegativeTags);
    m_templateImagePositiveTags = new TagFlowWidget(ui->templateImagePositiveTagsContainer);
    m_templateImageNegativeTags = new TagFlowWidget(ui->templateImageNegativeTagsContainer);
    m_templateImagePositiveTags->setTranslationMap(m_translationMap);
    m_templateImageNegativeTags->setTranslationMap(m_translationMap);
    ui->layoutTemplateImagePositiveTags->addWidget(m_templateImagePositiveTags);
    ui->layoutTemplateImageNegativeTags->addWidget(m_templateImageNegativeTags);

    // 为四个提示词输入框添加“标签视图”切换与自动补全。
    for (QPlainTextEdit *edit : {ui->textGeneratedPositive, ui->textGeneratedNegative,
                                 ui->textTemplatePositive, ui->textTemplateNegative}) {
        setupPromptTagFlowView(edit);
        setupAutocompleteForEditor(edit);
    }
    // 占位选项内容编辑框也启用自动补全（无需标签视图）。
    setupAutocompleteForEditor(ui->editPlaceholderOptionValue);

    // 设置页：每个输入框单独的自动补全开关。
    m_autocompleteToggles = {
        { ui->textGeneratedPositive,      ui->chkAcGeneratePositive, "autocomplete_enable_generate_positive" },
        { ui->textGeneratedNegative,      ui->chkAcGenerateNegative, "autocomplete_enable_generate_negative" },
        { ui->textTemplatePositive,       ui->chkAcTemplatePositive, "autocomplete_enable_template_positive" },
        { ui->textTemplateNegative,       ui->chkAcTemplateNegative, "autocomplete_enable_template_negative" },
        { ui->editPlaceholderOptionValue, ui->chkAcOptionValue,      "autocomplete_enable_option_value" },
    };
    for (const AutocompleteToggle &toggle : std::as_const(m_autocompleteToggles)) {
        m_autocompleteEnabledByEdit.insert(toggle.edit, true);
        QPlainTextEdit *edit = toggle.edit;
        connect(toggle.check, &QCheckBox::toggled, this, [this, edit](bool on) {
            m_autocompleteEnabledByEdit[edit] = on;
            if (!on) hideAutocompletePopup();
            saveAutocompleteSettings();
        });
    }
    connect(ui->chkAddLoraTagWithTrigger, &QCheckBox::toggled, this, [this](bool on) {
        m_addLoraTagWithTrigger = on;
        saveAutocompleteSettings();
    });

    loadAutocompleteSettings();
    connect(ui->spinAutocompleteLimit, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_autocompleteLimit = qBound(1, value, 50);
        saveAutocompleteSettings();
    });

    connect(ui->tabTemplateLibrary, &QTabWidget::currentChanged, this, &PromptTemplateLibraryWidget::onTabChanged);
    connect(ui->comboGenerateTemplate, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &PromptTemplateLibraryWidget::onGenerateTemplateChanged);
    connect(ui->btnReloadLibrary, &QPushButton::clicked, this, &PromptTemplateLibraryWidget::reloadTemplateLibrary);
    connect(ui->btnCopyPositive, &QPushButton::clicked, this, [this]() { copyText(ui->textGeneratedPositive->toPlainText()); });
    connect(ui->btnCopyNegative, &QPushButton::clicked, this, [this]() { copyText(ui->textGeneratedNegative->toPlainText()); });
    connect(ui->btnCopyAll, &QPushButton::clicked, this, [this]() {
        copyText("Positive prompt:\n" + ui->textGeneratedPositive->toPlainText() + "\n\nNegative prompt:\n" + ui->textGeneratedNegative->toPlainText());
    });
    connect(ui->btnClearGenerate, &QPushButton::clicked, this, [this]() {
        m_loadingUi = true;
        for (QWidget *editor : std::as_const(m_placeholderEditors)) {
            if (auto *line = qobject_cast<QLineEdit*>(editor)) line->clear();
            else if (auto *combo = qobject_cast<QComboBox*>(editor)) combo->setCurrentIndex(-1);
            else if (auto *container = qobject_cast<QWidget*>(editor)) {
                const auto boxes = container->findChildren<QCheckBox*>();
                for (QCheckBox *box : boxes) box->setChecked(false);
            }
        }
        ui->textGeneratedPositive->clear();
        ui->textGeneratedNegative->clear();
        m_lastRenderedPositivePrompt.clear();
        m_lastRenderedNegativePrompt.clear();
        m_loadingUi = false;
        updateGeneratedPrompt();
    });
    connect(ui->btnAddPromptFavorite, &QPushButton::clicked, this, &PromptTemplateLibraryWidget::addCurrentPromptToFavorites);

    connect(ui->listTemplates, &QListWidget::currentRowChanged, this, &PromptTemplateLibraryWidget::onTemplateListCurrentRowChanged);
    connect(ui->btnSaveTemplate, &QPushButton::clicked, this, &PromptTemplateLibraryWidget::onSaveTemplateClicked);
    connect(ui->btnNewTemplate, &QPushButton::clicked, this, &PromptTemplateLibraryWidget::onNewTemplateClicked);
    connect(ui->btnDuplicateTemplate, &QPushButton::clicked, this, &PromptTemplateLibraryWidget::onDuplicateTemplateClicked);
    connect(ui->btnDeleteTemplate, &QPushButton::clicked, this, &PromptTemplateLibraryWidget::onDeleteTemplateClicked);

    connect(ui->tablePlaceholders, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) {
                if (row >= 0) updatePlaceholderEditorFromSelection();
            });
    connect(ui->btnSavePlaceholder, &QPushButton::clicked, this, &PromptTemplateLibraryWidget::onSavePlaceholderClicked);
    connect(ui->btnNewPlaceholder, &QPushButton::clicked, this, &PromptTemplateLibraryWidget::onNewPlaceholderClicked);
    connect(ui->btnDeletePlaceholder, &QPushButton::clicked, this, &PromptTemplateLibraryWidget::onDeletePlaceholderClicked);

    connect(ui->tabTemplateManageSide, &QTabWidget::currentChanged, this, [this](int index) {
        if (ui->tabTemplateManageSide->widget(index) == m_templateTagPicker.page) loadTagPickerRows(m_templateTagPicker, false);
        if (ui->tabTemplateManageSide->widget(index) == m_templateModelTriggerPicker.page
            && (m_modelTriggerRowsDirty || m_templateModelTriggerPicker.tree->topLevelItemCount() == 0)) {
            if (!m_modelTriggerRowsLoaded) emit modelTriggerRowsRequested();
            else refreshModelTriggerPickerTable(m_templateModelTriggerPicker);
        }
    });
    connect(ui->tabGenerateTools, &QTabWidget::currentChanged, this, [this](int index) {
        if (ui->tabGenerateTools->widget(index) == ui->tabGenerateTagPicker) loadTagPickerRows(m_generateTagPicker, false);
        if (ui->tabGenerateTools->widget(index) == m_generateModelTriggerPicker.page
            && (m_modelTriggerRowsDirty || m_generateModelTriggerPicker.tree->topLevelItemCount() == 0)) {
            if (!m_modelTriggerRowsLoaded) emit modelTriggerRowsRequested();
            else refreshModelTriggerPickerTable(m_generateModelTriggerPicker);
        }
        if (ui->tabGenerateTools->widget(index) == ui->tabGenerateFavorites) refreshFavoritesTable();
    });
    connect(ui->btnChooseGenerateImage, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, "选择图片", QString(), "Images (*.png *.jpg *.jpeg *.webp)");
        if (path.isEmpty()) return;
        ui->editGenerateImagePath->setText(path);
        parseGenerateImageTags();
    });
    connect(ui->btnParseGenerateImage, &QPushButton::clicked, this, &PromptTemplateLibraryWidget::parseGenerateImageTags);
    connect(ui->chkGenerateImageTranslation, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_generateImagePositiveTags) m_generateImagePositiveTags->setShowTranslation(checked);
        if (m_generateImageNegativeTags) m_generateImageNegativeTags->setShowTranslation(checked);
    });
    connect(ui->btnSelectAllGenerateImageTags, &QPushButton::clicked, this, [this]() {
        if (m_generateImagePositiveTags) m_generateImagePositiveTags->selectAllVisibleTags();
        if (m_generateImageNegativeTags) m_generateImageNegativeTags->selectAllVisibleTags();
    });
    connect(ui->btnClearGenerateImageTags, &QPushButton::clicked, this, [this]() {
        if (m_generateImagePositiveTags) m_generateImagePositiveTags->clearSelectedTags();
        if (m_generateImageNegativeTags) m_generateImageNegativeTags->clearSelectedTags();
    });
    connect(ui->btnAddGenerateImagePositive, &QPushButton::clicked, this, [this]() { addGenerateImageTagsToPrompt(true); });
    connect(ui->btnAddGenerateImageNegative, &QPushButton::clicked, this, [this]() { addGenerateImageTagsToPrompt(false); });
    connect(ui->btnChooseTemplateImage, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, "选择图片", QString(), "Images (*.png *.jpg *.jpeg *.webp)");
        if (path.isEmpty()) return;
        ui->editTemplateImagePath->setText(path);
        parseTemplateImageTags();
    });
    connect(ui->btnParseTemplateImage, &QPushButton::clicked, this, &PromptTemplateLibraryWidget::parseTemplateImageTags);
    connect(ui->chkTemplateImageTranslation, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_templateImagePositiveTags) m_templateImagePositiveTags->setShowTranslation(checked);
        if (m_templateImageNegativeTags) m_templateImageNegativeTags->setShowTranslation(checked);
    });
    connect(ui->btnSelectAllTemplateImageTags, &QPushButton::clicked, this, [this]() {
        if (m_templateImagePositiveTags) m_templateImagePositiveTags->selectAllVisibleTags();
        if (m_templateImageNegativeTags) m_templateImageNegativeTags->selectAllVisibleTags();
    });
    connect(ui->btnClearTemplateImageTags, &QPushButton::clicked, this, [this]() {
        if (m_templateImagePositiveTags) m_templateImagePositiveTags->clearSelectedTags();
        if (m_templateImageNegativeTags) m_templateImageNegativeTags->clearSelectedTags();
    });
    connect(ui->btnAddTemplateImagePositive, &QPushButton::clicked, this, [this]() { addTemplateImageTagsToTemplate(true); });
    connect(ui->btnAddTemplateImageNegative, &QPushButton::clicked, this, [this]() { addTemplateImageTagsToTemplate(false); });
    connect(ui->tablePlaceholderOptions, &QTableWidget::currentCellChanged, this,
            [this](int, int, int, int) {
                updatePlaceholderOptionEditorFromSelection();
                updatePlaceholderOptionControls();
            });

    connect(ui->comboPlaceholderType, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this]() {
                updatePlaceholderOptionControls();
            });

    connect(ui->btnAddPlaceholderOption, &QPushButton::clicked,
            this, &PromptTemplateLibraryWidget::addPlaceholderOptionFromEditor);

    connect(ui->btnUpdatePlaceholderOption, &QPushButton::clicked,
            this, &PromptTemplateLibraryWidget::updateSelectedPlaceholderOptionFromEditor);

    connect(ui->btnDeletePlaceholderOption, &QPushButton::clicked,
            this, &PromptTemplateLibraryWidget::deleteSelectedPlaceholderOption);

    connect(ui->btnMovePlaceholderOptionUp, &QPushButton::clicked,
            this, &PromptTemplateLibraryWidget::moveSelectedPlaceholderOptionUp);

    connect(ui->btnMovePlaceholderOptionDown, &QPushButton::clicked,
            this, &PromptTemplateLibraryWidget::moveSelectedPlaceholderOptionDown);

    loadLibrary();
}

PromptTemplateLibraryWidget::~PromptTemplateLibraryWidget()
{
    if (m_tagWatcher) {
        m_tagWatcher->disconnect(this);
        m_tagWatcher->cancel();
        m_tagWatcher->waitForFinished();
        delete m_tagWatcher;
        m_tagWatcher = nullptr;
    }
    delete ui;
}

bool PromptTemplateLibraryWidget::eventFilter(QObject *watched, QEvent *event)
{
    // 点击收藏卡片头部展开/收起详情（按钮自身的点击不会到达头部）。
    if (event->type() == QEvent::MouseButtonPress) {
        if (QWidget *detail = m_favoriteHeaderToDetail.value(watched, nullptr)) {
            if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
                detail->setVisible(!detail->isVisible());
                return true;
            }
        }
    }

    if (auto *edit = qobject_cast<QPlainTextEdit*>(watched)) {
        if (event->type() == QEvent::KeyPress) {
            if (m_autocompleteEdit == edit && handleAutocompleteKeyPress(static_cast<QKeyEvent*>(event))) {
                return true;
            }
        } else if (event->type() == QEvent::FocusOut) {
            if (m_autocompleteEdit == edit) hideAutocompletePopup();
        }
        return QWidget::eventFilter(watched, event);
    }

    const bool isGenerateImagePath = watched == ui->editGenerateImagePath;
    const bool isTemplateImagePath = watched == ui->editTemplateImagePath;
    if (!isGenerateImagePath && !isTemplateImagePath) {
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::DragEnter) {
        auto *dragEvent = static_cast<QDragEnterEvent*>(event);
        if (!imagePathFromMimeData(dragEvent->mimeData()).isEmpty()) {
            dragEvent->acceptProposedAction();
            return true;
        }
    } else if (event->type() == QEvent::Drop) {
        auto *dropEvent = static_cast<QDropEvent*>(event);
        const QString path = imagePathFromMimeData(dropEvent->mimeData());
        if (!path.isEmpty()) {
            if (isGenerateImagePath) {
                ui->editGenerateImagePath->setText(path);
                parseGenerateImageTags();
            } else {
                ui->editTemplateImagePath->setText(path);
                parseTemplateImageTags();
            }
            dropEvent->acceptProposedAction();
            return true;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void PromptTemplateLibraryWidget::setTranslationMap(const QHash<QString, QString> *map)
{
    m_translationMap = map;
    if (m_generateImagePositiveTags) m_generateImagePositiveTags->setTranslationMap(map);
    if (m_generateImageNegativeTags) m_generateImageNegativeTags->setTranslationMap(map);
    if (m_templateImagePositiveTags) m_templateImagePositiveTags->setTranslationMap(map);
    if (m_templateImageNegativeTags) m_templateImageNegativeTags->setTranslationMap(map);
    for (PromptTagFlowView &view : m_tagFlowViews) {
        if (view.flow) view.flow->setTranslationMap(map);
        if (view.tagViewActive) refreshPromptTagFlowFromText(view);
    }
    rebuildAutocompleteIndex();
    refreshTagPickerTable(m_generateTagPicker);
    refreshTagPickerTable(m_templateTagPicker);
}

void PromptTemplateLibraryWidget::setModelTriggerRows(const QVector<ModelTriggerRow> &rows)
{
    m_modelTriggerRows = rows;
    m_modelTriggerRowsDirty = true;
    m_modelTriggerRowsLoaded = true;
    if (ui->tabGenerateTools->currentWidget() == m_generateModelTriggerPicker.page) {
        refreshModelTriggerPickerTable(m_generateModelTriggerPicker);
    }
    if (ui->tabTemplateManageSide->currentWidget() == m_templateModelTriggerPicker.page) {
        refreshModelTriggerPickerTable(m_templateModelTriggerPicker);
    }
}

void PromptTemplateLibraryWidget::reloadTemplateLibrary()
{
    loadLibrary();
    setStatus("模板库已重新加载");
}

QString PromptTemplateLibraryWidget::libraryPath() const
{
    return qApp->applicationDirPath() + "/config/prompt_templates.json";
}

QString PromptTemplateLibraryWidget::ensureId(const QString &prefix) const
{
    return prefix + "_" + QString::number(QDateTime::currentMSecsSinceEpoch(), 36);
}

QString PromptTemplateLibraryWidget::typeToString(PlaceholderType type) const
{
    if (type == PlaceholderType::SingleChoice) return "single";
    if (type == PlaceholderType::MultiChoice) return "multi";
    return "text";
}

PromptTemplateLibraryWidget::PlaceholderType PromptTemplateLibraryWidget::typeFromString(const QString &text) const
{
    if (text == "single" || text == "单选") return PlaceholderType::SingleChoice;
    if (text == "multi" || text == "多选") return PlaceholderType::MultiChoice;
    return PlaceholderType::Text;
}

QString PromptTemplateLibraryWidget::translatedTextForTag(const QString &tag) const
{
    if (!m_translationMap) return QString();
    QString translated = m_translationMap->value(tag);
    if (translated.isEmpty() && tag.contains(' ')) {
        QString key = tag;
        key.replace(' ', '_');
        translated = m_translationMap->value(key);
    }
    if (translated.isEmpty() && tag.contains('_')) {
        QString key = tag;
        key.replace('_', ' ');
        translated = m_translationMap->value(key);
    }
    return translated;
}

void PromptTemplateLibraryWidget::loadLibrary()
{
    m_loadingUi = true;
    m_templates.clear();
    m_placeholders.clear();
    m_imageTemplates.clear();
    m_favorites.clear();

    QFile file(libraryPath());
    if (!file.open(QIODevice::ReadOnly)) {
        createDefaultLibrary();
        saveLibrary();
        refreshAllLists();
        m_loadingUi = false;
        updateGeneratedPrompt();
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonArray placeholderArray = root["placeholders"].toArray();
    for (const QJsonValue &value : placeholderArray) {
        const QJsonObject obj = value.toObject();
        PromptPlaceholder item;
        item.id = jsonString(obj, "id", ensureId("placeholder"));
        item.name = obj["name"].toString().trimmed();
        item.label = jsonString(obj, "label", item.name);
        item.type = typeFromString(obj["type"].toString());
        item.defaultValue = obj["default"].toString();
        for (const QJsonValue &option : obj["options"].toArray()) item.options << option.toString();
        if (!item.name.isEmpty()) m_placeholders.append(item);
    }

    const QJsonArray templateArray = root["templates"].toArray();
    for (const QJsonValue &value : templateArray) {
        const QJsonObject obj = value.toObject();
        PromptTemplate item;
        item.id = jsonString(obj, "id", ensureId("template"));
        item.name = jsonString(obj, "name", "Untitled Template");
        item.category = obj["category"].toString();
        item.positiveTemplate = obj["positive"].toString();
        item.negativeTemplate = obj["negative"].toString();
        item.notes = obj["notes"].toString();
        item.placeholderDefaults = objectToHash(obj["placeholderDefaults"].toObject());
        m_templates.append(item);
    }

    const QJsonArray imageArray = root["image_extract_templates"].toArray();
    for (const QJsonValue &value : imageArray) {
        const QJsonObject obj = value.toObject();
        ImageExtractTemplate item;
        item.id = jsonString(obj, "id", ensureId("image_template"));
        item.name = jsonString(obj, "name", "Image Extract Template");
        item.positiveTemplate = obj["positive"].toString();
        item.negativeTemplate = obj["negative"].toString();
        item.notes = obj["notes"].toString();
        m_imageTemplates.append(item);
    }

    const QJsonArray favoriteArray = root["favorites"].toArray();
    for (const QJsonValue &value : favoriteArray) {
        const QJsonObject obj = value.toObject();
        PromptFavorite item;
        item.id = jsonString(obj, "id", ensureId("favorite"));
        item.name = jsonString(obj, "name", "Favorite Prompt");
        item.positive = obj["positive"].toString();
        item.negative = obj["negative"].toString();
        item.createdAt = obj["createdAt"].toString();
        item.updatedAt = obj["updatedAt"].toString(item.createdAt);
        if (!item.positive.trimmed().isEmpty() || !item.negative.trimmed().isEmpty()) {
            m_favorites.append(item);
        }
    }

    if (m_templates.isEmpty() || m_placeholders.isEmpty() || m_imageTemplates.isEmpty()) {
        const QVector<PromptTemplate> loadedTemplates = m_templates;
        const QVector<PromptPlaceholder> loadedPlaceholders = m_placeholders;
        const QVector<ImageExtractTemplate> loadedImageTemplates = m_imageTemplates;
        createDefaultLibrary();
        if (!loadedTemplates.isEmpty()) m_templates = loadedTemplates;
        if (!loadedPlaceholders.isEmpty()) m_placeholders = loadedPlaceholders;
        if (!loadedImageTemplates.isEmpty()) m_imageTemplates = loadedImageTemplates;
    }
    refreshAllLists();
    refreshFavoritesTable();
    m_loadingUi = false;
    updateGeneratedPrompt();
}

void PromptTemplateLibraryWidget::saveLibrary()
{
    QDir().mkpath(qApp->applicationDirPath() + "/config");

    QJsonArray placeholderArray;
    for (const PromptPlaceholder &item : std::as_const(m_placeholders)) {
        QJsonObject obj;
        obj["id"] = item.id;
        obj["name"] = item.name;
        obj["label"] = item.label;
        obj["type"] = typeToString(item.type);
        obj["default"] = item.defaultValue;
        QJsonArray options;
        for (const QString &option : item.options) options.append(option);
        obj["options"] = options;
        placeholderArray.append(obj);
    }

    QJsonArray templateArray;
    for (const PromptTemplate &item : std::as_const(m_templates)) {
        QJsonObject obj;
        obj["id"] = item.id;
        obj["name"] = item.name;
        obj["category"] = item.category;
        obj["positive"] = item.positiveTemplate;
        obj["negative"] = item.negativeTemplate;
        obj["notes"] = item.notes;
        obj["placeholderDefaults"] = hashToObject(item.placeholderDefaults);
        templateArray.append(obj);
    }

    QJsonArray imageArray;
    for (const ImageExtractTemplate &item : std::as_const(m_imageTemplates)) {
        QJsonObject obj;
        obj["id"] = item.id;
        obj["name"] = item.name;
        obj["positive"] = item.positiveTemplate;
        obj["negative"] = item.negativeTemplate;
        obj["notes"] = item.notes;
        imageArray.append(obj);
    }

    QJsonArray favoriteArray;
    for (const PromptFavorite &item : std::as_const(m_favorites)) {
        QJsonObject obj;
        obj["id"] = item.id;
        obj["name"] = item.name;
        obj["positive"] = item.positive;
        obj["negative"] = item.negative;
        obj["createdAt"] = item.createdAt;
        obj["updatedAt"] = item.updatedAt;
        favoriteArray.append(obj);
    }

    QJsonObject root;
    root["version"] = 1;
    root["templates"] = templateArray;
    root["placeholders"] = placeholderArray;
    root["image_extract_templates"] = imageArray;
    root["favorites"] = favoriteArray;

    QFile file(libraryPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, "保存失败", "无法写入 config/prompt_templates.json。");
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    m_dirty = false;
}

void PromptTemplateLibraryWidget::createDefaultLibrary()
{
    m_placeholders.clear();
    m_templates.clear();
    m_imageTemplates.clear();
    m_favorites.clear();

    m_placeholders.append({ensureId("placeholder"), "quality", "质量词", PlaceholderType::MultiChoice,
                           "masterpiece, best quality", {"masterpiece", "best quality", "very aesthetic", "absurdres"}});
    m_placeholders.append({ensureId("placeholder"), "subject", "主体", PlaceholderType::Text,
                           "1girl", {}});
    m_placeholders.append({ensureId("placeholder"), "style", "风格", PlaceholderType::SingleChoice,
                           "anime illustration", {"anime illustration", "game cg", "official art", "watercolor"}});
    m_placeholders.append({ensureId("placeholder"), "negative_extra", "额外负面词", PlaceholderType::Text,
                           "bad hands, text, watermark", {}});

    PromptTemplate base;
    base.id = ensureId("template");
    base.name = "通用二次元模板";
    base.category = "General";
    base.positiveTemplate = "{quality}, {subject}, {style}";
    base.negativeTemplate = "low quality, worst quality, {negative_extra}";
    base.notes = "内置示例模板，可复制后按自己的习惯修改。";
    m_templates.append(base);

    ImageExtractTemplate imageTemplate;
    imageTemplate.id = ensureId("image_template");
    imageTemplate.name = "保留图片提示词";
    imageTemplate.positiveTemplate = "{image_positive}";
    imageTemplate.negativeTemplate = "{image_negative}";
    imageTemplate.notes = "从图片元数据中提取正负提示词。";
    m_imageTemplates.append(imageTemplate);
}

void PromptTemplateLibraryWidget::refreshAllLists()
{
    refreshGenerateTemplateCombo();
    refreshTemplateList();
    refreshPlaceholderTable();
    refreshFavoritesTable();
    rebuildPlaceholderInputs();
    updateGeneratedPrompt();
}

void PromptTemplateLibraryWidget::refreshFavoritesTable()
{
    if (!ui->layoutFavorites) return;
    m_favoriteHeaderToDetail.clear();
    while (QLayoutItem *li = ui->layoutFavorites->takeAt(0)) {
        if (QWidget *w = li->widget()) w->deleteLater();
        delete li;
    }

    for (const PromptFavorite &fav : std::as_const(m_favorites)) {
        auto *card = new QFrame(ui->favoritesContainer);
        card->setObjectName("favoriteCard");
        card->setStyleSheet(
            "QFrame#favoriteCard{background:#1f2833;border:1px solid #31363d;border-radius:6px;}"
            "QFrame#favoriteCard QLabel{background:transparent;}");
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(0, 0, 0, 0);
        cardLayout->setSpacing(0);

        // 头部（可点击展开/收起）：第一行名称，第二行左侧时间、右侧三个按钮。
        auto *header = new QWidget(card);
        header->setObjectName("favoriteHeader");
        header->setCursor(Qt::PointingHandCursor);
        auto *headerLayout = new QVBoxLayout(header);
        headerLayout->setContentsMargins(10, 8, 10, 8);
        headerLayout->setSpacing(4);

        auto *nameLabel = new QLabel(fav.name, header);
        nameLabel->setStyleSheet("font-weight:bold;color:#dcdedf;");
        nameLabel->setToolTip(fav.name);
        nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        headerLayout->addWidget(nameLabel);

        auto *line2 = new QHBoxLayout();
        line2->setContentsMargins(0, 0, 0, 0);
        line2->setSpacing(6);
        auto *timeLabel = new QLabel(formatFavoriteTime(fav.updatedAt), header);
        timeLabel->setStyleSheet("color:#8c96a0;");
        timeLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        line2->addWidget(timeLabel);
        line2->addStretch(1);

        const QString id = fav.id;
        auto addBtn = [&](const QString &text, auto cb) {
            auto *b = new QPushButton(text, header);
            b->setCursor(Qt::PointingHandCursor);
            b->setFocusPolicy(Qt::NoFocus);
            b->setFixedHeight(24);
            b->setStyleSheet("QPushButton{padding:1px 12px;}");
            connect(b, &QPushButton::clicked, this, [this, id, cb]() { (this->*cb)(id); });
            line2->addWidget(b);
        };
        addBtn("复制", &PromptTemplateLibraryWidget::copyFavoriteById);
        addBtn("替换", &PromptTemplateLibraryWidget::replacePromptFromFavoriteById);
        addBtn("删除", &PromptTemplateLibraryWidget::deleteFavoriteById);
        headerLayout->addLayout(line2);
        cardLayout->addWidget(header);

        // 详情（默认隐藏）：正/负面提示词全文。
        auto *detail = new QFrame(card);
        detail->setObjectName("favoriteDetail");
        detail->setStyleSheet(
            "QFrame#favoriteDetail{background:#16191e;border:none;border-top:1px solid #31363d;"
            "border-bottom-left-radius:5px;border-bottom-right-radius:5px;}"
            "QFrame#favoriteDetail QLabel{background:transparent;}");
        auto *detailLayout = new QVBoxLayout(detail);
        detailLayout->setContentsMargins(12, 8, 12, 10);
        detailLayout->setSpacing(4);
        auto addSection = [&](const QString &title, const QString &text, const QString &color) {
            auto *t = new QLabel(title, detail);
            t->setStyleSheet(QString("color:%1;font-weight:bold;").arg(color));
            detailLayout->addWidget(t);
            auto *body = new QLabel(text.trimmed().isEmpty() ? "（空）" : text, detail);
            body->setWordWrap(true);
            body->setTextInteractionFlags(Qt::TextSelectableByMouse);
            body->setStyleSheet("color:#dcdedf;");
            detailLayout->addWidget(body);
        };
        addSection("正面", fav.positive, "#5fd38d");
        addSection("负面", fav.negative, "#ff6b6b");
        detail->setVisible(false);
        cardLayout->addWidget(detail);

        ui->layoutFavorites->addWidget(card);

        // 点击头部切换详情显示（按钮的点击不会到达头部）。
        header->installEventFilter(this);
        m_favoriteHeaderToDetail.insert(header, detail);
    }

    ui->layoutFavorites->addStretch(1);

    ui->lblPromptFavoritesStatus->setText(m_favorites.isEmpty()
        ? "暂无收藏。可在右侧当前提示词区域点击“添加至收藏”。"
        : QString("共 %1 条收藏。").arg(m_favorites.size()));
}

void PromptTemplateLibraryWidget::refreshGenerateTemplateCombo()
{
    QSignalBlocker blocker(ui->comboGenerateTemplate);
    const QString previous = ui->comboGenerateTemplate->currentData().toString();
    ui->comboGenerateTemplate->clear();
    for (const PromptTemplate &item : std::as_const(m_templates)) {
        const QString label = item.category.isEmpty() ? item.name : QString("%1 / %2").arg(item.category, item.name);
        ui->comboGenerateTemplate->addItem(label, item.id);
    }
    const int index = ui->comboGenerateTemplate->findData(previous);
    ui->comboGenerateTemplate->setCurrentIndex(index >= 0 ? index : 0);
}

void PromptTemplateLibraryWidget::refreshTemplateList()
{
    QSignalBlocker blocker(ui->listTemplates);
    const QString previous = ui->listTemplates->currentItem() ? ui->listTemplates->currentItem()->data(Qt::UserRole).toString() : QString();
    ui->listTemplates->clear();
    for (const PromptTemplate &item : std::as_const(m_templates)) {
        auto *listItem = new QListWidgetItem(item.category.isEmpty() ? item.name : QString("[%1] %2").arg(item.category, item.name));
        listItem->setData(Qt::UserRole, item.id);
        ui->listTemplates->addItem(listItem);
    }
    const int previousRow = [&]() {
        for (int i = 0; i < ui->listTemplates->count(); ++i) {
            if (ui->listTemplates->item(i)->data(Qt::UserRole).toString() == previous) return i;
        }
        return 0;
    }();
    if (ui->listTemplates->count() > 0) ui->listTemplates->setCurrentRow(previousRow);
    updateTemplateEditorFromSelection();
}

void PromptTemplateLibraryWidget::refreshPlaceholderTable()
{
    const QString previousName =
        ui->tablePlaceholders->currentRow() >= 0 && ui->tablePlaceholders->currentRow() < m_placeholders.size()
            ? m_placeholders.at(ui->tablePlaceholders->currentRow()).name
            : ui->editPlaceholderName->text().trimmed();

    QSignalBlocker blocker(ui->tablePlaceholders);

    ui->tablePlaceholders->setRowCount(0);

    for (const PromptPlaceholder &item : std::as_const(m_placeholders)) {
        const int row = ui->tablePlaceholders->rowCount();
        ui->tablePlaceholders->insertRow(row);
        ui->tablePlaceholders->setItem(row, 0, new QTableWidgetItem(item.name));
        ui->tablePlaceholders->setItem(row, 1, new QTableWidgetItem(item.label));
        ui->tablePlaceholders->setItem(row, 2, new QTableWidgetItem(typeToString(item.type)));
    }

    QHeaderView *header = ui->tablePlaceholders->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(0, QHeaderView::Interactive);
    header->setSectionResizeMode(1, QHeaderView::Interactive);
    header->setSectionResizeMode(2, QHeaderView::Stretch);
    header->resizeSection(0, 150);
    header->resizeSection(1, 150);

    int nextRow = -1;
    if (!previousName.isEmpty()) {
        nextRow = placeholderIndexByName(previousName);
    }
    if (nextRow < 0 && ui->tablePlaceholders->rowCount() > 0) {
        nextRow = qMin(ui->tablePlaceholders->rowCount() - 1, qMax(0, ui->tablePlaceholders->currentRow()));
    }

    if (nextRow >= 0) {
        ui->tablePlaceholders->setCurrentCell(nextRow, 0);
        ui->tablePlaceholders->selectRow(nextRow);
    }

    updatePlaceholderEditorFromSelection();
}

void PromptTemplateLibraryWidget::refreshPlaceholderTableKeepingName(const QString &name)
{
    QSignalBlocker blocker(ui->tablePlaceholders);

    ui->tablePlaceholders->setRowCount(0);

    for (const PromptPlaceholder &item : std::as_const(m_placeholders)) {
        const int row = ui->tablePlaceholders->rowCount();
        ui->tablePlaceholders->insertRow(row);
        ui->tablePlaceholders->setItem(row, 0, new QTableWidgetItem(item.name));
        ui->tablePlaceholders->setItem(row, 1, new QTableWidgetItem(item.label));
        ui->tablePlaceholders->setItem(row, 2, new QTableWidgetItem(typeToString(item.type)));
    }

    QHeaderView *header = ui->tablePlaceholders->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(0, QHeaderView::Interactive);
    header->setSectionResizeMode(1, QHeaderView::Interactive);
    header->setSectionResizeMode(2, QHeaderView::Stretch);
    header->resizeSection(0, 150);
    header->resizeSection(1, 150);

    int row = placeholderIndexByName(name);
    if (row < 0 && ui->tablePlaceholders->rowCount() > 0) row = 0;

    if (row >= 0) {
        ui->tablePlaceholders->setCurrentCell(row, 0);
        ui->tablePlaceholders->selectRow(row);
    }

    updatePlaceholderEditorFromSelection();
}

void PromptTemplateLibraryWidget::rebuildPlaceholderInputs()
{
    while (QLayoutItem *item = ui->layoutPlaceholderInputs->takeAt(0)) {
        if (QWidget *widget = item->widget()) widget->deleteLater();
        delete item;
    }
    m_placeholderEditors.clear();

    const int templateIndex = templateIndexById(selectedTemplateId());
    if (templateIndex < 0) {
        ui->layoutPlaceholderInputs->addStretch(1);
        return;
    }

    const PromptTemplate &currentTemplate = m_templates.at(templateIndex);
    const QStringList usedNames = placeholderNamesInTemplateText(
        currentTemplate.positiveTemplate,
        currentTemplate.negativeTemplate
        );

    QSet<QString> usedNameSet;
    for (const QString &name : usedNames) usedNameSet.insert(name);

    // 分别统计占位符出现在正面 / 负面模板中，用于卡片配色。
    QSet<QString> positiveNameSet;
    for (const QString &name : placeholderNamesInTemplateText(currentTemplate.positiveTemplate, QString()))
        positiveNameSet.insert(name);
    QSet<QString> negativeNameSet;
    for (const QString &name : placeholderNamesInTemplateText(QString(), currentTemplate.negativeTemplate))
        negativeNameSet.insert(name);

    const QHash<QString, QString> defaults = currentTemplate.placeholderDefaults;

    ui->layoutPlaceholderInputs->setSpacing(8);
    bool addedAny = false;

    for (const PromptPlaceholder &placeholder : std::as_const(m_placeholders)) {
        if (!usedNameSet.contains(placeholder.name)) continue;

        addedAny = true;

        const bool inPositive = positiveNameSet.contains(placeholder.name);
        const bool inNegative = negativeNameSet.contains(placeholder.name);
        QString accent;
        QString badgeText;
        if (inPositive && inNegative) { accent = "#66c0f4"; badgeText = "正面 · 负面"; }
        else if (inNegative)          { accent = "#ff6b6b"; badgeText = "负面"; }
        else                          { accent = "#5fd38d"; badgeText = "正面"; }

        auto *card = new QFrame(ui->placeholderInputContainer);
        card->setObjectName("placeholderCard");
        card->setStyleSheet(QString(
            "QFrame#placeholderCard{background:#1f2833;border:1px solid #31363d;"
            "border-left:3px solid %1;border-radius:6px;}").arg(accent));
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(10, 8, 10, 10);
        layout->setSpacing(6);

        auto *header = new QHBoxLayout();
        header->setContentsMargins(0, 0, 0, 0);
        header->setSpacing(6);
        auto *title = new QLabel(QString("%1  {%2}").arg(placeholder.label, placeholder.name), card);
        title->setStyleSheet("background:transparent;");
        auto *badge = new QLabel(badgeText, card);
        badge->setStyleSheet(QString(
            "color:#12161c;background:%1;border-radius:8px;padding:1px 8px;font-weight:bold;").arg(accent));
        header->addWidget(title);
        header->addStretch(1);
        header->addWidget(badge);
        layout->addLayout(header);

        const QString value = defaults.value(placeholder.name, placeholder.defaultValue);

        if (placeholder.type == PlaceholderType::Text) {
            auto *line = new QLineEdit(card);
            line->setText(value);
            connect(line, &QLineEdit::textChanged, this, &PromptTemplateLibraryWidget::updateGeneratedPrompt);
            layout->addWidget(line);
            m_placeholderEditors.insert(placeholder.name, line);
        } else if (placeholder.type == PlaceholderType::SingleChoice) {
            auto *combo = new QComboBox(card);
            combo->setEditable(true);
            for (const QString &option : placeholder.options) {
                combo->addItem(placeholderOptionDisplayText(option), option);
            }

            const int optionIndex = combo->findData(value);
            if (optionIndex >= 0) {
                combo->setCurrentIndex(optionIndex);
            } else {
                combo->setCurrentText(value);
            }
            connect(combo, &QComboBox::currentTextChanged, this, &PromptTemplateLibraryWidget::updateGeneratedPrompt);
            layout->addWidget(combo);
            m_placeholderEditors.insert(placeholder.name, combo);
        } else {
            // 多选：用 TagFlow 风格的可点击标签（无多选框、自动换行，比多行更省空间）。
            auto *flow = new TagFlowWidget(card);
            flow->setShowCount(false);
            flow->setPixmapCacheEnabled(false);
            flow->setTranslationMap(m_translationMap);

            // 工具按钮：翻译 / 全选·取消全选 / 取消选择。
            auto makeMini = [card](const QString &text, bool checkable) {
                auto *b = new QPushButton(text, card);
                b->setCheckable(checkable);
                b->setCursor(Qt::PointingHandCursor);
                b->setFocusPolicy(Qt::NoFocus);
                b->setFixedHeight(24);
                b->setStyleSheet("QPushButton{padding:1px 8px;}");
                b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
                QFont f = b->font();
                f.setPointSize(9);
                b->setFont(f);
                return b;
            };
            auto *btnTranslate = makeMini("文", true);
            btnTranslate->setObjectName("btnTranslate");   // 复用 QSS 选中高亮
            btnTranslate->setToolTip("显示/隐藏选项翻译");
            auto *btnSelectAll = makeMini("全选", false);
            btnSelectAll->setToolTip("全选 / 取消全选");
            auto *btnClear = makeMini("×", false);
            btnClear->setToolTip("取消选择");

            auto *btnRow = new QHBoxLayout();
            btnRow->setContentsMargins(0, 0, 0, 0);
            btnRow->setSpacing(4);
            btnRow->addStretch(1);
            btnRow->addWidget(btnTranslate);
            btnRow->addWidget(btnSelectAll);
            btnRow->addWidget(btnClear);
            layout->addLayout(btnRow);

            QStringList selected;
            QSet<QString> selectedSet;
            for (const QString &part : value.split(',', Qt::SkipEmptyParts)) {
                const QString trimmed = part.trimmed();
                if (!selectedSet.contains(trimmed)) { selectedSet.insert(trimmed); selected << trimmed; }
            }

            // 用递减的“计数”作为排序权重，保证按定义顺序排列，再隐藏计数显示。
            QMap<QString, int> data;
            int weight = placeholder.options.size();
            for (const QString &option : placeholder.options) {
                data.insert(option, weight--);
            }
            flow->setData(data);
            flow->setSelectedTags(selected);   // 在连接信号前设置初始选中，避免触发渲染

            connect(btnTranslate, &QPushButton::toggled, flow, [flow](bool on){ flow->setShowTranslation(on); });
            connect(btnSelectAll, &QPushButton::clicked, flow, [flow]() {
                if (flow->getSelectedTags().isEmpty()) flow->selectAllVisibleTags();
                else flow->clearSelectedTags();
            });
            connect(btnClear, &QPushButton::clicked, flow, [flow]() { flow->clearSelectedTags(); });
            connect(flow, &TagFlowWidget::filterChanged, this,
                    [this](const QSet<QString> &){ updateGeneratedPrompt(); });

            layout->addWidget(flow);
            m_placeholderEditors.insert(placeholder.name, flow);
        }

        ui->layoutPlaceholderInputs->addWidget(card);
    }

    if (!addedAny) {
        auto *emptyLabel = new QLabel("当前模板没有使用全局占位符。", ui->placeholderInputContainer);
        emptyLabel->setWordWrap(true);
        ui->layoutPlaceholderInputs->addWidget(emptyLabel);
    }

    ui->layoutPlaceholderInputs->addStretch(1);
}

QHash<QString, QString> PromptTemplateLibraryWidget::currentPlaceholderValues(QStringList *missing) const
{
    QHash<QString, QString> values;

    for (const PromptPlaceholder &placeholder : m_placeholders) {
        QWidget *editor = m_placeholderEditors.value(placeholder.name, nullptr);
        if (!editor) continue; // 没有出现在当前模板里的占位符，不参与渲染和未填写判断

        QString value;
        bool hasValidEmptyChoice = false;

        if (auto *line = qobject_cast<QLineEdit*>(editor)) {
            value = line->text();
        } else if (auto *combo = qobject_cast<QComboBox*>(editor)) {
            const QVariant data = combo->currentData();

            if (data.isValid()) {
                value = data.toString();
                hasValidEmptyChoice = value.isEmpty();
            } else {
                value = combo->currentText();
            }
        } else if (auto *flow = qobject_cast<TagFlowWidget*>(editor)) {
            const QSet<QString> chosen = flow->getSelectedTags();
            QStringList selected;
            // 按占位符定义顺序输出选中的选项。
            for (const QString &option : placeholder.options) {
                if (!chosen.contains(option)) continue;
                selected << option;
                if (option.isEmpty()) hasValidEmptyChoice = true;
            }
            value = selected.join(", ");
        } else {
            QStringList selected;
            const auto boxes = editor->findChildren<QCheckBox*>();

            for (QCheckBox *box : boxes) {
                if (!box->isChecked()) continue;

                const QString optionValue = box->property("optionValue").toString();
                selected << optionValue;

                if (optionValue.isEmpty()) {
                    hasValidEmptyChoice = true;
                }
            }

            value = selected.join(", ");
        }

        if (value.isEmpty() && missing && !hasValidEmptyChoice) {
            missing->append(placeholder.name);
        }

        values.insert(placeholder.name, value);
    }

    return values;
}

QString PromptTemplateLibraryWidget::renderTemplateText(const QString &text, QStringList *missing) const
{
    QString rendered = text;
    const QHash<QString, QString> values = currentPlaceholderValues(missing);
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        rendered.replace("{" + it.key() + "}", it.value());
    }
    return cleanupRenderedPrompt(rendered);
}

void PromptTemplateLibraryWidget::updateGeneratedPrompt()
{
    if (m_loadingUi) return;
    const int index = templateIndexById(selectedTemplateId());
    if (index < 0) {
        ui->textGeneratedPositive->clear();
        ui->textGeneratedNegative->clear();
        m_lastRenderedPositivePrompt.clear();
        m_lastRenderedNegativePrompt.clear();
        return;
    }

    const QStringList extraPositiveTags = promptTagsNotInBase(ui->textGeneratedPositive->toPlainText(), m_lastRenderedPositivePrompt);
    const QStringList extraNegativeTags = promptTagsNotInBase(ui->textGeneratedNegative->toPlainText(), m_lastRenderedNegativePrompt);

    QStringList missing;
    const QString positiveBase = renderTemplateText(m_templates.at(index).positiveTemplate, &missing);
    const QString negativeBase = renderTemplateText(m_templates.at(index).negativeTemplate, &missing);
    m_lastRenderedPositivePrompt = positiveBase;
    m_lastRenderedNegativePrompt = negativeBase;

    ui->textGeneratedPositive->setPlainText(positiveBase);
    ui->textGeneratedNegative->setPlainText(negativeBase);
    appendUniquePromptTags(ui->textGeneratedPositive, extraPositiveTags);
    appendUniquePromptTags(ui->textGeneratedNegative, extraNegativeTags);

    missing.removeDuplicates();
    if (missing.isEmpty()) {
        ui->lblGenerateStatus->setText("模板已渲染。");
    } else {
        ui->lblGenerateStatus->setText("存在未填写占位符: " + missing.join(", "));
    }
}

void PromptTemplateLibraryWidget::updateTemplateEditorFromSelection()
{
    const int index = templateIndexById(
        ui->listTemplates->currentItem()
            ? ui->listTemplates->currentItem()->data(Qt::UserRole).toString()
            : QString()
        );

    QSignalBlocker blocker(ui->tableTemplateDefaults);
    ui->tableTemplateDefaults->setRowCount(0);

    if (index < 0) return;

    const PromptTemplate item = m_templates.at(index);

    ui->editTemplateName->setText(item.name);
    ui->editTemplateCategory->setText(item.category);
    ui->textTemplatePositive->setPlainText(item.positiveTemplate);
    ui->textTemplateNegative->setPlainText(item.negativeTemplate);
    ui->textTemplateNotes->setPlainText(item.notes);

    const QStringList usedNames = placeholderNamesInTemplateText(
        item.positiveTemplate,
        item.negativeTemplate
        );

    QSet<QString> usedNameSet;
    for (const QString &name : usedNames) usedNameSet.insert(name);

    for (const PromptPlaceholder &placeholder : std::as_const(m_placeholders)) {
        if (!usedNameSet.contains(placeholder.name)) continue;

        const int row = ui->tableTemplateDefaults->rowCount();
        ui->tableTemplateDefaults->insertRow(row);

        auto *nameItem = new QTableWidgetItem(placeholder.name);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);

        ui->tableTemplateDefaults->setItem(row, 0, nameItem);
        ui->tableTemplateDefaults->setItem(
            row,
            1,
            new QTableWidgetItem(item.placeholderDefaults.value(placeholder.name, placeholder.defaultValue))
            );
    }
}

void PromptTemplateLibraryWidget::updatePlaceholderEditorFromSelection()
{
    const int row = ui->tablePlaceholders->currentRow();

    if (row < 0 || row >= m_placeholders.size()) {
        ui->editPlaceholderName->clear();
        ui->editPlaceholderLabel->clear();
        ui->comboPlaceholderType->setCurrentIndex(0);
        ui->editPlaceholderDefault->clear();
        setPlaceholderOptionValues({});
        updatePlaceholderOptionControls();
        return;
    }

    const PromptPlaceholder item = m_placeholders.at(row);

    ui->editPlaceholderName->setText(item.name);
    ui->editPlaceholderLabel->setText(item.label);
    ui->comboPlaceholderType->setCurrentIndex(
        item.type == PlaceholderType::Text ? 0
        : item.type == PlaceholderType::SingleChoice ? 1
                                                     : 2
        );
    ui->editPlaceholderDefault->setText(item.defaultValue);

    setPlaceholderOptionValues(item.options);
    updatePlaceholderOptionControls();
}

QStringList PromptTemplateLibraryWidget::currentPlaceholderOptionValues() const
{
    QStringList values;

    for (int row = 0; row < ui->tablePlaceholderOptions->rowCount(); ++row) {
        QTableWidgetItem *item = ui->tablePlaceholderOptions->item(row, 0);
        if (!item) {
            values << QString();
            continue;
        }

        values << item->data(Qt::UserRole).toString();
    }

    return values;
}

void PromptTemplateLibraryWidget::setPlaceholderOptionValues(const QStringList &options)
{
    {
        QSignalBlocker tableBlocker(ui->tablePlaceholderOptions);
        QSignalBlocker editorBlocker(ui->editPlaceholderOptionValue);

        ui->tablePlaceholderOptions->setRowCount(0);

        for (const QString &option : options) {
            const int row = ui->tablePlaceholderOptions->rowCount();
            ui->tablePlaceholderOptions->insertRow(row);
            ui->tablePlaceholderOptions->setItem(row, 0, makePlaceholderOptionItem(option));
        }

        if (ui->tablePlaceholderOptions->rowCount() > 0) {
            ui->tablePlaceholderOptions->setCurrentCell(0, 0);
            ui->tablePlaceholderOptions->selectRow(0);
        } else {
            ui->editPlaceholderOptionValue->clear();
        }
    }

    updatePlaceholderOptionEditorFromSelection();
    updatePlaceholderOptionControls();
}

void PromptTemplateLibraryWidget::updatePlaceholderOptionEditorFromSelection()
{
    const int row = ui->tablePlaceholderOptions->currentRow();

    QSignalBlocker blocker(ui->editPlaceholderOptionValue);

    if (row < 0 || row >= ui->tablePlaceholderOptions->rowCount()) {
        ui->editPlaceholderOptionValue->clear();
        return;
    }

    QTableWidgetItem *item = ui->tablePlaceholderOptions->item(row, 0);
    ui->editPlaceholderOptionValue->setPlainText(
        item ? item->data(Qt::UserRole).toString() : QString()
        );
}

void PromptTemplateLibraryWidget::updatePlaceholderOptionControls()
{
    const bool isChoiceType = ui->comboPlaceholderType->currentIndex() != 0;

    const int row = ui->tablePlaceholderOptions->currentRow();
    const int rowCount = ui->tablePlaceholderOptions->rowCount();

    const bool hasSelection = row >= 0 && row < rowCount;

    ui->tablePlaceholderOptions->setEnabled(isChoiceType);
    ui->editPlaceholderOptionValue->setEnabled(isChoiceType);

    ui->btnAddPlaceholderOption->setEnabled(isChoiceType);
    ui->btnUpdatePlaceholderOption->setEnabled(isChoiceType && hasSelection);
    ui->btnDeletePlaceholderOption->setEnabled(isChoiceType && hasSelection);

    ui->btnMovePlaceholderOptionUp->setEnabled(isChoiceType && hasSelection && row > 0);
    ui->btnMovePlaceholderOptionDown->setEnabled(isChoiceType && hasSelection && row < rowCount - 1);
}

void PromptTemplateLibraryWidget::addPlaceholderOptionFromEditor()
{
    const QString value = ui->editPlaceholderOptionValue->toPlainText();

    int insertRow = ui->tablePlaceholderOptions->currentRow();
    if (insertRow < 0) insertRow = ui->tablePlaceholderOptions->rowCount();
    else insertRow += 1;

    ui->tablePlaceholderOptions->insertRow(insertRow);
    ui->tablePlaceholderOptions->setItem(insertRow, 0, makePlaceholderOptionItem(value));
    ui->tablePlaceholderOptions->setCurrentCell(insertRow, 0);
    ui->tablePlaceholderOptions->selectRow(insertRow);

    updatePlaceholderOptionControls();
}

void PromptTemplateLibraryWidget::updateSelectedPlaceholderOptionFromEditor()
{
    const int row = ui->tablePlaceholderOptions->currentRow();
    if (row < 0 || row >= ui->tablePlaceholderOptions->rowCount()) return;

    const QString value = ui->editPlaceholderOptionValue->toPlainText();

    delete ui->tablePlaceholderOptions->takeItem(row, 0);
    ui->tablePlaceholderOptions->setItem(row, 0, makePlaceholderOptionItem(value));
    ui->tablePlaceholderOptions->setCurrentCell(row, 0);
    ui->tablePlaceholderOptions->selectRow(row);

    updatePlaceholderOptionControls();
}

void PromptTemplateLibraryWidget::deleteSelectedPlaceholderOption()
{
    const int row = ui->tablePlaceholderOptions->currentRow();
    if (row < 0 || row >= ui->tablePlaceholderOptions->rowCount()) return;

    ui->tablePlaceholderOptions->removeRow(row);

    if (ui->tablePlaceholderOptions->rowCount() > 0) {
        const int nextRow = qMin(row, ui->tablePlaceholderOptions->rowCount() - 1);
        ui->tablePlaceholderOptions->setCurrentCell(nextRow, 0);
        ui->tablePlaceholderOptions->selectRow(nextRow);
    } else {
        ui->editPlaceholderOptionValue->clear();
    }

    updatePlaceholderOptionEditorFromSelection();
    updatePlaceholderOptionControls();
}

void PromptTemplateLibraryWidget::moveSelectedPlaceholderOptionUp()
{
    const int row = ui->tablePlaceholderOptions->currentRow();
    if (row <= 0 || row >= ui->tablePlaceholderOptions->rowCount()) return;

    swapPlaceholderOptionRows(ui->tablePlaceholderOptions, row, row - 1);

    ui->tablePlaceholderOptions->setCurrentCell(row - 1, 0);
    ui->tablePlaceholderOptions->selectRow(row - 1);

    updatePlaceholderOptionEditorFromSelection();
    updatePlaceholderOptionControls();
}

void PromptTemplateLibraryWidget::moveSelectedPlaceholderOptionDown()
{
    const int row = ui->tablePlaceholderOptions->currentRow();
    const int rowCount = ui->tablePlaceholderOptions->rowCount();

    if (row < 0 || row >= rowCount - 1) return;

    swapPlaceholderOptionRows(ui->tablePlaceholderOptions, row, row + 1);

    ui->tablePlaceholderOptions->setCurrentCell(row + 1, 0);
    ui->tablePlaceholderOptions->selectRow(row + 1);

    updatePlaceholderOptionEditorFromSelection();
    updatePlaceholderOptionControls();
}

void PromptTemplateLibraryWidget::saveCurrentTemplateEditor()
{
    const int index = templateIndexById(
        ui->listTemplates->currentItem()
            ? ui->listTemplates->currentItem()->data(Qt::UserRole).toString()
            : QString()
        );

    if (index < 0) return;

    PromptTemplate &item = m_templates[index];

    QHash<QString, QString> editedDefaults;
    for (int row = 0; row < ui->tableTemplateDefaults->rowCount(); ++row) {
        const QString name = ui->tableTemplateDefaults->item(row, 0)
        ? ui->tableTemplateDefaults->item(row, 0)->text()
        : QString();

        const QString value = ui->tableTemplateDefaults->item(row, 1)
                                  ? ui->tableTemplateDefaults->item(row, 1)->text()
                                  : QString();

        if (!name.isEmpty()) editedDefaults.insert(name, value);
    }

    const QHash<QString, QString> oldDefaults = item.placeholderDefaults;

    item.name = ui->editTemplateName->text().trimmed();
    if (item.name.isEmpty()) item.name = "Untitled Template";

    item.category = ui->editTemplateCategory->text().trimmed();
    item.positiveTemplate = ui->textTemplatePositive->toPlainText();
    item.negativeTemplate = ui->textTemplateNegative->toPlainText();
    item.notes = ui->textTemplateNotes->toPlainText();

    const QStringList usedNames = placeholderNamesInTemplateText(
        item.positiveTemplate,
        item.negativeTemplate
        );

    QSet<QString> usedNameSet;
    for (const QString &name : usedNames) usedNameSet.insert(name);

    item.placeholderDefaults.clear();

    for (const PromptPlaceholder &placeholder : std::as_const(m_placeholders)) {
        if (!usedNameSet.contains(placeholder.name)) continue;

        const QString value = editedDefaults.value(
            placeholder.name,
            oldDefaults.value(placeholder.name, placeholder.defaultValue)
            );

        item.placeholderDefaults.insert(placeholder.name, value);
    }
}

void PromptTemplateLibraryWidget::saveCurrentPlaceholderEditor()
{
    const int row = ui->tablePlaceholders->currentRow();
    if (row < 0 || row >= m_placeholders.size()) return;

    PromptPlaceholder &item = m_placeholders[row];

    item.name = ui->editPlaceholderName->text().trimmed();
    item.label = ui->editPlaceholderLabel->text().trimmed();
    if (item.label.isEmpty()) item.label = item.name;

    item.type = ui->comboPlaceholderType->currentIndex() == 1 ? PlaceholderType::SingleChoice
                : ui->comboPlaceholderType->currentIndex() == 2 ? PlaceholderType::MultiChoice
                                                                : PlaceholderType::Text;

    item.defaultValue = ui->editPlaceholderDefault->text();

    item.options = currentPlaceholderOptionValues();
}

int PromptTemplateLibraryWidget::templateIndexById(const QString &id) const
{
    for (int i = 0; i < m_templates.size(); ++i) {
        if (m_templates.at(i).id == id) return i;
    }
    return -1;
}

int PromptTemplateLibraryWidget::placeholderIndexByName(const QString &name) const
{
    for (int i = 0; i < m_placeholders.size(); ++i) {
        if (m_placeholders.at(i).name == name) return i;
    }
    return -1;
}

QString PromptTemplateLibraryWidget::selectedTemplateId() const
{
    return ui->comboGenerateTemplate->currentData().toString();
}

void PromptTemplateLibraryWidget::setStatus(const QString &text)
{
    ui->lblGenerateStatus->setText(text);
}

void PromptTemplateLibraryWidget::copyText(const QString &text) const
{
    QApplication::clipboard()->setText(text);
}

void PromptTemplateLibraryWidget::setupTagPickerUi(TagPickerUi &picker)
{
    if (!picker.table) return;

    picker.table->setColumnCount(5);
    picker.table->setHorizontalHeaderLabels({"Tag", "类型", "类别", "翻译", "使用次数"});

    QHeaderView *header = picker.table->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionsClickable(true);
    header->setMinimumSectionSize(48);

    header->setSectionResizeMode(0, QHeaderView::Interactive); // Tag
    header->setSectionResizeMode(1, QHeaderView::Fixed);       // 类型
    header->setSectionResizeMode(2, QHeaderView::Fixed);       // 类别
    header->setSectionResizeMode(3, QHeaderView::Stretch);     // 翻译
    header->setSectionResizeMode(4, QHeaderView::Fixed);       // 使用次数

    header->resizeSection(0, 170);
    header->resizeSection(1, 56);
    header->resizeSection(2, 72);
    header->resizeSection(3, 220);
    header->resizeSection(4, 76);

    picker.table->verticalHeader()->hide();
    picker.table->setShowGrid(false);
    picker.table->setFocusPolicy(Qt::NoFocus);
    picker.table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    picker.table->setSortingEnabled(true);
    header->setSortIndicatorShown(true);
    header->setSortIndicator(4, Qt::DescendingOrder);

    if (picker.search) {
        connect(picker.search, &QLineEdit::textChanged, this, [this, &picker]() {
            onTagPickerFiltersChanged(picker);
        });
    }
    if (picker.scope) {
        connect(picker.scope, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, &picker]() {
            onTagPickerFiltersChanged(picker);
        });
    }
    if (picker.refresh) {
        connect(picker.refresh, &QPushButton::clicked, this, [this, &picker]() {
            loadTagPickerRows(picker, true);
        });
    }
    if (picker.insertPositive) {
        connect(picker.insertPositive, &QPushButton::clicked, this, [this, &picker]() {
            addPickerTags(picker, true);
        });
    }
    if (picker.insertNegative) {
        connect(picker.insertNegative, &QPushButton::clicked, this, [this, &picker]() {
            addPickerTags(picker, false);
        });
    }
    connect(picker.table, &QTableWidget::cellDoubleClicked, this, [this, &picker](int, int) {
        addPickerTags(picker, true);
    });
}

void PromptTemplateLibraryWidget::setupModelTriggerPickerUi(ModelTriggerPickerUi &picker)
{
    if (!picker.tree) return;
    picker.tree->setColumnCount(1);
    picker.tree->setHeaderHidden(true);
    picker.tree->setFocusPolicy(Qt::NoFocus);
    picker.tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    picker.tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    picker.tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    picker.tree->setRootIsDecorated(false);
    picker.tree->setUniformRowHeights(false);
    picker.tree->setIconSize(QSize(52, 52));
    // 缩进设为 0，子项的视觉缩进完全由 delegate 的卡片偏移(+26)呈现。
    // 否则左侧缩进列会显示默认的选中/悬停高亮（不属于 ::item，无法被样式表去掉）。
    picker.tree->setIndentation(0);
    picker.tree->setTextElideMode(Qt::ElideNone);
    picker.tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    picker.tree->setItemDelegate(new ModelTriggerTreeDelegate(picker.tree, picker.tree));
    picker.tree->setStyleSheet(
        "QTreeWidget#treeGenerateModelTriggers, QTreeWidget#treeTemplateModelTriggers {"
        " background-color:#16191e; border:1px solid #31363d; border-radius:3px; padding:4px;"
        "}"
        "QTreeWidget#treeGenerateModelTriggers::item, QTreeWidget#treeTemplateModelTriggers::item {"
        " background:transparent; border:none; padding:0; margin:0;"
        "}"
        "QTreeWidget#treeGenerateModelTriggers::item:selected, QTreeWidget#treeTemplateModelTriggers::item:selected,"
        "QTreeWidget#treeGenerateModelTriggers::item:hover, QTreeWidget#treeTemplateModelTriggers::item:hover {"
        " background:transparent; color:inherit;"
        "}"
        "QTreeWidget#treeGenerateModelTriggers::branch, QTreeWidget#treeTemplateModelTriggers::branch,"
        "QTreeWidget#treeGenerateModelTriggers::branch:selected, QTreeWidget#treeTemplateModelTriggers::branch:selected,"
        "QTreeWidget#treeGenerateModelTriggers::branch:hover, QTreeWidget#treeTemplateModelTriggers::branch:hover {"
        " background:transparent; border:none; image:none;"
        "}");

    picker.tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    const bool insertIntoTemplate = picker.insertIntoTemplate;
    auto pickerRef = [this, insertIntoTemplate]() -> ModelTriggerPickerUi& {
        return insertIntoTemplate ? m_templateModelTriggerPicker : m_generateModelTriggerPicker;
    };
    if (picker.search) {
        connect(picker.search, &QLineEdit::textChanged, this, [this, pickerRef]() mutable {
            refreshModelTriggerPickerTable(pickerRef());
        });
    }
    if (picker.insertPositive) {
        connect(picker.insertPositive, &QPushButton::clicked, this, [this, pickerRef]() mutable {
            addModelTriggerTags(pickerRef());
        });
    }
    // 单击模型条目即展开/收缩（不再显示第一列的展开箭头）
    connect(picker.tree, &QTreeWidget::itemClicked, this, [this, pickerRef](QTreeWidgetItem *item, int) mutable {
        Q_UNUSED(pickerRef);
        if (!item || item->parent()) return; // 仅顶层模型条目响应展开/收缩
        item->setExpanded(!item->isExpanded());
    });
    // 仅双击展开后的某条触发词行时，把该触发词填入；双击模型本身不再填入全部触发词
    connect(picker.tree, &QTreeWidget::itemDoubleClicked, this, [this, pickerRef](QTreeWidgetItem *item, int) mutable {
        if (!item || !item->parent()) return; // 顶层模型条目不填入
        ModelTriggerPickerUi &p = pickerRef();
        QStringList tags;
        if (m_addLoraTagWithTrigger) {
            const QString lora = item->data(0, Qt::UserRole + 5).toString().trimmed();
            if (!lora.isEmpty()) tags << QString("<lora:%1:1>").arg(lora);
        }
        tags += item->data(0, Qt::UserRole + 1).toStringList();
        if (tags.isEmpty()) return;
        QPlainTextEdit *target = p.insertIntoTemplate ? ui->textTemplatePositive : ui->textGeneratedPositive;
        const int added = appendUniquePromptTags(target, tags);
        if (p.status) {
            p.status->setText(added == 0
                ? "目标中已经包含该触发词。"
                : QString("已添加 %1 个触发词到%2。").arg(added).arg(p.insertIntoTemplate ? "正面模板" : "正面提示词"));
        }
    });
    connect(picker.tree, &QTreeWidget::itemExpanded, this, [this, pickerRef](QTreeWidgetItem *item) mutable {
        if (!item || item->parent()) return;
        populateModelTriggerChildren(item);
        pickerRef().tree->doItemsLayout();
        pickerRef().expandedModelKeys.insert(item->data(0, Qt::UserRole).toString());
    });
    connect(picker.tree, &QTreeWidget::itemCollapsed, this, [this, pickerRef](QTreeWidgetItem *item) mutable {
        if (item && !item->parent()) pickerRef().expandedModelKeys.remove(item->data(0, Qt::UserRole).toString());
    });
}

QStringList PromptTemplateLibraryWidget::selectedModelTriggerTexts(const ModelTriggerPickerUi &picker) const
{
    QStringList triggers;
    if (!picker.tree) return triggers;
    QSet<QString> seen;
    for (QTreeWidgetItem *item : picker.tree->selectedItems()) {
        for (const QString &trigger : item->data(0, Qt::UserRole + 1).toStringList()) {
            const QString clean = trigger.trimmed();
            if (clean.isEmpty()) continue;
            const QString key = clean.toCaseFolded();
            if (seen.contains(key)) continue;
            seen.insert(key);
            triggers << clean;
        }
    }
    return triggers;
}

void PromptTemplateLibraryWidget::refreshModelTriggerPickerTable(ModelTriggerPickerUi &picker)
{
    if (!picker.tree || !picker.status) return;
    const QString needle = picker.search ? normalizeTagSearch(picker.search->text()) : QString();
    QSignalBlocker blocker(picker.tree);
    picker.tree->clear();

    struct ModelTriggerGroup {
        QString key;
        QString modelName;
        QString previewPath;
        QIcon previewIcon;
        QString modelType;
        QString loraName;
        QStringList metadataTriggers;
        QStringList customTriggers;
    };

    QVector<ModelTriggerGroup> groups;
    QHash<QString, int> groupIndexByKey;
    auto appendUnique = [](QStringList &list, const QString &value) {
        const QString clean = value.trimmed();
        if (clean.isEmpty()) return;
        for (const QString &existing : std::as_const(list)) {
            if (existing.compare(clean, Qt::CaseInsensitive) == 0) return;
        }
        list << clean;
    };

    for (const ModelTriggerRow &rowData : std::as_const(m_modelTriggerRows)) {
        if (rowData.trigger.trimmed().isEmpty()) continue;
        if (!needle.isEmpty()) {
            const QString modelHaystack = normalizeTagSearch(rowData.modelName + " " + rowData.source + " " + rowData.modelType);
            const QString triggerHaystack = normalizeTagSearch(rowData.trigger);
            if (!modelHaystack.contains(needle) && !triggerHaystack.contains(needle)) continue;
        }

        const QString key = rowData.modelKey.isEmpty() ? rowData.modelName : rowData.modelKey;
        int groupIndex = groupIndexByKey.value(key, -1);
        if (groupIndex < 0) {
            ModelTriggerGroup group;
            group.key = key;
            group.modelName = rowData.modelName;
            group.previewPath = rowData.previewPath;
            group.previewIcon = rowData.previewIcon;
            group.modelType = rowData.modelType;
            group.loraName = rowData.loraName;
            groupIndex = groups.size();
            groups.append(group);
            groupIndexByKey.insert(key, groupIndex);
        }

        QStringList &target = rowData.source.compare("Custom", Qt::CaseInsensitive) == 0
            ? groups[groupIndex].customTriggers
            : groups[groupIndex].metadataTriggers;
        appendUnique(target, rowData.trigger);
    }

    for (const ModelTriggerGroup &group : std::as_const(groups)) {
        const QStringList allTriggers = group.metadataTriggers + group.customTriggers;
        if (allTriggers.isEmpty()) continue;
        const QStringList sources = QStringList()
            << (group.metadataTriggers.isEmpty() ? QString() : QString("Metadata(%1)").arg(group.metadataTriggers.size()))
            << (group.customTriggers.isEmpty() ? QString() : QString("Custom(%1)").arg(group.customTriggers.size()));

        auto *modelItem = new QTreeWidgetItem(picker.tree);
        const QString sourceText = sources.filter(QRegularExpression(".+")).join(" / ");
        const QString detailLine = QStringList{group.modelType, sourceText}.filter(QRegularExpression(".+")).join(" · ");
        modelItem->setText(0, detailLine.isEmpty() ? group.modelName : QString("%1\n%2").arg(group.modelName, detailLine));
        modelItem->setData(0, Qt::UserRole, group.key);
        modelItem->setData(0, Qt::UserRole + 1, allTriggers);
        modelItem->setData(0, Qt::UserRole + 2, group.metadataTriggers);
        modelItem->setData(0, Qt::UserRole + 3, group.customTriggers);
        modelItem->setData(0, Qt::UserRole + 4, group.modelType);
        modelItem->setData(0, Qt::UserRole + 5, group.loraName);
        modelItem->setToolTip(0, group.modelName);
        if (!group.previewIcon.isNull()) modelItem->setIcon(0, group.previewIcon);
        modelItem->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        if (picker.expandedModelKeys.contains(group.key)) {
            populateModelTriggerChildren(modelItem);
            modelItem->setExpanded(true);
        }
    }

    picker.status->setText(picker.tree->topLevelItemCount() == 0
        ? "没有可显示的模型触发词。请先同步模型元数据或编辑自定义触发词。"
        : QString("显示 %1 个模型，共 %2 条触发词。")
              .arg(picker.tree->topLevelItemCount())
              .arg(std::accumulate(groups.cbegin(), groups.cend(), 0, [](int total, const ModelTriggerGroup &group) {
                  return total + group.metadataTriggers.size() + group.customTriggers.size();
              })));
    m_modelTriggerRowsDirty = false;
}

void PromptTemplateLibraryWidget::addModelTriggerTags(ModelTriggerPickerUi &picker)
{
    const QStringList triggers = selectedModelTriggerTexts(picker);

    // 选项开启时，连同所属模型的 LoRA 标签一起加入（去重）。
    QStringList loraTags;
    if (m_addLoraTagWithTrigger && picker.tree) {
        QSet<QString> seenLora;
        for (QTreeWidgetItem *item : picker.tree->selectedItems()) {
            const QString lora = item->data(0, Qt::UserRole + 5).toString().trimmed();
            if (lora.isEmpty()) continue;
            const QString tag = QString("<lora:%1:1>").arg(lora);
            if (seenLora.contains(tag.toCaseFolded())) continue;
            seenLora.insert(tag.toCaseFolded());
            loraTags << tag;
        }
    }

    QStringList tags = loraTags;
    tags += triggers;
    if (tags.isEmpty()) {
        if (picker.status) picker.status->setText("请先选择模型触发词。");
        return;
    }

    QPlainTextEdit *target = picker.insertIntoTemplate ? ui->textTemplatePositive : ui->textGeneratedPositive;
    const int added = appendUniquePromptTags(target, tags);
    if (picker.status) {
        picker.status->setText(added == 0
            ? "目标中已经包含选中的触发词。"
            : QString("已添加 %1 个触发词到%2。").arg(added).arg(picker.insertIntoTemplate ? "正面模板" : "正面提示词"));
    }
}

void PromptTemplateLibraryWidget::setupPromptTagFlowView(QPlainTextEdit *edit)
{
    if (!edit) return;
    QWidget *host = edit->parentWidget();
    auto *parentLayout = host ? qobject_cast<QBoxLayout*>(host->layout()) : nullptr;
    if (!parentLayout) return;
    const int editIndex = parentLayout->indexOf(edit);
    if (editIndex < 0) return;

    // 紧邻编辑框上方的标题标签（“正面提示词”/“负面提示词”），把按钮放到它右侧。
    QLabel *titleLabel = nullptr;
    if (editIndex > 0) {
        if (QLayoutItem *prev = parentLayout->itemAt(editIndex - 1))
            titleLabel = qobject_cast<QLabel*>(prev->widget());
    }

    PromptTagFlowView view;
    view.edit = edit;

    auto makeMiniButton = [host](const QString &text, bool checkable) {
        auto *b = new QPushButton(text, host);
        b->setCheckable(checkable);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedHeight(26);
        // 收紧内边距并让按钮按内容自适应宽度，避免文本被压缩截断。
        b->setStyleSheet("QPushButton{padding:2px 10px;}");
        b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        QFont f = b->font();
        f.setPointSize(9);
        b->setFont(f);
        return b;
    };

    view.translateButton = makeMiniButton("文", true);
    view.translateButton->setObjectName("btnTranslate"); // 复用 QSS 的选中高亮样式
    view.translateButton->setToolTip("显示/隐藏 Tag 翻译");
    view.translateButton->setChecked(true);
    view.selectAllButton = makeMiniButton("全选", false);
    view.selectAllButton->setToolTip("全选 / 取消全选当前显示的 Tag");
    view.clearButton = makeMiniButton("×", false);
    view.clearButton->setToolTip("取消选中的 Tag");
    view.toggleButton = makeMiniButton("标签", true);
    view.toggleButton->setToolTip("在文本编辑与标签视图之间切换");

    // 标签视图相关按钮仅在标签视图下显示。
    view.translateButton->setVisible(false);
    view.selectAllButton->setVisible(false);
    view.clearButton->setVisible(false);

    auto *header = new QWidget(host);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(4);
    headerLayout->addStretch(1);
    headerLayout->addWidget(view.translateButton);
    headerLayout->addWidget(view.selectAllButton);
    headerLayout->addWidget(view.clearButton);
    headerLayout->addWidget(view.toggleButton); // 切换按钮固定在最右侧

    // 堆叠控件：第 0 页文本编辑，第 1 页标签视图。
    view.stack = new QStackedWidget(host);
    view.stack->setSizePolicy(edit->sizePolicy());

    auto *area = new QScrollArea(view.stack);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    // 与文本编辑框保持一致的背景。
    area->setStyleSheet("QScrollArea { background-color:#16191e; border:1px solid #31363d; border-radius:3px; }");
    auto *areaContents = new QWidget(area);
    areaContents->setStyleSheet("background:transparent;");
    auto *areaLayout = new QVBoxLayout(areaContents);
    areaLayout->setContentsMargins(4, 4, 4, 4);
    areaLayout->setSpacing(0);
    view.flow = new TagFlowWidget(areaContents);
    view.flow->setTranslationMap(m_translationMap);
    view.flow->setShowTranslation(true);
    view.flow->setPixmapCacheEnabled(false);
    areaLayout->addWidget(view.flow);
    areaLayout->addStretch(1);
    area->setWidget(areaContents);

    // 用 header 接管标题标签所在位置，并把标签移到 header 最左侧。
    if (titleLabel) {
        delete parentLayout->replaceWidget(titleLabel, header);
        headerLayout->insertWidget(0, titleLabel); // 重新父化到 header 最左
    } else {
        parentLayout->insertWidget(editIndex, header);
    }
    // 用 stack 接管编辑框所在位置。
    delete parentLayout->replaceWidget(edit, view.stack);
    view.stack->addWidget(edit);   // 重新父化到 stack
    view.stack->addWidget(area);
    view.stack->setCurrentIndex(0);

    const int viewIndex = m_tagFlowViews.size();
    m_tagFlowViews.append(view);

    connect(view.toggleButton, &QPushButton::toggled, this, [this, viewIndex](bool checked) {
        togglePromptTagFlowView(viewIndex, checked);
    });
    connect(view.clearButton, &QPushButton::clicked, this, [this, viewIndex]() {
        if (m_tagFlowViews[viewIndex].flow) m_tagFlowViews[viewIndex].flow->clearSelectedTags();
    });
    connect(view.selectAllButton, &QPushButton::clicked, this, [this, viewIndex]() {
        TagFlowWidget *flow = m_tagFlowViews[viewIndex].flow;
        if (!flow) return;
        if (flow->getSelectedTags().isEmpty()) flow->selectAllVisibleTags();
        else flow->clearSelectedTags();
    });
    connect(view.translateButton, &QPushButton::toggled, this, [this, viewIndex](bool checked) {
        if (m_tagFlowViews[viewIndex].flow) m_tagFlowViews[viewIndex].flow->setShowTranslation(checked);
    });
    connect(edit, &QPlainTextEdit::textChanged, this, [this, viewIndex]() {
        PromptTagFlowView &v = m_tagFlowViews[viewIndex];
        if (v.tagViewActive) refreshPromptTagFlowFromText(v);
    });
}

void PromptTemplateLibraryWidget::togglePromptTagFlowView(int viewIndex, bool tagView)
{
    if (viewIndex < 0 || viewIndex >= m_tagFlowViews.size()) return;
    PromptTagFlowView &view = m_tagFlowViews[viewIndex];
    view.tagViewActive = tagView;
    if (view.translateButton) view.translateButton->setVisible(tagView);
    if (view.selectAllButton) view.selectAllButton->setVisible(tagView);
    if (view.clearButton) view.clearButton->setVisible(tagView);
    if (view.toggleButton) view.toggleButton->setText(tagView ? "文本" : "标签");
    if (tagView) {
        hideAutocompletePopup();
        refreshPromptTagFlowFromText(view);
    }
    if (view.stack) view.stack->setCurrentIndex(tagView ? 1 : 0);
}

void PromptTemplateLibraryWidget::refreshPromptTagFlowFromText(PromptTagFlowView &view)
{
    if (!view.flow || !view.edit) return;
    view.flow->setTranslationMap(m_translationMap);
    view.flow->setData(tagCountsFromPrompt(view.edit->toPlainText()));
    if (view.translateButton) view.flow->setShowTranslation(view.translateButton->isChecked());
}

void PromptTemplateLibraryWidget::setupAutocompleteForEditor(QPlainTextEdit *edit)
{
    if (!edit) return;
    edit->installEventFilter(this);
    connect(edit, &QPlainTextEdit::textChanged, this, [this, edit]() {
        if (m_autocompleteInserting || !edit->hasFocus()) return;
        updateAutocompletePopup(edit);
    });
    connect(edit, &QPlainTextEdit::cursorPositionChanged, this, [this, edit]() {
        if (m_autocompleteInserting || !edit->hasFocus()) return;
        if (m_autocompletePopup && m_autocompletePopup->isVisible() && m_autocompleteEdit == edit) {
            updateAutocompletePopup(edit);
        }
    });
}

void PromptTemplateLibraryWidget::rebuildAutocompleteIndex()
{
    m_autocompleteEntries.clear();
    if (!m_translationMap || m_translationMap->isEmpty()) {
        hideAutocompletePopup();
        return;
    }
    m_autocompleteEntries.reserve(m_translationMap->size());
    for (auto it = m_translationMap->constBegin(); it != m_translationMap->constEnd(); ++it) {
        const QString tag = it.key().trimmed();
        if (tag.isEmpty()) continue;
        AutocompleteEntry entry;
        entry.tag = tag;
        parseTranslationValue(it.value(), entry.translation, entry.count);
        entry.translation = stripLeadingTagFromTranslation(tag, entry.translation);
        entry.foldedTag = normalizeTagSearch(tag);
        if (entry.foldedTag.isEmpty()) continue;
        m_autocompleteEntries.append(entry);
    }
    std::sort(m_autocompleteEntries.begin(), m_autocompleteEntries.end(),
        [](const AutocompleteEntry &a, const AutocompleteEntry &b) {
            if (a.foldedTag != b.foldedTag) return a.foldedTag < b.foldedTag;
            return a.count > b.count;
        });
}

void PromptTemplateLibraryWidget::updateAutocompletePopup(QPlainTextEdit *edit)
{
    if (m_autocompleteInserting) return;
    if (!edit || m_autocompleteEntries.isEmpty()) { hideAutocompletePopup(); return; }
    if (!m_autocompleteEnabledByEdit.value(edit, true)) { hideAutocompletePopup(); return; }
    for (const PromptTagFlowView &v : m_tagFlowViews) {
        if (v.edit == edit && v.tagViewActive) { hideAutocompletePopup(); return; }
    }

    const QPair<int, int> range = currentAutocompleteTokenRange(edit);
    if (range.second <= range.first) { hideAutocompletePopup(); return; }
    const QString token = edit->toPlainText().mid(range.first, range.second - range.first).trimmed();
    if (token.isEmpty() || token.startsWith('{') || token.startsWith('<')) { hideAutocompletePopup(); return; }
    const QString needle = normalizeTagSearch(token);
    if (needle.isEmpty()) { hideAutocompletePopup(); return; }

    const bool ascii = isAsciiText(needle);
    const int kMaxResults = qBound(1, m_autocompleteLimit, 50);

    struct Scored { const AutocompleteEntry *e; int score; };
    QVector<Scored> scored;
    QSet<QString> seen;

    // 1) 标签前缀匹配（已按 foldedTag 排序，二分定位）
    auto lower = std::lower_bound(m_autocompleteEntries.cbegin(), m_autocompleteEntries.cend(), needle,
        [](const AutocompleteEntry &entry, const QString &key) { return entry.foldedTag < key; });
    for (auto it = lower; it != m_autocompleteEntries.cend(); ++it) {
        if (!it->foldedTag.startsWith(needle)) break;
        if (seen.contains(it->tag)) continue;
        seen.insert(it->tag);
        scored.append({ &(*it), it->foldedTag == needle ? 3 : 2 });
        if (scored.size() >= 400) break;
    }

    // 2) 补充：标签子串 / 翻译子串匹配（中文输入或中间匹配），限制扫描量
    if (scored.size() < kMaxResults * 6) {
        int scan = 0;
        for (const AutocompleteEntry &entry : m_autocompleteEntries) {
            if (++scan > 60000) break;
            if (seen.contains(entry.tag)) continue;
            bool hit = ascii && entry.foldedTag.contains(needle);
            if (!hit && !entry.translation.isEmpty() && entry.translation.contains(token, Qt::CaseInsensitive)) hit = true;
            if (!hit) continue;
            seen.insert(entry.tag);
            scored.append({ &entry, 1 });
            if (scored.size() >= kMaxResults * 6) break;
        }
    }

    if (scored.isEmpty()) { hideAutocompletePopup(); return; }

    std::sort(scored.begin(), scored.end(), [](const Scored &a, const Scored &b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.e->count != b.e->count) return a.e->count > b.e->count;
        if (a.e->tag.size() != b.e->tag.size()) return a.e->tag.size() < b.e->tag.size();
        return a.e->tag < b.e->tag;
    });

    if (!m_autocompletePopup) {
        m_autocompletePopup = new QListWidget(this);
        m_autocompletePopup->setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
        m_autocompletePopup->setAttribute(Qt::WA_ShowWithoutActivating);
        m_autocompletePopup->setFocusPolicy(Qt::NoFocus);
        m_autocompletePopup->setUniformItemSizes(true);
        m_autocompletePopup->setSelectionMode(QAbstractItemView::SingleSelection);
        m_autocompletePopup->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_autocompletePopup->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_autocompletePopup->setStyleSheet(
            "QListWidget { background:#1f2833; color:#dcdedf; border:1px solid #3d4450; outline:0; }");
        m_autocompletePopup->setItemDelegate(new AutocompleteItemDelegate(m_autocompletePopup));
        connect(m_autocompletePopup, &QListWidget::itemClicked, this, [this](QListWidgetItem *) {
            acceptAutocompleteSelection();
        });
    }

    m_autocompletePopup->clear();
    const int shown = qMin<int>(kMaxResults, scored.size());
    const QFontMetrics fm(m_autocompletePopup->font());
    int contentW = 0;
    for (int i = 0; i < shown; ++i) {
        const AutocompleteEntry *e = scored[i].e;
        auto *item = new QListWidgetItem(e->tag, m_autocompletePopup); // text 仅作回退/无障碍用
        item->setData(Qt::UserRole, e->tag);
        item->setData(Qt::UserRole + 1, e->translation);
        int w = fm.horizontalAdvance(e->tag);
        if (!e->translation.isEmpty()) w += 28 + fm.horizontalAdvance(e->translation);
        contentW = qMax(contentW, w);
    }
    m_autocompletePopup->setCurrentRow(0);
    m_autocompleteEdit = edit;

    const int rowH = fm.height() + 8;
    const int h = rowH * shown + 6;
    const int w = qBound(200, contentW + 28, 620);
    m_autocompletePopup->resize(w, h);
    const QRect cr = edit->cursorRect();
    const QPoint pos = edit->viewport()->mapToGlobal(cr.bottomLeft()) + QPoint(0, 2);
    m_autocompletePopup->move(pos);
    m_autocompletePopup->show();
}

void PromptTemplateLibraryWidget::hideAutocompletePopup()
{
    if (m_autocompletePopup && m_autocompletePopup->isVisible()) m_autocompletePopup->hide();
    m_autocompleteEdit = nullptr;
}

void PromptTemplateLibraryWidget::acceptAutocompleteSelection()
{
    if (!m_autocompletePopup || !m_autocompleteEdit) { hideAutocompletePopup(); return; }
    QListWidgetItem *item = m_autocompletePopup->currentItem();
    if (!item) item = m_autocompletePopup->item(0);
    QPlainTextEdit *edit = m_autocompleteEdit;
    if (!item || !edit) { hideAutocompletePopup(); return; }
    const QString tag = item->data(Qt::UserRole).toString();
    if (tag.isEmpty()) { hideAutocompletePopup(); return; }

    const QPair<int, int> range = currentAutocompleteTokenRange(edit);
    const QString full = edit->toPlainText();
    QString insert = tag;
    const QChar nextChar = range.second < full.size() ? full.at(range.second) : QChar();
    if (nextChar != ',') insert += ", ";

    m_autocompleteInserting = true;
    QTextCursor cursor = edit->textCursor();
    cursor.beginEditBlock();
    cursor.setPosition(range.first);
    cursor.setPosition(range.second, QTextCursor::KeepAnchor);
    cursor.insertText(insert);
    cursor.endEditBlock();
    edit->setTextCursor(cursor);
    m_autocompleteInserting = false;

    hideAutocompletePopup();
}

bool PromptTemplateLibraryWidget::handleAutocompleteKeyPress(QKeyEvent *event)
{
    if (!m_autocompletePopup || !m_autocompletePopup->isVisible()) return false;
    const int n = m_autocompletePopup->count();
    switch (event->key()) {
    case Qt::Key_Down:
        if (n > 0) m_autocompletePopup->setCurrentRow((m_autocompletePopup->currentRow() + 1) % n);
        return true;
    case Qt::Key_Up:
        if (n > 0) m_autocompletePopup->setCurrentRow((m_autocompletePopup->currentRow() - 1 + n) % n);
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Tab:
        acceptAutocompleteSelection();
        return true;
    case Qt::Key_Escape:
        hideAutocompletePopup();
        return true;
    default:
        return false;
    }
}

QPair<int, int> PromptTemplateLibraryWidget::currentAutocompleteTokenRange(QPlainTextEdit *edit) const
{
    if (!edit) return {0, 0};
    const QTextCursor cursor = edit->textCursor();
    if (cursor.hasSelection()) return {0, 0};
    const int pos = cursor.position();
    const QString text = edit->toPlainText();
    int start = pos;
    while (start > 0) {
        const QChar c = text.at(start - 1);
        if (c == ',' || c == '\n' || c == '\r') break;
        --start;
    }
    while (start < pos) {
        const QChar c = text.at(start);
        if (c == ' ' || c == '\t') ++start;
        else break;
    }
    return {start, pos};
}

QString PromptTemplateLibraryWidget::autocompleteSettingsPath() const
{
    const QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);
    return configDir + "/settings.json";
}

void PromptTemplateLibraryWidget::loadAutocompleteSettings()
{
    QJsonObject root;
    QFile file(autocompleteSettingsPath());
    if (file.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
    }
    m_autocompleteLimit = qBound(1, root.value("prompt_autocomplete_limit").toInt(12), 50);
    if (ui->spinAutocompleteLimit) {
        QSignalBlocker blocker(ui->spinAutocompleteLimit);
        ui->spinAutocompleteLimit->setValue(m_autocompleteLimit);
    }

    for (const AutocompleteToggle &toggle : std::as_const(m_autocompleteToggles)) {
        const bool on = root.value(toggle.key).toBool(true);
        m_autocompleteEnabledByEdit[toggle.edit] = on;
        if (toggle.check) {
            QSignalBlocker blocker(toggle.check);
            toggle.check->setChecked(on);
        }
    }

    m_addLoraTagWithTrigger = root.value("model_trigger_add_lora_tag").toBool(true);
    if (ui->chkAddLoraTagWithTrigger) {
        QSignalBlocker blocker(ui->chkAddLoraTagWithTrigger);
        ui->chkAddLoraTagWithTrigger->setChecked(m_addLoraTagWithTrigger);
    }
}

void PromptTemplateLibraryWidget::saveAutocompleteSettings() const
{
    QFile file(autocompleteSettingsPath());
    QJsonObject root;
    if (file.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
    }
    root["prompt_autocomplete_limit"] = m_autocompleteLimit;
    root["model_trigger_add_lora_tag"] = m_addLoraTagWithTrigger;
    for (const AutocompleteToggle &toggle : m_autocompleteToggles) {
        root[toggle.key] = m_autocompleteEnabledByEdit.value(toggle.edit, true);
    }
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.close();
    }
}

QMap<QString, int> PromptTemplateLibraryWidget::tagCountsFromPrompt(const QString &prompt) const
{
    QMap<QString, int> counts;
    for (const QString &tag : parsePromptTags(prompt)) {
        counts[tag] += 1;
    }
    return counts;
}

QStringList PromptTemplateLibraryWidget::selectedGenerateImageTags(bool positiveTarget) const
{
    QSet<QString> tags;
    TagFlowWidget *source = positiveTarget ? m_generateImagePositiveTags : m_generateImageNegativeTags;
    if (source) tags.unite(source->getSelectedTags());
    QStringList out = tags.values();
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
        return QString::compare(a, b, Qt::CaseInsensitive) < 0;
    });
    return out;
}

QStringList PromptTemplateLibraryWidget::selectedTemplateImageTags(bool positiveTarget) const
{
    QSet<QString> tags;
    TagFlowWidget *source = positiveTarget ? m_templateImagePositiveTags : m_templateImageNegativeTags;
    if (source) tags.unite(source->getSelectedTags());
    QStringList out = tags.values();
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
        return QString::compare(a, b, Qt::CaseInsensitive) < 0;
    });
    return out;
}

void PromptTemplateLibraryWidget::addGenerateImageTagsToPrompt(bool positiveTarget)
{
    const QStringList tags = selectedGenerateImageTags(positiveTarget);
    if (tags.isEmpty()) {
        ui->lblGenerateImageStatus->setText(positiveTarget
            ? "请先在正面 TagFlow 中选择要添加的 Tag。"
            : "请先在负面 TagFlow 中选择要添加的 Tag。");
        return;
    }
    QPlainTextEdit *target = positiveTarget ? ui->textGeneratedPositive : ui->textGeneratedNegative;
    const int added = appendUniquePromptTags(target, tags);
    if (added <= 0) {
        ui->lblGenerateImageStatus->setText(positiveTarget
            ? "选中的 Tag 已存在于正面提示词中。"
            : "选中的 Tag 已存在于负面提示词中。");
        return;
    }
    ui->lblGenerateImageStatus->setText(QString("已添加 %1 个新 Tag 到%2提示词。")
        .arg(added)
        .arg(positiveTarget ? "正面" : "负面"));
}

void PromptTemplateLibraryWidget::addTemplateImageTagsToTemplate(bool positiveTarget)
{
    const QStringList tags = selectedTemplateImageTags(positiveTarget);
    if (tags.isEmpty()) {
        ui->lblTemplateImageStatus->setText(positiveTarget
            ? "请先在正面 TagFlow 中选择要添加到模板的 Tag。"
            : "请先在负面 TagFlow 中选择要添加到模板的 Tag。");
        return;
    }

    QPlainTextEdit *target = positiveTarget ? ui->textTemplatePositive : ui->textTemplateNegative;
    const int added = appendUniquePromptTags(target, tags);
    if (added <= 0) {
        ui->lblTemplateImageStatus->setText(positiveTarget
            ? "选中的 Tag 已存在于正面模板中。"
            : "选中的 Tag 已存在于负面模板中。");
        return;
    }
    ui->lblTemplateImageStatus->setText(QString("已添加 %1 个新 Tag 到%2模板。")
        .arg(added)
        .arg(positiveTarget ? "正面" : "负面"));
}

QStringList PromptTemplateLibraryWidget::selectedTagTexts(const TagPickerUi &picker) const
{
    QStringList tags;
    if (!picker.table) return tags;
    const auto ranges = picker.table->selectedRanges();
    QSet<int> seenRows;
    for (const QTableWidgetSelectionRange &range : ranges) {
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) {
            if (seenRows.contains(row)) continue;
            seenRows.insert(row);
            if (auto *item = picker.table->item(row, 0)) tags << item->text();
        }
    }
    return tags;
}

void PromptTemplateLibraryWidget::loadTagPickerRows(TagPickerUi &picker, bool force)
{
    if (!picker.table || !picker.scope || !picker.status) return;
    const int scope = picker.scope->currentIndex();
    if (m_tagWatcher) {
        picker.status->setText("Tag 统计正在加载中...");
        return;
    }
    if (!force && !m_allTagRows.isEmpty() && m_loadedTagScope == scope) {
        refreshTagPickerTable(picker);
        return;
    }
    picker.status->setText("正在读取 user_gallery_cache.json 并统计常用 Tag...");
    picker.table->setRowCount(0);
    const QString cachePath = qApp->applicationDirPath() + "/config/user_gallery_cache.json";
    m_tagWatcher = new QFutureWatcher<QVector<TagUsageRow>>(this);
    connect(m_tagWatcher, &QFutureWatcher<QVector<TagUsageRow>>::finished, this, [this, &picker, scope]() {
        if (!m_tagWatcher) return;
        m_allTagRows = m_tagWatcher->result();
        m_loadedTagScope = scope;
        m_tagWatcher->deleteLater();
        m_tagWatcher = nullptr;
        refreshTagPickerTable(picker);
    });
    m_tagWatcher->setFuture(QtConcurrent::run([cachePath, scope]() {
        return readTagRowsWorker(cachePath, scope);
    }));
}

void PromptTemplateLibraryWidget::refreshTagPickerTable(TagPickerUi &picker)
{
    if (!picker.table || !picker.search || !picker.status) return;

    const int pickerScope = picker.scope ? picker.scope->currentIndex() : -1;
    if (m_loadedTagScope != pickerScope) return;

    QVector<TagUsageRow> rows = m_allTagRows;
    const QString needle = normalizeTagSearch(picker.search->text());

    if (!needle.isEmpty()) {
        QVector<TagUsageRow> filtered;

        for (const TagUsageRow &row : rows) {
            const QString displayTranslation = translatedTextForTag(row.tag);
            const QString cleanedTranslation = removeLeadingTagFromDisplayText(row.tag, displayTranslation);

            QString category;
            QString translation;
            splitCategoryAndTranslationText(cleanedTranslation, category, translation);

            const QString tagText = normalizeTagSearch(row.tag);
            const QString translationText = normalizeTagSearch(translation);

            // 只匹配 Tag 和翻译。
            // 不匹配类型、类别、优先级、使用次数。
            if (tagText.contains(needle) || translationText.contains(needle)) {
                filtered.append(row);
            }
        }

        rows = filtered;
    }

    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        if (a.count != b.count) return a.count > b.count;
        return QString::compare(a.tag, b.tag, Qt::CaseInsensitive) < 0;
    });

    const int sortColumn = picker.table->horizontalHeader()->sortIndicatorSection() >= 0
                               ? picker.table->horizontalHeader()->sortIndicatorSection()
                               : 4;
    const Qt::SortOrder sortOrder = picker.table->horizontalHeader()->sortIndicatorOrder();

    picker.table->setSortingEnabled(false);
    picker.table->setRowCount(0);
    picker.table->setColumnCount(5);
    picker.table->setHorizontalHeaderLabels({"Tag", "类型", "类别", "翻译", "使用次数"});

    for (const TagUsageRow &rowData : rows) {
        const QString displayTranslation = translatedTextForTag(rowData.tag);
        const QString cleanedTranslation = removeLeadingTagFromDisplayText(rowData.tag, displayTranslation);

        QString category;
        QString translation;
        splitCategoryAndTranslationText(cleanedTranslation, category, translation);

        const int row = picker.table->rowCount();
        picker.table->insertRow(row);

        picker.table->setItem(row, 0, makePromptTemplateTableItem(rowData.tag));
        picker.table->setItem(row, 1, makePromptTemplateTableItem(rowData.kind));
        picker.table->setItem(row, 2, makePromptTemplateTableItem(category));
        picker.table->setItem(row, 3, makePromptTemplateTableItem(translation));
        picker.table->setItem(row, 4, makePromptTemplateTableItem(rowData.count));
    }

    picker.table->setSortingEnabled(true);
    picker.table->sortItems(sortColumn, sortOrder);

    picker.status->setText(rows.isEmpty()
                               ? "未找到常用 Tag。请先扫描本地图库，或调整搜索条件。"
                               : QString("显示 %1 条常用 Tag").arg(rows.size()));
}

void PromptTemplateLibraryWidget::addPickerTags(TagPickerUi &picker, bool positiveTarget)
{
    const QStringList tags = selectedTagTexts(picker);
    if (tags.isEmpty()) {
        if (picker.status) picker.status->setText("请先在表格中选择 Tag。");
        return;
    }

    QPlainTextEdit *target = nullptr;
    QLabel *status = picker.status;
    QString targetName;
    if (picker.insertIntoTemplate) {
        target = positiveTarget ? ui->textTemplatePositive : ui->textTemplateNegative;
        targetName = positiveTarget ? "正面模板" : "负面模板";
    } else {
        target = positiveTarget ? ui->textGeneratedPositive : ui->textGeneratedNegative;
        targetName = positiveTarget ? "正面提示词" : "负面提示词";
        status = ui->lblGenerateStatus;
    }

    const int added = appendUniquePromptTags(target, tags);
    if (status) {
        status->setText(added == 0
            ? QString("%1中已经包含选中的 Tag。").arg(targetName)
            : QString("已添加 %1 个新 Tag 到%2。").arg(added).arg(targetName));
    }
}

void PromptTemplateLibraryWidget::addCurrentPromptToFavorites()
{
    const QString positive = ui->textGeneratedPositive->toPlainText().trimmed();
    const QString negative = ui->textGeneratedNegative->toPlainText().trimmed();
    if (positive.isEmpty() && negative.isEmpty()) {
        ui->lblGenerateStatus->setText("当前提示词为空，无法添加收藏。");
        return;
    }

    QString defaultName = ui->comboGenerateTemplate->currentText().trimmed();
    if (defaultName.isEmpty()) defaultName = "Favorite Prompt";
    bool ok = false;
    const QString name = QInputDialog::getText(this,
                                               "添加提示词收藏",
                                               "收藏名称:",
                                               QLineEdit::Normal,
                                               defaultName,
                                               &ok).trimmed();
    if (!ok) return;

    PromptFavorite item;
    item.id = ensureId("favorite");
    item.name = name.isEmpty() ? defaultName : name;
    item.positive = positive;
    item.negative = negative;
    item.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    item.updatedAt = item.createdAt;
    m_favorites.prepend(item);
    saveLibrary();
    refreshFavoritesTable();
    ui->tabGenerateTools->setCurrentWidget(ui->tabGenerateFavorites);
    ui->lblGenerateStatus->setText("已添加到提示词收藏。");
}

void PromptTemplateLibraryWidget::copyFavoriteById(const QString &id) const
{
    for (const PromptFavorite &item : m_favorites) {
        if (item.id != id) continue;
        QApplication::clipboard()->setText("Positive prompt:\n" + item.positive + "\n\nNegative prompt:\n" + item.negative);
        if (ui->lblPromptFavoritesStatus) ui->lblPromptFavoritesStatus->setText("已复制收藏提示词。");
        return;
    }
}

void PromptTemplateLibraryWidget::replacePromptFromFavoriteById(const QString &id)
{
    for (const PromptFavorite &item : std::as_const(m_favorites)) {
        if (item.id != id) continue;
        m_loadingUi = true;
        ui->textGeneratedPositive->setPlainText(item.positive);
        ui->textGeneratedNegative->setPlainText(item.negative);
        m_lastRenderedPositivePrompt = item.positive;
        m_lastRenderedNegativePrompt = item.negative;
        m_loadingUi = false;
        ui->lblPromptFavoritesStatus->setText("已用收藏替换当前提示词。");
        ui->lblGenerateStatus->setText("已用收藏替换当前提示词。");
        return;
    }
}

void PromptTemplateLibraryWidget::deleteFavoriteById(const QString &id)
{
    for (int i = 0; i < m_favorites.size(); ++i) {
        if (m_favorites.at(i).id != id) continue;
        if (QMessageBox::question(this,
                                  "删除收藏",
                                  QString("确定删除收藏“%1”吗？").arg(m_favorites.at(i).name))
            != QMessageBox::Yes) {
            return;
        }
        m_favorites.removeAt(i);
        saveLibrary();
        refreshFavoritesTable();
        return;
    }
}

void PromptTemplateLibraryWidget::parseGenerateImageTags()
{
    const QString path = ui->editGenerateImagePath->text().trimmed();
    if (path.isEmpty() || !QFile::exists(path)) {
        ui->lblGenerateImageStatus->setText("请先选择存在的图片。");
        return;
    }

    const ParsedImageMetadata meta = parseImageMetadataFromFile(path);
    const QMap<QString, int> positive = tagCountsFromPrompt(meta.positivePrompt);
    const QMap<QString, int> negative = tagCountsFromPrompt(meta.negativePrompt);
    if (m_generateImagePositiveTags) m_generateImagePositiveTags->setData(positive);
    if (m_generateImageNegativeTags) m_generateImageNegativeTags->setData(negative);
    if (m_generateImagePositiveTags) m_generateImagePositiveTags->setShowTranslation(ui->chkGenerateImageTranslation->isChecked());
    if (m_generateImageNegativeTags) m_generateImageNegativeTags->setShowTranslation(ui->chkGenerateImageTranslation->isChecked());

    const int total = positive.size() + negative.size();
    ui->lblGenerateImageStatus->setText(total == 0
        ? "图片元数据中没有解析出可用 Tag。"
        : QString("已解析 %1 个正面 Tag，%2 个负面 Tag。").arg(positive.size()).arg(negative.size()));
}

void PromptTemplateLibraryWidget::parseTemplateImageTags()
{
    const QString path = ui->editTemplateImagePath->text().trimmed();
    if (path.isEmpty() || !QFile::exists(path)) {
        ui->lblTemplateImageStatus->setText("请先选择存在的图片。");
        return;
    }

    const ParsedImageMetadata meta = parseImageMetadataFromFile(path);
    const QMap<QString, int> positive = tagCountsFromPrompt(meta.positivePrompt);
    const QMap<QString, int> negative = tagCountsFromPrompt(meta.negativePrompt);
    if (m_templateImagePositiveTags) m_templateImagePositiveTags->setData(positive);
    if (m_templateImageNegativeTags) m_templateImageNegativeTags->setData(negative);
    if (m_templateImagePositiveTags) m_templateImagePositiveTags->setShowTranslation(ui->chkTemplateImageTranslation->isChecked());
    if (m_templateImageNegativeTags) m_templateImageNegativeTags->setShowTranslation(ui->chkTemplateImageTranslation->isChecked());

    const int total = positive.size() + negative.size();
    ui->lblTemplateImageStatus->setText(total == 0
        ? "未从图片中解析到可用 Tag。"
        : QString("已解析 %1 个正面 Tag，%2 个负面 Tag，可选择后添加到当前模板。").arg(positive.size()).arg(negative.size()));
}

void PromptTemplateLibraryWidget::onTabChanged(int index)
{
    if (ui->tabTemplateLibrary->widget(index) == ui->tabGenerate &&
        ui->tabGenerateTools->currentWidget() == ui->tabGenerateTagPicker) {
        loadTagPickerRows(m_generateTagPicker, false);
    } else if (ui->tabTemplateLibrary->widget(index) == ui->tabTemplates &&
        ui->tabTemplateManageSide->currentWidget() == m_templateTagPicker.page) {
        loadTagPickerRows(m_templateTagPicker, false);
    }
}

void PromptTemplateLibraryWidget::onGenerateTemplateChanged(int)
{
    rebuildPlaceholderInputs();
    updateGeneratedPrompt();
}

void PromptTemplateLibraryWidget::onTemplateListCurrentRowChanged(int)
{
    updateTemplateEditorFromSelection();
}

void PromptTemplateLibraryWidget::onSaveTemplateClicked()
{
    const QString currentTemplateId =
        ui->listTemplates->currentItem()
            ? ui->listTemplates->currentItem()->data(Qt::UserRole).toString()
            : QString();

    saveCurrentTemplateEditor();
    saveLibrary();

    refreshGenerateTemplateCombo();
    refreshTemplateList();

    if (!currentTemplateId.isEmpty()) {
        for (int i = 0; i < ui->listTemplates->count(); ++i) {
            if (ui->listTemplates->item(i)->data(Qt::UserRole).toString() == currentTemplateId) {
                ui->listTemplates->setCurrentRow(i);
                break;
            }
        }
    }

    rebuildPlaceholderInputs();
    updateGeneratedPrompt();

    setStatus("模板已保存");
}
void PromptTemplateLibraryWidget::onNewTemplateClicked()
{
    PromptTemplate item;
    item.id = ensureId("template");
    item.name = "新模板";
    item.positiveTemplate = "{quality}, {subject}";
    item.negativeTemplate = "{negative_extra}";
    m_templates.append(item);
    saveLibrary();
    refreshAllLists();
    ui->listTemplates->setCurrentRow(ui->listTemplates->count() - 1);
}

void PromptTemplateLibraryWidget::onDuplicateTemplateClicked()
{
    const int index = templateIndexById(ui->listTemplates->currentItem() ? ui->listTemplates->currentItem()->data(Qt::UserRole).toString() : QString());
    if (index < 0) return;
    PromptTemplate item = m_templates.at(index);
    item.id = ensureId("template");
    item.name += " Copy";
    m_templates.append(item);
    saveLibrary();
    refreshAllLists();
    ui->listTemplates->setCurrentRow(ui->listTemplates->count() - 1);
}

void PromptTemplateLibraryWidget::onDeleteTemplateClicked()
{
    const int index = templateIndexById(ui->listTemplates->currentItem() ? ui->listTemplates->currentItem()->data(Qt::UserRole).toString() : QString());
    if (index < 0) return;
    if (QMessageBox::question(this, "确认删除", "确定删除当前模板吗？") != QMessageBox::Yes) return;
    m_templates.removeAt(index);
    if (m_templates.isEmpty()) createDefaultLibrary();
    saveLibrary();
    refreshAllLists();
}

void PromptTemplateLibraryWidget::onSavePlaceholderClicked()
{
    const int currentRow = ui->tablePlaceholders->currentRow();
    const QString nextName = ui->editPlaceholderName->text().trimmed();

    if (nextName.isEmpty()) {
        QMessageBox::information(this, "提示", "占位符名称不能为空。");
        return;
    }

    const int duplicateIndex = placeholderIndexByName(nextName);
    if (duplicateIndex >= 0 && duplicateIndex != currentRow) {
        QMessageBox::information(this, "提示", "占位符名称已存在。");
        return;
    }

    saveCurrentPlaceholderEditor();
    saveLibrary();

    refreshPlaceholderTableKeepingName(nextName);
    rebuildPlaceholderInputs();
    updateGeneratedPrompt();

    setStatus("占位符已保存");
}
void PromptTemplateLibraryWidget::onNewPlaceholderClicked()
{
    QString name = "new_placeholder";
    int suffix = 2;

    while (placeholderIndexByName(name) >= 0) {
        name = QString("new_placeholder_%1").arg(suffix++);
    }

    PromptPlaceholder item;
    item.id = ensureId("placeholder");
    item.name = name;
    item.label = "新占位符";
    item.type = PlaceholderType::Text;

    m_placeholders.append(item);

    saveLibrary();
    refreshAllLists();

    const int row = placeholderIndexByName(name);
    if (row >= 0) {
        ui->tablePlaceholders->setCurrentCell(row, 0);
        ui->tablePlaceholders->selectRow(row);

        if (QTableWidgetItem *cell = ui->tablePlaceholders->item(row, 0)) {
            ui->tablePlaceholders->scrollToItem(cell, QAbstractItemView::PositionAtCenter);
        }

        updatePlaceholderEditorFromSelection();

        ui->editPlaceholderName->setFocus();
        ui->editPlaceholderName->selectAll();
    }
}

void PromptTemplateLibraryWidget::onDeletePlaceholderClicked()
{
    const int row = ui->tablePlaceholders->currentRow();
    if (row < 0 || row >= m_placeholders.size()) return;
    if (QMessageBox::question(this, "确认删除", "确定删除当前占位符吗？模板中的 {name} 文本会保留，方便之后手动处理。") != QMessageBox::Yes) return;
    m_placeholders.removeAt(row);
    saveLibrary();
    refreshAllLists();
}

void PromptTemplateLibraryWidget::onTagPickerFiltersChanged(TagPickerUi &picker)
{
    if (sender() == picker.scope) {
        m_allTagRows.clear();
        m_loadedTagScope = -1;
        loadTagPickerRows(picker, true);
        return;
    }
    const int scope = picker.scope ? picker.scope->currentIndex() : -1;
    if (m_allTagRows.isEmpty() || m_loadedTagScope != scope) {
        loadTagPickerRows(picker, false);
    } else {
        refreshTagPickerTable(picker);
    }
}
