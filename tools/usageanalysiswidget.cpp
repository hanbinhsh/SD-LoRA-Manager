#include "utils/applanguage.h"
#include "usageanalysiswidget.h"
#include "chartwidgets.h"
#include "styleconstants.h"
#include "tableviewstylehelper.h"
#include "ui_usageanalysiswidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrent>
#include <QtMath>
#include <algorithm>
#include <functional>
#include <vector>

namespace {
class NumericTableItem : public QTableWidgetItem
{
public:
    explicit NumericTableItem(int value)
        : QTableWidgetItem(QString::number(value))
        , numericValue(value)
    {
        setData(Qt::UserRole, value);
    }

    bool operator<(const QTableWidgetItem &other) const override
    {
        return numericValue < other.data(Qt::UserRole).toInt();
    }

private:
    int numericValue = 0;
};

QString humanSize(qint64 bytes)
{
    if (bytes <= 0) return "0 B";
    constexpr double kb = 1024.0, mb = kb * 1024.0, gb = mb * 1024.0, tb = gb * 1024.0;
    const double b = static_cast<double>(bytes);
    if (b >= tb) return QString::number(b / tb, 'f', 2) + " TB";
    if (b >= gb) return QString::number(b / gb, 'f', 2) + " GB";
    if (b >= mb) return QString::number(b / mb, 'f', 1) + " MB";
    if (b >= kb) return QString::number(b / kb, 'f', 0) + " KB";
    return QString::number(bytes) + " B";
}

// 把 Civitai 类型规范化为展示类别。
QString normalizeCategory(const QString &modelType, const QString &rootName)
{
    const QString t = modelType.trimmed().toLower();
    if (t.contains("lora") || t.contains("locon") || t.contains("dora") || t.contains("lycoris")) return "LoRA";
    if (t.contains("checkpoint")) return AppText::tr("底模 (Checkpoint)");
    if (t.contains("textualinversion") || t.contains("embedding")) return "Embedding";
    if (t.contains("vae")) return "VAE";
    if (t.contains("hypernetwork")) return "Hypernetwork";
    if (t.contains("controlnet")) return "ControlNet";
    if (!t.isEmpty()) return modelType.trimmed();
    return rootName.trimmed().isEmpty() ? AppText::tr("其他") : rootName.trimmed();
}

// 一组与深色主题搭配的稳定调色板（按索引取色）。
QColor chartColorAt(int index)
{
    static const QStringList palette = {
        "#66c0f4", "#5fd38d", "#ffcc00", "#ff6b6b", "#c678dd",
        "#56b6c2", "#e08e5a", "#8fb4ff", "#b7d96a", "#f06292"
    };
    return QColor(palette.at(index % palette.size()));
}

// 按字节数排序、显示人类可读大小的表格项。
class SizeTableItem : public QTableWidgetItem
{
public:
    explicit SizeTableItem(qint64 bytes)
        : QTableWidgetItem(humanSize(bytes)), m_bytes(bytes)
    {
        setData(Qt::UserRole, static_cast<qlonglong>(bytes));
    }

    bool operator<(const QTableWidgetItem &other) const override
    {
        return m_bytes < other.data(Qt::UserRole).toLongLong();
    }

private:
    qint64 m_bytes = 0;
};

enum class GraphNodeType {
    Tag = 0,
    Lora = 1,
    Checkpoint = 2
};

struct GraphNode {
    QString id;
    QString label;
    QString rawName;
    QString translation;
    GraphNodeType type = GraphNodeType::Tag;
    int count = 0;
    QPointF pos;
    QPointF vel;
    double radius = 18.0;
    bool pinned = false;
};

struct GraphEdge {
    int source = 0;
    int target = 0;
    int weight = 0;
};

QColor nodeColorForType(GraphNodeType type)
{
    switch (type) {
    case GraphNodeType::Checkpoint:
        return QColor("#c678dd");
    case GraphNodeType::Lora:
        return AppStyle::color("successGreenBright");
    case GraphNodeType::Tag:
    default:
        return AppStyle::color("accentBlue");
    }
}

QString nodeTypeDisplayName(GraphNodeType type)
{
    switch (type) {
    case GraphNodeType::Checkpoint:
        return AppText::tr("📦 底模");
    case GraphNodeType::Lora:
        return AppText::tr("🏷️ LoRA");
    case GraphNodeType::Tag:
    default:
        return AppText::tr("🔹 Tag");
    }
}

} // namespace

class TagModelGraphCanvas : public QWidget
{
public:
    explicit TagModelGraphCanvas(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setMinimumSize(420, 360);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        m_physicsTimer = new QTimer(this);
        m_physicsTimer->setInterval(16);
        connect(m_physicsTimer, &QTimer::timeout, this, [this]() {
            stepPhysics(0.85);
            update();
            if (++m_animSteps >= m_maxAnimSteps) {
                m_physicsTimer->stop();
            }
        });

        m_loadingTimer = new QTimer(this);
        m_loadingTimer->setInterval(30);
        connect(m_loadingTimer, &QTimer::timeout, this, [this]() {
            m_loadingAngle = (m_loadingAngle + 15) % 360;
            update();
        });
    }

    std::function<void(const QString &nodeId)> onNodeSelected;
    std::function<void(const QString &nodeId)> onNodeDoubleClicked;
    std::function<void()> onEmptyDoubleClicked;

    void setLoading(bool loading, const QString &message = QString())
    {
        m_loading = loading;
        if (!message.isEmpty()) {
            m_loadingText = message;
        } else if (m_loadingText.isEmpty()) {
            m_loadingText = AppText::tr("正在加载 Tag 与模型共现图谱...");
        }
        if (m_loading) {
            m_physicsTimer->stop();
            if (!m_loadingTimer->isActive()) {
                m_loadingTimer->start();
            }
        } else {
            m_loadingTimer->stop();
        }
        update();
    }

    bool isLoading() const { return m_loading; }

    void setShowTranslation(bool show)
    {
        if (m_showTranslation == show) return;
        m_showTranslation = show;
        update();
    }

    void setSearchHighlight(const QString &needle)
    {
        m_searchNeedle = needle.trimmed().toLower();
        update();
    }

    void setSelectedNodeId(const QString &nodeId)
    {
        m_selectedNodeId = nodeId;
        update();
    }

    QString selectedNodeId() const { return m_selectedNodeId; }

    void setGraph(QVector<GraphNode> nodes,
                  QVector<GraphEdge> edges,
                  const QString &focusedNodeId,
                  bool runLayout,
                  bool precomputedPositions = false)
    {
        m_nodes = std::move(nodes);
        m_edges = std::move(edges);
        m_focusedNodeId = focusedNodeId;
        m_hoveredIndex = -1;
        m_draggingIndex = -1;

        m_idToIndex.clear();
        m_neighborsOf.clear();
        m_neighborsOf.resize(m_nodes.size());
        m_maxEdgeWeight = 1;
        for (int i = 0; i < m_nodes.size(); ++i) {
            m_idToIndex.insert(m_nodes[i].id, i);
        }
        for (const GraphEdge &e : m_edges) {
            if (e.source >= 0 && e.source < m_nodes.size() && e.target >= 0 && e.target < m_nodes.size()) {
                m_neighborsOf[e.source].insert(e.target);
                m_neighborsOf[e.target].insert(e.source);
                m_maxEdgeWeight = qMax(m_maxEdgeWeight, e.weight);
            }
        }

        if (runLayout) {
            if (!precomputedPositions) {
                initializePositions();
                for (int i = 0; i < 65; ++i) {
                    stepPhysics(1.0);
                }
            }
            m_animSteps = 0;
            m_maxAnimSteps = 45;
            m_physicsTimer->start();
        }
        update();
    }

    void updateTranslations(const std::function<QString(const QString &)> &translator)
    {
        for (GraphNode &n : m_nodes) {
            if (n.type == GraphNodeType::Tag) {
                n.translation = translator(n.rawName);
            }
        }
        update();
    }

    void relayout()
    {
        for (GraphNode &n : m_nodes) {
            n.pinned = (!m_focusedNodeId.isEmpty() && n.id == m_focusedNodeId);
        }
        initializePositions();
        for (int i = 0; i < 65; ++i) {
            stepPhysics(1.0);
        }
        m_animSteps = 0;
        m_maxAnimSteps = 55;
        m_physicsTimer->start();
        update();
    }

    void resetView()
    {
        m_zoom = 1.0;
        m_panOffset = QPointF(0.0, 0.0);
        update();
    }

    void centerOnNode(const QString &nodeId)
    {
        const int idx = m_idToIndex.value(nodeId, -1);
        if (idx < 0 || idx >= m_nodes.size()) return;
        m_panOffset = -m_nodes[idx].pos * m_zoom;
        update();
    }

protected:
    QPointF worldToScreen(const QPointF &worldPos) const
    {
        const QPointF center(width() / 2.0 + m_panOffset.x(), height() / 2.0 + m_panOffset.y());
        return center + worldPos * m_zoom;
    }

    QPointF screenToWorld(const QPointF &screenPos) const
    {
        const QPointF center(width() / 2.0 + m_panOffset.x(), height() / 2.0 + m_panOffset.y());
        return (screenPos - center) / qMax(0.1, m_zoom);
    }

    int hitTestNode(const QPointF &screenPos) const
    {
        const QPointF worldPos = screenToWorld(screenPos);
        for (int i = m_nodes.size() - 1; i >= 0; --i) {
            const double r = m_nodes[i].radius + 4.0;
            const QPointF d = worldPos - m_nodes[i].pos;
            if (d.x() * d.x() + d.y() * d.y() <= r * r) {
                return i;
            }
        }
        return -1;
    }

public:
    static void initializeNodePositions(QVector<GraphNode> &nodes, int focusIdx)
    {
        const int n = nodes.size();
        if (n == 0) return;

        if (focusIdx >= 0 && focusIdx < n) {
            nodes[focusIdx].pos = QPointF(0.0, 0.0);
            nodes[focusIdx].vel = QPointF(0.0, 0.0);
            nodes[focusIdx].pinned = true;

            int ringIdx = 0;
            const int totalOther = qMax(1, n - 1);
            for (int i = 0; i < n; ++i) {
                if (i == focusIdx) continue;
                const double ratio = static_cast<double>(ringIdx) / totalOther;
                const int ring = (ringIdx < 12) ? 0 : (ringIdx < 32 ? 1 : 2);
                const double baseR = (ring == 0) ? 155.0 : (ring == 1 ? 265.0 : 365.0);
                const double angle = ratio * 2.0 * M_PI + (ring * 0.45);
                nodes[i].pos = QPointF(std::cos(angle) * baseR, std::sin(angle) * baseR);
                nodes[i].vel = QPointF(0.0, 0.0);
                nodes[i].pinned = false;
                ++ringIdx;
            }
            return;
        }

        // 全局模式：使用黄金角螺旋初始分布，模型靠内圈，Tag 均匀散布
        const double goldenAngle = M_PI * (3.0 - std::sqrt(5.0));
        for (int i = 0; i < n; ++i) {
            const double radiusScale = (nodes[i].type != GraphNodeType::Tag) ? 34.0 : 44.0;
            const double r = radiusScale * std::sqrt(i + 1);
            const double theta = i * goldenAngle;
            nodes[i].pos = QPointF(std::cos(theta) * r, std::sin(theta) * r);
            nodes[i].vel = QPointF(0.0, 0.0);
            nodes[i].pinned = false;
        }
    }

