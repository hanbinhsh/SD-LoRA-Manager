#include "highlightitemdelegate.h"
#include "core/modelitemroles.h"
#include "utils/styleconstants.h"

#include <QApplication>
#include <QColor>
#include <QPainter>
#include <QStyle>

HighlightItemDelegate::HighlightItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

void HighlightItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    const QWidget *widget = opt.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();

    // Draw the native item background first, then place the user highlight
    // under the icon/text so translucent colors do not wash out content.
    QStyleOptionViewItem backgroundOpt(opt);
    backgroundOpt.text.clear();
    backgroundOpt.icon = QIcon();
    backgroundOpt.features &= ~QStyleOptionViewItem::HasDisplay;
    backgroundOpt.features &= ~QStyleOptionViewItem::HasDecoration;
    style->drawControl(QStyle::CE_ItemViewItem, &backgroundOpt, painter, widget);

    const QColor highlight = index.data(ROLE_MODEL_HIGHLIGHT_COLOR).value<QColor>();
    if (highlight.isValid() && highlight.alpha() > 0) {
        painter->save();
        painter->fillRect(opt.rect, highlight);
        painter->restore();
    }

    if (!opt.icon.isNull()) {
        const QRect iconRect = style->subElementRect(QStyle::SE_ItemViewItemDecoration, &opt, widget);
        QIcon::Mode mode = QIcon::Normal;
        if (!(opt.state & QStyle::State_Enabled)) {
            mode = QIcon::Disabled;
        } else if (opt.state & QStyle::State_Selected) {
            mode = QIcon::Selected;
        }
        const QIcon::State state = (opt.state & QStyle::State_Open) ? QIcon::On : QIcon::Off;
        opt.icon.paint(painter, iconRect, opt.decorationAlignment, mode, state);
    }

    if (!opt.text.isEmpty()) {
        const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
        const QString text = opt.fontMetrics.elidedText(opt.text, opt.textElideMode, textRect.width());
        // 用主题色而非 palette 角色（QSS 设的颜色被 delegate 自绘绕过，会在浅色主题下变白看不清）。
        const bool sel = opt.state & QStyle::State_Selected;
        painter->save();
        painter->setPen(sel ? AppStyle::color("primaryText") : AppStyle::color("mutedText2"));
        painter->drawText(textRect, opt.displayAlignment, text);
        painter->restore();
    }
}
