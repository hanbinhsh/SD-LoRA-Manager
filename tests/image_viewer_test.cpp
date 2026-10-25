#include "dialogs/imageviewerdialog.h"

#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEvent>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <functional>

namespace {
int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        ++failures;
        qCritical() << "FAIL:" << message;
    }
}

bool waitUntil(const std::function<bool()> &condition)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(5);
    }
    return condition();
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory is available");
    const QString path = dir.filePath("preview.png");
    QImage image(400, 300, QImage::Format_RGB32);
    image.fill(Qt::blue);
    check(image.save(path), "create preview fixture");

    QWidget host;
    QPushButton button("Parent action", &host);
    int clicks = 0;
    QObject::connect(&button, &QPushButton::clicked, &host, [&] { ++clicks; });
    host.show();

    QPointer<ImageViewerDialog> viewer = new ImageViewerDialog({path, path}, 0, &host);
    viewer->setAttribute(Qt::WA_DeleteOnClose);
    viewer->show();
    QCoreApplication::processEvents();
    check(viewer->isVisible() && !viewer->isModal() && viewer->windowModality() == Qt::NonModal,
          "viewer opens nonmodally");
    check(QApplication::activeModalWidget() == nullptr, "viewer does not block the main window");
    button.click();
    check(clicks == 1 && host.isEnabled(), "parent actions remain usable with the viewer open");

    auto *view = viewer->findChild<QGraphicsView*>("imageViewerView");
    check(view && waitUntil([&] {
        return !view->scene()->items().isEmpty()
            && view->scene()->items().first()->type() == QGraphicsPixmapItem::Type;
    }), "async preview loads in the nonmodal window");
    viewer->findChild<QPushButton*>("btnNext")->click();
    check(viewer->findChild<QPushButton*>("btnPrevious")->isEnabled(), "navigation still works");

    const QString replacement = dir.filePath("replacement.png");
    QImage replacementImage(750, 225, QImage::Format_RGB32);
    replacementImage.fill(Qt::green);
    check(replacementImage.save(replacement), "create replacement fixture");
    const QSize windowSize = viewer->size();
    viewer->setImages({replacement, path}, 0);
    viewer->setImages({path}, 0);
    viewer->setImages({replacement, path}, 0);
    check(viewer->findChild<QLabel*>("lblFileName")->text() == "replacement.png"
          && !viewer->findChild<QPushButton*>("btnPrevious")->isEnabled()
          && viewer->findChild<QPushButton*>("btnNext")->isEnabled(),
          "reused window replaces the image list and navigation snapshot");
    check(waitUntil([&] {
        if (view->scene()->items().isEmpty()) return false;
        auto *item = qgraphicsitem_cast<QGraphicsPixmapItem*>(view->scene()->items().first());
        return item && item->pixmap().size() == replacementImage.size();
    }), "rapid reuse displays the newest image instead of a stale load");
    check(viewer->size() == windowSize && !viewer->isModal(),
          "reusing the viewer preserves geometry and nonmodal behavior");
    viewer->setImages({}, 0);
    check(!viewer->findChild<QPushButton*>("btnNext")->isEnabled()
          && !viewer->findChild<QPushButton*>("btnPrevious")->isEnabled(), "empty replacement is safe");
    viewer->setImages({path}, 0);
    viewer->close();
    check(waitUntil([&] { return viewer.isNull(); }), "closing during a load releases the viewer");
    button.click();
    check(clicks == 2 && host.isVisible(), "closing the viewer leaves the parent usable");

    auto *owner = new QWidget;
    QPointer<ImageViewerDialog> child = new ImageViewerDialog({path}, 0, owner);
    child->setAttribute(Qt::WA_DeleteOnClose);
    child->show();
    delete owner;
    check(child.isNull(), "destroying the parent also releases an open viewer");
    QThreadPool::globalInstance()->waitForDone();
    QCoreApplication::processEvents();
    qInfo() << (failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
