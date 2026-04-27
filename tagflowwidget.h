#ifndef TAGFLOWWIDGET_H
#define TAGFLOWWIDGET_H

#include <QWidget>
#include <QPainter>
#include <QMouseEvent>
#include <QMap>
#include <QSet>
#include <QPair>
#include <QVector>
#include <algorithm>
#include <QMenu>
#include <QCheckBox>
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
#include <QStringConverter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QContextMenuEvent>
#include <QPixmap>
#include <QtMath>

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
        m_lastClickedIndex = -1;
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
        invalidateLayoutAndCache();
    }

    void setTranslationMap(const QHash<QString, QString> *map) {
        m_translationMap = map;
        invalidateLayoutAndCache();
    }

    void setShowTranslation(bool show) {
        if (m_showTranslation == show) return;
        m_showTranslation = show;
        invalidateLayoutAndCache();
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
    bool m_layoutDirty = true;
    bool m_cacheDirty = true;
    int m_layoutWidth = -1;
    QPixmap m_cachedPixmap;
    static constexpr qsizetype kMaxCachedPixels = 8 * 1024 * 1024;

    void invalidateLayoutAndCache() {
        m_layoutDirty = true;
        m_cacheDirty = true;
        updateGeometry();
        update();
    }

    void ensureLayout() {
        const int widgetWidth = qMax(1, width());
        if (!m_layoutDirty && m_layoutWidth == widgetWidth) return;

        m_layoutWidth = widgetWidth;
        m_layoutDirty = false;
        m_cacheDirty = true;

        int x = 0;
        int y = 0;
        const int paddingX = 10;
        const int marginX = 6;
        const int marginY = 6;

        QFont fontEn = font();
        fontEn.setPixelSize(12);

        QFont fontCn = font();
        fontCn.setPixelSize(10);

        QFontMetrics fmEn(fontEn);
        QFontMetrics fmCn(fontCn);
        const int itemH = m_showTranslation ? 42 : 26;

        for (int i = 0; i < m_tags.size(); ++i) {
            TagState &tag = m_tags[i];
            const QString line1 = QString("%1  %2").arg(tag.text).arg(tag.count);
            QString line2;
            if (m_showTranslation) {
                line2 = tryGetTranslation(tag.text);
            }

            int textW = fmEn.horizontalAdvance(line1);
            if (!line2.isEmpty()) {
                textW = qMax(textW, fmCn.horizontalAdvance(line2));
            }

            const int itemW = textW + paddingX * 2;
            if (x + itemW > widgetWidth && x > 0) {
                x = 0;
                y += itemH + marginY;
            }

            tag.rect = QRect(x, y, itemW, itemH);
            x += itemW + marginX;
        }

        const int newHeight = m_tags.isEmpty() ? 50 : (y + itemH + 20);
        if (m_calculatedHeight != newHeight) {
            m_calculatedHeight = newHeight;
            if (minimumHeight() != m_calculatedHeight) {
                setMinimumHeight(m_calculatedHeight);
            }
            updateGeometry();
        }
    }

    bool shouldUsePixmapCache(const QSize &logicalSize) const {
        return qsizetype(logicalSize.width()) * qsizetype(logicalSize.height()) <= kMaxCachedPixels;
    }

    void paintTags(QPainter &p, const QRect &clipRect) const {
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);

        QFont fontEn = font();
        fontEn.setPixelSize(12);

        QFont fontCn = font();
        fontCn.setPixelSize(10);

        for (const TagState &tag : m_tags) {
            if (!clipRect.isNull() && !tag.rect.intersects(clipRect.adjusted(-4, -4, 4, 4))) {
                continue;
            }

            const QString line1 = QString("%1  %2").arg(tag.text).arg(tag.count);
            const QString line2 = m_showTranslation ? tryGetTranslation(tag.text) : QString();

            const QColor bgColor = tag.selected ? QColor("#66c0f4") : QColor("#2a3f5a");
            p.setBrush(bgColor);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(tag.rect, 4, 4);

            if (m_showTranslation) {
                p.setFont(fontEn);
                p.setPen(tag.selected ? QColor("#000000") : QColor("#dcdedf"));
                QRect rectLine1(tag.rect.x(), tag.rect.y() + 2, tag.rect.width(), 20);
                p.drawText(rectLine1, Qt::AlignCenter, line1);

                if (!line2.isEmpty()) {
                    p.setFont(fontCn);
                    p.setPen(tag.selected ? QColor("#333333") : QColor("#8c96a0"));
                    QRect rectLine2(tag.rect.x(), tag.rect.y() + 20, tag.rect.width(), 18);
                    p.drawText(rectLine2, Qt::AlignCenter, line2);
                }
            } else {
                p.setFont(fontEn);
                p.setPen(tag.selected ? QColor("#000000") : QColor("#dcdedf"));
                p.drawText(tag.rect, Qt::AlignCenter, line1);
            }
        }
    }

    void rebuildCache() {
        ensureLayout();

        const QSize cacheSize(qMax(1, width()), qMax(1, m_calculatedHeight));
        if (!shouldUsePixmapCache(cacheSize)) {
            m_cachedPixmap = QPixmap();
            m_cacheDirty = false;
            return;
        }

        const qreal dpr = qMax<qreal>(1.0, devicePixelRatioF());
        const QSize physicalSize(qCeil(cacheSize.width() * dpr), qCeil(cacheSize.height() * dpr));
        m_cachedPixmap = QPixmap(physicalSize);
        m_cachedPixmap.setDevicePixelRatio(dpr);
        m_cachedPixmap.fill(Qt::transparent);

        QPainter p(&m_cachedPixmap);
        paintTags(p, QRect(QPoint(0, 0), cacheSize));

        m_cacheDirty = false;
    }

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
        const QSize expectedSize(qMax(1, width()), qMax(1, m_calculatedHeight));
        const qreal expectedDpr = qMax<qreal>(1.0, devicePixelRatioF());
        const bool useCache = shouldUsePixmapCache(expectedSize);
        const QSize cachedLogicalSize(
            qRound(m_cachedPixmap.width() / qMax<qreal>(1.0, m_cachedPixmap.devicePixelRatio())),
            qRound(m_cachedPixmap.height() / qMax<qreal>(1.0, m_cachedPixmap.devicePixelRatio()))
        );

        if (m_cacheDirty ||
            m_layoutDirty ||
            (useCache && m_cachedPixmap.isNull()) ||
            (useCache && cachedLogicalSize != expectedSize) ||
            (useCache && !qFuzzyCompare(m_cachedPixmap.devicePixelRatio(), expectedDpr))) {
            rebuildCache();
        }

        QPainter p(this);
        p.setRenderHint(QPainter::TextAntialiasing);
        if (useCache && !m_cachedPixmap.isNull()) {
            p.drawPixmap(0, 0, m_cachedPixmap);
        } else {
            paintTags(p, event->rect());
        }
    }

    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        if (event->size().width() != event->oldSize().width()) {
            invalidateLayoutAndCache();
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        ensureLayout();
        if (event->button() == Qt::LeftButton) {
            bool changed = false;
            int clickedIndex = -1;
            for (int i = 0; i < m_tags.size(); ++i) {
                if (m_tags[i].rect.contains(event->pos())) {
                    clickedIndex = i;
                    break;
                }
            }
            if (clickedIndex >= 0) {
                const bool shiftPressed = event->modifiers().testFlag(Qt::ShiftModifier);
                const bool ctrlPressed = event->modifiers().testFlag(Qt::ControlModifier);
                if (shiftPressed && m_lastClickedIndex >= 0) {
                    if (!ctrlPressed) {
                        for (auto &tag : m_tags) tag.selected = false;
                    }
                    const int begin = qMin(m_lastClickedIndex, clickedIndex);
                    const int end = qMax(m_lastClickedIndex, clickedIndex);
                    for (int i = begin; i <= end; ++i) {
                        m_tags[i].selected = true;
                    }
                } else {
                    m_tags[clickedIndex].selected = !m_tags[clickedIndex].selected;
                }
                m_lastClickedIndex = clickedIndex;
                changed = true;
            }
            if (changed) {
                m_cacheDirty = true;
                update();
                emit filterChanged(getSelectedTags());
            }
        }
        QWidget::mousePressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override {
        ensureLayout();
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

        QAction *actBatchExport = menu.addAction("批量导出 Tags...");
        connect(actBatchExport, &QAction::triggered, this, [this](){
            showBatchExportDialog();
        });

        if (!menu.isEmpty()) menu.exec(event->globalPos());
    }

private:
    QList<TagState> m_tags;
    int m_lastClickedIndex = -1;

    QString escapeCsvField(QString text) const {
        if (text.contains('"')) text.replace("\"", "\"\"");
        if (text.contains(',') || text.contains('"') || text.contains('\n') || text.contains('\r')) {
            text = "\"" + text + "\"";
        }
        return text;
    }

    QVector<QPair<QString, int>> allTagRows() const {
        QVector<QPair<QString, int>> rows;
        rows.reserve(m_tags.size());
        for (const auto &tag : m_tags) {
            rows.append(qMakePair(tag.text, tag.count));
        }
        return rows;
    }

    QVector<QPair<QString, int>> selectedTagRows() const {
        QVector<QPair<QString, int>> rows;
        for (const auto &tag : m_tags) {
            if (tag.selected) rows.append(qMakePair(tag.text, tag.count));
        }
        return rows;
    }

    void showBatchExportDialog() {
        const QVector<QPair<QString, int>> allRows = allTagRows();
        if (allRows.isEmpty()) {
            QMessageBox::information(nullptr, "提示", "没有可导出的 Tag。");
            return;
        }

        const QVector<QPair<QString, int>> selectedRows = selectedTagRows();
        QDialog dlg;
        dlg.setWindowTitle("批量导出 Tags");
        dlg.setMinimumWidth(420);

        QVBoxLayout *root = new QVBoxLayout(&dlg);
        root->addWidget(new QLabel("导出范围:", &dlg));

        QComboBox *rangeBox = new QComboBox(&dlg);
        if (!selectedRows.isEmpty()) rangeBox->addItem("选中项", "selected");
        rangeBox->addItem("全部", "all");
        rangeBox->addItem("Top K（按使用次数排序）", "top");
        rangeBox->addItem("使用次数 >= K", "count");
        root->addWidget(rangeBox);

        QHBoxLayout *valueRow = new QHBoxLayout();
        QLabel *valueLabel = new QLabel("K:", &dlg);
        QSpinBox *valueSpin = new QSpinBox(&dlg);
        int maxCount = 1;
        for (const auto &row : allRows) maxCount = qMax(maxCount, row.second);
        const int allRowCount = allRows.size();
        valueSpin->setRange(1, qMax(1, allRowCount));
        valueSpin->setValue(qMin(50, allRowCount));
        valueRow->addWidget(valueLabel);
        valueRow->addWidget(valueSpin, 1);
        root->addLayout(valueRow);

        root->addWidget(new QLabel("输出方式:", &dlg));
        QComboBox *outputBox = new QComboBox(&dlg);
        outputBox->addItem("复制到剪贴板", "copy");
        outputBox->addItem("导出 CSV", "csv");
        root->addWidget(outputBox);

        QLabel *columnsLabel = new QLabel("CSV 导出列:", &dlg);
        root->addWidget(columnsLabel);
        QHBoxLayout *columnsRow = new QHBoxLayout();
        QCheckBox *chkTag = new QCheckBox("Tag", &dlg);
        QCheckBox *chkCount = new QCheckBox("Count", &dlg);
        chkTag->setChecked(true);
        chkCount->setChecked(true);
        columnsRow->addWidget(chkTag);
        columnsRow->addWidget(chkCount);
        columnsRow->addStretch(1);
        root->addLayout(columnsRow);

        auto refreshValueRow = [rangeBox, valueLabel, valueSpin, maxCount, count = allRowCount]() {
            const QString mode = rangeBox->currentData().toString();
            const bool needsValue = (mode == "top" || mode == "count");
            valueLabel->setEnabled(needsValue);
            valueSpin->setEnabled(needsValue);
            if (mode == "top") {
                valueLabel->setText("K:");
                valueSpin->setRange(1, qMax(1, count));
                valueSpin->setValue(qMin(valueSpin->value(), count));
            } else if (mode == "count") {
                valueLabel->setText("K:");
                valueSpin->setRange(1, maxCount);
                valueSpin->setValue(qMin(valueSpin->value(), maxCount));
            }
        };
        connect(rangeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, refreshValueRow);
        refreshValueRow();

        auto refreshColumnOptions = [outputBox, columnsLabel, chkTag, chkCount]() {
            const bool csvMode = outputBox->currentData().toString() == "csv";
            columnsLabel->setEnabled(csvMode);
            chkTag->setEnabled(csvMode);
            chkCount->setEnabled(csvMode);
        };
        connect(outputBox, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, refreshColumnOptions);
        refreshColumnOptions();

        QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        root->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

        if (dlg.exec() != QDialog::Accepted) return;

        QVector<QPair<QString, int>> rows;
        const QString rangeMode = rangeBox->currentData().toString();
        if (rangeMode == "selected") {
            rows = selectedRows;
        } else if (rangeMode == "all") {
            rows = allRows;
        } else if (rangeMode == "top") {
            rows = allRows;
            std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });
            rows.resize(qMin(valueSpin->value(), int(rows.size())));
        } else {
            const int threshold = valueSpin->value();
            for (const auto &row : allRows) {
                if (row.second >= threshold) rows.append(row);
            }
        }

        if (rows.isEmpty()) {
            QMessageBox::information(nullptr, "提示", "没有符合条件的 Tag。");
            return;
        }

        if (outputBox->currentData().toString() == "copy") {
            QStringList tags;
            tags.reserve(rows.size());
            for (const auto &row : rows) tags.append(row.first);
            QApplication::clipboard()->setText(tags.join(", "));
            return;
        }

        QString fileName = QFileDialog::getSaveFileName(nullptr, "导出 Tags 到 CSV", "", "CSV Files (*.csv)");
        if (fileName.isEmpty()) return;
        if (!fileName.endsWith(".csv", Qt::CaseInsensitive)) fileName += ".csv";

        QStringList headers;
        if (chkTag->isChecked()) headers << "Tag";
        if (chkCount->isChecked()) headers << "Count";
        if (headers.isEmpty()) {
            QMessageBox::information(nullptr, "提示", "请至少选择一列导出。");
            return;
        }

        QFile file(fileName);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(nullptr, "错误", "无法写入文件。");
            return;
        }

        file.write("\xEF\xBB\xBF", 3);
        QTextStream out(&file);
        out.setEncoding(QStringConverter::Utf8);
        out << headers.join(",") << "\n";
        for (const auto &row : rows) {
            QStringList values;
            if (chkTag->isChecked()) values << escapeCsvField(row.first);
            if (chkCount->isChecked()) values << QString::number(row.second);
            out << values.join(",") << "\n";
        }
        QMessageBox::information(nullptr, "成功", "导出成功！");
    }
};

#endif // TAGFLOWWIDGET_H