    static void stepNodePhysics(QVector<GraphNode> &nodes,
                                const QVector<GraphEdge> &edges,
                                int maxEdgeWeight,
                                int draggingIndex,
                                double dampingFactor)
    {
        const int n = nodes.size();
        if (n <= 1) return;

        QVector<QPointF> forces(n, QPointF(0.0, 0.0));
        const double repulsionK = 24000.0;

        // 1. 节点间斥力
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) {
                QPointF delta = nodes[i].pos - nodes[j].pos;
                double distSq = delta.x() * delta.x() + delta.y() * delta.y();
                const double minDist = nodes[i].radius + nodes[j].radius + 42.0;
                if (distSq < 1.0) {
                    delta = QPointF((i % 2 == 0) ? 1.0 : -1.0, (j % 2 == 0) ? 1.0 : -1.0);
                    distSq = 2.0;
                }
                const double dist = std::sqrt(distSq);
                double f = repulsionK / qMax(120.0, distSq);
                if (dist < minDist) {
                    f += (minDist - dist) * 2.2;
                }
                const QPointF forceVec = (delta / dist) * f;
                forces[i] += forceVec;
                forces[j] -= forceVec;
            }
        }

        // 2. 连线弹簧引力
        for (const GraphEdge &e : edges) {
            if (e.source < 0 || e.source >= n || e.target < 0 || e.target >= n) continue;
            const QPointF delta = nodes[e.target].pos - nodes[e.source].pos;
            const double dist = std::sqrt(delta.x() * delta.x() + delta.y() * delta.y());
            if (dist < 1.0) continue;

            const double weightRatio = static_cast<double>(e.weight) / qMax(1, maxEdgeWeight);
            const double idealLen = 195.0 - weightRatio * 65.0;
            const double kSpring = 0.045 * (0.6 + 0.8 * weightRatio);
            const double f = (dist - idealLen) * kSpring;
            const QPointF forceVec = (delta / dist) * f;
            forces[e.source] += forceVec;
            forces[e.target] -= forceVec;
        }

        // 3. 中心引力
        for (int i = 0; i < n; ++i) {
            forces[i] -= nodes[i].pos * 0.012;
        }

        // 4. 更新位置
        for (int i = 0; i < n; ++i) {
            if (nodes[i].pinned || i == draggingIndex) {
                nodes[i].vel = QPointF(0.0, 0.0);
                continue;
            }
            nodes[i].vel = (nodes[i].vel + forces[i]) * 0.65 * dampingFactor;
            const double speed = std::sqrt(nodes[i].vel.x() * nodes[i].vel.x() +
                                           nodes[i].vel.y() * nodes[i].vel.y());
            if (speed > 28.0) {
                nodes[i].vel = (nodes[i].vel / speed) * 28.0;
            }
            nodes[i].pos += nodes[i].vel;
        }
    }

