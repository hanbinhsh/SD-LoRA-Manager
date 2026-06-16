#ifndef USAGEANALYSISWIDGET_H
#define USAGEANALYSISWIDGET_H

#include <QDateTime>
#include <QMap>
#include <QString>
#include <QVector>
#include <QWidget>

class QTableWidget;
class PieChartWidget;
class BarChartWidget;

namespace Ui {
class UsageAnalysisWidget;
}

struct UsageAnalysisModel
{
    QString filePath;
    QString jsonPath;
    QString previewPath;
    QString displayName;
    QString baseName;
    QString rootName;
    QString baseModel;
    QString modelType;        // Civitai 类型：LoRA / Checkpoint / VAE / TextualInversion ...
    QString syncFailure;
    int modelId = 0;
    int versionId = 0;
    int usageCount = 0;
    qint64 lastUsed = 0;
    qint64 modelFileBytes = 0;   // 模型文件本体大小
    qint64 sidecarBytes = 0;     // 附属文件（预览图 + json 元数据）大小
    bool localEdited = false;
    bool hasSha256 = false;
};

struct UsageAnalysisData
{
    QVector<UsageAnalysisModel> models;
    QMap<QString, int> positiveTagCounts;
    int galleryImageCount = 0;
    QDateTime generatedAt;
};

class UsageAnalysisWidget : public QWidget
{
    Q_OBJECT

public:
    explicit UsageAnalysisWidget(QWidget *parent = nullptr);
    ~UsageAnalysisWidget() override;

    void setAnalysisData(const UsageAnalysisData &data);

signals:
    void requestRefresh();
    void requestOpenModel(const QString &filePath);

private slots:
    void onSearchTextChanged(const QString &text);
    void onExportAnalysisCsvClicked();

private:
    Ui::UsageAnalysisWidget *ui;
    UsageAnalysisData analysisData;

    void refreshSummary();
    void refreshModelTable();
    void refreshCharts();
    void refreshDiskSpace();
    void setStatus(const QString &text);

    PieChartWidget *folderPie = nullptr;
    BarChartWidget *folderBar = nullptr;
    PieChartWidget *categoryPie = nullptr;
    BarChartWidget *categoryBar = nullptr;
    bool modelMatchesSearch(const UsageAnalysisModel &model, const QString &query) const;
    void fillTopTable(QTableWidget *table, const QVector<QPair<QString, int>> &rows, const QString &nameHeader);
    static QString formatTime(qint64 msecs);
};

#endif // USAGEANALYSISWIDGET_H
