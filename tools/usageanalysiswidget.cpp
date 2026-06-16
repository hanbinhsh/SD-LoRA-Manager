#include "usageanalysiswidget.h"
#include "chartwidgets.h"
#include "styleconstants.h"
#include "tableviewstylehelper.h"
#include "ui_usageanalysiswidget.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProgressBar>
#include <QSaveFile>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>
#include <algorithm>

namespace {
class NumericTableItem : public QTableWidgetItem
{
public:
    explicit NumericTableItem(int value)
        : QTableWidgetItem(QString::number(value))
        , numericValue(value)
    {
        setData(Qt::UserRole, value);
    }

    bool operator<(const QTableWidgetItem &other) const override
    {
        return numericValue < other.data(Qt::UserRole).toInt();
    }

private:
    int numericValue = 0;
};

QString humanSize(qint64 bytes)
{
    if (bytes <= 0) return "0 B";
    constexpr double kb = 1024.0, mb = kb * 1024.0, gb = mb * 1024.0, tb = gb * 1024.0;
    const double b = static_cast<double>(bytes);
    if (b >= tb) return QString::number(b / tb, 'f', 2) + " TB";
    if (b >= gb) return QString::number(b / gb, 'f', 2) + " GB";
    if (b >= mb) return QString::number(b / mb, 'f', 1) + " MB";
    if (b >= kb) return QString::number(b / kb, 'f', 0) + " KB";
    return QString::number(bytes) + " B";
}

// 把 Civitai 类型规范化为展示类别。
QString normalizeCategory(const QString &modelType, const QString &rootName)
{
    const QString t = modelType.trimmed().toLower();
    if (t.contains("lora") || t.contains("locon") || t.contains("dora") || t.contains("lycoris")) return "LoRA";
    if (t.contains("checkpoint")) return "底模 (Checkpoint)";
    if (t.contains("textualinversion") || t.contains("embedding")) return "Embedding";
    if (t.contains("vae")) return "VAE";
    if (t.contains("hypernetwork")) return "Hypernetwork";
    if (t.contains("controlnet")) return "ControlNet";
    if (!t.isEmpty()) return modelType.trimmed();
    return rootName.trimmed().isEmpty() ? "其他" : rootName.trimmed();
}

// 一组与深色主题搭配的稳定调色板（按索引取色）。
QColor chartColorAt(int index)
{
    static const QStringList palette = {
        "#66c0f4", "#5fd38d", "#ffcc00", "#ff6b6b", "#c678dd",
        "#56b6c2", "#e08e5a", "#8fb4ff", "#b7d96a", "#f06292"
    };
    return QColor(palette.at(index % palette.size()));
}

// 按字节数排序、显示人类可读大小的表格项。
class SizeTableItem : public QTableWidgetItem
{
public:
    explicit SizeTableItem(qint64 bytes)
        : QTableWidgetItem(humanSize(bytes)), m_bytes(bytes)
    {
        setData(Qt::UserRole, static_cast<qlonglong>(bytes));
    }

    bool operator<(const QTableWidgetItem &other) const override
    {
        return m_bytes < other.data(Qt::UserRole).toLongLong();
    }

private:
    qint64 m_bytes = 0;
};
}

