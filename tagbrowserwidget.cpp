#include "tagbrowserwidget.h"
#include "ui_tagbrowserwidget.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QMessageBox>
#include <QStringConverter>
#include <QTextStream>
#include <QShowEvent>
#include <algorithm>

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
    m_csvPath = path;
    ui->editCsvPath->setText(path);
    loadCsv();
}

QString TagBrowserWidget::csvPath() const
{
    return m_csvPath;
}

void TagBrowserWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if ((!m_csvPath.isEmpty() && m_model->rowCount() == 0) || ui->tableTags->model() == nullptr) {
        loadCsv();
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
    m_loading = true;
    m_model->removeRows(0, m_model->rowCount());

    if (m_csvPath.isEmpty() || !QFile::exists(m_csvPath)) {
        m_loading = false;
        m_dirty = false;
        if (m_proxy) m_proxy->invalidate();
        updateStatusLabel();
        return;
    }

    QFile file(m_csvPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_loading = false;
        QMessageBox::warning(this, "错误", "无法读取 Tag 翻译表。");
        return;
    }

    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    int rowIndex = 0;
    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line.trimmed().isEmpty()) continue;
        QStringList parts = parseCsvLine(line);
        if (parts.isEmpty()) continue;

        QString tagText = parts.value(0).trimmed();
        if (rowIndex == 0 && tagText.startsWith(QChar(0xFEFF))) {
            tagText.remove(0, 1);
        }

        QList<QStandardItem*> row;
        QStandardItem *tagItem = new QStandardItem(tagText);
        QStandardItem *translationItem = new QStandardItem(parts.mid(1).join(",").trimmed());
        row << tagItem << translationItem;
        m_model->appendRow(row);
        ++rowIndex;
    }

    m_loading = false;
    m_dirty = false;
    m_proxy->setFilterFixedString(ui->editSearch->text().trimmed());
    onSortModeChanged(ui->comboSort->currentIndex());
    ui->tableTags->resizeRowsToContents();
    ui->tableTags->viewport()->update();
    updateStatusLabel();
}

void TagBrowserWidget::updateStatusLabel()
{
    QString status;
    if (m_csvPath.isEmpty()) {
        status = "未配置翻译表路径";
        ui->lblEmptyState->setText("未配置 Tag 翻译表路径。请先在设置页选择 CSV，或点击“新增”手动创建。");
    } else if (!QFile::exists(m_csvPath)) {
        status = "CSV 文件不存在";
        ui->lblEmptyState->setText("找不到当前 CSV 文件。请检查路径，或点击“新增”手动创建后保存。");
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
    m_proxy->setFilterFixedString(text.trimmed());
}

void TagBrowserWidget::onSortModeChanged(int index)
{
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
