#include "comfyworkflowviewer.h"

#include "imagemetadataparser.h"
#include "styleconstants.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTextEdit>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>

namespace {

constexpr qreal NodeWidth = 220.0;
constexpr qreal NodeMinHeight = 92.0;
constexpr qreal ColumnWidth = 280.0;
constexpr qreal RowHeight = 135.0;

QString jsonValueToString(const QJsonValue &value)
{
    if (value.isString()) return value.toString();
    if (value.isDouble()) return QString::number(value.toDouble());
    if (value.isBool()) return value.toBool() ? "true" : "false";
    return QString();
}

QStringList detailLinesForNode(const QString &type, const QJsonObject &inputs)
{
    QStringList details;
    const QString lowerType = type.toLower();
    if (lowerType.contains("ksampler")) {
        const QStringList keys = {"seed", "steps", "cfg", "sampler_name", "scheduler", "denoise"};
        for (const QString &key : keys) {
            const QString value = jsonValueToString(inputs.value(key));
            if (!value.isEmpty()) details << QString("%1: %2").arg(key, value);
        }
    } else if (lowerType.contains("cliptextencode") || lowerType.contains("text")) {
        const QString text = inputs.value("text").toString().trimmed();
        if (!text.isEmpty()) details << text.left(180);
    } else if (lowerType.contains("loraloader")) {
        const QString name = inputs.value("lora_name").toString();
        const QString strength = jsonValueToString(inputs.value("strength_model"));
        if (!name.isEmpty()) details << QString("LoRA: %1").arg(name);
        if (!strength.isEmpty()) details << QString("strength: %1").arg(strength);
    } else if (lowerType.contains("checkpointloader")) {
        const QString ckpt = inputs.value("ckpt_name").toString();
        if (!ckpt.isEmpty()) details << QString("Checkpoint: %1").arg(ckpt);
    }
    return details;
}

QColor nodeColor(const ComfyWorkflowNode &node)
{
    return AppStyle::workflowNodeColor(node.type, node.highlighted);
}

QString shortText(QString text, int maxLen)
{
    text.replace('\n', ' ');
    text = text.simplified();
    if (text.size() <= maxLen) return text;
    return text.left(maxLen - 1) + QChar(0x2026);
}

QGraphicsTextItem *addNodeText(QGraphicsItem *parent, const QString &text, const QColor &color,
                               const QPointF &pos, qreal width, int maxLines, bool bold = false)
{
    auto *item = new QGraphicsTextItem(parent);
    QFont font = item->font();
    font.setPointSizeF(8.5);
    font.setBold(bold);
    item->setFont(font);
    item->setDefaultTextColor(color);
    item->setTextWidth(width);
    item->document()->setDocumentMargin(0);
    item->setPlainText(maxLines == 1 ? QFontMetricsF(font).elidedText(text, Qt::ElideRight, width) : text);
    item->setPos(pos);

    if (maxLines == 1) return item;

    const QFontMetricsF metrics(font);
    const qreal maxHeight = metrics.lineSpacing() * maxLines + 6;
    const qreal currentHeight = item->boundingRect().height();
    if (currentHeight > maxHeight) {
        QString clipped = text;
        while (!clipped.isEmpty()) {
            clipped.chop(qMax(1, clipped.size() / 12));
            item->setPlainText(clipped.trimmed() + QChar(0x2026));
            if (item->boundingRect().height() <= maxHeight) break;
        }
    }

    return item;
}

class WorkflowGraphicsView : public QGraphicsView
{
public:
    explicit WorkflowGraphicsView(QGraphicsScene *scene, QWidget *parent = nullptr)
        : QGraphicsView(scene, parent)
    {
    }

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        const qreal factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
        scale(factor, factor);
        event->accept();
    }
};

} // namespace

ComfyWorkflowViewerDialog::ComfyWorkflowViewerDialog(const QString &imagePath, QWidget *parent)
    : QDialog(parent)
    , imagePath(imagePath)
{
    setWindowTitle("ComfyUI Workflow 查看器");
    resize(1280, 780);
    buildUi();
    graph = parseImageWorkflow(imagePath);
    renderGraph();
    updateNodeList();
    if (!graph.warning.isEmpty()) {
        detailsEdit->setPlainText(graph.warning);
    } else if (!graph.orderedNodeIds.isEmpty()) {
        showNodeDetails(graph.orderedNodeIds.first());
    }
}

void ComfyWorkflowViewerDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    auto *toolbar = new QToolBar(this);
    toolbar->setIconSize(QSize(16, 16));

    auto *fitAction = toolbar->addAction("适应视图");
    connect(fitAction, &QAction::triggered, this, &ComfyWorkflowViewerDialog::fitToView);
    auto *resetAction = toolbar->addAction("重置缩放");
    connect(resetAction, &QAction::triggered, this, &ComfyWorkflowViewerDialog::resetZoom);
    toolbar->addSeparator();

    searchEdit = new QLineEdit(toolbar);
    searchEdit->setPlaceholderText("搜索节点类型、标题或内容");
    searchEdit->setClearButtonEnabled(true);
    searchEdit->setMinimumWidth(260);
    toolbar->addWidget(searchEdit);
    connect(searchEdit, &QLineEdit::textChanged, this, &ComfyWorkflowViewerDialog::applySearch);

    highlightCheck = new QCheckBox("高亮采样链", toolbar);
    highlightCheck->setChecked(true);
    toolbar->addWidget(highlightCheck);
    connect(highlightCheck, &QCheckBox::toggled, this, &ComfyWorkflowViewerDialog::toggleHighlights);

    auto *copyAction = toolbar->addAction("复制节点 JSON");
    connect(copyAction, &QAction::triggered, this, &ComfyWorkflowViewerDialog::copySelectedNodeJson);

    root->addWidget(toolbar);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    scene = new QGraphicsScene(splitter);
    view = new WorkflowGraphicsView(scene, splitter);
    view->setRenderHint(QPainter::Antialiasing);
    view->setRenderHint(QPainter::TextAntialiasing);
    view->setDragMode(QGraphicsView::ScrollHandDrag);
    view->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    view->setBackgroundBrush(QColor(AppStyle::WorkflowBackground));
    splitter->addWidget(view);
    connect(scene, &QGraphicsScene::selectionChanged, this, &ComfyWorkflowViewerDialog::onSceneSelectionChanged);

    auto *sidePanel = new QWidget(splitter);
    auto *sideLayout = new QVBoxLayout(sidePanel);
    sideLayout->setContentsMargins(10, 0, 0, 0);
    sideLayout->setSpacing(8);

    auto *imageLabel = new QLabel(QFileInfo(imagePath).fileName(), sidePanel);
    imageLabel->setWordWrap(true);
    imageLabel->setStyleSheet("font-weight: bold;");
    sideLayout->addWidget(imageLabel);

    nodeList = new QListWidget(sidePanel);
    nodeList->setMinimumWidth(300);
    sideLayout->addWidget(nodeList, 2);
    connect(nodeList, &QListWidget::currentTextChanged, this, [this](const QString &) {
        showNodeDetails(selectedNodeId());
    });

    detailsEdit = new QTextEdit(sidePanel);
    detailsEdit->setReadOnly(true);
    detailsEdit->setAcceptRichText(false);
    sideLayout->addWidget(detailsEdit, 3);
    splitter->addWidget(sidePanel);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({900, 300});
    root->addWidget(splitter, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

QJsonObject ComfyWorkflowViewerDialog::parseJsonObject(const QString &text) const
{
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return {};
    return doc.object();
}

ComfyWorkflowGraph ComfyWorkflowViewerDialog::parseImageWorkflow(const QString &imagePath) const
{
    const QMap<QString, QString> chunks = extractImageMetadataTextChunks(imagePath);
    ComfyWorkflowGraph result;
    if (chunks.isEmpty()) {
        result.warning = "未找到图片 metadata。";
        return result;
    }

    const QJsonObject workflow = parseJsonObject(chunks.value("workflow"));
    result = parseWorkflowGraph(workflow);
    if (!result.isEmpty()) {
        result.sourceKey = "workflow";
        return result;
    }

    const QJsonObject prompt = parseJsonObject(chunks.value("prompt"));
    result = parsePromptGraph(prompt);
    if (!result.isEmpty()) {
        result.sourceKey = "prompt";
        applyAutomaticLayout(result);
        return result;
    }

    const QJsonObject parameters = parseJsonObject(chunks.value("parameters"));
    result = parsePromptGraph(parameters);
    if (!result.isEmpty()) {
        result.sourceKey = "parameters";
        applyAutomaticLayout(result);
        return result;
    }

    result.warning = "没有找到可视化 ComfyUI Workflow。可在“显示原始 Metadata”中查看图片原始信息。";
    return result;
}

ComfyWorkflowGraph ComfyWorkflowViewerDialog::parseWorkflowGraph(const QJsonObject &workflow) const
{
    ComfyWorkflowGraph graph;
    const QJsonArray nodes = workflow.value("nodes").toArray();
    if (nodes.isEmpty()) return graph;

    QSet<QString> positionKeys;
    for (const QJsonValue &value : nodes) {
        const QJsonObject raw = value.toObject();
        const QString id = jsonValueToString(raw.value("id"));
        if (id.isEmpty()) continue;

        ComfyWorkflowNode node;
        node.id = id;
        node.type = raw.value("type").toString("Unknown");
        node.title = raw.value("title").toString(node.type);
        node.raw = raw;
        const QJsonArray pos = raw.value("pos").toArray();
        node.position = QPointF(pos.size() > 0 ? pos.at(0).toDouble() : 0.0,
                                pos.size() > 1 ? pos.at(1).toDouble() : 0.0);
        if (pos.size() >= 2) {
            positionKeys.insert(QString::number(node.position.x()) + "," + QString::number(node.position.y()));
        }

        const QJsonArray widgets = raw.value("widgets_values").toArray();
        QStringList widgetText;
        for (const QJsonValue &widget : widgets) {
            const QString text = jsonValueToString(widget);
            if (!text.trimmed().isEmpty()) widgetText << text.trimmed();
        }
        node.subtitle = shortText(widgetText.join(" | "), 110);
        node.details = widgetText;
        graph.nodes.insert(node.id, node);
        graph.orderedNodeIds << node.id;
    }

    const QJsonArray links = workflow.value("links").toArray();
    for (const QJsonValue &value : links) {
        const QJsonArray link = value.toArray();
        if (link.size() < 6) continue;
        ComfyWorkflowEdge edge;
        edge.fromNodeId = jsonValueToString(link.at(1));
        edge.fromSlot = jsonValueToString(link.at(2));
        edge.toNodeId = jsonValueToString(link.at(3));
        edge.toSlot = jsonValueToString(link.at(4));
        if (graph.nodes.contains(edge.fromNodeId) && graph.nodes.contains(edge.toNodeId)) {
            graph.edges << edge;
        }
    }

    markImportantChain(graph);
    if (positionKeys.size() <= 1) applyAutomaticLayout(graph);
    return graph;
}

ComfyWorkflowGraph ComfyWorkflowViewerDialog::parsePromptGraph(const QJsonObject &prompt) const
{
    ComfyWorkflowGraph graph;
    if (prompt.isEmpty()) return graph;

    const QStringList ids = prompt.keys();
    for (const QString &id : ids) {
        const QJsonObject raw = prompt.value(id).toObject();
        if (raw.isEmpty()) continue;
        const QJsonObject inputs = raw.value("inputs").toObject();

        ComfyWorkflowNode node;
        node.id = id;
        node.type = raw.value("class_type").toString("Unknown");
        node.title = node.type;
        node.raw = raw;
        node.details = detailLinesForNode(node.type, inputs);
        node.subtitle = shortText(node.details.join(" | "), 110);
        graph.nodes.insert(node.id, node);
        graph.orderedNodeIds << node.id;

        for (auto it = inputs.begin(); it != inputs.end(); ++it) {
            if (!it.value().isArray()) continue;
            const QJsonArray link = it.value().toArray();
            if (link.isEmpty()) continue;
            ComfyWorkflowEdge edge;
            edge.fromNodeId = jsonValueToString(link.at(0));
            edge.toNodeId = id;
            edge.toSlot = it.key();
            if (!edge.fromNodeId.isEmpty()) graph.edges << edge;
        }
    }

    markImportantChain(graph);
    return graph;
}

void ComfyWorkflowViewerDialog::applyAutomaticLayout(ComfyWorkflowGraph &graph) const
{
    QHash<QString, int> depth;
    std::function<int(const QString &)> calcDepth = [&](const QString &id) -> int {
        if (depth.contains(id)) return depth.value(id);
        int best = 0;
        for (const ComfyWorkflowEdge &edge : graph.edges) {
            if (edge.toNodeId == id) best = std::max(best, calcDepth(edge.fromNodeId) + 1);
        }
        depth.insert(id, best);
        return best;
    };

    QHash<int, int> rowByDepth;
    for (const QString &id : graph.orderedNodeIds) {
        const int col = calcDepth(id);
        const int row = rowByDepth.value(col, 0);
        rowByDepth.insert(col, row + 1);
        graph.nodes[id].position = QPointF(40 + col * ColumnWidth, 40 + row * RowHeight);
    }
}

void ComfyWorkflowViewerDialog::markImportantChain(ComfyWorkflowGraph &graph) const
{
    QString samplerId;
    for (const QString &id : graph.orderedNodeIds) {
        const QString type = graph.nodes.value(id).type.toLower();
        if (type.contains("ksampler")) samplerId = id;
    }
    if (samplerId.isEmpty()) return;

    QSet<QString> seen;
    std::function<void(const QString &)> visitParents = [&](const QString &id) {
        if (seen.contains(id)) return;
        seen.insert(id);
        if (graph.nodes.contains(id)) graph.nodes[id].highlighted = true;
        for (ComfyWorkflowEdge &edge : graph.edges) {
            if (edge.toNodeId == id) {
                edge.highlighted = true;
                visitParents(edge.fromNodeId);
            }
        }
    };
    visitParents(samplerId);
}

void ComfyWorkflowViewerDialog::renderGraph()
{
    scene->clear();
    nodeItems.clear();

    if (graph.isEmpty()) {
        scene->addText(graph.warning.isEmpty() ? "没有可显示的 Workflow。" : graph.warning);
        return;
    }

    for (const QString &id : graph.orderedNodeIds) {
        const ComfyWorkflowNode node = graph.nodes.value(id);
        nodeItems.insert(id, createNodeItem(node));
    }
    for (const ComfyWorkflowEdge &edge : graph.edges) {
        addEdgeItem(edge);
    }
    for (QGraphicsItem *item : std::as_const(nodeItems)) {
        item->setZValue(10);
    }

    scene->setSceneRect(scene->itemsBoundingRect().adjusted(-80, -80, 120, 120));
    resetZoom();
}

QGraphicsRectItem *ComfyWorkflowViewerDialog::createNodeItem(const ComfyWorkflowNode &node)
{
    const qreal contentWidth = NodeWidth - 24;
    const qreal subtitleHeight = node.subtitle.isEmpty() ? 0 : qMin<qreal>(60, 18 + node.subtitle.size() / 4);
    const qreal height = NodeMinHeight + subtitleHeight;
    auto *rect = scene->addRect(QRectF(QPointF(0, 0), QSizeF(NodeWidth, height)),
                                QPen(node.highlighted && highlightCheck->isChecked() ? QColor(AppStyle::WorkflowEdgeHighlight) : QColor(AppStyle::WorkflowNodeBorder), 1.2),
                                QBrush(nodeColor(node)));
    rect->setPos(node.position);
    rect->setFlag(QGraphicsItem::ItemIsSelectable, true);
    rect->setData(0, node.id);
    rect->setToolTip(nodeDisplayText(node));

    addNodeText(rect, node.title.isEmpty() ? node.type : node.title, QColor(AppStyle::WorkflowTitleText),
                QPointF(12, 6), contentWidth, 1, true);
    addNodeText(rect, node.type, QColor(AppStyle::WorkflowTypeText), QPointF(12, 30), contentWidth, 1);

    if (!node.subtitle.isEmpty()) {
        addNodeText(rect, node.subtitle, QColor(AppStyle::WorkflowSubtitleText), QPointF(12, 54), contentWidth, 3);
    }

    return rect;
}

void ComfyWorkflowViewerDialog::addEdgeItem(const ComfyWorkflowEdge &edge)
{
    QGraphicsItem *fromItem = nodeItems.value(edge.fromNodeId, nullptr);
    QGraphicsItem *toItem = nodeItems.value(edge.toNodeId, nullptr);
    if (!fromItem || !toItem) return;

    const QRectF fromRect = fromItem->sceneBoundingRect();
    const QRectF toRect = toItem->sceneBoundingRect();
    const QPointF start(fromRect.right(), fromRect.center().y());
    const QPointF end(toRect.left(), toRect.center().y());
    const qreal dx = qMax<qreal>(80, qAbs(end.x() - start.x()) / 2);

    QPainterPath path(start);
    path.cubicTo(start + QPointF(dx, 0), end - QPointF(dx, 0), end);
    auto *item = scene->addPath(path, QPen(edge.highlighted && highlightCheck->isChecked() ? QColor(AppStyle::WorkflowEdgeHighlight) : QColor(AppStyle::WorkflowEdge),
                                           edge.highlighted && highlightCheck->isChecked() ? 2.4 : 1.3));
    item->setZValue(1);
}

void ComfyWorkflowViewerDialog::updateNodeList()
{
    nodeList->clear();
    for (const QString &id : graph.orderedNodeIds) {
        const ComfyWorkflowNode node = graph.nodes.value(id);
        auto *item = new QListWidgetItem(QString("%1  %2").arg(id, node.title.isEmpty() ? node.type : node.title));
        item->setData(Qt::UserRole, id);
        nodeList->addItem(item);
    }
    if (nodeList->count() > 0) nodeList->setCurrentRow(0);
}

QString ComfyWorkflowViewerDialog::nodeDisplayText(const ComfyWorkflowNode &node) const
{
    QStringList lines;
    lines << QString("ID: %1").arg(node.id);
    lines << QString("类型: %1").arg(node.type);
    if (!node.title.isEmpty() && node.title != node.type) lines << QString("标题: %1").arg(node.title);
    if (!node.details.isEmpty()) {
        lines << "";
        lines << node.details;
    }
    return lines.join('\n');
}

QString ComfyWorkflowViewerDialog::selectedNodeId() const
{
    if (auto *item = nodeList->currentItem()) return item->data(Qt::UserRole).toString();
    return QString();
}

void ComfyWorkflowViewerDialog::showNodeDetails(const QString &nodeId)
{
    if (!graph.nodes.contains(nodeId)) return;
    selectNodeInList(nodeId);
    const ComfyWorkflowNode node = graph.nodes.value(nodeId);
    QString text = nodeDisplayText(node);
    text += "\n\n===== JSON =====\n";
    text += QString::fromUtf8(QJsonDocument(node.raw).toJson(QJsonDocument::Indented));
    detailsEdit->setPlainText(text);

    if (QGraphicsItem *item = nodeItems.value(nodeId, nullptr)) {
        if (!item->isSelected()) {
            const QSignalBlocker blocker(scene);
            scene->clearSelection();
            item->setSelected(true);
        }
        raiseSelectedNode(nodeId);
    }
}

void ComfyWorkflowViewerDialog::onSceneSelectionChanged()
{
    const QList<QGraphicsItem *> selectedItems = scene->selectedItems();
    for (QGraphicsItem *item : selectedItems) {
        const QString nodeId = item->data(0).toString();
        if (!nodeId.isEmpty()) {
            showNodeDetails(nodeId);
            return;
        }
    }
}

void ComfyWorkflowViewerDialog::selectNodeInList(const QString &nodeId)
{
    if (!nodeList) return;
    const QSignalBlocker blocker(nodeList);
    for (int i = 0; i < nodeList->count(); ++i) {
        QListWidgetItem *item = nodeList->item(i);
        if (item && item->data(Qt::UserRole).toString() == nodeId) {
            nodeList->setCurrentItem(item);
            nodeList->scrollToItem(item, QAbstractItemView::PositionAtCenter);
            return;
        }
    }
}

void ComfyWorkflowViewerDialog::raiseSelectedNode(const QString &nodeId)
{
    for (QGraphicsItem *item : std::as_const(nodeItems)) {
        if (item) item->setZValue(10);
    }
    if (QGraphicsItem *item = nodeItems.value(nodeId, nullptr)) {
        item->setZValue(100);
    }
}

void ComfyWorkflowViewerDialog::fitToView()
{
    if (!scene || scene->items().isEmpty()) return;
    view->fitInView(scene->itemsBoundingRect().adjusted(-40, -40, 40, 40), Qt::KeepAspectRatio);
}

void ComfyWorkflowViewerDialog::resetZoom()
{
    view->resetTransform();
    view->centerOn(scene->sceneRect().center());
}

void ComfyWorkflowViewerDialog::applySearch()
{
    const QString query = searchEdit->text().trimmed();
    for (int i = 0; i < nodeList->count(); ++i) {
        QListWidgetItem *item = nodeList->item(i);
        const QString id = item->data(Qt::UserRole).toString();
        const ComfyWorkflowNode node = graph.nodes.value(id);
        const QString haystack = (id + " " + node.type + " " + node.title + " " + node.subtitle + " " + node.details.join(' ')).toLower();
        item->setHidden(!query.isEmpty() && !haystack.contains(query.toLower()));
    }
}

void ComfyWorkflowViewerDialog::toggleHighlights(bool)
{
    renderGraph();
}

void ComfyWorkflowViewerDialog::copySelectedNodeJson()
{
    const QString id = selectedNodeId();
    if (!graph.nodes.contains(id)) return;
    QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(graph.nodes.value(id).raw).toJson(QJsonDocument::Indented)));
}