UsageAnalysisWidget::UsageAnalysisWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::UsageAnalysisWidget)
{
    ui->setupUi(this);
    setStyleSheet(AppStyle::loadQss(":/styles/toolpage.qss"));

    auto setupTable = [](QTableWidget *table, bool stretchLast) {
        applyUnifiedTableRowStyle(table);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        table->horizontalHeader()->setStretchLastSection(stretchLast);
        table->horizontalHeader()->setSectionsClickable(true);
        table->setSortingEnabled(true);
        table->setAlternatingRowColors(false);
        table->setShowGrid(false);
        table->setFocusPolicy(Qt::NoFocus);
    };
    setupTable(ui->tableModels, true);
    setupTable(ui->tableTopUsed, true);
    setupTable(ui->tableTopTags, true);
    setupTable(ui->tableBaseModels, true);

    // 空间占用 Tab：图表 + 统计范围下拉 + 最大模型表。
    folderPie = new PieChartWidget(ui->spaceFolderPieContainer);
    folderBar = new BarChartWidget(ui->spaceFolderBarContainer);
    categoryPie = new PieChartWidget(ui->spaceCategoryPieContainer);
    categoryBar = new BarChartWidget(ui->spaceCategoryBarContainer);
    ui->layoutSpaceFolderPie->addWidget(folderPie);
    ui->layoutSpaceFolderBar->addWidget(folderBar);
    ui->layoutSpaceCategoryPie->addWidget(categoryPie);
    ui->layoutSpaceCategoryBar->addWidget(categoryBar);

    ui->comboSpaceScope->addItem("模型 + 附属文件", 2);
    ui->comboSpaceScope->addItem("仅模型文件", 0);
    ui->comboSpaceScope->addItem("仅附属文件", 1);
    connect(ui->comboSpaceScope, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { refreshDiskSpace(); });

    setupTable(ui->tableLargestModels, true);
    ui->tableLargestModels->setColumnCount(4);
    ui->tableLargestModels->setHorizontalHeaderLabels({"模型", "类别", "文件夹", "占用"});

    connect(ui->btnRefreshAnalysis, &QPushButton::clicked, this, &UsageAnalysisWidget::requestRefresh);
    connect(ui->editSearchModels, &QLineEdit::textChanged, this, &UsageAnalysisWidget::onSearchTextChanged);
    connect(ui->btnExportAnalysisCsv, &QPushButton::clicked, this, &UsageAnalysisWidget::onExportAnalysisCsvClicked);
    setStatus("等待模型库数据。");
}

UsageAnalysisWidget::~UsageAnalysisWidget()
{
    delete ui;
}

void UsageAnalysisWidget::setAnalysisData(const UsageAnalysisData &data)
{
    analysisData = data;
    refreshSummary();
    refreshCharts();
    refreshDiskSpace();
    refreshModelTable();
    setStatus(QString("已载入 %1 个模型，缓存图片 %2 张。")
                  .arg(analysisData.models.size())
                  .arg(analysisData.galleryImageCount));
}

void UsageAnalysisWidget::refreshSummary()
{
    int civitai = 0;
    int localEdited = 0;
    int failed = 0;
    int used = 0;
    for (const UsageAnalysisModel &model : analysisData.models) {
        if (model.modelId > 0 || model.versionId > 0 || model.hasSha256) ++civitai;
        if (model.localEdited) ++localEdited;
        if (!model.syncFailure.isEmpty()) ++failed;
        if (model.usageCount > 0) ++used;
    }
    ui->lblSummaryTotal->setText(QString::number(analysisData.models.size()));
    ui->lblSummaryCivitai->setText(QString::number(civitai));
    ui->lblSummaryLocal->setText(QString::number(localEdited));
    ui->lblSummaryFailed->setText(QString::number(failed));
    ui->lblSummaryUsed->setText(QString::number(used));
    ui->lblSummaryUnused->setText(QString::number(qMax(0, analysisData.models.size() - used)));
}

void UsageAnalysisWidget::refreshCharts()
{
    QVector<QPair<QString, int>> usedRows;
    QMap<QString, int> baseCounts;
    for (const UsageAnalysisModel &model : analysisData.models) {
        if (model.usageCount > 0) usedRows.append(qMakePair(model.displayName, model.usageCount));
        const QString base = model.baseModel.trimmed().isEmpty() ? "Unknown" : model.baseModel.trimmed();
        baseCounts[base] += 1;
    }

    std::sort(usedRows.begin(), usedRows.end(), [](const auto &a, const auto &b) {
        if (a.second != b.second) return a.second > b.second;
        return QString::localeAwareCompare(a.first, b.first) < 0;
    });
    if (usedRows.size() > 20) usedRows.resize(20);
    fillTopTable(ui->tableTopUsed, usedRows, "LoRA");

    QVector<QPair<QString, int>> tagRows;
    for (auto it = analysisData.positiveTagCounts.cbegin(); it != analysisData.positiveTagCounts.cend(); ++it) {
        tagRows.append(qMakePair(it.key(), it.value()));
    }
    std::sort(tagRows.begin(), tagRows.end(), [](const auto &a, const auto &b) {
        if (a.second != b.second) return a.second > b.second;
        return QString::localeAwareCompare(a.first, b.first) < 0;
    });
    if (tagRows.size() > 20) tagRows.resize(20);
    fillTopTable(ui->tableTopTags, tagRows, "Tag");

    QVector<QPair<QString, int>> baseRows;
    for (auto it = baseCounts.cbegin(); it != baseCounts.cend(); ++it) {
        baseRows.append(qMakePair(it.key(), it.value()));
    }
    std::sort(baseRows.begin(), baseRows.end(), [](const auto &a, const auto &b) {
        if (a.second != b.second) return a.second > b.second;
        return QString::localeAwareCompare(a.first, b.first) < 0;
    });
    fillTopTable(ui->tableBaseModels, baseRows, "底模");
}