protected:
    void initializePositions()
    {
        const int focusIdx = m_idToIndex.value(m_focusedNodeId, -1);
        initializeNodePositions(m_nodes, focusIdx);
    }

    void stepPhysics(double dampingFactor)
    {
        stepNodePhysics(m_nodes, m_edges, m_maxEdgeWeight, m_draggingIndex, dampingFactor);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);

        const QColor bgColor = AppStyle::color("panelBg");
        const QColor borderColor = AppStyle::color("groupBorder");
        const QColor bodyColor = AppStyle::color("bodyText");
        const QColor mutedColor = AppStyle::color("mutedText");
        const bool isLight = bgColor.lightness() > 128;

        // 1. 背景与边框
        p.setPen(QPen(borderColor, 1));
        p.setBrush(bgColor);
        p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 6, 6);

        // 2. 微弱点阵网格
        QColor gridDot = borderColor;
        gridDot.setAlpha(isLight ? 90 : 45);
        p.setPen(QPen(gridDot, 1.5));
        const int step = qMax(24, qRound(36.0 * m_zoom));
        const int offsetX = qRound(width() / 2.0 + m_panOffset.x()) % step;
        const int offsetY = qRound(height() / 2.0 + m_panOffset.y()) % step;
        for (int x = offsetX; x < width(); x += step) {
            for (int y = offsetY; y < height(); y += step) {
                p.drawPoint(x, y);
            }
        }

        if (m_nodes.isEmpty()) {
            if (m_loading) {
                drawLoadingOverlay(p, bgColor, borderColor, bodyColor, mutedColor);
                return;
            }
            p.setPen(mutedColor);
            QFont emptyFont = font();
            emptyFont.setPointSize(11);
            p.setFont(emptyFont);
            p.drawText(rect(), Qt::AlignCenter,
                       AppText::tr("暂无可展示的 Tag / 模型共现数据。\n请先扫描本地返图或调低「最小共现」阈值。"));
            return;
        }

        const int selectedIdx = m_idToIndex.value(m_selectedNodeId, -1);
        const int activeIdx = (m_hoveredIndex >= 0) ? m_hoveredIndex : selectedIdx;
        const int focusedIdx = m_idToIndex.value(m_focusedNodeId, -1);

        p.save();
        p.setClipRect(rect().adjusted(2, 2, -2, -2));

        // 3. 绘制连线
        QFont badgeFont = font();
        badgeFont.setPixelSize(10);
        badgeFont.setBold(true);
        QFontMetrics fmBadge(badgeFont);

        for (const GraphEdge &e : m_edges) {
            if (e.source < 0 || e.source >= m_nodes.size() || e.target < 0 || e.target >= m_nodes.size())
                continue;

            const QPointF p1 = worldToScreen(m_nodes[e.source].pos);
            const QPointF p2 = worldToScreen(m_nodes[e.target].pos);

            const bool connectedToActive = (activeIdx >= 0) && (e.source == activeIdx || e.target == activeIdx);
            const double weightRatio = static_cast<double>(e.weight) / qMax(1, m_maxEdgeWeight);

            QColor edgeColor = isLight ? QColor("#64748b") : QColor("#94a3b8");
            double penW = (1.1 + weightRatio * 3.0) * qBound(0.7, m_zoom, 1.5);

            if (activeIdx >= 0) {
                if (connectedToActive) {
                    edgeColor = AppStyle::color("accentBlue");
                    edgeColor.setAlpha(235);
                    penW = qMax(2.2, penW + 0.8);
                } else {
                    edgeColor.setAlpha(isLight ? 35 : 22);
                }
            } else {
                edgeColor.setAlpha(qBound(45, qRound(55 + weightRatio * 145), 210));
            }

            p.setPen(QPen(edgeColor, penW, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(p1, p2);

            // 高亮边显示共现次数胶囊
            if (connectedToActive && m_zoom >= 0.55) {
                const QPointF mid = (p1 + p2) * 0.5;
                const QString wText = QString("×%1").arg(e.weight);
                const int bw = fmBadge.horizontalAdvance(wText) + 10;
                const int bh = 16;
                const QRectF pillRect(mid.x() - bw / 2.0, mid.y() - bh / 2.0, bw, bh);
                QColor pillBg = bgColor;
                pillBg.setAlpha(230);
                p.setPen(QPen(edgeColor, 1.0));
                p.setBrush(pillBg);
                p.drawRoundedRect(pillRect, 7, 7);
                p.setFont(badgeFont);
                p.setPen(bodyColor);
                p.drawText(pillRect, Qt::AlignCenter, wText);
            }
        }

        // 4. 绘制节点
        QFont labelFont = font();
        labelFont.setPixelSize(qBound(10, qRound(11.0 * qBound(0.85, m_zoom, 1.35)), 15));
        labelFont.setBold(true);
        QFont subFont = font();
        subFont.setPixelSize(qBound(9, qRound(10.0 * qBound(0.85, m_zoom, 1.25)), 13));
        QFont countFont = font();
        countFont.setPixelSize(qBound(9, qRound(10.0 * qBound(0.85, m_zoom, 1.3)), 14));
        countFont.setBold(true);

        QFontMetrics fmLabel(labelFont);
        QFontMetrics fmSub(subFont);

        for (int i = 0; i < m_nodes.size(); ++i) {
            const GraphNode &node = m_nodes[i];
            const QPointF sp = worldToScreen(node.pos);
            const double sr = node.radius * m_zoom;

            if (sp.x() + sr + 80 < 0 || sp.x() - sr - 80 > width() ||
                sp.y() + sr + 60 < 0 || sp.y() - sr - 60 > height()) {
                continue;
            }

            const bool isSelected = (i == selectedIdx);
            const bool isHovered = (i == m_hoveredIndex);
            const bool isFocused = (i == focusedIdx);
            const bool isNeighbor = (activeIdx >= 0) && (i == activeIdx || m_neighborsOf[activeIdx].contains(i));
            const bool isSearchHit = !m_searchNeedle.isEmpty() &&
                                     (node.label.toLower().contains(m_searchNeedle) ||
                                      node.translation.toLower().contains(m_searchNeedle));

            const double opacity = (activeIdx >= 0 && !isNeighbor) ? 0.25 : 1.0;
            p.setOpacity(opacity);

            const QColor baseColor = nodeColorForType(node.type);

            // 选中/聚焦/搜索命中外发光圈
            if (isSelected || isHovered || isFocused || isSearchHit) {
                QColor glow = isSearchHit ? AppStyle::color("warningYellow") : baseColor;
                glow.setAlpha(isLight ? 90 : 110);
                p.setPen(Qt::NoPen);
                p.setBrush(glow);
                p.drawEllipse(sp, sr + 6.0, sr + 6.0);
            }

            // 节点圆形主体
            QColor fillColor = baseColor;
            fillColor.setAlpha(isLight ? 210 : 195);
            QColor ringColor = (isSelected || isHovered || isFocused) ? AppStyle::color("primaryText") : baseColor.lighter(125);
            p.setPen(QPen(ringColor, (isSelected || isFocused) ? 2.6 : 1.6));
            p.setBrush(fillColor);
            p.drawEllipse(sp, sr, sr);

            // 节点圆内显示使用次数
            if (sr >= 12.0) {
                p.setFont(countFont);
                p.setPen(QColor("#ffffff"));
                p.drawText(QRectF(sp.x() - sr, sp.y() - sr, sr * 2.0, sr * 2.0),
                           Qt::AlignCenter, QString::number(node.count));
            }

            // 节点下方文字标签胶囊
            if (m_zoom >= 0.42 || isSelected || isHovered || isFocused || isSearchHit) {
                const QString line1 = node.label;
                const QString line2 = (m_showTranslation && !node.translation.isEmpty()) ? node.translation : QString();

                const int w1 = fmLabel.horizontalAdvance(line1);
                const int w2 = line2.isEmpty() ? 0 : fmSub.horizontalAdvance(line2);
                const int boxW = qMax(w1, w2) + 12;
                const int boxH = line2.isEmpty() ? (fmLabel.height() + 4) : (fmLabel.height() + fmSub.height() + 6);
                const QRectF labelRect(sp.x() - boxW / 2.0, sp.y() + sr + 4.0, boxW, boxH);

                QColor pillBg = bgColor;
                pillBg.setAlpha(isLight ? 225 : 205);
                p.setPen(QPen(borderColor, 0.8));
                p.setBrush(pillBg);
                p.drawRoundedRect(labelRect, 4, 4);

                p.setFont(labelFont);
                p.setPen(bodyColor);
                if (line2.isEmpty()) {
                    p.drawText(labelRect, Qt::AlignCenter, line1);
                } else {
                    const QRectF r1(labelRect.x(), labelRect.y() + 2, labelRect.width(), fmLabel.height());
                    const QRectF r2(labelRect.x(), r1.bottom(), labelRect.width(), fmSub.height());
                    p.drawText(r1, Qt::AlignCenter, line1);
                    p.setFont(subFont);
                    p.setPen(mutedColor);
                    p.drawText(r2, Qt::AlignCenter, line2);
                }
            }
        }

        p.setOpacity(1.0);

        // 5. 左上角图例与聚焦提示
        drawLegendOverlay(p, bgColor, borderColor, bodyColor);
        if (m_loading) {
            drawLoadingOverlay(p, bgColor, borderColor, bodyColor, mutedColor);
        }
        p.restore();
    }

    void drawLoadingOverlay(QPainter &p,
                            const QColor &bgColor,
                            const QColor &borderColor,
                            const QColor &bodyColor,
                            const QColor &mutedColor)
    {
        p.save();
        // 半透明遮罩（当已有旧图节点时轻微淡化背景）
        if (!m_nodes.isEmpty()) {
            QColor veil = bgColor;
            veil.setAlpha(150);
            p.fillRect(rect(), veil);
        }

        const double cardW = 320.0;
        const double cardH = 108.0;
        const QRectF cardRect((width() - cardW) / 2.0, (height() - cardH) / 2.0, cardW, cardH);

        QColor cardBg = bgColor;
        cardBg.setAlpha(242);
        p.setPen(QPen(borderColor, 1.2));
        p.setBrush(cardBg);
        p.drawRoundedRect(cardRect, 10, 10);

        // 旋转圆弧加载指示器
        const QPointF spinnerCenter(cardRect.center().x(), cardRect.top() + 30.0);
        const double r = 13.0;
        const QRectF arcRect(spinnerCenter.x() - r, spinnerCenter.y() - r, r * 2.0, r * 2.0);

        QColor trackColor = borderColor;
        trackColor.setAlpha(90);
        p.setPen(QPen(trackColor, 3.0, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(arcRect);

        QColor spinColor = AppStyle::color("accentBlue");
        p.setPen(QPen(spinColor, 3.0, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(arcRect, -m_loadingAngle * 16, 110 * 16);

        QFont titleFont = font();
        titleFont.setPixelSize(13);
        titleFont.setBold(true);
        p.setFont(titleFont);
        p.setPen(bodyColor);
        p.drawText(QRectF(cardRect.left() + 12, cardRect.top() + 52, cardRect.width() - 24, 22),
                   Qt::AlignCenter,
                   m_loadingText.isEmpty() ? AppText::tr("正在加载 Tag 与模型共现图谱...") : m_loadingText);

        QFont subFont = font();
        subFont.setPixelSize(11);
        p.setFont(subFont);
        p.setPen(mutedColor);
        p.drawText(QRectF(cardRect.left() + 12, cardRect.top() + 76, cardRect.width() - 24, 20),
                   Qt::AlignCenter,
                   AppText::tr("正在后台统计图库提示词与模型关联，请稍候..."));
        p.restore();
    }

    void drawLegendOverlay(QPainter &p, const QColor &bgColor, const QColor &borderColor, const QColor &bodyColor)
    {
        QFont legendFont = font();
        legendFont.setPixelSize(11);
        p.setFont(legendFont);

        const QRectF legendBox(12, 12, 235, m_focusedNodeId.isEmpty() ? 32 : 56);
        QColor boxBg = bgColor;
        boxBg.setAlpha(220);
        p.setPen(QPen(borderColor, 1));
        p.setBrush(boxBg);
        p.drawRoundedRect(legendBox, 6, 6);

        struct Item { QColor color; QString text; };
        const QVector<Item> items = {
            { nodeColorForType(GraphNodeType::Checkpoint), AppText::tr("底模") },
            { nodeColorForType(GraphNodeType::Lora),       AppText::tr("LoRA") },
            { nodeColorForType(GraphNodeType::Tag),        AppText::tr("Tag") }
        };

        double x = legendBox.left() + 12;
        const double y = legendBox.top() + 16;
        for (const Item &it : items) {
            p.setPen(Qt::NoPen);
            p.setBrush(it.color);
            p.drawEllipse(QPointF(x + 5, y), 5.5, 5.5);
            p.setPen(bodyColor);
            p.drawText(QRectF(x + 14, y - 9, 60, 18), Qt::AlignVCenter | Qt::AlignLeft, it.text);
            x += 74;
        }

        if (!m_focusedNodeId.isEmpty()) {
            const int fIdx = m_idToIndex.value(m_focusedNodeId, -1);
            const QString focusName = (fIdx >= 0 && fIdx < m_nodes.size()) ? m_nodes[fIdx].label : m_focusedNodeId;
            p.setPen(AppStyle::color("accentBlue"));
            p.drawText(QRectF(legendBox.left() + 12, legendBox.top() + 31, legendBox.width() - 20, 20),
                       Qt::AlignVCenter | Qt::AlignLeft,
                       AppText::tr("🎯 聚焦: %1 (双击空白还原)").arg(focusName));
        }
    }

    void wheelEvent(QWheelEvent *event) override
    {
        const int delta = event->angleDelta().y();
        if (delta == 0) return;

        const QPointF cursorPos = event->position();
        const QPointF worldBefore = screenToWorld(cursorPos);

        const double factor = std::pow(1.0012, delta);
        m_zoom = qBound(0.25, m_zoom * factor, 3.5);

        const QPointF center(width() / 2.0, height() / 2.0);
        m_panOffset = cursorPos - center - worldBefore * m_zoom;
        update();
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            const int hit = hitTestNode(event->position());
            if (hit >= 0) {
                m_draggingIndex = hit;
                m_nodes[hit].pinned = true;
                m_selectedNodeId = m_nodes[hit].id;
                setCursor(Qt::ClosedHandCursor);
                if (onNodeSelected) onNodeSelected(m_selectedNodeId);
                update();
            } else {
                m_panning = true;
                m_lastMousePos = event->position();
                setCursor(Qt::ClosedHandCursor);
            }
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_draggingIndex >= 0 && m_draggingIndex < m_nodes.size()) {
            m_nodes[m_draggingIndex].pos = screenToWorld(event->position());
            m_animSteps = 0;
            m_maxAnimSteps = 20;
            if (!m_physicsTimer->isActive()) m_physicsTimer->start();
            update();
            return;
        }
        if (m_panning) {
            const QPointF delta = event->position() - m_lastMousePos;
            m_lastMousePos = event->position();
            m_panOffset += delta;
            update();
            return;
        }

        const int hit = hitTestNode(event->position());
        if (hit != m_hoveredIndex) {
            m_hoveredIndex = hit;
            setCursor(hit >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
            if (hit >= 0 && hit < m_nodes.size()) {
                const GraphNode &n = m_nodes[hit];
                QString tip = QString("%1: %2\n%3: %4")
                                  .arg(nodeTypeDisplayName(n.type),
                                       n.label,
                                       AppText::tr("出现图片数"),
                                       QString::number(n.count));
                if (!n.translation.isEmpty()) {
                    tip += QString("\n%1: %2").arg(AppText::tr("翻译"), n.translation);
                }
                tip += QString("\n%1").arg(AppText::tr("单击查看关联详情，双击以此节点为中心聚焦"));
                QToolTip::showText(event->globalPosition().toPoint(), tip, this);
            } else {
                QToolTip::hideText();
            }
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            if (m_draggingIndex >= 0 && m_draggingIndex < m_nodes.size()) {
                if (m_nodes[m_draggingIndex].id != m_focusedNodeId) {
                    m_nodes[m_draggingIndex].pinned = false;
                }
                m_draggingIndex = -1;
            }
            m_panning = false;
            setCursor(m_hoveredIndex >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        }
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            const int hit = hitTestNode(event->position());
            if (hit >= 0 && hit < m_nodes.size()) {
                if (onNodeDoubleClicked) onNodeDoubleClicked(m_nodes[hit].id);
            } else if (!m_focusedNodeId.isEmpty()) {
                if (onEmptyDoubleClicked) onEmptyDoubleClicked();
            }
        }
    }

public:
    struct NeighborEntry {
        QString neighborId;
        int coCount = 0;
    };

    void setFullAdjacency(QHash<QString, GraphNode> allNodes,
                          QHash<QString, QVector<NeighborEntry>> allNeighbors)
    {
        m_allNodes = std::move(allNodes);
        m_allNeighbors = std::move(allNeighbors);
    }

    const QHash<QString, GraphNode> &allNodes() const { return m_allNodes; }
    const QHash<QString, QVector<NeighborEntry>> &allNeighbors() const { return m_allNeighbors; }

private:
    QVector<GraphNode> m_nodes;
    QVector<GraphEdge> m_edges;
    QHash<QString, GraphNode> m_allNodes;
    QHash<QString, QVector<NeighborEntry>> m_allNeighbors;
    QHash<QString, int> m_idToIndex;
    QVector<QSet<int>> m_neighborsOf;
    QString m_selectedNodeId;
    QString m_focusedNodeId;
    QString m_searchNeedle;
    int m_hoveredIndex = -1;
    int m_draggingIndex = -1;
    int m_maxEdgeWeight = 1;
    double m_zoom = 1.0;
    QPointF m_panOffset{0.0, 0.0};
    QPointF m_lastMousePos;
    bool m_panning = false;
    bool m_showTranslation = true;
    bool m_loading = false;
    QString m_loadingText;
    int m_loadingAngle = 0;
    QTimer *m_physicsTimer = nullptr;
    QTimer *m_loadingTimer = nullptr;
    int m_animSteps = 0;
    int m_maxAnimSteps = 45;
};

UsageAnalysisWidget::UsageAnalysisWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::UsageAnalysisWidget)
{
    ui->setupUi(this);
    setStyleSheet(AppStyle::loadToolPageQss());

    auto setupTable = [](QTableWidget *table, bool stretchLast) {
        applyUnifiedTableRowStyle(table);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        table->horizontalHeader()->setStretchLastSection(stretchLast);
        table->horizontalHeader()->setSectionsClickable(true);
        table->setSortingEnabled(true);
        table->setAlternatingRowColors(false);
        table->setShowGrid(false);
        table->setFocusPolicy(Qt::NoFocus);
    };
    setupTable(ui->tableModels, true);
    setupTable(ui->tableTopUsed, false);
    setupTable(ui->tableTopTags, true);
    setupTable(ui->tableBaseModels, true);

    ui->tableTopUsed->setColumnCount(4);
    ui->tableTopUsed->setHorizontalHeaderLabels({AppText::tr("模型"), AppText::tr("类别"), AppText::tr("数量"), AppText::tr("占比")});
    QHeaderView *usageHeader = ui->tableTopUsed->horizontalHeader();
    usageHeader->setStretchLastSection(false);
    usageHeader->setSectionResizeMode(0, QHeaderView::Stretch);
    usageHeader->setSectionResizeMode(1, QHeaderView::Fixed);
    usageHeader->setSectionResizeMode(2, QHeaderView::Fixed);
    usageHeader->setSectionResizeMode(3, QHeaderView::Fixed);
    usageHeader->resizeSection(1, 160);
    usageHeader->resizeSection(2, 90);
    usageHeader->resizeSection(3, 180);

    // 空间占用 Tab：图表 + 统计范围下拉 + 最大模型表。
    folderPie = new PieChartWidget(ui->spaceFolderPieContainer);
    folderBar = new BarChartWidget(ui->spaceFolderBarContainer);
    categoryPie = new PieChartWidget(ui->spaceCategoryPieContainer);
    categoryBar = new BarChartWidget(ui->spaceCategoryBarContainer);
    ui->layoutSpaceFolderPie->addWidget(folderPie);
    ui->layoutSpaceFolderBar->addWidget(folderBar);
    ui->layoutSpaceCategoryPie->addWidget(categoryPie);
    ui->layoutSpaceCategoryBar->addWidget(categoryBar);

    ui->comboSpaceScope->addItem(AppText::tr("模型 + 附属文件"), 2);
    ui->comboSpaceScope->addItem(AppText::tr("仅模型文件"), 0);
    ui->comboSpaceScope->addItem(AppText::tr("仅附属文件"), 1);
    connect(ui->comboSpaceScope, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { refreshDiskSpace(); });

    setupTable(ui->tableLargestModels, true);
    ui->tableLargestModels->setColumnCount(4);
    ui->tableLargestModels->setHorizontalHeaderLabels({AppText::tr("模型"), AppText::tr("类别"), AppText::tr("文件夹"), AppText::tr("占用")});
    QHeaderView *largestHeader = ui->tableLargestModels->horizontalHeader();
    largestHeader->setStretchLastSection(false);
    largestHeader->setSectionResizeMode(0, QHeaderView::Stretch);
    largestHeader->setSectionResizeMode(1, QHeaderView::Stretch);
    largestHeader->setSectionResizeMode(2, QHeaderView::Stretch);
    largestHeader->setSectionResizeMode(3, QHeaderView::Fixed);
    largestHeader->resizeSection(3, 110);

    // Tag 与模型关系图谱 Tab
    m_graphCanvas = new TagModelGraphCanvas(ui->graphCanvasContainer);
    ui->layoutGraphCanvas->addWidget(m_graphCanvas);
    if (ui->verticalLayoutGraph) {
        ui->verticalLayoutGraph->setStretch(0, 0);
        ui->verticalLayoutGraph->setStretch(1, 1);
        ui->verticalLayoutGraph->setStretch(2, 0);
    }
    ui->splitterGraph->setStretchFactor(0, 3);
    ui->splitterGraph->setStretchFactor(1, 1);
    ui->splitterGraph->setSizes({820, 320});

    m_graphRebuildTimer = new QTimer(this);
    m_graphRebuildTimer->setSingleShot(true);
    connect(m_graphRebuildTimer, &QTimer::timeout, this, [this]() {
        const bool doRelayout = m_pendingGraphRelayout;
        m_pendingGraphRelayout = false;
        rebuildGraphIndexAndView(doRelayout);
    });

    connect(ui->tabUsage, &QTabWidget::currentChanged, this, [this](int index) {
        if (ui->tabUsage->widget(index) == ui->tabTagModelGraph && m_graphIndexDirty) {
            scheduleGraphRebuild(true);
        }
    });

    setupTable(ui->tableGraphNeighbors, true);
    ui->tableGraphNeighbors->setEditTriggers(QAbstractItemView::NoEditTriggers);
    QHeaderView *neighborHeader = ui->tableGraphNeighbors->horizontalHeader();
    neighborHeader->setStretchLastSection(false);
    neighborHeader->setSectionResizeMode(0, QHeaderView::Stretch);
    neighborHeader->setSectionResizeMode(1, QHeaderView::Fixed);
    neighborHeader->setSectionResizeMode(2, QHeaderView::Fixed);
    neighborHeader->setSectionResizeMode(3, QHeaderView::Fixed);
    neighborHeader->resizeSection(1, 78);
    neighborHeader->resizeSection(2, 74);
    neighborHeader->resizeSection(3, 68);

    m_graphCanvas->onNodeSelected = [this](const QString &nodeId) {
        m_selectedGraphNodeId = nodeId;
        updateGraphDetailPanel();
    };
    m_graphCanvas->onNodeDoubleClicked = [this](const QString &nodeId) {
        m_selectedGraphNodeId = nodeId;
        if (m_focusedGraphNodeId == nodeId) {
            m_focusedGraphNodeId.clear();
        } else {
            m_focusedGraphNodeId = nodeId;
        }
        scheduleGraphRebuild(true);
    };
    m_graphCanvas->onEmptyDoubleClicked = [this]() {
        if (!m_focusedGraphNodeId.isEmpty()) {
            m_focusedGraphNodeId.clear();
            scheduleGraphRebuild(true);
        }
    };

    connect(ui->comboGraphScope, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { scheduleGraphRebuild(true); });
    connect(ui->comboGraphNodeFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { scheduleGraphRebuild(true); });
    connect(ui->spinGraphMaxNodes, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int) { scheduleGraphRebuild(true); });
    connect(ui->spinGraphMinWeight, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int) { scheduleGraphRebuild(true); });
    connect(ui->chkGraphTranslation, &QCheckBox::toggled,
            this, [this](bool checked) {
                if (m_graphCanvas) m_graphCanvas->setShowTranslation(checked);
                updateGraphDetailPanel();
            });
    connect(ui->editGraphSearch, &QLineEdit::textChanged,
            this, [this](const QString &text) {
                if (m_graphCanvas) m_graphCanvas->setSearchHighlight(text);
            });
    connect(ui->editGraphSearch, &QLineEdit::returnPressed,
            this, [this]() {
                const QString q = ui->editGraphSearch->text().trimmed().toLower();
                if (q.isEmpty()) return;
                // 在当前数据中查找匹配的节点并聚焦
                m_focusedGraphNodeId.clear();
                scheduleGraphRebuild(true);
            });
    connect(ui->btnGraphClearFocus, &QPushButton::clicked,
            this, [this]() {
                m_focusedGraphNodeId.clear();
                scheduleGraphRebuild(true);
            });
    connect(ui->btnGraphRelayout, &QPushButton::clicked,
            this, [this]() {
                if (m_graphCanvas) m_graphCanvas->relayout();
            });
    connect(ui->btnGraphResetView, &QPushButton::clicked,
            this, [this]() {
                if (m_graphCanvas) m_graphCanvas->resetView();
            });
    connect(ui->btnGraphFocusSelected, &QPushButton::clicked,
            this, [this]() {
                if (m_selectedGraphNodeId.isEmpty()) return;
                m_focusedGraphNodeId = m_selectedGraphNodeId;
                scheduleGraphRebuild(true);
            });
    connect(ui->btnGraphJumpToModel, &QPushButton::clicked,
            this, [this]() {
                if (m_selectedGraphNodeId.startsWith("lora:") || m_selectedGraphNodeId.startsWith("ckpt:")) {
                    const int colon = m_selectedGraphNodeId.indexOf(':');
                    const QString raw = ui->btnGraphJumpToModel->property("rawModelName").toString();
                    emit requestJumpToModel(raw.isEmpty() ? m_selectedGraphNodeId.mid(colon + 1) : raw);
                }
            });
    connect(ui->btnGraphFilterInGallery, &QPushButton::clicked,
            this, [this]() {
                if (m_selectedGraphNodeId.isEmpty()) return;
                const bool isLora = m_selectedGraphNodeId.startsWith("lora:");
                const QString raw = ui->btnGraphFilterInGallery->property("rawNodeName").toString();
                if (!raw.isEmpty()) {
                    emit requestFilterInGallery(raw, isLora);
                }
            });
    connect(ui->tableGraphNeighbors, &QTableWidget::cellClicked,
            this, [this](int row, int) {
                QTableWidgetItem *it = ui->tableGraphNeighbors->item(row, 0);
                if (!it) return;
                const QString targetId = it->data(Qt::UserRole).toString();
                if (targetId.isEmpty()) return;
                m_selectedGraphNodeId = targetId;
                if (m_graphCanvas) {
                    m_graphCanvas->setSelectedNodeId(targetId);
                    m_graphCanvas->centerOnNode(targetId);
                }
                updateGraphDetailPanel();
            });
    connect(ui->tableGraphNeighbors, &QTableWidget::cellDoubleClicked,
            this, [this](int row, int) {
                QTableWidgetItem *it = ui->tableGraphNeighbors->item(row, 0);
                if (!it) return;
                const QString targetId = it->data(Qt::UserRole).toString();
                if (targetId.isEmpty()) return;
                m_selectedGraphNodeId = targetId;
                m_focusedGraphNodeId = targetId;
                scheduleGraphRebuild(true);
            });

    connect(ui->btnRefreshAnalysis, &QPushButton::clicked, this, &UsageAnalysisWidget::requestRefresh);
    connect(ui->editSearchModels, &QLineEdit::textChanged, this, &UsageAnalysisWidget::onSearchTextChanged);
    connect(ui->btnExportAnalysisCsv, &QPushButton::clicked, this, &UsageAnalysisWidget::onExportAnalysisCsvClicked);
    setStatus("等待模型库数据。");
}

UsageAnalysisWidget::~UsageAnalysisWidget()
{
    ++m_graphBuildToken;
    delete ui;
}

void UsageAnalysisWidget::setAnalysisData(const UsageAnalysisData &data)
{
    analysisData = data;
    m_parsedGraphImages.clear();
    m_parsedGraphImagesReady = false;
    m_graphIndexDirty = true;

    refreshSummary();
    refreshCharts();
    refreshDiskSpace();
    refreshModelTable();

    // 仅当用户当前正处于「Tag 与模型图谱」标签页时才触发后台加载，进入「模型使用分析」不产生图谱开销
    if (ui->tabUsage && ui->tabUsage->currentWidget() == ui->tabTagModelGraph) {
        scheduleGraphRebuild(true);
    }
    setStatus(QString("已载入 %1 个模型，缓存图片 %2 张。")
                  .arg(analysisData.models.size())
                  .arg(analysisData.galleryImageCount));
}

void UsageAnalysisWidget::refreshSummary()
{
    int civitai = 0;
    int localEdited = 0;
    int failed = 0;
    int used = 0;
    for (const UsageAnalysisModel &model : analysisData.models) {
        if (model.modelId > 0 || model.versionId > 0 || model.hasSha256) ++civitai;
        if (model.localEdited) ++localEdited;
        if (!model.syncFailure.isEmpty()) ++failed;
        if (model.usageCount > 0) ++used;
    }
    ui->lblSummaryTotal->setText(QString::number(analysisData.models.size()));
    ui->lblSummaryCivitai->setText(QString::number(civitai));
    ui->lblSummaryLocal->setText(QString::number(localEdited));
    ui->lblSummaryFailed->setText(QString::number(failed));
    ui->lblSummaryUsed->setText(QString::number(used));
    ui->lblSummaryUnused->setText(QString::number(qMax(0, analysisData.models.size() - used)));
}

void UsageAnalysisWidget::refreshCharts()
{
    QVector<const UsageAnalysisModel *> usedModels;
    QMap<QString, int> baseCounts;
    for (const UsageAnalysisModel &model : analysisData.models) {
        if (model.usageCount > 0) usedModels.append(&model);
        const QString base = model.baseModel.trimmed().isEmpty() ? "Unknown" : model.baseModel.trimmed();
        baseCounts[base] += 1;
    }

    std::sort(usedModels.begin(), usedModels.end(), [](const auto *a, const auto *b) {
        if (a->usageCount != b->usageCount) return a->usageCount > b->usageCount;
        return QString::localeAwareCompare(a->displayName, b->displayName) < 0;
    });
    if (usedModels.size() > 20) usedModels.resize(20);

    ui->tableTopUsed->setSortingEnabled(false);
    ui->tableTopUsed->clear();
    ui->tableTopUsed->setColumnCount(4);
    ui->tableTopUsed->setHorizontalHeaderLabels({AppText::tr("模型"), AppText::tr("类别"), AppText::tr("数量"), AppText::tr("占比")});
    ui->tableTopUsed->setRowCount(usedModels.size());
    int maxUsage = 1;
    for (const UsageAnalysisModel *model : std::as_const(usedModels))
        maxUsage = qMax(maxUsage, model->usageCount);
    for (int row = 0; row < usedModels.size(); ++row) {
        const UsageAnalysisModel &model = *usedModels.at(row);
        ui->tableTopUsed->setItem(row, 0, new QTableWidgetItem(model.displayName));
        ui->tableTopUsed->setItem(row, 1, new QTableWidgetItem(normalizeCategory(model.modelType, model.rootName)));
        ui->tableTopUsed->setItem(row, 2, new NumericTableItem(model.usageCount));
        auto *ratioItem = new NumericTableItem(model.usageCount);
        ui->tableTopUsed->setItem(row, 3, ratioItem);
        auto *bar = new QProgressBar(ui->tableTopUsed);
        bar->setRange(0, maxUsage);
        bar->setValue(model.usageCount);
        bar->setTextVisible(true);
        bar->setFormat(QString("%1%").arg(model.usageCount * 100 / maxUsage));
        ui->tableTopUsed->setCellWidget(row, 3, bar);
    }
    ui->tableTopUsed->setSortingEnabled(true);
    ui->tableTopUsed->sortItems(2, Qt::DescendingOrder);

    QVector<QPair<QString, int>> tagRows;
    for (auto it = analysisData.positiveTagCounts.cbegin(); it != analysisData.positiveTagCounts.cend(); ++it) {
        tagRows.append(qMakePair(it.key(), it.value()));
    }
    std::sort(tagRows.begin(), tagRows.end(), [](const auto &a, const auto &b) {
        if (a.second != b.second) return a.second > b.second;
        return QString::localeAwareCompare(a.first, b.first) < 0;
    });
    if (tagRows.size() > 20) tagRows.resize(20);
    fillTopTable(ui->tableTopTags, tagRows, "Tag");

    QVector<QPair<QString, int>> baseRows;
    for (auto it = baseCounts.cbegin(); it != baseCounts.cend(); ++it) {
        baseRows.append(qMakePair(it.key(), it.value()));
    }
    std::sort(baseRows.begin(), baseRows.end(), [](const auto &a, const auto &b) {
        if (a.second != b.second) return a.second > b.second;
        return QString::localeAwareCompare(a.first, b.first) < 0;
    });
    fillTopTable(ui->tableBaseModels, baseRows, "底模");
}

void UsageAnalysisWidget::refreshDiskSpace()
{
    const int scope = ui->comboSpaceScope ? ui->comboSpaceScope->currentData().toInt() : 2;
    auto bytesOf = [scope](const UsageAnalysisModel &m) -> qint64 {
        switch (scope) {
        case 0: return m.modelFileBytes;
        case 1: return m.sidecarBytes;
        default: return m.modelFileBytes + m.sidecarBytes;
        }
    };

    QMap<QString, qint64> folderBytes;
    QMap<QString, qint64> categoryBytes;
    qint64 total = 0;
    for (const UsageAnalysisModel &m : analysisData.models) {
        const qint64 b = bytesOf(m);
        if (b <= 0) continue;
        total += b;
        const QString folder = m.rootName.trimmed().isEmpty() ? "未分类" : m.rootName.trimmed();
        folderBytes[folder] += b;
        categoryBytes[normalizeCategory(m.modelType, m.rootName)] += b;
    }

    auto buildSlices = [](const QMap<QString, qint64> &grouped) {
        QVector<QPair<QString, qint64>> rows;
        for (auto it = grouped.cbegin(); it != grouped.cend(); ++it) rows.append(qMakePair(it.key(), it.value()));
        std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        QVector<ChartSlice> slices;
        for (int i = 0; i < rows.size(); ++i) {
            ChartSlice s;
            s.label = rows[i].first;
            s.value = static_cast<double>(rows[i].second);
            s.valueText = humanSize(rows[i].second);
            s.color = chartColorAt(i);
            slices.append(s);
        }
        return slices;
    };

    const QVector<ChartSlice> folderSlices = buildSlices(folderBytes);
    const QVector<ChartSlice> categorySlices = buildSlices(categoryBytes);
    if (folderPie) folderPie->setData(folderSlices);
    if (folderBar) folderBar->setData(folderSlices);
    if (categoryPie) categoryPie->setData(categorySlices);
    if (categoryBar) categoryBar->setData(categorySlices);

    ui->lblSpaceSummary->setText(AppText::tr("总占用 %1 · 模型 %2 个")
                                     .arg(humanSize(total))
                                     .arg(analysisData.models.size()));

    // 占用最大的模型 TopN。
    QVector<const UsageAnalysisModel*> sorted;
    sorted.reserve(analysisData.models.size());
    for (const UsageAnalysisModel &m : analysisData.models) {
        if (bytesOf(m) > 0) sorted.append(&m);
    }
    std::sort(sorted.begin(), sorted.end(), [&bytesOf](const UsageAnalysisModel *a, const UsageAnalysisModel *b) {
        return bytesOf(*a) > bytesOf(*b);
    });
    const int limit = qMin(50, sorted.size());

    ui->tableLargestModels->setSortingEnabled(false);
    ui->tableLargestModels->clearContents();
    ui->tableLargestModels->setRowCount(limit);
    for (int i = 0; i < limit; ++i) {
        const UsageAnalysisModel &m = *sorted[i];
        auto *nameItem = new QTableWidgetItem(m.displayName);
        nameItem->setToolTip(m.filePath);
        ui->tableLargestModels->setItem(i, 0, nameItem);
        ui->tableLargestModels->setItem(i, 1, new QTableWidgetItem(normalizeCategory(m.modelType, m.rootName)));
        ui->tableLargestModels->setItem(i, 2, new QTableWidgetItem(m.rootName));
        ui->tableLargestModels->setItem(i, 3, new SizeTableItem(bytesOf(m)));
    }
    ui->tableLargestModels->setSortingEnabled(true);
    ui->tableLargestModels->sortItems(3, Qt::DescendingOrder);
    ui->tableLargestModels->horizontalHeader()->resizeSection(3, 110);
}

void UsageAnalysisWidget::fillTopTable(QTableWidget *table, const QVector<QPair<QString, int>> &rows, const QString &nameHeader)
{
    table->setSortingEnabled(false);
    table->clear();
    table->setColumnCount(3);
    table->setHorizontalHeaderLabels({nameHeader, AppText::tr("数量"), AppText::tr("占比")});
    table->setRowCount(rows.size());

    int maxValue = 1;
    for (const auto &row : rows) maxValue = qMax(maxValue, row.second);

    for (int row = 0; row < rows.size(); ++row) {
        table->setItem(row, 0, new QTableWidgetItem(rows[row].first));
        table->setItem(row, 1, new NumericTableItem(rows[row].second));

        table->setItem(row, 2, new NumericTableItem(rows[row].second));
        QProgressBar *bar = new QProgressBar(table);
        bar->setRange(0, maxValue);
        bar->setValue(rows[row].second);
        bar->setTextVisible(true);
        bar->setFormat(QString("%1%").arg(rows[row].second * 100 / maxValue));
        table->setCellWidget(row, 2, bar);
    }
    table->setSortingEnabled(true);
}

void UsageAnalysisWidget::refreshModelTable()
{
    const QString query = ui->editSearchModels->text().trimmed();
    QVector<UsageAnalysisModel> rows;
    for (const UsageAnalysisModel &model : analysisData.models) {
        if (modelMatchesSearch(model, query)) rows.append(model);
    }

    ui->tableModels->setSortingEnabled(false);
    ui->tableModels->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const UsageAnalysisModel &model = rows[row];
        auto *nameItem = new QTableWidgetItem(model.displayName);
        nameItem->setData(Qt::UserRole, model.filePath);
        ui->tableModels->setItem(row, 0, nameItem);
        ui->tableModels->setItem(row, 1, new QTableWidgetItem(model.baseModel.isEmpty() ? "Unknown" : model.baseModel));
        ui->tableModels->setItem(row, 2, new QTableWidgetItem(model.localEdited ? "本地/已编辑" : "Civitai"));
        ui->tableModels->setItem(row, 3, new NumericTableItem(model.usageCount));
        ui->tableModels->setItem(row, 4, new QTableWidgetItem(formatTime(model.lastUsed)));
        ui->tableModels->setItem(row, 5, new QTableWidgetItem(model.syncFailure.isEmpty() ? "正常" : "同步失败"));
        ui->tableModels->setItem(row, 6, new QTableWidgetItem(model.filePath));
    }
    ui->tableModels->setSortingEnabled(true);
    ui->lblAnalysisStatus->setText(AppText::tr("显示 %1 / %2 个模型").arg(rows.size()).arg(analysisData.models.size()));
}

