#include "tagbrowserwidget.h"
#include "styleconstants.h"
#include "tableviewstylehelper.h"
#include "ui_tagbrowserwidget.h"

#include <QtConcurrent/QtConcurrent>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSet>
#include <QStringConverter>
#include <QTextStream>
#include <QShowEvent>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <QSignalBlocker>

namespace {
QStringList parseCsvLineWorker(const QString &line)
{
    QStringList parts;
    QString current;
    bool inQuotes = false;

    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (ch == '"') {
            if (inQuotes && i + 1 < line.size() && line.at(i + 1) == '"') {
                current += '"';
                ++i;
            } else {
                inQuotes = !inQuotes;
            }
        } else if (ch == ',' && !inQuotes) {
            parts.append(current.trimmed());
            current.clear();
        } else {
            current += ch;
        }
    }
    parts.append(current.trimmed());
    return parts;
}

bool isIntegerTextWorker(const QString &text)
{
    if (text.isEmpty()) return false;
    for (const QChar ch : text) {
        if (!ch.isDigit()) return false;
    }
    return true;
}

QString removeLeadingTagFromDisplayWorker(const QString &tag, QString display)
{
    display = display.trimmed();

    const QString prefix1 = tag + " ";
    const QString prefix2 = tag + "\t";

    if (display.startsWith(prefix1, Qt::CaseSensitive)) {
        return display.mid(prefix1.size()).trimmed();
    }
    if (display.startsWith(prefix2, Qt::CaseSensitive)) {
        return display.mid(prefix2.size()).trimmed();
    }

    return display;
}

void splitCategoryAndTranslationWorker(const QString &text, QString &category, QString &translation)
{
    QString value = text.trimmed();
    category.clear();
    translation.clear();

    if (value.isEmpty()) return;

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

TagTranslationRow parseTagCsvRowWorker(const QStringList &parts)
{
    TagTranslationRow row;

    row.tag = parts.value(0).trimmed();

    QString displayOrTranslation;
    QString count;

    // 新格式：
    // 1girl,1girl 人物-一个女孩,4114588
    //
    // 只有最后一列是纯数字时，才认为它是优先级。
    if (parts.size() >= 3 && isIntegerTextWorker(parts.last().trimmed())) {
        count = parts.last().trimmed();
        displayOrTranslation = parts.mid(1, parts.size() - 2).join(",").trimmed();
    } else {
        // 旧格式：
        // 1girl,一个女孩
        //
        // 兼容未加引号但翻译里带逗号的旧数据。
        displayOrTranslation = parts.mid(1).join(",").trimmed();
    }

    QString cleaned = removeLeadingTagFromDisplayWorker(row.tag, displayOrTranslation);
    splitCategoryAndTranslationWorker(cleaned, row.category, row.translation);
    row.count = count;

    return row;
}

QVector<TagTranslationRow> readCsvRowsWorker(const QString &csvPath)
{
    QVector<TagTranslationRow> rows;
    if (csvPath.isEmpty() || !QFile::exists(csvPath)) return rows;

    QFile file(csvPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return rows;

    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);

    int rowIndex = 0;
    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line.trimmed().isEmpty()) continue;

        QStringList parts = parseCsvLineWorker(line);
        if (parts.isEmpty()) continue;

        QString tagText = parts.value(0).trimmed();
        if (rowIndex == 0 && tagText.startsWith(QChar(0xFEFF))) {
            tagText.remove(0, 1);
            parts[0] = tagText;
        }

        // 只跳过真正的表头，不再误删 tag,1705 这种真实 tag。
        if (rowIndex == 0) {
            const QString first = parts.value(0).trimmed().toLower();
            const QString second = parts.value(1).trimmed().toLower();
            if ((first == "tag" || first == "name") &&
                (second == "translation" || second == "count" || second == "post_count" || second == "postcount")) {
                ++rowIndex;
                continue;
            }
        }

        TagTranslationRow row = parseTagCsvRowWorker(parts);
        if (!row.tag.isEmpty()) {
            rows.append(row);
        }

        ++rowIndex;
    }

    return rows;
}

QString cleanUserTagTextWorker(QString tag)
{
    tag = tag.trimmed();
    if (tag.isEmpty()) return QString();

    static const QSet<QString> emoticons = {":)", ":-)", ":(", ":-(", "^_^", "T_T", "o_o", "O_O"};
    if (emoticons.contains(tag)) return tag;

    static QRegularExpression weightRegex(":[0-9.]+$");
    tag.remove(weightRegex);

    static QRegularExpression bracketRegex("[\\{\\}\\[\\]\\(\\)]");
    tag.remove(bracketRegex);

    return tag.trimmed();
}

QStringList parseUserPromptTagsWorker(const QString &prompt)
{
    QString normalized = prompt;
    normalized.replace("\r\n", ",");
    normalized.replace('\n', ',');
    normalized.replace('\r', ',');

    static const QSet<QString> blockedTags = {
        "BREAK", "ADDCOMM", "ADDBASE", "ADDCOL", "ADDROW"
    };

    QStringList tags;
    const QStringList parts = normalized.split(',', Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QString tag = cleanUserTagTextWorker(part);
        if (tag.isEmpty()) continue;
        bool blocked = false;
        for (const QString &blockedTag : blockedTags) {
            if (tag.compare(blockedTag, Qt::CaseInsensitive) == 0) {
                blocked = true;
                break;
            }
        }
        if (!blocked) tags.append(tag);
    }
    return tags;
}

void addPromptTagCounts(const QString &prompt, QMap<QString, int> &counts)
{
    if (prompt.trimmed().isEmpty()) return;
    for (const QString &tag : parseUserPromptTagsWorker(prompt)) {
        counts[tag]++;
    }
}

void appendUserTagRows(const QMap<QString, int> &counts, const QString &kind, QVector<UserTagUsageRow> &rows)
{
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        UserTagUsageRow row;
        row.tag = it.key();
        row.kind = kind;
        row.count = it.value();
        rows.append(row);
    }
}

QVector<UserTagUsageRow> readUserTagRowsWorker(const QString &cachePath, int scope)
{
    QVector<UserTagUsageRow> rows;
    QFile file(cachePath);
    if (!file.open(QIODevice::ReadOnly)) return rows;

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    QMap<QString, int> positiveCounts;
    QMap<QString, int> negativeCounts;
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        const QJsonObject obj = it.value().toObject();
        if (scope == 0 || scope == 2) {
            addPromptTagCounts(obj["p"].toString(), positiveCounts);
        }
        if (scope == 1 || scope == 2) {
            addPromptTagCounts(obj["np"].toString(), negativeCounts);
        }
    }

    if (scope == 0 || scope == 2) appendUserTagRows(positiveCounts, "正面", rows);
    if (scope == 1 || scope == 2) appendUserTagRows(negativeCounts, "负面", rows);

    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        if (a.count != b.count) return a.count > b.count;
        if (a.tag != b.tag) return a.tag < b.tag;
        return a.kind < b.kind;
    });
    return rows;
}
}

