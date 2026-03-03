#ifndef PROMPTPARSERWIDGET_H
#define PROMPTPARSERWIDGET_H

#include <QWidget>
#include <QHash>
#include <QString>
#include "tagflowwidget.h"

namespace Ui {
class PromptParserWidget;
}

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

    // 核心函数
    void processImage(const QString &filePath);
    QString extractPngParameters(const QString &filePath);
    void updateImagePreview(const QString &filePath);

    // 解析辅助函数
    QString cleanTagText(QString t);
    QMap<QString, int> parsePromptToMap(const QString &rawPrompt);
};

#endif // PROMPTPARSERWIDGET_H