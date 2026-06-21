#ifndef CHARTWIDGETS_H
#define CHARTWIDGETS_H

// 轻量 QPainter 自绘图表控件（无 Qt Charts 依赖、无 Q_OBJECT）。
// 用于「模型空间占用分析」：饼图展示占比，条形图展示具体大小。

#include <QColor>
#include "styleconstants.h"
#include <QFont>
#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QSizePolicy>
#include <QString>
#include <QVector>
#include <QWidget>
#include <QtMath>

struct ChartSlice {
    QString label;
    double value = 0.0;
    QString valueText;   // 展示用文本（如 "1.2 GB"）
    QColor color;
};

// 饼图 + 右侧图例（标签 / 大小 / 占比）。
class PieChartWidget : public QWidget {
public:
    explicit PieChartWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        setMinimumHeight(170);
    }

    void setData(const QVector<ChartSlice> &slices) {
        m_slices = slices;
        m_total = 0.0;
        for (const ChartSlice &s : m_slices) m_total += qMax(0.0, s.value);
        updateGeometry();
        update();
    }

    QSize sizeHint() const override {
        return QSize(380, qMax(170, m_slices.size() * 20 + 24));
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);

        const int margin = 10;
        if (m_slices.isEmpty() || m_total <= 0) {
            p.setPen(AppStyle::color("mutedText"));
            p.drawText(rect(), Qt::AlignCenter, "暂无数据");
            return;
        }

        const int diameter = qMax(40, qMin(height() - 2 * margin, width() / 2 - margin));
        const QRectF pieRect(margin, (height() - diameter) / 2.0, diameter, diameter);

        double startAngle = 90.0 * 16.0; // 顶部起，顺时针
        for (const ChartSlice &s : m_slices) {
            if (s.value <= 0) continue;
            const double span = -(s.value / m_total) * 360.0 * 16.0;
            p.setPen(QPen(AppStyle::color("inputBg"), 1));
            p.setBrush(s.color);
            p.drawPie(pieRect, qRound(startAngle), qRound(span));
            startAngle += span;
        }

        // 右侧图例
        const int legendX = qRound(pieRect.right()) + 16;
        QFont f = font();
        f.setPixelSize(12);
        p.setFont(f);
        const QFontMetrics fm(f);
        const int lh = qMax(18, fm.height() + 4);
        int y = margin;
        for (const ChartSlice &s : m_slices) {
            if (y + lh > height() - margin) break;
            p.setPen(Qt::NoPen);
            p.setBrush(s.color);
            p.drawRoundedRect(legendX, y + 2, 12, 12, 2, 2);
            p.setPen(AppStyle::color("bodyText"));
            const double pct = m_total > 0 ? s.value / m_total * 100.0 : 0.0;
            const QString text = QString("%1  %2  %3%").arg(s.label, s.valueText).arg(pct, 0, 'f', 1);
            const int textX = legendX + 18;
            const QString elided = fm.elidedText(text, Qt::ElideRight, width() - textX - margin);
            p.drawText(textX, y, width() - textX - margin, lh, Qt::AlignVCenter | Qt::AlignLeft, elided);
            y += lh;
        }
    }

private:
    QVector<ChartSlice> m_slices;
    double m_total = 0.0;
};

// 水平条形图：标签 + 归一化条 + 右侧大小文本。
class BarChartWidget : public QWidget {
public:
    explicit BarChartWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    }

    void setData(const QVector<ChartSlice> &slices) {
        m_slices = slices;
        m_max = 0.0;
        for (const ChartSlice &s : m_slices) m_max = qMax(m_max, s.value);
        setMinimumHeight(qMax(60, m_slices.size() * kRowH + 12));
        updateGeometry();
        update();
    }

    QSize sizeHint() const override {
        return QSize(380, qMax(60, m_slices.size() * kRowH + 12));
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);

        if (m_slices.isEmpty() || m_max <= 0) {
            p.setPen(AppStyle::color("mutedText"));
            p.drawText(rect(), Qt::AlignCenter, "暂无数据");
            return;
        }

        QFont f = font();
        f.setPixelSize(12);
        p.setFont(f);
        const QFontMetrics fm(f);

        const int margin = 6;
        const int labelW = qBound(80, width() / 3, 180);
        const int valueW = 96;
        const int barLeft = margin + labelW + 8;
        const int barRight = width() - margin - valueW - 8;
        const int barAreaW = qMax(10, barRight - barLeft);

        int y = margin;
        for (const ChartSlice &s : m_slices) {
            const int barH = 14;
            const int barY = y + (kRowH - barH) / 2;

            p.setPen(AppStyle::color("bodyText"));
            p.drawText(margin, y, labelW, kRowH, Qt::AlignVCenter | Qt::AlignLeft,
                       fm.elidedText(s.label, Qt::ElideRight, labelW));

            p.setPen(Qt::NoPen);
            p.setBrush(AppStyle::color("chartBar"));
            p.drawRoundedRect(barLeft, barY, barAreaW, barH, 3, 3);

            const int w = qRound(barAreaW * (s.value / m_max));
            if (w > 0) {
                p.setBrush(s.color);
                p.drawRoundedRect(barLeft, barY, qMax(2, w), barH, 3, 3);
            }

            p.setPen(AppStyle::color("chartAxis"));
            p.drawText(barRight + 8, y, valueW, kRowH, Qt::AlignVCenter | Qt::AlignRight, s.valueText);
            y += kRowH;
        }
    }

private:
    static constexpr int kRowH = 24;
    QVector<ChartSlice> m_slices;
    double m_max = 0.0;
};

#endif // CHARTWIDGETS_H
