#ifndef IMAGEVIEWERDIALOG_H
#define IMAGEVIEWERDIALOG_H

#include <QDialog>
#include <QFutureWatcher>
#include <QImage>
#include <QPointF>
#include <QStringList>

namespace Ui {
class ImageViewerDialog;
}

class QGraphicsPixmapItem;
class QGraphicsScene;
class QKeyEvent;
class QResizeEvent;

struct ImageViewerLoadResult
{
    QString path;
    QImage image;
    QString error;
};

class ImageViewerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ImageViewerDialog(const QStringList &imagePaths,
                               int initialIndex = 0,
                               QWidget *parent = nullptr);
    ~ImageViewerDialog() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void showPrevious();
    void showNext();
    void zoomIn();
    void zoomOut();
    void fitToWindow();
    void showActualSize();
    void onImageLoaded();

private:
    static ImageViewerLoadResult loadImage(const QString &path);
    void requestCurrentImage();
    void startPendingLoad();
    void displayImage(const QImage &image);
    void displayMessage(const QString &message);
    void applyZoom(double targetScale, const QPointF &sceneAnchor = QPointF());
    void updateControls();
    void updateZoomLabel();

    Ui::ImageViewerDialog *ui;
    QStringList paths;
    int currentIndex = 0;
    int loadingIndex = -1;
    bool loadPending = false;
    bool fitMode = true;
    QGraphicsScene *scene = nullptr;
    QGraphicsPixmapItem *pixmapItem = nullptr;
    QFutureWatcher<ImageViewerLoadResult> *loadWatcher = nullptr;
};

#endif // IMAGEVIEWERDIALOG_H
