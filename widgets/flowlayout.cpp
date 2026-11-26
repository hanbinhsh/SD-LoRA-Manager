#include "flowlayout.h"

#include <QPainter>
#include <QStyle>
#include <QStyleOptionButton>
#include <QWidget>

FlowChipButton::FlowChipButton(const QString &text, QWidget *parent)
    : QPushButton(parent)
    , m_fullText(text)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}

void FlowChipButton::setChipText(const QString &text)
{
    m_fullText = text;
    update();
    updateGeometry();
}

void FlowChipButton::setFullLine(bool fullLine)
{
    if (m_isFullLine != fullLine) {
        m_isFullLine = fullLine;
        update();
    }
}

QSize FlowChipButton::calculateSizeForWidth(int availableW) const
{
    const int padX = 22;
    const int padY = 8;
    const QFontMetrics fm = fontMetrics();
    const int singleW = fm.horizontalAdvance(m_fullText) + padX;
    const int defaultH = qMax(24, fm.height() + padY);

    if (availableW <= 0 || singleW < availableW) {
        return QSize(singleW, defaultH);
    }

    // 仅当单个 tag 非常长需占据整行时，折行并增高
    const int innerW = qMax(20, availableW - padX);
    const QRect bound = fm.boundingRect(QRect(0, 0, innerW, 10000),
                                        Qt::AlignCenter | Qt::TextWordWrap | Qt::TextWrapAnywhere,
                                        m_fullText);
    return QSize(availableW, qMax(defaultH, bound.height() + padY));
}

QSize FlowChipButton::sizeHint() const
{
    return calculateSizeForWidth(-1);
}

QSize FlowChipButton::minimumSizeHint() const
{
    return QSize(40, 24);
}

void FlowChipButton::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    QStyleOptionButton opt;
    initStyleOption(&opt);
    opt.text.clear();
    style()->drawControl(QStyle::CE_PushButton, &opt, &p, this);

    const QRect textRect = rect().adjusted(9, 4, -9, -4);
    p.setFont(font());
    p.setPen(opt.palette.buttonText().color());
    if (m_isFullLine) {
        p.drawText(textRect, Qt::AlignCenter | Qt::TextWordWrap | Qt::TextWrapAnywhere, m_fullText);
    } else {
        p.drawText(textRect, Qt::AlignCenter | Qt::TextSingleLine, m_fullText);
    }
}

FlowLayout::FlowLayout(QWidget *parent, int margin, int hSpacing, int vSpacing)
    : QLayout(parent)
    , m_hSpace(hSpacing)
    , m_vSpace(vSpacing)
{
    setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::~FlowLayout()
{
    QLayoutItem *item = nullptr;
    while ((item = takeAt(0)) != nullptr) delete item;
}

void FlowLayout::addItem(QLayoutItem *item)
{
    m_itemList.append(item);
}

int FlowLayout::count() const
{
    return m_itemList.size();
}

QLayoutItem *FlowLayout::itemAt(int index) const
{
    return m_itemList.value(index);
}

QLayoutItem *FlowLayout::takeAt(int index)
{
    if (index < 0 || index >= m_itemList.size()) return nullptr;
    return m_itemList.takeAt(index);
}

Qt::Orientations FlowLayout::expandingDirections() const
{
    return {};
}

bool FlowLayout::hasHeightForWidth() const
{
    return true;
}

int FlowLayout::heightForWidth(int width) const
{
    return doLayout(QRect(0, 0, width, 0), true);
}

QSize FlowLayout::minimumSize() const
{
    return QSize(50, 24);
}

QSize FlowLayout::sizeHint() const
{
    return QSize(100, 24);
}

void FlowLayout::setGeometry(const QRect &rect)
{
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

int FlowLayout::doLayout(const QRect &rect, bool testOnly) const
{
    const QMargins margins = contentsMargins();
    const QRect effectiveRect = rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
    const int maxLineWidth = qMax(40, effectiveRect.width());
    const int spaceX = m_hSpace;
    const int spaceY = m_vSpace;

    int x = effectiveRect.x();
    int y = effectiveRect.y();
    int lineHeight = 0;

    for (QLayoutItem *item : m_itemList) {
        QWidget *w = item->widget();
        FlowChipButton *chip = dynamic_cast<FlowChipButton*>(w);

        QSize itemSize;
        bool isFullLine = false;

        if (chip) {
            itemSize = chip->calculateSizeForWidth(maxLineWidth);
            if (itemSize.width() >= maxLineWidth) {
                isFullLine = true;
                itemSize.setWidth(maxLineWidth);
                chip->setFullLine(true);
            } else {
                chip->setFullLine(false);
            }
        } else {
            itemSize = item->sizeHint();
            if (itemSize.width() >= maxLineWidth) {
                isFullLine = true;
                itemSize.setWidth(maxLineWidth);
            }
        }

        if (isFullLine) {
            if (x > effectiveRect.x()) {
                x = effectiveRect.x();
                y += lineHeight + spaceY;
                lineHeight = 0;
            }
            if (!testOnly) {
                item->setGeometry(QRect(QPoint(effectiveRect.x(), y), itemSize));
            }
            y += itemSize.height() + spaceY;
            x = effectiveRect.x();
            lineHeight = 0;
            continue;
        }

        int nextX = x + itemSize.width() + spaceX;
        if (nextX - spaceX > effectiveRect.right() && x > effectiveRect.x()) {
            x = effectiveRect.x();
            y += lineHeight + spaceY;
            nextX = x + itemSize.width() + spaceX;
            lineHeight = 0;
        }
        if (!testOnly) {
            item->setGeometry(QRect(QPoint(x, y), itemSize));
        }
        x = nextX;
        lineHeight = qMax(lineHeight, itemSize.height());
    }

    const int totalH = (lineHeight > 0) ? (y + lineHeight) : (y > effectiveRect.y() ? (y - spaceY) : y);
    return totalH - rect.y() + margins.bottom();
}