bool UsageAnalysisWidget::modelMatchesSearch(const UsageAnalysisModel &model, const QString &query) const
{
    if (query.isEmpty()) return true;
    return model.displayName.contains(query, Qt::CaseInsensitive)
        || model.baseName.contains(query, Qt::CaseInsensitive)
        || model.baseModel.contains(query, Qt::CaseInsensitive)
        || model.rootName.contains(query, Qt::CaseInsensitive)
        || model.filePath.contains(query, Qt::CaseInsensitive);
}

void UsageAnalysisWidget::onSearchTextChanged(const QString &)
{
    refreshModelTable();
}

void UsageAnalysisWidget::onExportAnalysisCsvClicked()
{
    const QString path = QFileDialog::getSaveFileName(this, AppText::tr("导出使用分析 CSV"), QString(), AppText::tr("CSV Files (*.csv)"));
    if (path.isEmpty()) return;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    file.write("\xEF\xBB\xBF");
    QTextStream out(&file);
    out << "Name,BaseModel,Status,UsageCount,LastUsed,MetadataStatus,Path\n";
    for (const UsageAnalysisModel &model : analysisData.models) {
        auto esc = [](QString s) {
            s.replace('"', "\"\"");
            return QString("\"") + s + "\"";
        };
        out << esc(model.displayName) << ','
            << esc(model.baseModel) << ','
            << esc(model.localEdited ? "Local/Edited" : "Civitai") << ','
            << model.usageCount << ','
            << esc(formatTime(model.lastUsed)) << ','
            << esc(model.syncFailure.isEmpty() ? "OK" : "Sync Failed") << ','
            << esc(model.filePath) << '\n';
    }
    file.commit();
    setStatus("使用分析 CSV 已导出。");
}

