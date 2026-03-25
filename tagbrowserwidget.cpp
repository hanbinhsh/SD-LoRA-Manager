#include "tagbrowserwidget.h"
#include "ui_tagbrowserwidget.h"

#include <QtConcurrent/QtConcurrent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QMessageBox>
#include <QStringConverter>
#include <QTextStream>
#include <QShowEvent>
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
}

TagBrowserWidget::TagBrowserWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::TagBrowserWidget)
    , m_model(new QStandardItemModel(this))
    , m_proxy(new QSortFilterProxyModel(this))
{
    ui->setupUi(this);

    m_model->setColumnCount(2);
    m_model->setHorizontalHeaderLabels({"Tag", "Translation"});

    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterKeyColumn(-1);

    ui->tableTags->setModel(m_proxy);
    ui->tableTags->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableTags->setAlternatingRowColors(true);
    ui->tableTags->verticalHeader()->setVisible(false);
    ui->tableTags->horizontalHeader()->setStretchLastSection(true);
    ui->tableTags->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ui->tableTags->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->tableTags->setSortingEnabled(false);

    connect(ui->editSearch, &QLineEdit::textChanged, this, &TagBrowserWidget::onSearchTextChanged);
    connect(ui->comboSort, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &TagBrowserWidget::onSortModeChanged);
    connect(ui->btnAdd, &QPushButton::clicked, this, &TagBrowserWidget::onAddRowClicked);
    connect(ui->btnDelete, &QPushButton::clicked, this, &TagBrowserWidget::onDeleteRowsClicked);
    connect(ui->btnResetSort, &QPushButton::clicked, this, &TagBrowserWidget::onResetSortClicked);
    connect(ui->btnReload, &QPushButton::clicked, this, &TagBrowserWidget::onReloadClicked);
    connect(ui->btnSave, &QPushButton::clicked, this, &TagBrowserWidget::onSaveClicked);
    connect(m_model, &QStandardItemModel::itemChanged, this, &TagBrowserWidget::onModelChanged);

    updateStatusLabel();
}

TagBrowserWidget::~TagBrowserWidget()
{
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
    if (!m_csvLoaded || ui->tableTags->model() == nullptr) {
        loadCsv();
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
    const int generation = m_loadGeneration;
    m_model->removeRows(0, m_model->rowCount());

    if (m_csvPath.isEmpty() || !QFile::exists(m_csvPath)) {
        m_loading = false;
        m_dirty = false;
        if (m_proxy) m_proxy->invalidate();
        updateStatusLabel();
        return;
    }

    setLoadingState(true, "正在加载词表，请稍候...");
    const QString csvPath = m_csvPath;

    auto *watcher = new QFutureWatcher<QVector<QPair<QString, QString>>>(this);
    connect(watcher, &QFutureWatcher<QVector<QPair<QString, QString>>>::finished, this, [this, watcher, generation, csvPath]() {
        const QVector<QPair<QString, QString>> rows = watcher->result();
        watcher->deleteLater();

        if (generation != m_loadGeneration || csvPath != m_csvPath) {
            return;
        }

        m_loading = true;
        m_model->removeRows(0, m_model->rowCount());
        for (const auto &rowData : rows) {
            QList<QStandardItem*> row;
            row << new QStandardItem(rowData.first) << new QStandardItem(rowData.second);
            m_model->appendRow(row);
        }
        m_loading = false;
        m_dirty = false;
        m_csvLoaded = true;

        m_proxy->setFilterFixedString(ui->editSearch->text().trimmed());
        onSortModeChanged(ui->comboSort->currentIndex());
        ui->tableTags->resizeRowsToContents();
        ui->tableTags->viewport()->update();
        setLoadingState(false);
        updateStatusLabel();
    });

    watcher->setFuture(QtConcurrent::run([csvPath]() {
        return readCsvRowsWorker(csvPath);
    }));
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

void TagBrowserWidget::onSearchTextChanged(const QString &text)
{
    ensureCsvLoadedForEditing();
    m_proxy->setFilterFixedString(text.trimmed());
}

void TagBrowserWidget::onSortModeChanged(int index)
{
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
    ensureCsvLoadedForEditing();
    if (m_csvPath.isEmpty()) {
        QMessageBox::information(this, "提示", "请先在设置中配置 Tag 翻译表 CSV 路径。");
        return;
    }

    QFileInfo info(m_csvPath);
    QDir().mkpath(info.absolutePath());

    QFile file(m_csvPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "错误", "无法保存 Tag 翻译表。");
        return;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << "\xEF\xBB\xBF";
    for (int row = 0; row < m_model->rowCount(); ++row) {
        QString tag = m_model->item(row, 0) ? m_model->item(row, 0)->text().trimmed() : QString();
        QString translation = m_model->item(row, 1) ? m_model->item(row, 1)->text().trimmed() : QString();
        if (tag.isEmpty() && translation.isEmpty()) continue;
        out << escapeCsvField(tag) << "," << escapeCsvField(translation) << "\n";
    }
    file.close();

    m_dirty = false;
    updateStatusLabel();
    emit csvSaved(m_csvPath);
}

void TagBrowserWidget::onModelChanged()
{
    if (m_loading) return;
    m_dirty = true;
    updateStatusLabel();
}