TagSearchProxyModel::TagSearchProxyModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setFilterCaseSensitivity(Qt::CaseInsensitive);
    setFilterKeyColumn(-1);
    setDynamicSortFilter(false);
}

void TagSearchProxyModel::setSearchText(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (m_searchText == trimmed) return;
    m_searchText = trimmed;
    m_normalizedSearchText = normalizedSearchText(trimmed);
    m_wordSearchText = m_normalizedSearchText.isEmpty()
                           ? QString()
                           : QString(" %1 ").arg(m_normalizedSearchText);
    QSortFilterProxyModel::setFilterFixedString(trimmed);
}

void TagSearchProxyModel::setMatchMode(int mode)
{
    MatchMode nextMode = ContainsMatch;
    if (mode == WordMatch) nextMode = WordMatch;
    else if (mode == ExactMatch) nextMode = ExactMatch;

    if (m_matchMode == nextMode) return;
    m_matchMode = nextMode;
    invalidateFilter();
}

QString TagSearchProxyModel::normalizedSearchText(const QString &text)
{
    const QString folded = text.toCaseFolded().trimmed();
    QString normalized;
    normalized.reserve(folded.size());

    bool lastWasSeparator = true;
    for (const QChar ch : folded) {
        const bool isSeparator = ch.isSpace()
                                 || ch == '_'
                                 || ch == '-'
                                 || ch == '.'
                                 || ch == ','
                                 || ch == ';'
                                 || ch == ':'
                                 || ch == '!'
                                 || ch == '?'
                                 || ch == '|'
                                 || ch == '/'
                                 || ch == '\\'
                                 || ch == '('
                                 || ch == ')'
                                 || ch == '['
                                 || ch == ']'
                                 || ch == '{'
                                 || ch == '}'
                                 || ch == '<'
                                 || ch == '>'
                                 || ch == '"';

        if (isSeparator) {
            if (!lastWasSeparator) normalized.append(' ');
            lastWasSeparator = true;
        } else {
            normalized.append(ch);
            lastWasSeparator = false;
        }
    }

    if (normalized.endsWith(' ')) normalized.chop(1);
    return normalized;
}

bool TagSearchProxyModel::matchesText(const QString &value) const
{
    if (m_normalizedSearchText.isEmpty()) return true;

    const QString haystack = normalizedSearchText(value);
    if (haystack.isEmpty()) return false;

    switch (m_matchMode) {
    case ExactMatch:
        return haystack == m_normalizedSearchText;
    case WordMatch:
        return QString(" %1 ").arg(haystack).contains(m_wordSearchText);
    case ContainsMatch:
    default:
        return haystack.contains(m_normalizedSearchText);
    }
}

bool TagSearchProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (m_searchText.isEmpty()) return true;
    if (!sourceModel()) return true;

    const int columnCount = sourceModel()->columnCount(sourceParent);

    QVector<int> searchColumns;

    // 只搜索 Tag 和翻译列，避免匹配类别、优先级、使用次数等列。
    for (int column = 0; column < columnCount; ++column) {
        const QString header = sourceModel()
        ->headerData(column, Qt::Horizontal, Qt::DisplayRole)
            .toString()
            .trimmed();

        if (header == "Tag" || header == "翻译" || header == "Translation") {
            searchColumns.append(column);
        }
    }

    // 兜底兼容旧表：
    // 2列：Tag / Translation
    // 4列：Tag / 类别 / 翻译 / 优先级
    // 6列：Tag / 类型 / 类别 / 翻译 / 优先级 / 使用次数
    if (searchColumns.isEmpty()) {
        searchColumns.append(0);

        if (columnCount >= 6) {
            searchColumns.append(3);
        } else if (columnCount >= 4) {
            searchColumns.append(2);
        } else if (columnCount >= 2) {
            searchColumns.append(1);
        }
    }

    for (int column : searchColumns) {
        if (column < 0 || column >= columnCount) continue;

        const QModelIndex index = sourceModel()->index(sourceRow, column, sourceParent);
        const QString value = sourceModel()->data(index, Qt::DisplayRole).toString();

        if (matchesText(value)) {
            return true;
        }
    }

    return false;
}