void UsageAnalysisWidget::setStatus(const QString &text)
{
    ui->lblAnalysisStatus->setText(text);
}

QString UsageAnalysisWidget::formatTime(qint64 msecs)
{
    if (msecs <= 0) return "-";
    return QDateTime::fromMSecsSinceEpoch(msecs).toString("yyyy-MM-dd HH:mm");
}

void UsageAnalysisWidget::setTranslationMap(const QHash<QString, QString> *map)
{
    m_translationMap = map;
    if (m_graphCanvas) {
        m_graphCanvas->updateTranslations([this](const QString &tag) {
            return translateTag(tag);
        });
    }
    updateGraphDetailPanel();
}

QString UsageAnalysisWidget::translateTag(const QString &tag) const
{
    if (!m_translationMap) return QString();
    return translateTagWithMap(*m_translationMap, tag);
}

QString UsageAnalysisWidget::translateTagWithMap(const QHash<QString, QString> &map, const QString &tag)
{
    if (map.isEmpty() || tag.isEmpty()) return QString();
    QString key = tag.trimmed().toLower();
    QString raw = map.value(key);
    if (raw.isEmpty()) {
        QString under = key;
        under.replace(' ', '_');
        raw = map.value(under);
    }
    if (raw.isEmpty()) {
        QString spaced = key;
        spaced.replace('_', ' ');
        raw = map.value(spaced);
    }
    if (raw.isEmpty()) return QString();

    // 剥离末尾 ",数字" 优先级以及开头重复的英文 tag
    raw = raw.trimmed();
    const int lastComma = raw.lastIndexOf(',');
    if (lastComma > 0) {
        const QString tail = raw.mid(lastComma + 1).trimmed();
        bool numeric = !tail.isEmpty();
        for (const QChar c : tail) {
            if (!c.isDigit()) { numeric = false; break; }
        }
        if (numeric) raw = raw.left(lastComma).trimmed();
    }
    if (raw.size() > tag.size() + 1 &&
        raw.startsWith(tag, Qt::CaseInsensitive) &&
        raw.at(tag.size()).isSpace()) {
        raw = raw.mid(tag.size()).trimmed();
    }
    return raw;
}