void UsageAnalysisWidget::refreshDiskSpace()
{
    const int scope = ui->comboSpaceScope ? ui->comboSpaceScope->currentData().toInt() : 2;
    auto bytesOf = [scope](const UsageAnalysisModel &m) -> qint64 {
        switch (scope) {
        case 0: return m.modelFileBytes;
        case 1: return m.sidecarBytes;
        default: return m.modelFileBytes + m.sidecarBytes;
        }
    };

    QMap<QString, qint64> folderBytes;
    QMap<QString, qint64> categoryBytes;
    qint64 total = 0;
    for (const UsageAnalysisModel &m : analysisData.models) {
        const qint64 b = bytesOf(m);
        if (b <= 0) continue;
        total += b;
        const QString folder = m.rootName.trimmed().isEmpty() ? "未分类" : m.rootName.trimmed();
        folderBytes[folder] += b;
        categoryBytes[normalizeCategory(m.modelType, m.rootName)] += b;
    }

    auto buildSlices = [](const QMap<QString, qint64> &grouped) {
        QVector<QPair<QString, qint64>> rows;
        for (auto it = grouped.cbegin(); it != grouped.cend(); ++it) rows.append(qMakePair(it.key(), it.value()));
        std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        QVector<ChartSlice> slices;
        for (int i = 0; i < rows.size(); ++i) {
            ChartSlice s;
            s.label = rows[i].first;
            s.value = static_cast<double>(rows[i].second);
            s.valueText = humanSize(rows[i].second);
            s.color = chartColorAt(i);
            slices.append(s);
        }
        return slices;
    };

    const QVector<ChartSlice> folderSlices = buildSlices(folderBytes);
    const QVector<ChartSlice> categorySlices = buildSlices(categoryBytes);
    if (folderPie) folderPie->setData(folderSlices);
    if (folderBar) folderBar->setData(folderSlices);
    if (categoryPie) categoryPie->setData(categorySlices);
    if (categoryBar) categoryBar->setData(categorySlices);

    ui->lblSpaceSummary->setText(QString("总占用 %1 · 模型 %2 个")
                                     .arg(humanSize(total))
                                     .arg(analysisData.models.size()));

    // 占用最大的模型 TopN。
    QVector<const UsageAnalysisModel*> sorted;
    sorted.reserve(analysisData.models.size());
    for (const UsageAnalysisModel &m : analysisData.models) {
        if (bytesOf(m) > 0) sorted.append(&m);
    }
    std::sort(sorted.begin(), sorted.end(), [&bytesOf](const UsageAnalysisModel *a, const UsageAnalysisModel *b) {
        return bytesOf(*a) > bytesOf(*b);
    });
    const int limit = qMin(50, sorted.size());

    ui->tableLargestModels->setSortingEnabled(false);
    ui->tableLargestModels->clearContents();
    ui->tableLargestModels->setRowCount(limit);
    for (int i = 0; i < limit; ++i) {
        const UsageAnalysisModel &m = *sorted[i];
        auto *nameItem = new QTableWidgetItem(m.displayName);
        nameItem->setToolTip(m.filePath);
        ui->tableLargestModels->setItem(i, 0, nameItem);
        ui->tableLargestModels->setItem(i, 1, new QTableWidgetItem(normalizeCategory(m.modelType, m.rootName)));
        ui->tableLargestModels->setItem(i, 2, new QTableWidgetItem(m.rootName));
        ui->tableLargestModels->setItem(i, 3, new SizeTableItem(bytesOf(m)));
    }
    ui->tableLargestModels->setSortingEnabled(true);
}

