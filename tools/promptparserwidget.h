#ifndef PROMPTPARSERWIDGET_H
#define PROMPTPARSERWIDGET_H

#include "imagemetadataparser.h"
#include "wd14historymodel.h"

#include <QWidget>
#include <QHash>
#include <QFutureWatcher>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include "tagflowwidget.h"

namespace Ui {
class PromptParserWidget;
}

class QLabel;
class QListWidget;
class QProcess;
class QPlainTextEdit;
class QStackedWidget;

class PromptParserWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PromptParserWidget(QWidget *parent = nullptr);
    ~PromptParserWidget();

    void setTranslationMap(const QHash<QString, QString> *map);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    Ui::PromptParserWidget *ui;
    const QHash<QString, QString> *m_translationMap;

    // 两个流式布局控件
    TagFlowWidget *posTagWidget;
    TagFlowWidget *negTagWidget;
    // 文本/标签双视图：每侧用 QStackedWidget 在“可编辑文本框”与“tagflow 滚动区”之间切换。
    QPlainTextEdit *m_posEdit = nullptr;
    QPlainTextEdit *m_negEdit = nullptr;
    QStackedWidget *m_posStack = nullptr;
    QStackedWidget *m_negStack = nullptr;
    bool m_tagViewActive = true;
    QString m_lastParsedPositive;   // 最近一次解析图片得到的正面提示词原文（供“刷新”覆盖）
    QString m_lastParsedNegative;   // 最近一次解析图片得到的负面提示词原文
    TagFlowWidget *compareTagWidgetA = nullptr;
    TagFlowWidget *compareTagWidgetB = nullptr;
    QProcess *wd14Process = nullptr;
    QString wd14ImagePath;
    QString wd14LastTagsText;
    QHash<QString, int> m_wd14TagUsageCounts;
    QFutureWatcher<QHash<QString, int>> *m_wd14UsageWatcher = nullptr;
    qint64 m_wd14UsageCacheModified = -1;
    Wd14HistoryModel *m_wd14HistoryModel = nullptr;
    QFutureWatcher<QVector<Wd14HistoryEntry>> *m_wd14HistoryWatcher = nullptr;
    bool m_wd14HistoryLoaded = false;
    QVector<Wd14HistoryEntry> m_pendingWd14HistoryEntries;
    Wd14RenderSettings m_activeWd14Settings;
    ParsedImageMetadata compareMetaA;
    ParsedImageMetadata compareMetaB;
    QString compareImagePathA;
    QString compareImagePathB;
    QStringList compareOnlyATags;
    QStringList compareOnlyBTags;
    QStringList compareCommonTags;

    // 核心函数
    void processImage(const QString &filePath);
    void processCompareImage(bool imageA, const QString &filePath);
    void updateImageCompare();
    void copyCompareTags(const QStringList &tags, const QString &label);
    void copyCompareAll();
    void processWd14Image(const QString &filePath);
    void updateImagePreview(const QString &filePath);
    void updateWd14ImagePreview(const QString &filePath);
    void updateImageLabelPreview(QLabel *label, const QString &filePath, const QString &fallbackText);
    void runWd14Tagger();
    void loadWd14Settings();
    void saveWd14Settings() const;
    void setWd14Running(bool running);
    void browseWd14ModelPath();
    void browseWd14PythonPath();
    void browseWd14ScriptPath();
    void saveWd14Preset();
    void deleteWd14Preset();
    void loadWd14Preset(const QString &presetName);
    bool applyWd14Preset(const QString &presetName, bool persistActivePreset);
    void copySelectedWd14Tags();
    void showWd14TagContextMenu(const QPoint &pos);
    QString wd14PresetDirectory() const;
    QString extractedWd14ScriptPath() const;
    QString defaultWd14ScriptPath() const;
    QString selectedWd14ScriptPath() const;
    QString selectedPythonPath() const;
    void updateWd14ThresholdFromSlider(int value);
    void updateWd14ThresholdFromSpin(double value);
    void applyWd14Result(const Wd14InferenceResult &result, const Wd14RenderSettings *settings = nullptr);
    void updateWd14MemoryLabel(quint64 totalBytes, quint64 availableBytes);
    Wd14InferenceResult parseWd14ProcessOutput(const QByteArray &stdoutBytes, const QByteArray &stderrBytes, int exitCode) const;
    void loadWd14TagUsageCounts();
    void updateWd14TagUsageColumn();
    QString formatWd14Tag(const QString &tag, const Wd14RenderSettings *settings = nullptr) const;
    QStringList splitWd14TagList(const QString &text) const;
    Wd14RenderSettings currentWd14Settings() const;
    QString wd14HistoryPath() const;
    void loadWd14History();
    void appendWd14History(const Wd14InferenceResult &result, const Wd14RenderSettings &settings);
    bool rewriteWd14History() const;
    void updateWd14HistoryActions();
    const Wd14HistoryEntry *currentWd14HistoryEntry() const;
    void previewWd14HistoryEntry(const QModelIndex &index);
    void restoreWd14HistoryEntry();
    void applyWd14HistorySettings();
    void deleteSelectedWd14History();
    void clearWd14History();

    // 解析辅助函数
    QMap<QString, int> parsePromptToMap(const QString &rawPrompt);
    QStringList parsePromptOrder(const QString &rawPrompt) const;  // 提示词中 tag 的出现顺序（去重、保留首次）
    void applyTagSortMode();                                       // 根据“原顺序”开关切换正/负面 tagflow 的排序
    void setupPromptTextToggle();                                  // 把正/负面 tagflow 滚动区包成“文本/标签”双视图
    void setTagViewActive(bool tagView);                          // 切换文本/标签视图（正负面同步）
    void refreshTagFlowsFromText();                               // 从两个文本框内容重新解析 tagflow

    QStringList m_posTagOrder;  // 当前图正面提示词的原始顺序（供 SortByGivenOrder）
    QStringList m_negTagOrder;  // 当前图负面提示词的原始顺序
    QString normalizeCompareTag(QString tag) const;
    void fillCompareList(QListWidget *list, const QStringList &tags);
    void fillCompareParams();
    QString compareParamValue(const ParsedImageMetadata &meta, const QString &key) const;
    QString extractParameterLine(const QString &parameters, const QStringList &keys) const;
};

#endif // PROMPTPARSERWIDGET_H