TagBrowserWidget::TagBrowserWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::TagBrowserWidget)
    , m_model(new QStandardItemModel(this))
    , m_proxy(new TagSearchProxyModel(this))
    , m_userTagModel(new QStandardItemModel(this))
    , m_userTagProxy(new TagSearchProxyModel(this))
{
    ui->setupUi(this);
    setStyleSheet(AppStyle::loadQss(":/styles/toolpage.qss"));

    m_model->setColumnCount(4);
    m_model->setHorizontalHeaderLabels({"Tag", "类别", "翻译", "优先级"});

    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterKeyColumn(-1);
    m_proxy->setDynamicSortFilter(false);
    m_proxy->setSortRole(Qt::UserRole);

    ui->tableTags->setModel(m_proxy);
    applyUnifiedTableRowStyle(ui->tableTags);
    ui->tableTags->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableTags->setAlternatingRowColors(true);
    ui->tableTags->setShowGrid(false);
    ui->tableTags->setFocusPolicy(Qt::NoFocus);
    ui->tableTags->verticalHeader()->setVisible(false);
    QHeaderView *tagHeader = ui->tableTags->horizontalHeader();

    tagHeader->setStretchLastSection(false);
    tagHeader->setSectionResizeMode(QHeaderView::Interactive);
    tagHeader->setSectionsClickable(true);
    tagHeader->setSortIndicatorShown(false);

    tagHeader->resizeSection(0, 240);  // Tag
    tagHeader->resizeSection(1, 80);   // 类别
    tagHeader->resizeSection(2, 420);  // 翻译
    tagHeader->resizeSection(3, 90);   // 优先级

    tagHeader->setSectionResizeMode(0, QHeaderView::Interactive);
    tagHeader->setSectionResizeMode(1, QHeaderView::Fixed);
    tagHeader->setSectionResizeMode(2, QHeaderView::Stretch);
    tagHeader->setSectionResizeMode(3, QHeaderView::Fixed);

    ui->tableTags->setSortingEnabled(false);

    m_userTagModel->setColumnCount(6);
    m_userTagModel->setHorizontalHeaderLabels({"Tag", "类型", "类别", "翻译", "优先级", "使用次数"});

    m_userTagProxy->setSourceModel(m_userTagModel);
    m_userTagProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_userTagProxy->setFilterKeyColumn(-1);
    m_userTagProxy->setDynamicSortFilter(false);
    m_userTagProxy->setSortRole(Qt::UserRole);

    ui->tableUserTags->setModel(m_userTagProxy);
    applyUnifiedTableRowStyle(ui->tableUserTags);
    ui->tableUserTags->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableUserTags->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->tableUserTags->setAlternatingRowColors(true);
    ui->tableUserTags->setShowGrid(false);
    ui->tableUserTags->setFocusPolicy(Qt::NoFocus);
    ui->tableUserTags->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->tableUserTags->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->tableUserTags->verticalHeader()->setVisible(false);

    QHeaderView *userHeader = ui->tableUserTags->horizontalHeader();
    userHeader->setStretchLastSection(false);
    userHeader->setSectionsClickable(true);
    userHeader->setSortIndicatorShown(true);

    userHeader->setSectionResizeMode(0, QHeaderView::Interactive); // Tag
    userHeader->setSectionResizeMode(1, QHeaderView::Fixed);       // 类型
    userHeader->setSectionResizeMode(2, QHeaderView::Fixed);       // 类别
    userHeader->setSectionResizeMode(3, QHeaderView::Stretch);     // 翻译
    userHeader->setSectionResizeMode(4, QHeaderView::Fixed);       // 优先级
    userHeader->setSectionResizeMode(5, QHeaderView::Fixed);       // 使用次数

    userHeader->resizeSection(0, 220);
    userHeader->resizeSection(1, 70);
    userHeader->resizeSection(2, 80);
    userHeader->resizeSection(3, 320);
    userHeader->resizeSection(4, 90);
    userHeader->resizeSection(5, 90);

    ui->tableUserTags->setSortingEnabled(true);

    connect(ui->tabWidgetTagBrowser, &QTabWidget::currentChanged, this, &TagBrowserWidget::onTabChanged);
    connect(ui->editSearch, &QLineEdit::textChanged, this, &TagBrowserWidget::onSearchTextChanged);
    connect(ui->comboSearchMatchMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        m_proxy->setMatchMode(index);
    });
    connect(ui->tableTags->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int section) {
        if (m_loading) return;
        ensureCsvLoadedForEditing();

        QHeaderView *header = ui->tableTags->horizontalHeader();

        Qt::SortOrder nextOrder = Qt::AscendingOrder;
        if (m_tagSortSection == section) {
            nextOrder = (m_tagSortOrder == Qt::AscendingOrder)
            ? Qt::DescendingOrder
            : Qt::AscendingOrder;
        }

        m_tagSortSection = section;
        m_tagSortOrder = nextOrder;

        header->setSortIndicatorShown(true);
        header->setSortIndicator(section, nextOrder);
        m_proxy->sort(section, nextOrder);

        ui->tableTags->viewport()->update();
    });
    connect(ui->editUserTagSearch, &QLineEdit::textChanged, this, &TagBrowserWidget::onUserTagSearchTextChanged);
    connect(ui->comboUserTagSearchMatchMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        m_userTagProxy->setMatchMode(index);
    });
    connect(ui->comboUserTagScope, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &TagBrowserWidget::onUserTagScopeChanged);
    connect(ui->btnRefreshUserTags, &QPushButton::clicked, this, &TagBrowserWidget::onRefreshUserTagsClicked);
    connect(ui->tableUserTags, &QWidget::customContextMenuRequested, this, &TagBrowserWidget::onUserTagContextMenu);
    connect(ui->btnAdd, &QPushButton::clicked, this, &TagBrowserWidget::onAddRowClicked);
    connect(ui->btnDelete, &QPushButton::clicked, this, &TagBrowserWidget::onDeleteRowsClicked);
    connect(ui->btnResetSort, &QPushButton::clicked, this, &TagBrowserWidget::onResetSortClicked);
    connect(ui->btnReload, &QPushButton::clicked, this, &TagBrowserWidget::onReloadClicked);
    connect(ui->btnSave, &QPushButton::clicked, this, &TagBrowserWidget::onSaveClicked);
    connect(m_model, &QStandardItemModel::itemChanged, this, &TagBrowserWidget::onModelChanged);

    m_batchAppendTimer = new QTimer(this);
    m_batchAppendTimer->setInterval(0);
    connect(m_batchAppendTimer, &QTimer::timeout, this, &TagBrowserWidget::appendPendingRowsBatch);

    updateStatusLabel();
    updateUserTagStatusLabel();
}

TagBrowserWidget::~TagBrowserWidget()
{
    if (m_batchAppendTimer) m_batchAppendTimer->stop();
    ++m_loadGeneration;
    if (m_loadWatcher) {
        m_loadWatcher->disconnect(this);
        m_loadWatcher->cancel();
        m_loadWatcher->waitForFinished();
        delete m_loadWatcher;
        m_loadWatcher = nullptr;
    }
    ++m_userTagLoadGeneration;
    if (m_userTagWatcher) {
        m_userTagWatcher->disconnect(this);
        m_userTagWatcher->cancel();
        m_userTagWatcher->waitForFinished();
        delete m_userTagWatcher;
        m_userTagWatcher = nullptr;
    }
    delete ui;
}

void TagBrowserWidget::setCsvPath(const QString &path)
{
    const QString normalized = path.trimmed();
    const bool pathChanged = (m_csvPath != normalized);
    m_csvPath = normalized;
    ui->editCsvPath->setText(normalized);

    if (pathChanged) {
        m_dirty = false;
        m_loading = false;
        m_csvLoaded = false;
        m_pendingRows.clear();
        m_pendingRowIndex = 0;
        if (m_batchAppendTimer) m_batchAppendTimer->stop();
        m_model->removeRows(0, m_model->rowCount());
        if (m_proxy) m_proxy->invalidate();
    }

    if (isVisible()) {
        loadCsv();
    } else {
        updateStatusLabel();
    }
}

QString TagBrowserWidget::csvPath() const
{
    return m_csvPath;
}

void TagBrowserWidget::setMergedTranslationMap(const QHash<QString, QString> *map)
{
    m_mergedTranslationMap = map;
    updateUserTagTranslations();
}

void TagBrowserWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (ui->tabWidgetTagBrowser->currentWidget() == ui->tabTranslation && (!m_csvLoaded || ui->tableTags->model() == nullptr)) {
        loadCsv();
    } else if (ui->tabWidgetTagBrowser->currentWidget() == ui->tabUserTags && !m_userTagsLoaded && !m_userTagsLoading) {
        loadUserTags();
    }
}

void TagBrowserWidget::ensureCsvLoadedForEditing()
{
    if (m_csvLoaded || m_loading) return;
    if (m_csvPath.isEmpty() || !QFile::exists(m_csvPath)) return;
    loadCsv();
}