void UsageAnalysisWidget::fillTopTable(QTableWidget *table, const QVector<QPair<QString, int>> &rows, const QString &nameHeader)
{
    table->setSortingEnabled(false);
    table->clear();
    table->setColumnCount(3);
    table->setHorizontalHeaderLabels({nameHeader, "数量", "占比"});
    table->setRowCount(rows.size());

    int maxValue = 1;
    for (const auto &row : rows) maxValue = qMax(maxValue, row.second);

    for (int row = 0; row < rows.size(); ++row) {
        table->setItem(row, 0, new QTableWidgetItem(rows[row].first));
        table->setItem(row, 1, new NumericTableItem(rows[row].second));

        table->setItem(row, 2, new NumericTableItem(rows[row].second));
        QProgressBar *bar = new QProgressBar(table);
        bar->setRange(0, maxValue);
        bar->setValue(rows[row].second);
        bar->setTextVisible(true);
        bar->setFormat(QString("%1%").arg(rows[row].second * 100 / maxValue));
        table->setCellWidget(row, 2, bar);
    }
    table->setSortingEnabled(true);
}

void UsageAnalysisWidget::refreshModelTable()
{
    const QString query = ui->editSearchModels->text().trimmed();
    QVector<UsageAnalysisModel> rows;
    for (const UsageAnalysisModel &model : analysisData.models) {
        if (modelMatchesSearch(model, query)) rows.append(model);
    }

    ui->tableModels->setSortingEnabled(false);
    ui->tableModels->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const UsageAnalysisModel &model = rows[row];
        auto *nameItem = new QTableWidgetItem(model.displayName);
        nameItem->setData(Qt::UserRole, model.filePath);
        ui->tableModels->setItem(row, 0, nameItem);
        ui->tableModels->setItem(row, 1, new QTableWidgetItem(model.baseModel.isEmpty() ? "Unknown" : model.baseModel));
        ui->tableModels->setItem(row, 2, new QTableWidgetItem(model.localEdited ? "本地/已编辑" : "Civitai"));
        ui->tableModels->setItem(row, 3, new NumericTableItem(model.usageCount));
        ui->tableModels->setItem(row, 4, new QTableWidgetItem(formatTime(model.lastUsed)));
        ui->tableModels->setItem(row, 5, new QTableWidgetItem(model.syncFailure.isEmpty() ? "正常" : "同步失败"));
        ui->tableModels->setItem(row, 6, new QTableWidgetItem(model.filePath));
    }
    ui->tableModels->setSortingEnabled(true);
    ui->lblAnalysisStatus->setText(QString("显示 %1 / %2 个模型").arg(rows.size()).arg(analysisData.models.size()));
}

bool UsageAnalysisWidget::modelMatchesSearch(const UsageAnalysisModel &model, const QString &query) const
{
    if (query.isEmpty()) return true;
    return model.displayName.contains(query, Qt::CaseInsensitive)
        || model.baseName.contains(query, Qt::CaseInsensitive)
        || model.baseModel.contains(query, Qt::CaseInsensitive)
        || model.rootName.contains(query, Qt::CaseInsensitive)
        || model.filePath.contains(query, Qt::CaseInsensitive);
}

void UsageAnalysisWidget::onSearchTextChanged(const QString &)
{
    refreshModelTable();
}

void UsageAnalysisWidget::onExportAnalysisCsvClicked()
{
    const QString path = QFileDialog::getSaveFileName(this, "导出使用分析 CSV", QString(), "CSV Files (*.csv)");
    if (path.isEmpty()) return;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    file.write("\xEF\xBB\xBF");
    QTextStream out(&file);
    out << "Name,BaseModel,Status,UsageCount,LastUsed,MetadataStatus,Path\n";
    for (const UsageAnalysisModel &model : analysisData.models) {
        auto esc = [](QString s) {
            s.replace('"', "\"\"");
            return QString("\"") + s + "\"";
        };
        out << esc(model.displayName) << ','
            << esc(model.baseModel) << ','
            << esc(model.localEdited ? "Local/Edited" : "Civitai") << ','
            << model.usageCount << ','
            << esc(formatTime(model.lastUsed)) << ','
            << esc(model.syncFailure.isEmpty() ? "OK" : "Sync Failed") << ','
            << esc(model.filePath) << '\n';
    }
    file.commit();
    setStatus("使用分析 CSV 已导出。");
}

void UsageAnalysisWidget::setStatus(const QString &text)
{
    ui->lblAnalysisStatus->setText(text);
}

QString UsageAnalysisWidget::formatTime(qint64 msecs)
{
    if (msecs <= 0) return "-";
    return QDateTime::fromMSecsSinceEpoch(msecs).toString("yyyy-MM-dd HH:mm");
}
