#ifndef PROMPTPARSERWIDGET_H
#define PROMPTPARSERWIDGET_H

#include <QWidget>
#include <QHash>
#include <QProcess>
#include <QString>
#include "tagflowwidget.h"

namespace Ui {
class PromptParserWidget;
}

class QLabel;

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
    TagFlowWidget *wd14TagWidget;
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
    void onWd14Finished(int exitCode, QProcess::ExitStatus exitStatus);
    void loadWd14Settings();
    void saveWd14Settings() const;
    QString buildWd14Command() const;
    void setWd14Running(bool running);

    // 解析辅助函数
    QString cleanTagText(QString t);
    QMap<QString, int> parsePromptToMap(const QString &rawPrompt);
};

#endif // PROMPTPARSERWIDGET_H