void TagBrowserWidget::setLoadingState(bool loading, const QString &message)
{
    m_loading = loading;
    ui->tableTags->setEnabled(!loading);
    ui->tableTags->setVisible(!loading);
    ui->editSearch->setEnabled(!loading);
    ui->comboSearchMatchMode->setEnabled(!loading);
    ui->btnAdd->setEnabled(!loading);
    ui->btnDelete->setEnabled(!loading);
    ui->btnResetSort->setEnabled(!loading);
    ui->btnSave->setEnabled(!loading);
    ui->lblEmptyState->setVisible(loading || m_model->rowCount() == 0);
    if (loading) {
        ui->lblEmptyState->setText(message.isEmpty() ? "正在加载词表，请稍候..." : message);
        ui->lblStatus->setText("词表加载中...");
    }
}

QString TagBrowserWidget::escapeCsvField(const QString &value) const
{
    QString text = value;
    if (text.contains('"')) text.replace("\"", "\"\"");
    if (text.contains(',') || text.contains('"') || text.contains('\n') || text.contains('\r')) {
        text = "\"" + text + "\"";
    }
    return text;
}

void TagBrowserWidget::loadCsv()
{
    m_csvLoaded = false;
    ++m_loadGeneration;
    m_pendingRows.clear();
    m_pendingRowIndex = 0;
    if (m_batchAppendTimer) m_batchAppendTimer->stop();
    const int generation = m_loadGeneration;
    m_model->removeRows(0, m_model->rowCount());
    m_proxy->setSearchText(QString());
    m_proxy->sort(-1);

    if (m_csvPath.isEmpty() || !QFile::exists(m_csvPath)) {
        m_loading = false;
        m_dirty = false;
        if (m_proxy) m_proxy->invalidate();
        updateStatusLabel();
        return;
    }

    setLoadingState(true, "正在加载词表，请稍候...");
    const QString csvPath = m_csvPath;

    if (m_loadWatcher) {
        m_loadWatcher->disconnect(this);
        m_loadWatcher->cancel();
        m_loadWatcher->waitForFinished();
        delete m_loadWatcher;
        m_loadWatcher = nullptr;
    }

    m_loadWatcher = new QFutureWatcher<QVector<TagTranslationRow>>(this);
    connect(m_loadWatcher, &QFutureWatcher<QVector<TagTranslationRow>>::finished, this, [this, generation, csvPath]() {
        if (!m_loadWatcher) return;
        const QVector<TagTranslationRow> rows = m_loadWatcher->result();
        m_loadWatcher->deleteLater();
        m_loadWatcher = nullptr;

        if (generation != m_loadGeneration || csvPath != m_csvPath) {
            return;
        }

        m_model->removeRows(0, m_model->rowCount());
        m_pendingRows = rows;
        m_pendingRowIndex = 0;
        ui->lblEmptyState->setText(QString("已读取 %1 条记录，正在填充表格...").arg(rows.size()));
        if (m_batchAppendTimer) {
            m_batchAppendTimer->start();
        } else {
            appendPendingRowsBatch();
        }
    });

    m_loadWatcher->setFuture(QtConcurrent::run([csvPath]() {
        return readCsvRowsWorker(csvPath);
    }));
}

void TagBrowserWidget::appendPendingRowsBatch()
{
    if (!m_loading) {
        if (m_batchAppendTimer) m_batchAppendTimer->stop();
        return;
    }

    constexpr int kRowsPerBatch = 300;
    const int end = qMin(m_pendingRowIndex + kRowsPerBatch, m_pendingRows.size());
    for (; m_pendingRowIndex < end; ++m_pendingRowIndex) {
        const auto &rowData = m_pendingRows[m_pendingRowIndex];

        QStandardItem *tagItem = new QStandardItem(rowData.tag);
        QStandardItem *categoryItem = new QStandardItem(rowData.category);
        QStandardItem *translationItem = new QStandardItem(rowData.translation);
        QStandardItem *countItem = new QStandardItem(rowData.count);

        tagItem->setData(rowData.tag, Qt::UserRole);
        categoryItem->setData(rowData.category, Qt::UserRole);
        translationItem->setData(rowData.translation, Qt::UserRole);

        bool ok = false;
        const int countValue = rowData.count.toInt(&ok);
        countItem->setData(ok ? countValue : 0, Qt::UserRole);

        QList<QStandardItem*> row;
        row << tagItem << categoryItem << translationItem << countItem;
        m_model->appendRow(row);
    }

    if (m_pendingRowIndex < m_pendingRows.size()) {
        ui->lblEmptyState->setText(QString("正在填充表格... %1 / %2").arg(m_pendingRowIndex).arg(m_pendingRows.size()));
        return;
    }

    if (m_batchAppendTimer) m_batchAppendTimer->stop();
    m_pendingRows.clear();
    m_pendingRowIndex = 0;
    m_dirty = false;
    m_csvLoaded = true;
    m_loading = false;

    m_proxy->setMatchMode(ui->comboSearchMatchMode->currentIndex());
    m_proxy->setSearchText(ui->editSearch->text());
    resetTagSort();
    if (m_userTagsLoaded) {
        updateUserTagTranslations();
        updateUserTagStatusLabel();
    }
    ui->tableTags->setVisible(true);
    ui->tableTags->setEnabled(true);
    ui->tableTags->reset();
    ui->tableTags->viewport()->update();
    setLoadingState(false);
    updateStatusLabel();
}

void TagBrowserWidget::updateStatusLabel()
{
    if (m_loading) {
        ui->lblStatus->setText("词表加载中...");
        ui->lblEmptyState->setVisible(true);
        ui->tableTags->setMinimumHeight(220);
        return;
    }

    QString status;
    if (m_csvPath.isEmpty()) {
        status = "未配置翻译表路径";
        ui->lblEmptyState->setText("未配置 Tag 翻译表路径。请先在设置页选择 CSV，或点击“新增”手动创建。");
    } else if (!QFile::exists(m_csvPath)) {
        status = "CSV 文件不存在";
        ui->lblEmptyState->setText("找不到当前 CSV 文件。请检查路径，或点击“新增”手动创建后保存。");
    } else if (!m_csvLoaded) {
        status = "词表未加载";
        ui->lblEmptyState->setText("为提升启动速度，词表将于进入/操作本页面时自动加载。");
    } else {
        status = QString("共 %1 条 Tag 记录%2")
                     .arg(m_model->rowCount())
                     .arg(m_dirty ? "  |  未保存修改" : "");
        ui->lblEmptyState->setText("CSV 已加载，但当前没有任何 Tag 记录。可点击“新增”开始编辑。");
    }
    ui->lblStatus->setText(status);
    bool empty = (m_model->rowCount() == 0);
    ui->lblEmptyState->setVisible(empty);
    ui->tableTags->setMinimumHeight(empty ? 220 : 0);
}

