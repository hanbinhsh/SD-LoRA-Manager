#ifndef USAGEANALYSISWIDGET_H
#define USAGEANALYSISWIDGET_H

#include <QDateTime>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "core/modeltypes.h"

class QTableWidget;
class QTimer;
class PieChartWidget;
class BarChartWidget;
class TagModelGraphCanvas;

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
    QString sha256;
    int modelId = 0;
    int versionId = 0;
    int usageCount = 0;
    qint64 lastUsed = 0;
    qint64 modelFileBytes = 0;   // 模型文件本体大小
    qint64 sidecarBytes = 0;     // 附属文件（预览图 + json 元数据）大小
    bool localEdited = false;
    bool hasSha256 = false;
};

struct UsageGraphImageRecord
{
    QStringList tags;              // 正面普通 Tag 列表（不含 <lora:...>）
    QStringList loras;             // 使用的 LoRA 名称列表
    QString checkpoint;            // 使用的底模 (Model / Checkpoint) 名称
    bool inCurrentGallery = false; // 是否在当前本地返图列表中
};

struct UsageAnalysisData
{
    QVector<UsageAnalysisModel> models;
    QMap<QString, int> positiveTagCounts;
    QMap<QString, UserImageInfo> rawImageCache;
    QSet<QString> currentGalleryPaths;
    QVector<UsageGraphImageRecord> graphImages;
    QStringList loraRootPaths;
    bool loraRecursive = true;
    int galleryImageCount = 0;
    int currentGalleryImageCount = 0;
    QDateTime generatedAt;
};

class UsageAnalysisWidget : public QWidget
{
    Q_OBJECT

public:
    explicit UsageAnalysisWidget(QWidget *parent = nullptr);
    ~UsageAnalysisWidget() override;

    void setAnalysisData(const UsageAnalysisData &data);
    void setTranslationMap(const QHash<QString, QString> *map);
    void openTagModelGraph(int scope, const QString &focusQuery = QString());

signals:
    void requestRefresh();
    void requestOpenModel(const QString &filePath);
    void requestJumpToModel(const QString &modelOrLoraName);
    void requestFilterInGallery(const QString &tagOrLora, bool isLora);
    void requestFilterMultiInGallery(const QStringList &tags, const QStringList &loras);

private slots:
    void onSearchTextChanged(const QString &text);
    void onExportAnalysisCsvClicked();
    void onScanCleanupClicked();
    void onDeleteOrphanFilesClicked();

private:
    Ui::UsageAnalysisWidget *ui;
    UsageAnalysisData analysisData;
    const QHash<QString, QString> *m_translationMap = nullptr;
    TagModelGraphCanvas *m_graphCanvas = nullptr;
    QTimer *m_graphRebuildTimer = nullptr;
    QVector<UsageGraphImageRecord> m_parsedGraphImages;
    bool m_parsedGraphImagesReady = false;
    bool m_graphIndexDirty = true;
    bool m_pendingGraphRelayout = true;
    int m_graphBuildToken = 0;
    int m_cleanupScanToken = 0;
    QString m_selectedGraphNodeId;
    QSet<QString> m_selectedGraphNodeIds;
    QString m_focusedGraphNodeId;

    void refreshSummary();
    void refreshModelTable();
    void refreshCharts();
    void refreshDiskSpace();
    void scheduleGraphRebuild(bool relayout = true);
    void rebuildGraphIndexAndView(bool relayout = true);
    void updateGraphDetailPanel();
    QString translateTag(const QString &tag) const;
    static QString translateTagWithMap(const QHash<QString, QString> &map, const QString &tag);
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
