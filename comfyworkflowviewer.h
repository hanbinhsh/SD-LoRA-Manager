#ifndef COMFYWORKFLOWVIEWER_H
#define COMFYWORKFLOWVIEWER_H

#include <QDialog>
#include <QHash>
#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QStringList>

class QCheckBox;
class QGraphicsItem;
class QGraphicsRectItem;
class QGraphicsScene;
class QGraphicsView;
class QLineEdit;
class QListWidget;
class QTextEdit;

struct ComfyWorkflowNode
{
    QString id;
    QString type;
    QString title;
    QString subtitle;
    QPointF position;
    QJsonObject raw;
    QStringList details;
    bool highlighted = false;
};

struct ComfyWorkflowEdge
{
    QString fromNodeId;
    QString fromSlot;
    QString toNodeId;
    QString toSlot;
    bool highlighted = false;
};

struct ComfyWorkflowGraph
{
    QString sourceKey;
    QString warning;
    QHash<QString, ComfyWorkflowNode> nodes;
    QList<ComfyWorkflowEdge> edges;
    QStringList orderedNodeIds;

    bool isEmpty() const { return nodes.isEmpty(); }
};

class ComfyWorkflowViewerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ComfyWorkflowViewerDialog(const QString &imagePath, QWidget *parent = nullptr);

private slots:
    void fitToView();
    void resetZoom();
    void applySearch();
    void toggleHighlights(bool enabled);
    void copySelectedNodeJson();
    void showNodeDetails(const QString &nodeId);
    void onSceneSelectionChanged();

private:
    ComfyWorkflowGraph parseImageWorkflow(const QString &imagePath) const;
    ComfyWorkflowGraph parseWorkflowGraph(const QJsonObject &workflow) const;
    ComfyWorkflowGraph parsePromptGraph(const QJsonObject &prompt) const;
    void applyAutomaticLayout(ComfyWorkflowGraph &graph) const;
    void markImportantChain(ComfyWorkflowGraph &graph) const;
    void buildUi();
    void renderGraph();
    void updateNodeList();
    void selectNodeInList(const QString &nodeId);
    void raiseSelectedNode(const QString &nodeId);
    QGraphicsRectItem *createNodeItem(const ComfyWorkflowNode &node);
    void addEdgeItem(const ComfyWorkflowEdge &edge);
    QString nodeDisplayText(const ComfyWorkflowNode &node) const;
    QString selectedNodeId() const;
    QJsonObject parseJsonObject(const QString &text) const;

    QString imagePath;
    ComfyWorkflowGraph graph;
    QGraphicsScene *scene = nullptr;
    QGraphicsView *view = nullptr;
    QListWidget *nodeList = nullptr;
    QTextEdit *detailsEdit = nullptr;
    QLineEdit *searchEdit = nullptr;
    QCheckBox *highlightCheck = nullptr;
    QHash<QString, QGraphicsItem *> nodeItems;
};

#endif // COMFYWORKFLOWVIEWER_H