TagTranslationInfo translatedInfoForTag(const QString &tag, const QHash<QString, TagTranslationInfo> &infos)
{
    TagTranslationInfo info = infos.value(tag);

    if (info.translation.isEmpty() && info.category.isEmpty() && info.priority.isEmpty() && tag.contains(' ')) {
        QString key = tag;
        key.replace(' ', '_');
        info = infos.value(key);
    }

    if (info.translation.isEmpty() && info.category.isEmpty() && info.priority.isEmpty() && tag.contains('_')) {
        QString key = tag;
        key.replace('_', ' ');
        info = infos.value(key);
    }

    return info;
}

QHash<QString, TagTranslationInfo> TagBrowserWidget::currentTranslationInfoMap() const
{
    QHash<QString, TagTranslationInfo> out;

    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QString tag = m_model->item(row, 0) ? m_model->item(row, 0)->text().trimmed() : QString();
        if (tag.isEmpty()) continue;

        TagTranslationInfo info;
        info.category = m_model->item(row, 1) ? m_model->item(row, 1)->text().trimmed() : QString();
        info.translation = m_model->item(row, 2) ? m_model->item(row, 2)->text().trimmed() : QString();
        info.priority = m_model->item(row, 3) ? m_model->item(row, 3)->text().trimmed() : QString();

        if (!info.category.isEmpty() || !info.translation.isEmpty() || !info.priority.isEmpty()) {
            out.insert(tag, info);
        }
    }

    return out;
}

QString TagBrowserWidget::translatedTextForTag(const QString &tag, const QHash<QString, QString> &translations) const
{
    QString translated = translations.value(tag);
    if (translated.isEmpty() && tag.contains(' ')) {
        QString key = tag;
        key.replace(' ', '_');
        translated = translations.value(key);
    }
    if (translated.isEmpty() && tag.contains('_')) {
        QString key = tag;
        key.replace('_', ' ');
        translated = translations.value(key);
    }
    return translated;
}

void TagBrowserWidget::updateUserTagTranslations()
{
    const QHash<QString, TagTranslationInfo> infos = currentTranslationInfoMap();

    for (int row = 0; row < m_userTagModel->rowCount(); ++row) {
        const QString tag = m_userTagModel->item(row, 0) ? m_userTagModel->item(row, 0)->text() : QString();
        TagTranslationInfo info = translatedInfoForTag(tag, infos);
        if (info.translation.isEmpty() && m_mergedTranslationMap) {
            info.translation = translatedTextForTag(tag, *m_mergedTranslationMap);
        }

        QStandardItem *categoryItem = m_userTagModel->item(row, 2);
        QStandardItem *translationItem = m_userTagModel->item(row, 3);
        QStandardItem *priorityItem = m_userTagModel->item(row, 4);

        if (categoryItem) {
            categoryItem->setText(info.category);
            categoryItem->setData(info.category, Qt::UserRole);
        }

        if (translationItem) {
            translationItem->setText(info.translation);
            translationItem->setData(info.translation, Qt::UserRole);
        }

        if (priorityItem) {
            priorityItem->setText(info.priority);
            bool ok = false;
            const int value = info.priority.toInt(&ok);
            priorityItem->setData(ok ? value : 0, Qt::UserRole);
        }
    }

    if (m_userTagProxy) m_userTagProxy->invalidate();
}

void TagBrowserWidget::updateUserTagStatusLabel()
{
    if (m_userTagsLoading) {
        ui->lblUserTagEmptyState->setText("正在读取 user_gallery_cache.json 并统计 Tag...");
        ui->lblUserTagEmptyState->setVisible(true);
        ui->lblUserTagStatus->setText("用户 Tag 加载中...");
        ui->tableUserTags->setVisible(false);
        return;
    }

    const bool empty = m_userTagModel->rowCount() == 0;
    ui->lblUserTagEmptyState->setVisible(empty);
    ui->tableUserTags->setVisible(!empty);
    if (empty) {
        ui->lblUserTagEmptyState->setText("未找到用户使用 Tag。请先扫描本地图库生成 user_gallery_cache.json。");
    }
    ui->lblUserTagStatus->setText(m_userTagsLoaded
        ? QString("共 %1 条用户使用 Tag 记录").arg(m_userTagModel->rowCount())
        : "用户 Tag 未加载");
}

void TagBrowserWidget::loadUserTags()
{
    ++m_userTagLoadGeneration;
    const int generation = m_userTagLoadGeneration;
    m_userTagsLoading = true;
    m_userTagsLoaded = false;

    ui->tableUserTags->setSortingEnabled(false);

    m_userTagModel->removeRows(0, m_userTagModel->rowCount());
    updateUserTagStatusLabel();

    if (m_userTagWatcher) {
        m_userTagWatcher->disconnect(this);
        m_userTagWatcher->cancel();
        m_userTagWatcher->waitForFinished();
        delete m_userTagWatcher;
        m_userTagWatcher = nullptr;
    }

    const QString cachePath = qApp->applicationDirPath() + "/config/user_gallery_cache.json";
    const int scope = ui->comboUserTagScope->currentIndex();
    m_userTagWatcher = new QFutureWatcher<QVector<UserTagUsageRow>>(this);
    connect(m_userTagWatcher, &QFutureWatcher<QVector<UserTagUsageRow>>::finished, this, [this, generation]() {
        if (!m_userTagWatcher) return;
        const QVector<UserTagUsageRow> rows = m_userTagWatcher->result();
        m_userTagWatcher->deleteLater();
        m_userTagWatcher = nullptr;
        if (generation != m_userTagLoadGeneration) return;

        const QHash<QString, TagTranslationInfo> infos = currentTranslationInfoMap();
        m_userTagModel->removeRows(0, m_userTagModel->rowCount());

        for (const auto &rowData : rows) {
            const QString tag = rowData.tag;
            const int count = rowData.count;
            const TagTranslationInfo info = translatedInfoForTag(tag, infos);

            QList<QStandardItem*> row;

            QStandardItem *tagItem = new QStandardItem(tag);
            QStandardItem *kindItem = new QStandardItem(rowData.kind);
            QStandardItem *categoryItem = new QStandardItem(info.category);
            QStandardItem *translationItem = new QStandardItem(info.translation);
            QStandardItem *priorityItem = new QStandardItem(info.priority);
            QStandardItem *countItem = new QStandardItem(QString::number(count));

            tagItem->setData(tag, Qt::UserRole);
            kindItem->setData(rowData.kind == "负面" ? 1 : 0, Qt::UserRole);
            categoryItem->setData(info.category, Qt::UserRole);
            translationItem->setData(info.translation, Qt::UserRole);

            bool priorityOk = false;
            const int priorityValue = info.priority.toInt(&priorityOk);
            priorityItem->setData(priorityOk ? priorityValue : 0, Qt::UserRole);

            countItem->setData(count, Qt::UserRole);

            row << tagItem
                << kindItem
                << categoryItem
                << translationItem
                << priorityItem
                << countItem;

            m_userTagModel->appendRow(row);
        }

        m_userTagsLoading = false;
        m_userTagsLoaded = true;
        m_userTagProxy->setMatchMode(ui->comboUserTagSearchMatchMode->currentIndex());
        m_userTagProxy->setSearchText(ui->editUserTagSearch->text());

        // 默认按“用户实际使用次数”降序显示。
        // 后续用户点击表头时，QTableView 会自动按对应列排序。
        ui->tableUserTags->setSortingEnabled(true);
        ui->tableUserTags->sortByColumn(5, Qt::DescendingOrder);

        updateUserTagStatusLabel();
    });

    m_userTagWatcher->setFuture(QtConcurrent::run([cachePath, scope]() {
        return readUserTagRowsWorker(cachePath, scope);
    }));
}

