#include "usageanalysiswidget.h"
#include "tableviewstylehelper.h"
#include "ui_usageanalysiswidget.h"

#include <QtConcurrent/QtConcurrent>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProgressBar>
#include <QSaveFile>
#include <QTableWidget>
#include <QTextStream>
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

QString loadToolPageStyle()
{
    QFile file(":/styles/toolpage.qss");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(file.readAll());
}
}

UsageAnalysisWidget::UsageAnalysisWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::UsageAnalysisWidget)
{
    ui->setupUi(this);
    setStyleSheet(loadToolPageStyle());

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
    setupTable(ui->tableHealth, true);
    setupTable(ui->tableTopUsed, true);
    setupTable(ui->tableTopTags, true);
    setupTable(ui->tableBaseModels, true);

    connect(ui->btnRefreshAnalysis, &QPushButton::clicked, this, &UsageAnalysisWidget::requestRefresh);
    connect(ui->editSearchModels, &QLineEdit::textChanged, this, &UsageAnalysisWidget::onSearchTextChanged);
    connect(ui->btnExportAnalysisCsv, &QPushButton::clicked, this, &UsageAnalysisWidget::onExportAnalysisCsvClicked);
    connect(ui->btnRunHealthCheck, &QPushButton::clicked, this, &UsageAnalysisWidget::onRunHealthCheckClicked);
    connect(ui->btnCopyHealth, &QPushButton::clicked, this, &UsageAnalysisWidget::onCopyHealthClicked);
    connect(ui->btnOpenHealthModel, &QPushButton::clicked, this, &UsageAnalysisWidget::onOpenSelectedHealthModelClicked);

    setStatus("等待模型库数据。");
}

UsageAnalysisWidget::~UsageAnalysisWidget()
{
    if (healthWatcher) {
        healthWatcher->cancel();
        healthWatcher->waitForFinished();
    }
    delete ui;
}

void UsageAnalysisWidget::setAnalysisData(const UsageAnalysisData &data)
{
    analysisData = data;
    refreshSummary();
    refreshCharts();
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

void UsageAnalysisWidget::onRunHealthCheckClicked()
{
    if (healthWatcher && healthWatcher->isRunning()) return;
    ui->btnRunHealthCheck->setEnabled(false);
    ui->lblHealthStatus->setText("正在后台检查元数据...");

    if (!healthWatcher) healthWatcher = new QFutureWatcher<QVector<MetadataHealthIssue>>(this);
    disconnect(healthWatcher, nullptr, this, nullptr);
    connect(healthWatcher, &QFutureWatcherBase::finished, this, [this]() {
        const QVector<MetadataHealthIssue> issues = healthWatcher->result();
        ui->btnRunHealthCheck->setEnabled(true);
        refreshHealthTable(issues);
        ui->lblHealthStatus->setText(QString("检查完成，共发现 %1 条问题/提示。").arg(issues.size()));
    });
    const QVector<UsageAnalysisModel> models = analysisData.models;
    healthWatcher->setFuture(QtConcurrent::run([models]() {
        return UsageAnalysisWidget::runHealthCheckWorker(models);
    }));
}

QVector<MetadataHealthIssue> UsageAnalysisWidget::runHealthCheckWorker(QVector<UsageAnalysisModel> models)
{
    QVector<MetadataHealthIssue> issues;
    for (const UsageAnalysisModel &model : models) {
        QFileInfo modelFile(model.filePath);
        if (!modelFile.exists()) {
            issues.append({"错误", model.displayName, "模型文件不存在", "检查模型路径或从列表移除失效项", model.filePath});
            continue;
        }

        if (!model.jsonPath.isEmpty() && QFileInfo(model.jsonPath).exists()) {
            QFile jsonFile(model.jsonPath);
            if (!jsonFile.open(QIODevice::ReadOnly)) {
                issues.append({"错误", model.displayName, "无法读取 metadata JSON", "检查文件权限", model.filePath});
            } else {
                QJsonParseError err;
                QJsonDocument doc = QJsonDocument::fromJson(jsonFile.readAll(), &err);
                if (err.error != QJsonParseError::NoError || !doc.isObject()) {
                    issues.append({"错误", model.displayName, "metadata JSON 解析失败", err.errorString(), model.filePath});
                }
            }
        } else {
            issues.append({"警告", model.displayName, "缺少 metadata JSON", "点击模型详情刷新以同步 Civitai 元数据", model.filePath});
        }

        if (!model.syncFailure.isEmpty()) {
            issues.append({"警告", model.displayName, "存在同步失败缓存", model.syncFailure, model.filePath});
        }
        if (model.modelId <= 0 && model.versionId <= 0 && !model.hasSha256) {
            issues.append({"警告", model.displayName, "缺少 Civitai 识别字段", "缺少 modelId/versionId/sha256，更新检测可能无法判断", model.filePath});
        }
        if (model.previewPath.isEmpty() || !QFileInfo::exists(model.previewPath)) {
            issues.append({"信息", model.displayName, "缺少本地封面预览图", "可在详情页重新同步预览图", model.filePath});
        }
        if (model.localEdited) {
            issues.append({"信息", model.displayName, "本地/已编辑模型", "同步或更新前会按本地保护逻辑处理", model.filePath});
        }
    }
    return issues;
}

void UsageAnalysisWidget::refreshHealthTable(const QVector<MetadataHealthIssue> &issues)
{
    ui->tableHealth->setSortingEnabled(false);
    ui->tableHealth->setRowCount(issues.size());
    for (int row = 0; row < issues.size(); ++row) {
        const MetadataHealthIssue &issue = issues[row];
        auto *severityItem = new QTableWidgetItem(issue.severity);
        severityItem->setData(Qt::UserRole, issue.filePath);
        ui->tableHealth->setItem(row, 0, severityItem);
        ui->tableHealth->setItem(row, 1, new QTableWidgetItem(issue.modelName));
        ui->tableHealth->setItem(row, 2, new QTableWidgetItem(issue.issue));
        ui->tableHealth->setItem(row, 3, new QTableWidgetItem(issue.suggestion));
        ui->tableHealth->setItem(row, 4, new QTableWidgetItem(issue.filePath));
    }
    ui->tableHealth->setSortingEnabled(true);
}

void UsageAnalysisWidget::onCopyHealthClicked()
{
    QStringList lines;
    const auto ranges = ui->tableHealth->selectedRanges();
    if (ranges.isEmpty()) return;
    for (const QTableWidgetSelectionRange &range : ranges) {
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) {
            QStringList cols;
            for (int col = 0; col < ui->tableHealth->columnCount(); ++col) {
                if (QTableWidgetItem *item = ui->tableHealth->item(row, col)) cols << item->text();
            }
            lines << cols.join('\t');
        }
    }
    QApplication::clipboard()->setText(lines.join('\n'));
}

void UsageAnalysisWidget::onOpenSelectedHealthModelClicked()
{
    const int row = ui->tableHealth->currentRow();
    if (row < 0) return;
    QTableWidgetItem *item = ui->tableHealth->item(row, 0);
    if (!item) return;
    const QString filePath = item->data(Qt::UserRole).toString();
    if (!filePath.isEmpty()) emit requestOpenModel(filePath);
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