void UsageAnalysisWidget::openTagModelGraph(int scope, const QString &focusQuery)
{
    if (ui->comboGraphScope && scope >= 0 && scope < ui->comboGraphScope->count()) {
        QSignalBlocker blocker(ui->comboGraphScope);
        ui->comboGraphScope->setCurrentIndex(scope);
    }
    if (ui->editGraphSearch) {
        QSignalBlocker blocker(ui->editGraphSearch);
        ui->editGraphSearch->setText(focusQuery.trimmed());
    }
    m_focusedGraphNodeId.clear();
    if (ui->tabUsage && ui->tabTagModelGraph) {
        QSignalBlocker blocker(ui->tabUsage);
        ui->tabUsage->setCurrentWidget(ui->tabTagModelGraph);
    }
    scheduleGraphRebuild(true);
}

void UsageAnalysisWidget::scheduleGraphRebuild(bool relayout)
{
    m_graphIndexDirty = true;
    m_pendingGraphRelayout = m_pendingGraphRelayout || relayout;
    if (m_graphCanvas) {
        m_graphCanvas->setLoading(true, AppText::tr("正在加载 Tag 与模型共现图谱..."));
    }
    if (ui->lblGraphStatus) {
        ui->lblGraphStatus->setText(AppText::tr("正在后台构建 Tag 与模型共现图谱，请稍候..."));
    }
    if (m_graphRebuildTimer) {
        m_graphRebuildTimer->start(16);
    }
}

