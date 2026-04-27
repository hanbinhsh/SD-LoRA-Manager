#include "tagbrowserwidget.h"
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

QVector<QPair<QString, QString>> readCsvRowsWorker(const QString &csvPath)
{
    QVector<QPair<QString, QString>> rows;
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
        }
        QString translationText = parts.mid(1).join(",").trimmed();
        rows.append(qMakePair(tagText, translationText));
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

TagBrowserWidget::TagBrowserWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::TagBrowserWidget)
    , m_model(new QStandardItemModel(this))
    , m_proxy(new QSortFilterProxyModel(this))
    , m_userTagModel(new QStandardItemModel(this))
    , m_userTagProxy(new QSortFilterProxyModel(this))
{
    ui->setupUi(this);

    m_model->setColumnCount(2);
    m_model->setHorizontalHeaderLabels({"Tag", "Translation"});

    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterKeyColumn(-1);
    m_proxy->setDynamicSortFilter(false);

    ui->tableTags->setModel(m_proxy);
    ui->tableTags->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableTags->setAlternatingRowColors(true);
    ui->tableTags->verticalHeader()->setVisible(false);
    ui->tableTags->horizontalHeader()->setStretchLastSection(true);
    ui->tableTags->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ui->tableTags->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->tableTags->setSortingEnabled(false);

    m_userTagModel->setColumnCount(4);
    m_userTagModel->setHorizontalHeaderLabels({"Tag", "类型", "使用次数", "Translation"});

    m_userTagProxy->setSourceModel(m_userTagModel);
    m_userTagProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_userTagProxy->setFilterKeyColumn(-1);
    m_userTagProxy->setDynamicSortFilter(false);
    m_userTagProxy->setSortRole(Qt::UserRole);

    ui->tableUserTags->setModel(m_userTagProxy);
    ui->tableUserTags->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableUserTags->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->tableUserTags->setAlternatingRowColors(true);
    ui->tableUserTags->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->tableUserTags->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->tableUserTags->verticalHeader()->setVisible(false);
    ui->tableUserTags->horizontalHeader()->setStretchLastSection(true);
    ui->tableUserTags->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ui->tableUserTags->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ui->tableUserTags->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    ui->tableUserTags->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    ui->tableUserTags->setSortingEnabled(true);

    connect(ui->tabWidgetTagBrowser, &QTabWidget::currentChanged, this, &TagBrowserWidget::onTabChanged);
    connect(ui->editSearch, &QLineEdit::textChanged, this, &TagBrowserWidget::onSearchTextChanged);
    connect(ui->comboSort, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &TagBrowserWidget::onSortModeChanged);
    connect(ui->editUserTagSearch, &QLineEdit::textChanged, this, &TagBrowserWidget::onUserTagSearchTextChanged);
    connect(ui->comboUserTagSort, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &TagBrowserWidget::onUserTagSortModeChanged);
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
    ui->comboSort->setEnabled(!loading);
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

QStringList TagBrowserWidget::parseCsvLine(const QString &line) const
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
    m_proxy->setFilterFixedString(QString());
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

    m_loadWatcher = new QFutureWatcher<QVector<QPair<QString, QString>>>(this);
    connect(m_loadWatcher, &QFutureWatcher<QVector<QPair<QString, QString>>>::finished, this, [this, generation, csvPath]() {
        if (!m_loadWatcher) return;
        const QVector<QPair<QString, QString>> rows = m_loadWatcher->result();
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
        QList<QStandardItem*> row;
        row << new QStandardItem(rowData.first) << new QStandardItem(rowData.second);
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

    m_proxy->setFilterFixedString(ui->editSearch->text().trimmed());
    m_proxy->invalidate();
    onSortModeChanged(ui->comboSort->currentIndex());
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

QHash<QString, QString> TagBrowserWidget::currentTranslationMap() const
{
    QHash<QString, QString> out;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QString tag = m_model->item(row, 0) ? m_model->item(row, 0)->text().trimmed() : QString();
        const QString translation = m_model->item(row, 1) ? m_model->item(row, 1)->text().trimmed() : QString();
        if (!tag.isEmpty() && !translation.isEmpty()) {
            out.insert(tag, translation);
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
    const QHash<QString, QString> translations = currentTranslationMap();
    for (int row = 0; row < m_userTagModel->rowCount(); ++row) {
        const QString tag = m_userTagModel->item(row, 0) ? m_userTagModel->item(row, 0)->text() : QString();
        const QString translated = translatedTextForTag(tag, translations);
        QStandardItem *item = m_userTagModel->item(row, 3);
        if (item) {
            item->setText(translated);
            item->setData(translated, Qt::UserRole);
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

        const QHash<QString, QString> translations = currentTranslationMap();
        m_userTagModel->removeRows(0, m_userTagModel->rowCount());
        for (const auto &rowData : rows) {
            const QString tag = rowData.tag;
            const int count = rowData.count;
            const QString translated = translatedTextForTag(tag, translations);

            QList<QStandardItem*> row;
            QStandardItem *tagItem = new QStandardItem(tag);
            QStandardItem *kindItem = new QStandardItem(rowData.kind);
            QStandardItem *countItem = new QStandardItem(QString::number(count));
            QStandardItem *translationItem = new QStandardItem(translated);
            tagItem->setData(tag, Qt::UserRole);
            kindItem->setData(rowData.kind == "负面" ? 1 : 0, Qt::UserRole);
            countItem->setData(count, Qt::UserRole);
            translationItem->setData(translated, Qt::UserRole);
            row << tagItem << kindItem << countItem << translationItem;
            m_userTagModel->appendRow(row);
        }

        m_userTagsLoading = false;
        m_userTagsLoaded = true;
        m_userTagProxy->setFilterFixedString(ui->editUserTagSearch->text().trimmed());
        onUserTagSortModeChanged(ui->comboUserTagSort->currentIndex());
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
        item.count = m_userTagModel->item(row, 2) ? m_userTagModel->item(row, 2)->text().toInt() : 0;
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
        UserTagUsageRow item;
        item.tag = m_userTagModel->item(sourceIndex.row(), 0) ? m_userTagModel->item(sourceIndex.row(), 0)->text() : QString();
        item.kind = m_userTagModel->item(sourceIndex.row(), 1) ? m_userTagModel->item(sourceIndex.row(), 1)->text() : QString();
        item.count = m_userTagModel->item(sourceIndex.row(), 2) ? m_userTagModel->item(sourceIndex.row(), 2)->text().toInt() : 0;
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
        item.count = m_userTagModel->item(sourceIndex.row(), 2) ? m_userTagModel->item(sourceIndex.row(), 2)->text().toInt() : 0;
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
    QCheckBox *chkCount = new QCheckBox("使用次数", &dlg);
    QCheckBox *chkTranslation = new QCheckBox("Translation", &dlg);
    chkTag->setChecked(true);
    chkType->setChecked(true);
    chkCount->setChecked(true);
    chkTranslation->setChecked(true);
    columnsRow->addWidget(chkTag);
    columnsRow->addWidget(chkType);
    columnsRow->addWidget(chkCount);
    columnsRow->addWidget(chkTranslation);
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

    auto refreshColumnOptions = [outputBox, columnsLabel, chkTag, chkType, chkCount, chkTranslation]() {
        const bool csvMode = outputBox->currentData().toString() == "csv";
        columnsLabel->setEnabled(csvMode);
        chkTag->setEnabled(csvMode);
        chkType->setEnabled(csvMode);
        chkCount->setEnabled(csvMode);
        chkTranslation->setEnabled(csvMode);
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
    if (chkCount->isChecked()) headers << "Count";
    if (chkTranslation->isChecked()) headers << "Translation";
    if (headers.isEmpty()) {
        QMessageBox::information(nullptr, "提示", "请至少选择一列导出。");
        return;
    }

    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(nullptr, "错误", "无法写入文件。");
        return;
    }

    const QHash<QString, QString> translations = currentTranslationMap();
    file.write("\xEF\xBB\xBF", 3);
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << headers.join(",") << "\n";
    for (const auto &row : rows) {
        QStringList values;
        if (chkTag->isChecked()) values << escapeUserTagCsvField(row.tag);
        if (chkType->isChecked()) values << escapeUserTagCsvField(row.kind);
        if (chkCount->isChecked()) values << QString::number(row.count);
        if (chkTranslation->isChecked()) values << escapeUserTagCsvField(translatedTextForTag(row.tag, translations));
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
    m_proxy->setFilterFixedString(text.trimmed());
}

void TagBrowserWidget::onSortModeChanged(int index)
{
    if (m_loading) return;
    ensureCsvLoadedForEditing();
    if (index <= 0) {
        ui->tableTags->setSortingEnabled(false);
        m_proxy->sort(-1);
    } else {
        ui->tableTags->setSortingEnabled(true);
        Qt::SortOrder order = (index == 2) ? Qt::DescendingOrder : Qt::AscendingOrder;
        ui->tableTags->sortByColumn(0, order);
    }
    ui->tableTags->viewport()->update();
}

void TagBrowserWidget::onUserTagSearchTextChanged(const QString &text)
{
    if (m_userTagsLoading) return;
    if (!m_userTagsLoaded) loadUserTags();
    m_userTagProxy->setFilterFixedString(text.trimmed());
}

void TagBrowserWidget::onUserTagSortModeChanged(int index)
{
    if (m_userTagsLoading) return;
    if (!m_userTagsLoaded) return;
    ui->tableUserTags->setSortingEnabled(true);
    if (index <= 0) {
        ui->tableUserTags->sortByColumn(2, Qt::DescendingOrder);
    } else {
        ui->tableUserTags->sortByColumn(0, index == 2 ? Qt::DescendingOrder : Qt::AscendingOrder);
    }
    ui->tableUserTags->viewport()->update();
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
    if (ui->comboSort->currentIndex() != 0) {
        ui->comboSort->setCurrentIndex(0);
    } else {
        onSortModeChanged(0);
    }
}

void TagBrowserWidget::onAddRowClicked()
{
    if (m_loading) return;
    ensureCsvLoadedForEditing();
    QList<QStandardItem*> row;
    row << new QStandardItem() << new QStandardItem();
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
        QString translation = m_model->item(row, 1) ? m_model->item(row, 1)->text().trimmed() : QString();
        if (tag.isEmpty() && translation.isEmpty()) continue;
        out << escapeCsvField(tag) << "," << escapeCsvField(translation) << "\n";
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
