#ifndef PROMPTPARSERWIDGET_H
#define PROMPTPARSERWIDGET_H

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
class QProcess;

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
    QProcess *wd14Process = nullptr;
    QString wd14ImagePath;
    QString wd14LastTagsText;

    // 核心函数
    void processImage(const QString &filePath);
    void processWd14Image(const QString &filePath);
    QString extractPngParameters(const QString &filePath);
    void updateImagePreview(const QString &filePath);
    void updateWd14ImagePreview(const QString &filePath);
    void updateImageLabelPreview(QLabel *label, const QString &filePath, const QString &fallbackText);
    void runWd14Tagger();
    void onWd14Finished();
    void loadWd14Settings();
    void saveWd14Settings() const;
    void setWd14Running(bool running);
    void browseWd14ModelPath();
    void browseWd14PythonPath();
    void browseWd14ScriptPath();
    void saveWd14Preset();
    void loadWd14Preset(const QString &presetName);
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
    QString cleanTagText(QString t);
    QMap<QString, int> parsePromptToMap(const QString &rawPrompt);
};

#endif // PROMPTPARSERWIDGET_H
