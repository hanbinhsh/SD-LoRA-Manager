#ifndef TAGFLOWWIDGET_H
#define TAGFLOWWIDGET_H

#include <QWidget>
#include <QPainter>
#include <QMouseEvent>
#include <QMap>
#include <QSet>
#include <vector>
#include <algorithm>

// Tag 的状态结构体
struct TagState {
    QString text;
    int count;
    bool selected;
    QRect rect;
};

class TagFlowWidget : public QWidget {
    Q_OBJECT
public:
    explicit TagFlowWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        setMouseTracking(true);
    }

    // 设置数据：接收 Key=Tag名, Value=出现次数
    void setData(const QMap<QString, int> &data) {
        m_tags.clear();

        // 排序：按次数降序
        QList<QPair<QString, int>> sorted;
        for(auto it = data.begin(); it != data.end(); ++it) {
            sorted << qMakePair(it.key(), it.value());
        }

        std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b){
            return a.second > b.second;
        });

        for(const auto &pair : sorted) {
            m_tags.append({pair.first, pair.second, false, QRect()});
        }

        update();
        updateGeometry();
    }

    // 获取当前所有被选中的 Tag
    QSet<QString> getSelectedTags() const {
        QSet<QString> set;
        for(const auto &t : m_tags) {
            if(t.selected) set.insert(t.text);
        }
        return set;
    }

    // 告知布局系统理想的大小
    QSize sizeHint() const override {
        return QSize(400, m_calculatedHeight > 0 ? m_calculatedHeight : 50);
    }

signals:
    // 当筛选发生变化时发送信号
    void filterChanged(const QSet<QString> &selectedTags);

protected:
    int m_calculatedHeight = 0;

    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        int x = 0;
        int y = 0;
        int paddingX = 10;
        int marginX = 6;
        int marginY = 6;
        int widgetWidth = width();

        QFont font = p.font();
        font.setPixelSize(12);
        p.setFont(font);
        QFontMetrics fm(font);

        for (int i = 0; i < m_tags.size(); ++i) {
            TagState &tag = m_tags[i];
            QString displayText = QString("%1  %2").arg(tag.text).arg(tag.count);

            int textW = fm.horizontalAdvance(displayText);
            int itemW = textW + paddingX * 2;
            int itemH = 26;

            // 换行逻辑
            if (x + itemW > widgetWidth && x > 0) {
                x = 0;
                y += (itemH + marginY);
            }

            tag.rect = QRect(x, y, itemW, itemH);

            // 绘制背景 (选中变蓝，未选中深灰)
            QColor bgColor = tag.selected ? QColor("#66c0f4") : QColor("#2a3f5a");
            p.setBrush(bgColor);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(tag.rect, 4, 4);

            // 绘制文字 (选中变黑，未选中灰白)
            p.setPen(tag.selected ? QColor("#000000") : QColor("#dcdedf"));
            p.drawText(tag.rect, Qt::AlignCenter, displayText);

            x += (itemW + marginX);
        }

        m_calculatedHeight = y + 40; // 底部留白
        if (minimumHeight() != m_calculatedHeight) {
            setMinimumHeight(m_calculatedHeight);
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        bool changed = false;
        for (int i = 0; i < m_tags.size(); ++i) {
            if (m_tags[i].rect.contains(event->pos())) {
                m_tags[i].selected = !m_tags[i].selected;
                changed = true;
                break;
            }
        }

        if (changed) {
            update();
            emit filterChanged(getSelectedTags());
        }
    }

private:
    QList<TagState> m_tags;
};

#endif // TAGFLOWWIDGET_H
