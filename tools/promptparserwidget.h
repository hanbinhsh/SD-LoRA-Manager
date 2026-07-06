#ifndef PROMPTPARSERWIDGET_H
#define PROMPTPARSERWIDGET_H

#include "imagemetadataparser.h"

#include <QWidget>
#include <QHash>
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

struct Wd14TagScore
{
    QString tag;
    QString category;
    float confidence = 0.0f;
    QString translation;
};

struct Wd14InferenceResult
{
    bool ok = false;
    QString error;
    QString finalTags;
    QVector<Wd14TagScore> ratings;
    QVector<Wd14TagScore> tags;
    double elapsedSec = 0.0;
    quint64 totalMemory = 0;
    quint64 availableMemory = 0;
};

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
    void applyWd14Result(const Wd14InferenceResult &result);
    void updateWd14MemoryLabel(quint64 totalBytes, quint64 availableBytes);
    Wd14InferenceResult parseWd14ProcessOutput(const QByteArray &stdoutBytes, const QByteArray &stderrBytes, int exitCode) const;
    QString translateTag(const QString &tag) const;
    QString formatWd14Tag(const QString &tag) const;
    QStringList splitWd14TagList(const QString &text) const;

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
