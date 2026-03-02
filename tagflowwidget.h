#ifndef TAGFLOWWIDGET_H
#define TAGFLOWWIDGET_H

#include <QWidget>
#include <QPainter>
#include <QMouseEvent>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <QMenu>
#include <QClipboard>
#include <QApplication>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>

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

    void setData(const QMap<QString, int> &data) {
        m_tags.clear();
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

    void setTranslationMap(const QHash<QString, QString> *map) {
        m_translationMap = map;
        update();
    }

    void setShowTranslation(bool show) {
        m_showTranslation = show;
        update();
        updateGeometry();
    }

    QSet<QString> getSelectedTags() const {
        QSet<QString> set;
        for(const auto &t : m_tags) {
            if(t.selected) set.insert(t.text);
        }
        return set;
    }

    QSize sizeHint() const override {
        return QSize(400, m_calculatedHeight > 0 ? m_calculatedHeight : 50);
    }

signals:
    void filterChanged(const QSet<QString> &selectedTags);

protected:
    int m_calculatedHeight = 0;
    const QHash<QString, QString> *m_translationMap = nullptr;
    bool m_showTranslation = false;

    // === 新增：模糊匹配查找函数 ===
    QString tryGetTranslation(const QString &key) const {
        if (!m_translationMap) return "";

        // 1. 尝试精确匹配 (white_hair -> white_hair)
        if (m_translationMap->contains(key)) {
            return m_translationMap->value(key);
        }

        // 2. 尝试将 空格 替换为 下划线 (white hair -> white_hair)
        if (key.contains(' ')) {
            QString k = key;
            k.replace(' ', '_');
            if (m_translationMap->contains(k)) return m_translationMap->value(k);
        }

        // 3. 尝试将 下划线 替换为 空格 (white_hair -> white hair)
        if (key.contains('_')) {
            QString k = key;
            k.replace('_', ' ');
            if (m_translationMap->contains(k)) return m_translationMap->value(k);
        }

        return ""; // 没找到
    }

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

        QFont fontEn = p.font();
        fontEn.setPixelSize(12);

        QFont fontCn = p.font();
        fontCn.setPixelSize(10);

        p.setFont(fontEn);
        QFontMetrics fmEn(fontEn);
        QFontMetrics fmCn(fontCn);

        // 统一的高度设定
        // 如果开启翻译模式，强制所有 Tag 都是 42px 高
        // 如果关闭，则是 26px
        int itemH = m_showTranslation ? 42 : 26;

        for (int i = 0; i < m_tags.size(); ++i) {
            TagState &tag = m_tags[i];

            QString line1 = QString("%1  %2").arg(tag.text).arg(tag.count);
            QString line2 = "";

            // 只有开启模式时才去查字典
            if (m_showTranslation) {
                line2 = tryGetTranslation(tag.text);
            }

            // 计算宽度
            int textW = fmEn.horizontalAdvance(line1);
            if (!line2.isEmpty()) {
                int cnW = fmCn.horizontalAdvance(line2);
                if (cnW > textW) textW = cnW;
            }

            int itemW = textW + paddingX * 2;

            // 换行逻辑
            if (x + itemW > widgetWidth && x > 0) {
                x = 0;
                y += (itemH + marginY);
            }

            tag.rect = QRect(x, y, itemW, itemH);

            // 绘制背景
            QColor bgColor = tag.selected ? QColor("#66c0f4") : QColor("#2a3f5a");
            p.setBrush(bgColor);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(tag.rect, 4, 4);

            if (m_showTranslation) {
                // === 双行模式 (统一高度) ===

                // 第一行：英文 (始终显示)
                p.setFont(fontEn);
                p.setPen(tag.selected ? QColor("#000000") : QColor("#dcdedf"));
                QRect rectLine1(x, y + 2, itemW, 20);
                p.drawText(rectLine1, Qt::AlignCenter, line1);

                // 第二行：中文 (有就显示，没有就空着，但高度保留)
                if (!line2.isEmpty()) {
                    p.setFont(fontCn);
                    p.setPen(tag.selected ? QColor("#333333") : QColor("#8c96a0"));
                    QRect rectLine2(x, y + 20, itemW, 18);
                    p.drawText(rectLine2, Qt::AlignCenter, line2);
                } else {
                    // 可选：如果没有翻译，画个小横杠或者什么都不画
                    // p.setFont(fontCn);
                    // p.setPen(tag.selected ? QColor("#555555") : QColor("#4a5f7a"));
                    // QRect rectLine2(x, y + 20, itemW, 18);
                    // p.drawText(rectLine2, Qt::AlignCenter, "-");
                }
            }
            else {
                // === 单行模式 ===
                p.setFont(fontEn);
                p.setPen(tag.selected ? QColor("#000000") : QColor("#dcdedf"));
                p.drawText(tag.rect, Qt::AlignCenter, line1);
            }

            x += (itemW + marginX);
        }

        m_calculatedHeight = y + itemH + 20;

        if (minimumHeight() != m_calculatedHeight) {
            setMinimumHeight(m_calculatedHeight);
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
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
        QWidget::mousePressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override {
        QString clickedTag;
        for (const auto &tag : m_tags) {
            if (tag.rect.contains(event->pos())) {
                clickedTag = tag.text;
                break;
            }
        }

        QMenu menu(this);
        if (!clickedTag.isEmpty()) {
            QAction *actCopy = menu.addAction("复制 \"" + clickedTag + "\"");
            connect(actCopy, &QAction::triggered, this, [clickedTag](){
                QApplication::clipboard()->setText(clickedTag);
            });
        }

        bool hasSelection = !getSelectedTags().isEmpty();
        if (hasSelection) {
            QAction *actCopyAll = menu.addAction("复制已选中的 Tags");
            connect(actCopyAll, &QAction::triggered, this, [this](){
                QStringList list = getSelectedTags().values();
                QApplication::clipboard()->setText(list.join(", "));
            });
        }

        QAction *actCopyEverything = menu.addAction("复制全部 Tags");
        connect(actCopyEverything, &QAction::triggered, this, [this](){
            QStringList list;
            list.reserve(m_tags.size());
            for (const auto &tag : m_tags) {
                list.append(tag.text);
            }
            QApplication::clipboard()->setText(list.join(", "));
        });

        QAction *actCopyByRule = menu.addAction("批量复制 Tags...");
        connect(actCopyByRule, &QAction::triggered, this, [this](){
            if (m_tags.isEmpty()) return;

            QDialog dlg(this);
            dlg.setWindowTitle("批量复制 Tags");
            dlg.setMinimumWidth(360);

            QVBoxLayout *root = new QVBoxLayout(&dlg);
            QLabel *lblMode = new QLabel("复制方式:", &dlg);
            root->addWidget(lblMode);

            QComboBox *modeBox = new QComboBox(&dlg);
            modeBox->addItem("复制前 N 个 Tag（按使用次数排序）");
            modeBox->addItem("复制使用次数 >= X 的 Tag");
            root->addWidget(modeBox);

            QHBoxLayout *valueRow = new QHBoxLayout();
            QLabel *lblValue = new QLabel("N:", &dlg);
            QSpinBox *spinValue = new QSpinBox(&dlg);
            spinValue->setRange(1, qMax(1, m_tags.size()));
            spinValue->setValue(qMin(20, m_tags.size()));
            valueRow->addWidget(lblValue);
            valueRow->addWidget(spinValue, 1);
            root->addLayout(valueRow);

            connect(modeBox, &QComboBox::currentIndexChanged, &dlg, [this, lblValue, spinValue](int idx){
                if (idx == 0) {
                    lblValue->setText("N:");
                    spinValue->setRange(1, qMax(1, m_tags.size()));
                    spinValue->setValue(qMin(20, m_tags.size()));
                } else {
                    int maxCount = 1;
                    for (const auto &tag : m_tags) {
                        if (tag.count > maxCount) maxCount = tag.count;
                    }
                    lblValue->setText("X:");
                    spinValue->setRange(1, maxCount);
                    spinValue->setValue(qMin(5, maxCount));
                }
            });

            QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
            root->addWidget(buttons);
            connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

            if (dlg.exec() != QDialog::Accepted) return;

            QStringList out;
            int inputValue = spinValue->value();
            if (modeBox->currentIndex() == 0) {
                int n = qMin(inputValue, m_tags.size());
                out.reserve(n);
                for (int i = 0; i < n; ++i) out.append(m_tags[i].text);
            } else {
                for (const auto &tag : m_tags) {
                    if (tag.count >= inputValue) out.append(tag.text);
                }
            }

            if (out.isEmpty()) {
                QMessageBox::information(this, "提示", "没有符合条件的 Tag。");
                return;
            }
            QApplication::clipboard()->setText(out.join(", "));
        });

        // ================= 新增导出 CSV 功能 =================
        menu.addSeparator(); // 加上一条分隔线
        QAction *actExportCsv = menu.addAction("导出 Tags 到 CSV...");
        connect(actExportCsv, &QAction::triggered, this, [this](){
            if (m_tags.isEmpty()) {
                QMessageBox::information(this, "提示", "没有可导出的 Tag。");
                return;
            }

            QString fileName = QFileDialog::getSaveFileName(this, "导出 Tags 到 CSV", "", "CSV Files (*.csv)");
            if (fileName.isEmpty()) return;

            QFile file(fileName);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QMessageBox::warning(this, "错误", "无法写入文件。");
                return;
            }

            QTextStream out(&file);
            // 写入 UTF-8 BOM 头，保证生成的 CSV 在 Excel 中双击打开不会乱码
            out << "\xEF\xBB\xBF";
            out << "Tag,Count\n";

            for (const auto &tag : m_tags) {
                QString t = tag.text;
                // CSV 格式转义：如果内容本身包含逗号、双引号或换行符，必须用双引号包起来，并且内部双引号要双写
                if (t.contains('"') || t.contains(',') || t.contains('\n')) {
                    t.replace("\"", "\"\"");
                    t = "\"" + t + "\"";
                }
                out << t << "," << tag.count << "\n";
            }
            file.close();
            QMessageBox::information(this, "成功", "导出成功！");
        });

        // =====================================================

        if (!menu.isEmpty()) menu.exec(event->globalPos());
    }

private:
    QList<TagState> m_tags;
};

#endif // TAGFLOWWIDGET_H
