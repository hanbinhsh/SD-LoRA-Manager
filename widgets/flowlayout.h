#ifndef FLOWLAYOUT_H
#define FLOWLAYOUT_H

#include <QLayout>
#include <QList>
#include <QPushButton>
#include <QString>

class FlowChipButton : public QPushButton
{
public:
    explicit FlowChipButton(const QString &text, QWidget *parent = nullptr);

    void setChipText(const QString &text);
    QString chipText() const { return m_fullText; }

    void setFullLine(bool fullLine);
    bool isFullLine() const { return m_isFullLine; }

    QSize calculateSizeForWidth(int availableW) const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_fullText;
    bool m_isFullLine = false;
};

class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget *parent = nullptr, int margin = 0, int hSpacing = 6, int vSpacing = 6);
    ~FlowLayout() override;

    void addItem(QLayoutItem *item) override;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QLayoutItem *takeAt(int index) override;

    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect &rect) override;

private:
    int doLayout(const QRect &rect, bool testOnly) const;

    QList<QLayoutItem*> m_itemList;
    int m_hSpace = 6;
    int m_vSpace = 6;
};

#endif // FLOWLAYOUT_H