void UsageAnalysisWidget::rebuildGraphIndexAndView(bool relayout)
{
    if (!m_graphCanvas) return;

    const int token = ++m_graphBuildToken;
    const bool needParseRaw = !m_parsedGraphImagesReady;
    const QMap<QString, UserImageInfo> rawCache = needParseRaw ? analysisData.rawImageCache : QMap<QString, UserImageInfo>();
    const QSet<QString> currentPaths = needParseRaw ? analysisData.currentGalleryPaths : QSet<QString>();
    QVector<UsageGraphImageRecord> cachedParsedImages = needParseRaw ? analysisData.graphImages : m_parsedGraphImages;
    const QHash<QString, QString> transMapCopy = m_translationMap ? *m_translationMap : QHash<QString, QString>();

    const bool currentGalleryOnly = (ui->comboGraphScope && ui->comboGraphScope->currentIndex() == 1);
    const int nodeFilter = ui->comboGraphNodeFilter ? ui->comboGraphNodeFilter->currentIndex() : 0;
    const int maxNodes = ui->spinGraphMaxNodes ? ui->spinGraphMaxNodes->value() : 60;
    const int minWeight = ui->spinGraphMinWeight ? ui->spinGraphMinWeight->value() : 2;
    const QString searchQuery = ui->editGraphSearch ? ui->editGraphSearch->text().trimmed().toLower() : QString();
    const QString initialFocusedNodeId = m_focusedGraphNodeId;

    m_graphCanvas->setLoading(true, AppText::tr("正在加载 Tag 与模型共现图谱..."));
    if (ui->lblGraphStatus) {
        ui->lblGraphStatus->setText(AppText::tr("正在后台统计图库 Tag 与模型共现关系，请稍候..."));
    }

    struct GraphBuildOutput {
        int token = 0;
        bool parsedFromRaw = false;
        QVector<UsageGraphImageRecord> parsedImages;
        QHash<QString, GraphNode> allNodes;
        QHash<QString, QVector<TagModelGraphCanvas::NeighborEntry>> allNeighbors;
        QVector<GraphNode> visibleNodes;
        QVector<GraphEdge> visibleEdges;
        QString resolvedFocusedNodeId;
        int activeImages = 0;
        bool relayout = true;
    };

    auto *watcher = new QFutureWatcher<GraphBuildOutput>(this);
    QPointer<UsageAnalysisWidget> self(this);
    connect(watcher, &QFutureWatcher<GraphBuildOutput>::finished, this, [self, watcher]() {
        GraphBuildOutput out = watcher->result();
        watcher->deleteLater();
        if (!self || out.token != self->m_graphBuildToken) return;

        if (out.parsedFromRaw) {
            self->m_parsedGraphImages = std::move(out.parsedImages);
            self->m_parsedGraphImagesReady = true;
        }
        self->m_graphIndexDirty = false;
        self->m_focusedGraphNodeId = out.resolvedFocusedNodeId;

        if (self->m_selectedGraphNodeId.isEmpty() && !self->m_focusedGraphNodeId.isEmpty()) {
            self->m_selectedGraphNodeId = self->m_focusedGraphNodeId;
        } else if (!self->m_selectedGraphNodeId.isEmpty() && !out.allNodes.contains(self->m_selectedGraphNodeId)) {
            self->m_selectedGraphNodeId.clear();
        }

        const int visNodeCount = out.visibleNodes.size();
        const int visEdgeCount = out.visibleEdges.size();
        const QString curSearch = self->ui->editGraphSearch
                                      ? self->ui->editGraphSearch->text().trimmed().toLower()
                                      : QString();
        self->m_graphCanvas->setLoading(false);
        self->m_graphCanvas->setShowTranslation(self->ui->chkGraphTranslation && self->ui->chkGraphTranslation->isChecked());
        self->m_graphCanvas->setSearchHighlight(curSearch);
        self->m_graphCanvas->setSelectedNodeId(self->m_selectedGraphNodeId);
        self->m_graphCanvas->setFullAdjacency(std::move(out.allNodes), std::move(out.allNeighbors));
        self->m_graphCanvas->setGraph(std::move(out.visibleNodes),
                                      std::move(out.visibleEdges),
                                      self->m_focusedGraphNodeId,
                                      out.relayout,
                                      true);

        self->updateGraphDetailPanel();

        if (self->ui->lblGraphStatus) {
            self->ui->lblGraphStatus->setText(
                AppText::tr("统计图片 %1 张 · 当前展示 %2 个节点、%3 条共现连线（滚轮缩放，拖拽平移/移动节点，单击查看关联，双击聚焦中心节点）")
                    .arg(out.activeImages)
                    .arg(visNodeCount)
                    .arg(visEdgeCount));
        }
    });

    watcher->setFuture(QtConcurrent::run(
        [token, needParseRaw, rawCache, currentPaths, cachedParsedImages = std::move(cachedParsedImages),
         transMapCopy, currentGalleryOnly, nodeFilter, maxNodes, minWeight,
         searchQuery, initialFocusedNodeId, relayout]() mutable -> GraphBuildOutput {
            GraphBuildOutput out;
            out.token = token;
            out.relayout = relayout;
            out.resolvedFocusedNodeId = initialFocusedNodeId;

            // 0. 若尚未解析原始图片缓存，则在后台线程中执行正则提取
            if (needParseRaw && cachedParsedImages.isEmpty() && !rawCache.isEmpty()) {
                out.parsedFromRaw = true;
                out.parsedImages.reserve(rawCache.size());

                static const QRegularExpression graphModelRegex(
                    "(?:^|[,\\n\\r])\\s*(?:Model|Checkpoint)\\s*:\\s*([^,\\n\\r]+)",
                    QRegularExpression::CaseInsensitiveOption);
                static const QRegularExpression graphPromptLoraRegex(
                    "<\\s*(?:lora|lyco)\\s*:\\s*([^:>]+)(?::[^:>]*)?>?",
                    QRegularExpression::CaseInsensitiveOption);
                static const QRegularExpression graphComfyLoraRegex(
                    "(?:^|[,\\n\\r])\\s*ComfyUI\\s+LoRAs\\s*:\\s*([^\\n\\r]*)",
                    QRegularExpression::CaseInsensitiveOption);
                static const QRegularExpression graphLoraHashesRegex(
                    "(?:^|[,\\n\\r])\\s*(?:ComfyUI\\s+)?Lora\\s+hashes\\s*:\\s*(\"[^\"]*\"|[^\\n\\r]*)",
                    QRegularExpression::CaseInsensitiveOption);
                static const QRegularExpression graphPairRegex("([^:,]+?)\\s*:\\s*[A-Fa-f0-9]+");

                auto cleanModelBaseName = [](QString s) -> QString {
                    s = s.trimmed();
                    s.replace('\\', '/');
                    if (s.contains('/')) s = s.section('/', -1);
                    if (s.endsWith(".safetensors", Qt::CaseInsensitive) ||
                        s.endsWith(".ckpt", Qt::CaseInsensitive) ||
                        s.endsWith(".pt", Qt::CaseInsensitive)) {
                        s = QFileInfo(s).completeBaseName();
                    }
                    return s.trimmed();
                };

                for (auto it = rawCache.cbegin(); it != rawCache.cend(); ++it) {
                    const UserImageInfo &info = it.value();
                    UsageGraphImageRecord rec;
                    rec.inCurrentGallery = currentPaths.contains(it.key());

                    const QRegularExpressionMatch mm = graphModelRegex.match(info.parameters);
                    if (mm.hasMatch()) {
                        rec.checkpoint = cleanModelBaseName(mm.captured(1));
                    }

                    QSet<QString> seenLoraLower;
                    auto addLora = [&](const QString &raw) {
                        const QString clean = cleanModelBaseName(raw);
                        if (clean.isEmpty()) return;
                        const QString low = clean.toLower();
                        if (!seenLoraLower.contains(low)) {
                            seenLoraLower.insert(low);
                            rec.loras.append(clean);
                        }
                    };

                    auto pit = graphPromptLoraRegex.globalMatch(info.prompt);
                    while (pit.hasNext()) {
                        addLora(pit.next().captured(1));
                    }
                    const QRegularExpressionMatch cm = graphComfyLoraRegex.match(info.parameters);
                    if (cm.hasMatch()) {
                        for (QString entry : cm.captured(1).split(',', Qt::SkipEmptyParts)) {
                            const int colon = entry.indexOf(':');
                            if (colon > 0) entry = entry.left(colon);
                            addLora(entry);
                        }
                    }
                    const QRegularExpressionMatch hm = graphLoraHashesRegex.match(info.parameters);
                    if (hm.hasMatch()) {
                        QString block = hm.captured(1).trimmed();
                        if (block.startsWith('"') && block.endsWith('"') && block.size() >= 2) {
                            block = block.mid(1, block.size() - 2);
                        }
                        auto pairIt = graphPairRegex.globalMatch(block);
                        while (pairIt.hasNext()) {
                            addLora(pairIt.next().captured(1));
                        }
                    }

                    QSet<QString> seenTagLower;
                    for (const QString &t : info.cleanTags) {
                        const QString trimmed = t.trimmed();
                        if (trimmed.isEmpty()) continue;
                        if (trimmed.startsWith("<lora:", Qt::CaseInsensitive) ||
                            trimmed.startsWith("<lyco:", Qt::CaseInsensitive)) {
                            continue;
                        }
                        const QString low = trimmed.toLower();
                        if (!seenTagLower.contains(low)) {
                            seenTagLower.insert(low);
                            rec.tags.append(trimmed);
                        }
                    }

                    if (!rec.tags.isEmpty() || !rec.loras.isEmpty() || !rec.checkpoint.isEmpty()) {
                        out.parsedImages.append(std::move(rec));
                    }
                }
            } else {
                out.parsedImages = std::move(cachedParsedImages);
            }

            const QVector<UsageGraphImageRecord> &images = out.parsedImages;

            // 1. 统计所有节点出现频次
            auto registerNode = [&](const QString &id, const QString &rawName, GraphNodeType type) {
                auto it = out.allNodes.find(id);
                if (it == out.allNodes.end()) {
                    GraphNode n;
                    n.id = id;
                    n.rawName = rawName;
                    n.label = rawName;
                    n.type = type;
                    if (type == GraphNodeType::Tag) {
                        n.translation = UsageAnalysisWidget::translateTagWithMap(transMapCopy, rawName);
                    }
                    n.count = 1;
                    out.allNodes.insert(id, n);
                } else {
                    it->count += 1;
                }
            };

            for (const UsageGraphImageRecord &img : images) {
                if (currentGalleryOnly && !img.inCurrentGallery) continue;
                ++out.activeImages;

                if (!img.checkpoint.isEmpty()) {
                    registerNode(QStringLiteral("ckpt:") + img.checkpoint.toLower(),
                                 img.checkpoint,
                                 GraphNodeType::Checkpoint);
                }
                for (const QString &lora : img.loras) {
                    if (!lora.isEmpty()) {
                        registerNode(QStringLiteral("lora:") + lora.toLower(),
                                     lora,
                                     GraphNodeType::Lora);
                    }
                }
                for (const QString &tag : img.tags) {
                    if (!tag.isEmpty()) {
                        registerNode(QStringLiteral("tag:") + tag.toLower(),
                                     tag,
                                     GraphNodeType::Tag);
                    }
                }
            }

            // 如果搜索框有内容且尚未指定聚焦节点，尝试自动匹配聚焦节点
            if (!searchQuery.isEmpty() && out.resolvedFocusedNodeId.isEmpty()) {
                QString bestMatchId;
                int bestScore = -1;
                for (auto it = out.allNodes.cbegin(); it != out.allNodes.cend(); ++it) {
                    const GraphNode &n = it.value();
                    const QString lbl = n.label.toLower();
                    const QString tr = n.translation.toLower();
                    int score = -1;
                    if (lbl == searchQuery || tr == searchQuery) score = 100000 + n.count;
                    else if (lbl.startsWith(searchQuery) || (!tr.isEmpty() && tr.startsWith(searchQuery))) score = 50000 + n.count;
                    else if (lbl.contains(searchQuery) || (!tr.isEmpty() && tr.contains(searchQuery))) score = n.count;
                    if (score > bestScore) {
                        bestScore = score;
                        bestMatchId = n.id;
                    }
                }
                if (!bestMatchId.isEmpty()) {
                    out.resolvedFocusedNodeId = bestMatchId;
                }
            }

            // 2. 选取高频候选集合并映射为连续整数索引 0..K-1，使用一维连续矩阵快速统计共现
            QVector<QString> candidateList;
            QHash<QString, int> candidateIndex;
            QVector<const GraphNode *> tagCandidates;
            for (auto it = out.allNodes.cbegin(); it != out.allNodes.cend(); ++it) {
                if (it->type != GraphNodeType::Tag || it.key() == out.resolvedFocusedNodeId) {
                    candidateIndex.insert(it.key(), candidateList.size());
                    candidateList.append(it.key());
                } else {
                    tagCandidates.append(&(*it));
                }
            }
            std::sort(tagCandidates.begin(), tagCandidates.end(), [](const GraphNode *a, const GraphNode *b) {
                return a->count > b->count;
            });
            const int tagCap = qMin<int>(400, tagCandidates.size());
            for (int i = 0; i < tagCap; ++i) {
                const QString &id = tagCandidates[i]->id;
                if (!candidateIndex.contains(id)) {
                    candidateIndex.insert(id, candidateList.size());
                    candidateList.append(id);
                }
            }

            const int K = candidateList.size();
            std::vector<int> pairMatrix(static_cast<size_t>(K) * static_cast<size_t>(K), 0);
            std::vector<int> imgItemIndices;
            imgItemIndices.reserve(64);

            for (const UsageGraphImageRecord &img : images) {
                if (currentGalleryOnly && !img.inCurrentGallery) continue;
                imgItemIndices.clear();

                if (!img.checkpoint.isEmpty()) {
                    const int idx = candidateIndex.value(QStringLiteral("ckpt:") + img.checkpoint.toLower(), -1);
                    if (idx >= 0) imgItemIndices.push_back(idx);
                }
                for (const QString &lora : img.loras) {
                    const int idx = candidateIndex.value(QStringLiteral("lora:") + lora.toLower(), -1);
                    if (idx >= 0) imgItemIndices.push_back(idx);
                }
                for (const QString &tag : img.tags) {
                    const int idx = candidateIndex.value(QStringLiteral("tag:") + tag.toLower(), -1);
                    if (idx >= 0) imgItemIndices.push_back(idx);
                }

                if (imgItemIndices.size() < 2) continue;
                std::sort(imgItemIndices.begin(), imgItemIndices.end());
                imgItemIndices.erase(std::unique(imgItemIndices.begin(), imgItemIndices.end()), imgItemIndices.end());

                const int m = static_cast<int>(imgItemIndices.size());
                for (int i = 0; i < m; ++i) {
                    const int u = imgItemIndices[i];
                    int *row = pairMatrix.data() + static_cast<size_t>(u) * K;
                    for (int j = i + 1; j < m; ++j) {
                        const int v = imgItemIndices[j];
                        row[v] += 1;
                        pairMatrix[static_cast<size_t>(v) * K + u] += 1;
                    }
                }
            }

            for (int u = 0; u < K; ++u) {
                const int *row = pairMatrix.data() + static_cast<size_t>(u) * K;
                QVector<TagModelGraphCanvas::NeighborEntry> list;
                for (int v = 0; v < K; ++v) {
                    if (u != v && row[v] > 0) {
                        list.append({candidateList[v], row[v]});
                    }
                }
                if (!list.isEmpty()) {
                    std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) {
                        return a.coCount > b.coCount;
                    });
                    out.allNeighbors.insert(candidateList[u], std::move(list));
                }
            }

            // 节点类型过滤器
            auto passesFilter = [nodeFilter](GraphNodeType t) -> bool {
                switch (nodeFilter) {
                case 1: return t == GraphNodeType::Tag || t == GraphNodeType::Lora;
                case 2: return t == GraphNodeType::Lora || t == GraphNodeType::Checkpoint;
                case 3: return t == GraphNodeType::Tag;
                default: return true;
                }
            };

            // 4. 筛选可见子图节点
            if (!out.resolvedFocusedNodeId.isEmpty() && out.allNodes.contains(out.resolvedFocusedNodeId)) {
                out.visibleNodes.append(out.allNodes.value(out.resolvedFocusedNodeId));
                const auto neighbors = out.allNeighbors.value(out.resolvedFocusedNodeId);
                for (const auto &nb : neighbors) {
                    if (out.visibleNodes.size() >= maxNodes) break;
                    if (nb.coCount < minWeight) continue;
                    const auto nodeIt = out.allNodes.constFind(nb.neighborId);
                    if (nodeIt == out.allNodes.constEnd()) continue;
                    if (!passesFilter(nodeIt->type)) continue;
                    out.visibleNodes.append(*nodeIt);
                }
            } else {
                out.resolvedFocusedNodeId.clear();
                QVector<GraphNode> modelPool;
                QVector<GraphNode> tagPool;
                for (auto it = out.allNodes.cbegin(); it != out.allNodes.cend(); ++it) {
                    if (!passesFilter(it->type)) continue;
                    if (it->type == GraphNodeType::Tag) tagPool.append(*it);
                    else modelPool.append(*it);
                }
                auto byCountDesc = [](const GraphNode &a, const GraphNode &b) {
                    if (a.count != b.count) return a.count > b.count;
                    return a.label < b.label;
                };
                std::sort(modelPool.begin(), modelPool.end(), byCountDesc);
                std::sort(tagPool.begin(), tagPool.end(), byCountDesc);

                const int modelQuota = (nodeFilter == 2) ? maxNodes
                                     : (nodeFilter == 3) ? 0
                                                         : qMin<int>(modelPool.size(), qMax(8, maxNodes * 2 / 5));
                for (int i = 0; i < modelQuota && out.visibleNodes.size() < maxNodes; ++i) {
                    out.visibleNodes.append(modelPool[i]);
                }
                for (int i = 0; i < tagPool.size() && out.visibleNodes.size() < maxNodes; ++i) {
                    out.visibleNodes.append(tagPool[i]);
                }
                for (int i = modelQuota; i < modelPool.size() && out.visibleNodes.size() < maxNodes; ++i) {
                    out.visibleNodes.append(modelPool[i]);
                }
            }

            // 5. 计算可见节点半径与可见边
            int maxNodeCount = 1;
            for (const GraphNode &n : out.visibleNodes) maxNodeCount = qMax(maxNodeCount, n.count);
            for (GraphNode &n : out.visibleNodes) {
                const double ratio = std::sqrt(static_cast<double>(n.count) / maxNodeCount);
                n.radius = 14.0 + ratio * 18.0;
                if (!out.resolvedFocusedNodeId.isEmpty() && n.id == out.resolvedFocusedNodeId) {
                    n.radius = qMax(n.radius, 28.0);
                }
            }

            QHash<QString, int> visIndex;
            for (int i = 0; i < out.visibleNodes.size(); ++i) {
                visIndex.insert(out.visibleNodes[i].id, i);
            }

            for (int i = 0; i < out.visibleNodes.size(); ++i) {
                const int uCand = candidateIndex.value(out.visibleNodes[i].id, -1);
                if (uCand < 0) continue;
                const int *row = pairMatrix.data() + static_cast<size_t>(uCand) * K;
                for (int j = i + 1; j < out.visibleNodes.size(); ++j) {
                    const int vCand = candidateIndex.value(out.visibleNodes[j].id, -1);
                    if (vCand < 0) continue;
                    const int w = row[vCand];
                    if (w >= minWeight) {
                        out.visibleEdges.append({i, j, w});
                    }
                }
            }
            std::sort(out.visibleEdges.begin(), out.visibleEdges.end(), [](const GraphEdge &a, const GraphEdge &b) {
                return a.weight > b.weight;
            });
            const int maxEdgeCap = qMax(120, maxNodes * 4);
            if (out.visibleEdges.size() > maxEdgeCap) {
                out.visibleEdges.resize(maxEdgeCap);
            }

            // 6. 在后台线程预先完成初始布局计算，避免主线程卡顿
            if (relayout && !out.visibleNodes.isEmpty()) {
                const int focusVisIdx = visIndex.value(out.resolvedFocusedNodeId, -1);
                int maxEdgeWeight = 1;
                for (const GraphEdge &e : out.visibleEdges) {
                    maxEdgeWeight = qMax(maxEdgeWeight, e.weight);
                }
                TagModelGraphCanvas::initializeNodePositions(out.visibleNodes, focusVisIdx);
                for (int step = 0; step < 65; ++step) {
                    TagModelGraphCanvas::stepNodePhysics(out.visibleNodes, out.visibleEdges, maxEdgeWeight, -1, 1.0);
                }
            }

            return out;
        }));
}

