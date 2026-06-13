#include "prompttemplatelibrarywidget.h"
#include "tableviewstylehelper.h"
#include "ui_prompttemplatelibrarywidget.h"

#include "imagemetadataparser.h"
#include "tagflowwidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTextStream>
#include <QUrl>
#include <QtConcurrent>
#include <algorithm>

namespace {

QString normalizeTagSearch(QString text)
{
    text = text.toCaseFolded().trimmed();
    text.replace('_', ' ');
    text.replace('-', ' ');
    static const QRegularExpression spaces("\\s+");
    text.replace(spaces, " ");
    return text;
}

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

QString cleanPromptTag(QString tag)
{
    tag = tag.trimmed();
    if (tag.isEmpty()) return QString();
    static const QRegularExpression weightRegex(":[0-9.]+$");
    tag.remove(weightRegex);
    static const QRegularExpression bracketRegex("[\\{\\}\\[\\]\\(\\)]");
    tag.remove(bracketRegex);
    return tag.trimmed();
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
        const QString tag = cleanPromptTag(part);
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
    return normalizeTagSearch(cleanPromptTag(tag));
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
        const QString tag = cleanPromptTag(rawTag);
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
    addCandidate(cleanPromptTag(tag));

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
    });
    connect(ui->tabGenerateTools, &QTabWidget::currentChanged, this, [this](int index) {
        if (ui->tabGenerateTools->widget(index) == ui->tabGenerateTagPicker) loadTagPickerRows(m_generateTagPicker, false);
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
    refreshTagPickerTable(m_generateTagPicker);
    refreshTagPickerTable(m_templateTagPicker);
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

    QJsonObject root;
    root["version"] = 1;
    root["templates"] = templateArray;
    root["placeholders"] = placeholderArray;
    root["image_extract_templates"] = imageArray;

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
    rebuildPlaceholderInputs();
    updateGeneratedPrompt();
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

    const QHash<QString, QString> defaults = currentTemplate.placeholderDefaults;

    bool addedAny = false;

    for (const PromptPlaceholder &placeholder : std::as_const(m_placeholders)) {
        if (!usedNameSet.contains(placeholder.name)) continue;

        addedAny = true;

        auto *row = new QWidget(ui->placeholderInputContainer);
        auto *layout = new QVBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 8);
        layout->addWidget(new QLabel(QString("%1  {%2}").arg(placeholder.label, placeholder.name), row));

        const QString value = defaults.value(placeholder.name, placeholder.defaultValue);

        if (placeholder.type == PlaceholderType::Text) {
            auto *line = new QLineEdit(row);
            line->setText(value);
            connect(line, &QLineEdit::textChanged, this, &PromptTemplateLibraryWidget::updateGeneratedPrompt);
            layout->addWidget(line);
            m_placeholderEditors.insert(placeholder.name, line);
        } else if (placeholder.type == PlaceholderType::SingleChoice) {
            auto *combo = new QComboBox(row);
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
            auto *container = new QWidget(row);
            auto *boxLayout = new QVBoxLayout(container);
            boxLayout->setContentsMargins(0, 0, 0, 0);

            QSet<QString> selected;
            for (const QString &part : value.split(',', Qt::SkipEmptyParts)) {
                selected.insert(part.trimmed());
            }

            for (const QString &option : placeholder.options) {
                auto *box = new QCheckBox(placeholderOptionDisplayText(option), container);
                box->setProperty("optionValue", option);
                box->setToolTip(option.isEmpty() ? "空选项" : option);
                box->setChecked(selected.contains(option));
                connect(box, &QCheckBox::toggled, this, &PromptTemplateLibraryWidget::updateGeneratedPrompt);
                boxLayout->addWidget(box);
            }

            layout->addWidget(container);
            m_placeholderEditors.insert(placeholder.name, container);
        }

        ui->layoutPlaceholderInputs->addWidget(row);
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
