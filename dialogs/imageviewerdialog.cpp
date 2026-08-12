#include "imageviewerdialog.h"
#include "ui_imageviewerdialog.h"

#include <QEvent>
#include <QFileInfo>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QImageReader>
#include <QKeyEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrent>

#include <cmath>

namespace {
constexpr double kMinimumScale = 0.01;
constexpr double kMaximumScale = 8.0;
constexpr double kZoomStep = 1.15;
}

ImageViewerDialog::ImageViewerDialog(const QStringList &imagePaths,
                                     int initialIndex,
                                     QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::ImageViewerDialog)
    , paths(imagePaths)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose, false);

    scene = new QGraphicsScene(this);
    ui->imageViewerView->setScene(scene);
    ui->imageViewerView->setDragMode(QGraphicsView::ScrollHandDrag);
    ui->imageViewerView->setTransformationAnchor(QGraphicsView::NoAnchor);
    ui->imageViewerView->setResizeAnchor(QGraphicsView::NoAnchor);
    ui->imageViewerView->setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    ui->imageViewerView->viewport()->installEventFilter(this);
    ui->imageViewerView->setFocus();

    loadWatcher = new QFutureWatcher<ImageViewerLoadResult>(this);
    connect(loadWatcher, &QFutureWatcher<ImageViewerLoadResult>::finished,
            this, &ImageViewerDialog::onImageLoaded);

    connect(ui->btnPrevious, &QPushButton::clicked, this, &ImageViewerDialog::showPrevious);
    connect(ui->btnNext, &QPushButton::clicked, this, &ImageViewerDialog::showNext);
    connect(ui->btnZoomIn, &QPushButton::clicked, this, &ImageViewerDialog::zoomIn);
    connect(ui->btnZoomOut, &QPushButton::clicked, this, &ImageViewerDialog::zoomOut);
    connect(ui->btnFit, &QPushButton::clicked, this, &ImageViewerDialog::fitToWindow);
    connect(ui->btnActualSize, &QPushButton::clicked, this, &ImageViewerDialog::showActualSize);

    currentIndex = paths.isEmpty() ? -1 : qBound(0, initialIndex, paths.size() - 1);
    updateControls();
    requestCurrentImage();
}

ImageViewerDialog::~ImageViewerDialog()
{
    delete ui;
}

ImageViewerLoadResult ImageViewerDialog::loadImage(const QString &path)
{
    ImageViewerLoadResult result;
    result.path = path;

    if (!QFileInfo::exists(path)) {
        result.error = QStringLiteral("图片不存在或已被移动。\n%1").arg(path);
        return result;
    }

    QImageReader reader(path);
    reader.setAutoTransform(true);
    result.image = reader.read();
    if (result.image.isNull()) {
        result.error = reader.errorString().trimmed();
        if (result.error.isEmpty()) result.error = QStringLiteral("无法读取图片。");
    }
    return result;
}

void ImageViewerDialog::requestCurrentImage()
{
    updateControls();
    if (currentIndex < 0 || currentIndex >= paths.size()) {
        displayMessage(QStringLiteral("没有可浏览的本地图片。"));
        return;
    }

    fitMode = true;
    ui->lblFileName->setText(QFileInfo(paths.at(currentIndex)).fileName());
    ui->lblFileName->setToolTip(paths.at(currentIndex));
    displayMessage(QStringLiteral("正在加载图片..."));

    if (loadWatcher->isRunning()) {
        loadPending = true;
        return;
    }
    startPendingLoad();
}

void ImageViewerDialog::startPendingLoad()
{
    if (currentIndex < 0 || currentIndex >= paths.size() || loadWatcher->isRunning()) return;
    loadPending = false;
    loadingIndex = currentIndex;
    const QString path = paths.at(loadingIndex);
    loadWatcher->setFuture(QtConcurrent::run([path]() { return loadImage(path); }));
}

void ImageViewerDialog::onImageLoaded()
{
    const ImageViewerLoadResult result = loadWatcher->result();
    const bool isCurrent = loadingIndex == currentIndex
        && currentIndex >= 0
        && currentIndex < paths.size()
        && result.path == paths.at(currentIndex);

    if (isCurrent) {
        if (result.image.isNull()) {
            displayMessage(QStringLiteral("无法加载图片\n%1").arg(result.error));
        } else {
            displayImage(result.image);
        }
    }

    loadingIndex = -1;
    if (loadPending || !isCurrent) startPendingLoad();
}

void ImageViewerDialog::displayImage(const QImage &image)
{
    scene->clear();
    pixmapItem = scene->addPixmap(QPixmap::fromImage(image));
    scene->setSceneRect(pixmapItem->boundingRect());
    fitToWindow();
}