QString TagBrowserWidget::escapeUserTagCsvField(const QString &value) const
{
    QString text = value;
    if (text.contains('"')) text.replace("\"", "\"\"");
    if (text.contains(',') || text.contains('"') || text.contains('\n') || text.contains('\r')) {
        text = "\"" + text + "\"";
    }
    return text;
}

QVector<UserTagUsageRow> TagBrowserWidget::allUserTagRows() const
{
    QVector<UserTagUsageRow> rows;
    rows.reserve(m_userTagModel->rowCount());

    for (int row = 0; row < m_userTagModel->rowCount(); ++row) {
        UserTagUsageRow item;
        item.tag = m_userTagModel->item(row, 0) ? m_userTagModel->item(row, 0)->text() : QString();
        item.kind = m_userTagModel->item(row, 1) ? m_userTagModel->item(row, 1)->text() : QString();
        item.category = m_userTagModel->item(row, 2) ? m_userTagModel->item(row, 2)->text() : QString();
        item.translation = m_userTagModel->item(row, 3) ? m_userTagModel->item(row, 3)->text() : QString();
        item.priority = m_userTagModel->item(row, 4) ? m_userTagModel->item(row, 4)->text() : QString();
        item.count = m_userTagModel->item(row, 5) ? m_userTagModel->item(row, 5)->text().toInt() : 0;

        if (!item.tag.isEmpty()) rows.append(item);
    }

    return rows;
}

QVector<UserTagUsageRow> TagBrowserWidget::visibleUserTagRows() const
{
    QVector<UserTagUsageRow> rows;
    rows.reserve(m_userTagProxy->rowCount());

    for (int row = 0; row < m_userTagProxy->rowCount(); ++row) {
        const QModelIndex sourceIndex = m_userTagProxy->mapToSource(m_userTagProxy->index(row, 0));
        if (!sourceIndex.isValid()) continue;

        const int sourceRow = sourceIndex.row();

        UserTagUsageRow item;
        item.tag = m_userTagModel->item(sourceRow, 0) ? m_userTagModel->item(sourceRow, 0)->text() : QString();
        item.kind = m_userTagModel->item(sourceRow, 1) ? m_userTagModel->item(sourceRow, 1)->text() : QString();
        item.category = m_userTagModel->item(sourceRow, 2) ? m_userTagModel->item(sourceRow, 2)->text() : QString();
        item.translation = m_userTagModel->item(sourceRow, 3) ? m_userTagModel->item(sourceRow, 3)->text() : QString();
        item.priority = m_userTagModel->item(sourceRow, 4) ? m_userTagModel->item(sourceRow, 4)->text() : QString();
        item.count = m_userTagModel->item(sourceRow, 5) ? m_userTagModel->item(sourceRow, 5)->text().toInt() : 0;

        if (!item.tag.isEmpty()) rows.append(item);
    }

    return rows;
}

QVector<UserTagUsageRow> TagBrowserWidget::selectedUserTagRows() const
{
    QVector<UserTagUsageRow> rows;
    if (!ui->tableUserTags->selectionModel()) return rows;

    QModelIndexList selected = ui->tableUserTags->selectionModel()->selectedRows(0);
    std::sort(selected.begin(), selected.end(), [](const QModelIndex &a, const QModelIndex &b) {
        return a.row() < b.row();
    });

    QSet<int> seenSourceRows;
    rows.reserve(selected.size());
    for (const QModelIndex &proxyIndex : selected) {
        const QModelIndex sourceIndex = m_userTagProxy->mapToSource(proxyIndex);
        if (!sourceIndex.isValid() || seenSourceRows.contains(sourceIndex.row())) continue;
        seenSourceRows.insert(sourceIndex.row());

        UserTagUsageRow item;
        item.tag = m_userTagModel->item(sourceIndex.row(), 0) ? m_userTagModel->item(sourceIndex.row(), 0)->text() : QString();
        item.kind = m_userTagModel->item(sourceIndex.row(), 1) ? m_userTagModel->item(sourceIndex.row(), 1)->text() : QString();
        item.category = m_userTagModel->item(sourceIndex.row(), 2) ? m_userTagModel->item(sourceIndex.row(), 2)->text() : QString();
        item.translation = m_userTagModel->item(sourceIndex.row(), 3) ? m_userTagModel->item(sourceIndex.row(), 3)->text() : QString();
        item.priority = m_userTagModel->item(sourceIndex.row(), 4) ? m_userTagModel->item(sourceIndex.row(), 4)->text() : QString();
        item.count = m_userTagModel->item(sourceIndex.row(), 5) ? m_userTagModel->item(sourceIndex.row(), 5)->text().toInt() : 0;

        if (!item.tag.isEmpty()) rows.append(item);
    }
    return rows;
}