void UsageAnalysisWidget::updateGraphDetailPanel()
{
    if (!m_graphCanvas || !ui->tableGraphNeighbors || !ui->lblGraphSelectedTitle) return;

    const auto &allNodes = m_graphCanvas->allNodes();
    const auto &allNeighbors = m_graphCanvas->allNeighbors();

    if (m_selectedGraphNodeId.isEmpty() || !allNodes.contains(m_selectedGraphNodeId)) {
        ui->lblGraphSelectedTitle->setText(AppText::tr("点击画布中的节点查看其共现 Tag 与模型。"));
        ui->btnGraphFocusSelected->setEnabled(false);
        ui->btnGraphJumpToModel->setEnabled(false);
        ui->btnGraphFilterInGallery->setEnabled(false);
        ui->tableGraphNeighbors->setRowCount(0);
        return;
    }

    const GraphNode &sel = allNodes.value(m_selectedGraphNodeId);
    QString titleHtml = QString("<b>%1</b> &nbsp;<span style='color:%2;'>[%3]</span><br>%4: <b>%5</b>")
                            .arg(sel.label.toHtmlEscaped(),
                                 nodeColorForType(sel.type).name(),
                                 nodeTypeDisplayName(sel.type),
                                 AppText::tr("出现图片数"),
                                 QString::number(sel.count));
    if (!sel.translation.isEmpty()) {
        titleHtml += QString(" &nbsp;·&nbsp; %1: <b>%2</b>")
                         .arg(AppText::tr("翻译"), sel.translation.toHtmlEscaped());
    }
    ui->lblGraphSelectedTitle->setText(titleHtml);

    ui->btnGraphFocusSelected->setEnabled(true);
    const bool isModelOrLora = (sel.type == GraphNodeType::Lora || sel.type == GraphNodeType::Checkpoint);
    ui->btnGraphJumpToModel->setEnabled(isModelOrLora);
    ui->btnGraphJumpToModel->setProperty("rawModelName", sel.rawName);

    const bool canFilterGallery = (sel.type == GraphNodeType::Tag || sel.type == GraphNodeType::Lora);
    ui->btnGraphFilterInGallery->setEnabled(canFilterGallery);
    ui->btnGraphFilterInGallery->setProperty("rawNodeName", sel.rawName);

    const auto neighbors = allNeighbors.value(m_selectedGraphNodeId);
    const bool showTrans = ui->chkGraphTranslation && ui->chkGraphTranslation->isChecked();

    ui->tableGraphNeighbors->setSortingEnabled(false);
    ui->tableGraphNeighbors->setRowCount(0);
    const int limit = qMin<int>(100, neighbors.size());
    for (int i = 0; i < limit; ++i) {
        const auto &nb = neighbors[i];
        const auto it = allNodes.constFind(nb.neighborId);
        if (it == allNodes.constEnd()) continue;

        const int row = ui->tableGraphNeighbors->rowCount();
        ui->tableGraphNeighbors->insertRow(row);

        QString disp = it->label;
        if (showTrans && !it->translation.isEmpty()) {
            disp += QString(" (%1)").arg(it->translation);
        }
        auto *nameItem = new QTableWidgetItem(disp);
        nameItem->setData(Qt::UserRole, it->id);
        nameItem->setToolTip(disp);

        auto *typeItem = new QTableWidgetItem(nodeTypeDisplayName(it->type));
        typeItem->setForeground(nodeColorForType(it->type));

        auto *countItem = new NumericTableItem(nb.coCount);
        const int pct = qBound(1, qRound(static_cast<double>(nb.coCount) * 100.0 / qMax(1, sel.count)), 100);
        auto *ratioItem = new NumericTableItem(pct);
        ratioItem->setText(QString("%1%").arg(pct));

        ui->tableGraphNeighbors->setItem(row, 0, nameItem);
        ui->tableGraphNeighbors->setItem(row, 1, typeItem);
        ui->tableGraphNeighbors->setItem(row, 2, countItem);
        ui->tableGraphNeighbors->setItem(row, 3, ratioItem);
    }
    ui->tableGraphNeighbors->setSortingEnabled(true);
    ui->tableGraphNeighbors->sortItems(2, Qt::DescendingOrder);
}