void ImageViewerDialog::displayMessage(const QString &message)
{
    scene->clear();
    pixmapItem = nullptr;
    scene->setSceneRect(QRectF(0, 0, 800, 500));
    QGraphicsTextItem *textItem = scene->addText(message);
    textItem->setDefaultTextColor(palette().color(QPalette::Text));
    textItem->setTextWidth(700);
    const QRectF bounds = textItem->boundingRect();
    textItem->setPos((800 - bounds.width()) / 2.0, (500 - bounds.height()) / 2.0);
    ui->imageViewerView->resetTransform();
    ui->imageViewerView->centerOn(scene->sceneRect().center());
    updateZoomLabel();
}

void ImageViewerDialog::showPrevious()
{
    if (currentIndex <= 0) return;
    --currentIndex;
    requestCurrentImage();
}

void ImageViewerDialog::showNext()
{
    if (currentIndex < 0 || currentIndex >= paths.size() - 1) return;
    ++currentIndex;
    requestCurrentImage();
}

void ImageViewerDialog::zoomIn()
{
    applyZoom(ui->imageViewerView->transform().m11() * kZoomStep);
}

void ImageViewerDialog::zoomOut()
{
    applyZoom(ui->imageViewerView->transform().m11() / kZoomStep);
}

void ImageViewerDialog::fitToWindow()
{
    if (!pixmapItem) return;
    fitMode = true;
    ui->imageViewerView->resetTransform();
    ui->imageViewerView->fitInView(pixmapItem, Qt::KeepAspectRatio);
    updateZoomLabel();
}

void ImageViewerDialog::showActualSize()
{
    if (!pixmapItem) return;
    fitMode = false;
    ui->imageViewerView->resetTransform();
    ui->imageViewerView->centerOn(pixmapItem);
    updateZoomLabel();
}

void ImageViewerDialog::applyZoom(double targetScale, const QPointF &sceneAnchor)
{
    if (!pixmapItem) return;
    fitMode = false;
    targetScale = qBound(kMinimumScale, targetScale, kMaximumScale);
    const double currentScale = ui->imageViewerView->transform().m11();
    if (currentScale <= 0.0) return;

    const QPointF anchor = sceneAnchor.isNull()
        ? ui->imageViewerView->mapToScene(ui->imageViewerView->viewport()->rect().center())
        : sceneAnchor;
    ui->imageViewerView->scale(targetScale / currentScale, targetScale / currentScale);
    const QPoint viewportPoint = sceneAnchor.isNull()
        ? ui->imageViewerView->viewport()->rect().center()
        : ui->imageViewerView->viewport()->mapFromGlobal(QCursor::pos());
    const QPointF shiftedAnchor = ui->imageViewerView->mapToScene(viewportPoint);
    const QPointF delta = shiftedAnchor - anchor;
    ui->imageViewerView->translate(delta.x(), delta.y());
    updateZoomLabel();
}

void ImageViewerDialog::updateControls()
{
    const bool hasImage = currentIndex >= 0 && currentIndex < paths.size();
    ui->btnPrevious->setEnabled(hasImage && currentIndex > 0);
    ui->btnNext->setEnabled(hasImage && currentIndex < paths.size() - 1);
    ui->btnZoomIn->setEnabled(hasImage);
    ui->btnZoomOut->setEnabled(hasImage);
    ui->btnFit->setEnabled(hasImage);
    ui->btnActualSize->setEnabled(hasImage);
    ui->lblPosition->setText(hasImage
                                 ? QStringLiteral("%1 / %2").arg(currentIndex + 1).arg(paths.size())
                                 : QStringLiteral("0 / 0"));
}

void ImageViewerDialog::updateZoomLabel()
{
    const int percent = qRound(ui->imageViewerView->transform().m11() * 100.0);
    ui->lblZoom->setText(QStringLiteral("%1%").arg(percent));
}

bool ImageViewerDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->imageViewerView->viewport() && event->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        if (!pixmapItem) return true;
        const QPointF anchor = ui->imageViewerView->mapToScene(wheel->position().toPoint());
        const double steps = wheel->angleDelta().y() / 120.0;
        const double target = ui->imageViewerView->transform().m11() * std::pow(kZoomStep, steps);
        applyZoom(target, anchor);
        wheel->accept();
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void ImageViewerDialog::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Left:
        showPrevious();
        return;
    case Qt::Key_Right:
        showNext();
        return;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomIn();
        return;
    case Qt::Key_Minus:
        zoomOut();
        return;
    case Qt::Key_0:
        showActualSize();
        return;
    case Qt::Key_F:
        fitToWindow();
        return;
    default:
        QDialog::keyPressEvent(event);
        return;
    }
}

void ImageViewerDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    if (fitMode && pixmapItem) fitToWindow();
}