void TagBrowserWidget::showUserTagExportDialog()
{
    const QVector<UserTagUsageRow> allRows = allUserTagRows();
    if (allRows.isEmpty()) {
        QMessageBox::information(nullptr, "提示", "没有可导出的 Tag。");
        return;
    }

    const QVector<UserTagUsageRow> selectedRows = selectedUserTagRows();
    QDialog dlg;
    dlg.setWindowTitle("批量导出 Tags");
    dlg.setMinimumWidth(420);

    QVBoxLayout *root = new QVBoxLayout(&dlg);
    root->addWidget(new QLabel("导出范围:", &dlg));

    QComboBox *rangeBox = new QComboBox(&dlg);
    if (!selectedRows.isEmpty()) rangeBox->addItem("选中项", "selected");
    rangeBox->addItem("当前筛选结果", "visible");
    rangeBox->addItem("全部", "all");
    rangeBox->addItem("Top K（按使用次数排序）", "top");
    rangeBox->addItem("使用次数 >= K", "count");
    root->addWidget(rangeBox);

    QHBoxLayout *valueRow = new QHBoxLayout();
    QLabel *valueLabel = new QLabel("K:", &dlg);
    QSpinBox *valueSpin = new QSpinBox(&dlg);
    int maxCount = 1;
    for (const auto &row : allRows) maxCount = qMax(maxCount, row.count);
    const int allRowCount = allRows.size();
    valueSpin->setRange(1, qMax(1, allRowCount));
    valueSpin->setValue(qMin(50, allRowCount));
    valueRow->addWidget(valueLabel);
    valueRow->addWidget(valueSpin, 1);
    root->addLayout(valueRow);

    root->addWidget(new QLabel("输出方式:", &dlg));
    QComboBox *outputBox = new QComboBox(&dlg);
    outputBox->addItem("复制到剪贴板", "copy");
    outputBox->addItem("导出 CSV", "csv");
    root->addWidget(outputBox);

    QLabel *columnsLabel = new QLabel("CSV 导出列:", &dlg);
    root->addWidget(columnsLabel);
    QHBoxLayout *columnsRow = new QHBoxLayout();

    QCheckBox *chkTag = new QCheckBox("Tag", &dlg);
    QCheckBox *chkType = new QCheckBox("类型", &dlg);
    QCheckBox *chkCategory = new QCheckBox("类别", &dlg);
    QCheckBox *chkTranslation = new QCheckBox("翻译", &dlg);
    QCheckBox *chkPriority = new QCheckBox("优先级", &dlg);
    QCheckBox *chkCount = new QCheckBox("使用次数", &dlg);

    chkTag->setChecked(true);
    chkType->setChecked(true);
    chkCategory->setChecked(true);
    chkTranslation->setChecked(true);
    chkPriority->setChecked(true);
    chkCount->setChecked(true);

    columnsRow->addWidget(chkTag);
    columnsRow->addWidget(chkType);
    columnsRow->addWidget(chkCategory);
    columnsRow->addWidget(chkTranslation);
    columnsRow->addWidget(chkPriority);
    columnsRow->addWidget(chkCount);

    columnsRow->addStretch(1);
    root->addLayout(columnsRow);

    auto refreshValueRow = [rangeBox, valueLabel, valueSpin, maxCount, count = allRowCount]() {
        const QString mode = rangeBox->currentData().toString();
        const bool needsValue = (mode == "top" || mode == "count");
        valueLabel->setEnabled(needsValue);
        valueSpin->setEnabled(needsValue);
        if (mode == "top") {
            valueLabel->setText("K:");
            valueSpin->setRange(1, qMax(1, count));
            valueSpin->setValue(qMin(valueSpin->value(), count));
        } else if (mode == "count") {
            valueLabel->setText("K:");
            valueSpin->setRange(1, maxCount);
            valueSpin->setValue(qMin(valueSpin->value(), maxCount));
        }
    };
    connect(rangeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, refreshValueRow);
    refreshValueRow();

    auto refreshColumnOptions = [outputBox, columnsLabel, chkTag, chkType, chkCategory, chkTranslation, chkPriority, chkCount]() {
        const bool csvMode = outputBox->currentData().toString() == "csv";
        columnsLabel->setEnabled(csvMode);
        chkTag->setEnabled(csvMode);
        chkType->setEnabled(csvMode);
        chkCategory->setEnabled(csvMode);
        chkTranslation->setEnabled(csvMode);
        chkPriority->setEnabled(csvMode);
        chkCount->setEnabled(csvMode);
    };
    connect(outputBox, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, refreshColumnOptions);
    refreshColumnOptions();

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    QVector<UserTagUsageRow> rows;
    const QString rangeMode = rangeBox->currentData().toString();
    if (rangeMode == "selected") {
        rows = selectedRows;
    } else if (rangeMode == "visible") {
        rows = visibleUserTagRows();
    } else if (rangeMode == "all") {
        rows = allRows;
    } else if (rangeMode == "top") {
        rows = allRows;
        std::sort(rows.begin(), rows.end(), [](const UserTagUsageRow &a, const UserTagUsageRow &b) {
            if (a.count != b.count) return a.count > b.count;
            if (a.tag != b.tag) return a.tag < b.tag;
            return a.kind < b.kind;
        });
        rows.resize(qMin(valueSpin->value(), int(rows.size())));
    } else {
        const int threshold = valueSpin->value();
        for (const auto &row : allRows) {
            if (row.count >= threshold) rows.append(row);
        }
    }

    if (rows.isEmpty()) {
        QMessageBox::information(nullptr, "提示", "没有符合条件的 Tag。");
        return;
    }

    if (outputBox->currentData().toString() == "copy") {
        QStringList tags;
        tags.reserve(rows.size());
        for (const auto &row : rows) tags.append(row.tag);
        QApplication::clipboard()->setText(tags.join(", "));
        return;
    }

    QString fileName = QFileDialog::getSaveFileName(nullptr, "导出 Tags 到 CSV", "", "CSV Files (*.csv)");
    if (fileName.isEmpty()) return;
    if (!fileName.endsWith(".csv", Qt::CaseInsensitive)) fileName += ".csv";

    QStringList headers;
    if (chkTag->isChecked()) headers << "Tag";
    if (chkType->isChecked()) headers << "Type";
    if (chkCategory->isChecked()) headers << "Category";
    if (chkTranslation->isChecked()) headers << "Translation";
    if (chkPriority->isChecked()) headers << "Priority";
    if (chkCount->isChecked()) headers << "Count";
    if (headers.isEmpty()) {
        QMessageBox::information(nullptr, "提示", "请至少选择一列导出。");
        return;
    }

    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(nullptr, "错误", "无法写入文件。");
        return;
    }

    file.write("\xEF\xBB\xBF", 3);
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << headers.join(",") << "\n";
    for (const auto &row : rows) {
        QStringList values;
        if (chkTag->isChecked()) values << escapeUserTagCsvField(row.tag);
        if (chkType->isChecked()) values << escapeUserTagCsvField(row.kind);
        if (chkCategory->isChecked()) values << escapeUserTagCsvField(row.category);
        if (chkTranslation->isChecked()) values << escapeUserTagCsvField(row.translation);
        if (chkPriority->isChecked()) values << escapeUserTagCsvField(row.priority);
        if (chkCount->isChecked()) values << QString::number(row.count);
        out << values.join(",") << "\n";
    }
    QMessageBox::information(nullptr, "成功", "导出成功！");
}

void TagBrowserWidget::onUserTagContextMenu(const QPoint &pos)
{
    if (m_userTagsLoading || !m_userTagsLoaded || m_userTagModel->rowCount() == 0) return;

    const QModelIndex clickedIndex = ui->tableUserTags->indexAt(pos);
    if (clickedIndex.isValid() &&
        ui->tableUserTags->selectionModel() &&
        !ui->tableUserTags->selectionModel()->isSelected(clickedIndex)) {
        ui->tableUserTags->selectionModel()->select(
            clickedIndex,
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }

    QMenu menu(this);
    if (!selectedUserTagRows().isEmpty()) {
        QAction *copySelected = menu.addAction("复制选中 Tags");
        connect(copySelected, &QAction::triggered, this, [this]() {
            QStringList tags;
            const QVector<UserTagUsageRow> rows = selectedUserTagRows();
            tags.reserve(rows.size());
            for (const auto &row : rows) tags.append(row.tag);
            QApplication::clipboard()->setText(tags.join(", "));
        });
    }

    QAction *batchExport = menu.addAction("批量导出 Tags...");
    connect(batchExport, &QAction::triggered, this, &TagBrowserWidget::showUserTagExportDialog);
    menu.exec(ui->tableUserTags->viewport()->mapToGlobal(pos));
}

void TagBrowserWidget::onTabChanged(int index)
{
    if (ui->tabWidgetTagBrowser->widget(index) == ui->tabTranslation) {
        if (!m_csvLoaded && !m_loading) loadCsv();
    } else if (ui->tabWidgetTagBrowser->widget(index) == ui->tabUserTags) {
        if (!m_userTagsLoaded && !m_userTagsLoading) loadUserTags();
    }
}

void TagBrowserWidget::onSearchTextChanged(const QString &text)
{
    if (m_loading) return;
    ensureCsvLoadedForEditing();
    m_proxy->setSearchText(text);
}

void TagBrowserWidget::onUserTagSearchTextChanged(const QString &text)
{
    if (m_userTagsLoading) return;
    if (!m_userTagsLoaded) loadUserTags();
    m_userTagProxy->setSearchText(text);
}

void TagBrowserWidget::onUserTagScopeChanged(int index)
{
    Q_UNUSED(index);
    if (m_userTagsLoading) return;
    if (m_userTagsLoaded || ui->tabWidgetTagBrowser->currentWidget() == ui->tabUserTags) {
        loadUserTags();
    }
}

void TagBrowserWidget::onRefreshUserTagsClicked()
{
    loadUserTags();
}

void TagBrowserWidget::onResetSortClicked()
{
    if (m_loading) return;
    ensureCsvLoadedForEditing();
    resetTagSort();
}

void TagBrowserWidget::onAddRowClicked()
{
    if (m_loading) return;
    ensureCsvLoadedForEditing();

    QStandardItem *tagItem = new QStandardItem();
    QStandardItem *categoryItem = new QStandardItem();
    QStandardItem *translationItem = new QStandardItem();
    QStandardItem *countItem = new QStandardItem();

    tagItem->setData(QString(), Qt::UserRole);
    categoryItem->setData(QString(), Qt::UserRole);
    translationItem->setData(QString(), Qt::UserRole);
    countItem->setData(0, Qt::UserRole);

    QList<QStandardItem*> row;
    row << tagItem << categoryItem << translationItem << countItem;

    m_model->appendRow(row);

    QModelIndex sourceIndex = m_model->index(m_model->rowCount() - 1, 0);
    QModelIndex proxyIndex = m_proxy->mapFromSource(sourceIndex);
    ui->tableTags->setCurrentIndex(proxyIndex);
    ui->tableTags->edit(proxyIndex);

    m_dirty = true;
    updateStatusLabel();
}

void TagBrowserWidget::onDeleteRowsClicked()
{
    if (m_loading) return;
    ensureCsvLoadedForEditing();
    QModelIndexList rows = ui->tableTags->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;

    std::sort(rows.begin(), rows.end(), [](const QModelIndex &a, const QModelIndex &b){
        return a.row() > b.row();
    });

    for (const QModelIndex &proxyIndex : rows) {
        QModelIndex sourceIndex = m_proxy->mapToSource(proxyIndex);
        m_model->removeRow(sourceIndex.row());
    }

    m_dirty = true;
    updateStatusLabel();
}

void TagBrowserWidget::onReloadClicked()
{
    loadCsv();
}

void TagBrowserWidget::onSaveClicked()
{
    if (m_loading) {
        QMessageBox::information(nullptr, "提示", "词表仍在加载，请稍候再保存。");
        return;
    }
    ensureCsvLoadedForEditing();
    if (m_csvPath.isEmpty()) {
        QMessageBox::information(nullptr, "提示", "请先在设置中配置 Tag 翻译表 CSV 路径。");
        return;
    }

    QFileInfo info(m_csvPath);
    QDir().mkpath(info.absolutePath());

    QFile file(m_csvPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(nullptr, "错误", "无法保存 Tag 翻译表。");
        return;
    }

    file.write("\xEF\xBB\xBF", 3);
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    for (int row = 0; row < m_model->rowCount(); ++row) {
        QString tag = m_model->item(row, 0) ? m_model->item(row, 0)->text().trimmed() : QString();
        QString category = m_model->item(row, 1) ? m_model->item(row, 1)->text().trimmed() : QString();
        QString translation = m_model->item(row, 2) ? m_model->item(row, 2)->text().trimmed() : QString();
        QString count = m_model->item(row, 3) ? m_model->item(row, 3)->text().trimmed() : QString();

        if (tag.isEmpty() && category.isEmpty() && translation.isEmpty() && count.isEmpty()) {
            continue;
        }

        // 如果有类别或使用次数，就保存为 ComfyUI-Custom-Scripts 兼容格式：
        // 1girl,1girl 人物-一个女孩,4114588
        //
        // 如果只是旧格式 tag,翻译，则继续保存为：
        // 1girl,一个女孩
        const bool saveAsAutocompleteFormat = !category.isEmpty() || !count.isEmpty();

        if (saveAsAutocompleteFormat) {
            QString display = tag;

            QString zhDisplay;
            if (!category.isEmpty() && !translation.isEmpty()) {
                zhDisplay = category + "-" + translation;
            } else if (!translation.isEmpty()) {
                zhDisplay = translation;
            } else if (!category.isEmpty()) {
                zhDisplay = category;
            }

            if (!zhDisplay.isEmpty()) {
                display += " " + zhDisplay;
            }

            out << escapeCsvField(tag)
                << ","
                << escapeCsvField(display)
                << ","
                << escapeCsvField(count)
                << "\n";
        } else {
            out << escapeCsvField(tag)
            << ","
            << escapeCsvField(translation)
            << "\n";
        }
    }
    file.close();

    m_dirty = false;
    updateStatusLabel();
    updateUserTagTranslations();
    updateUserTagStatusLabel();
    emit csvSaved(m_csvPath);
}

void TagBrowserWidget::onModelChanged()
{
    if (m_loading) return;
    m_dirty = true;
    if (m_userTagsLoaded) {
        updateUserTagTranslations();
        updateUserTagStatusLabel();
    }
    updateStatusLabel();
}

void TagBrowserWidget::resetTagSort()
{
    m_tagSortSection = -1;
    m_tagSortOrder = Qt::AscendingOrder;

    QHeaderView *header = ui->tableTags->horizontalHeader();
    header->setSortIndicatorShown(false);

    // 恢复为 source model 的原始顺序，也就是 CSV 加载顺序
    m_proxy->sort(-1);

    ui->tableTags->viewport()->update();
}
