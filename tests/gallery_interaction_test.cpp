#include "ui_mainwindow.h"
#include "utils/styleconstants.h"

#include <QApplication>
#include <QDebug>
#include <QEventLoop>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTimer>
#include <cstdio>
#include <memory>

namespace {
int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        ++failures;
        qCritical() << "FAIL:" << message;
    }
}

void settle()
{
    QEventLoop loop;
    QTimer::singleShot(60, &loop, &QEventLoop::quit);
    loop.exec();
}

void click(QListWidget &view, const QPoint &pos)
{
    for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, pos, view.viewport()->mapToGlobal(pos), Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                          Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &event);
    }
}

void verifyItem(QListWidget &view, QListWidgetItem *item)
{
    const QRect rect = view.visualItemRect(item);
    check(rect.isValid(), "visible row has geometry");
    const int scroll = view.verticalScrollBar()->value();
    for (const auto &point : {rect.center(), rect.topLeft() + QPoint(4, 4),
                             rect.center() + QPoint(35, 35)}) {
        auto *hit = view.itemAt(point);
        if (hit != item)
            qWarning() << "Hit mismatch at" << point << "expected row" << view.row(item)
                       << "got" << view.row(hit) << "hidden" << (hit && hit->isHidden());
        check(hit == item, "thumbnail center and edges hit the visible item");
        QListWidgetItem *clicked = nullptr;
        auto connection = QObject::connect(&view, &QListWidget::itemClicked, &view,
                                            [&](QListWidgetItem *result) { clicked = result; });
        click(view, point);
        QObject::disconnect(connection);
        check(clicked == item && view.currentItem() == item && item->isSelected(),
              "click selects and emits the visible item");
        check(view.verticalScrollBar()->value() == scroll, "click preserves gallery scroll");
    }

    QListWidgetItem *doubleClicked = nullptr;
    auto connection = QObject::connect(&view, &QListWidget::itemDoubleClicked, &view,
                                       [&](QListWidgetItem *result) { doubleClicked = result; });
    QMouseEvent event(QEvent::MouseButtonDblClick, rect.center(),
                      view.viewport()->mapToGlobal(rect.center()), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &event);
    QObject::disconnect(connection);
    check(doubleClicked == item, "double click opens the visible item");
}

void verifyFirstVisible(QListWidget &view)
{
    QListWidgetItem *first = nullptr;
    for (int row = 0; row < view.count(); ++row) {
        if (!view.item(row)->isHidden()) {
            first = view.item(row);
            break;
        }
    }
    if (!first) return;
    verifyItem(view, first);
}

void testGallery()
{
    // Use the real Designer properties without constructing/starting MainWindow.
    QMainWindow host;
    Ui::MainWindow ui;
    ui.setupUi(&host);
    auto &view = *ui.listUserImages;
    view.setParent(nullptr);
    std::unique_ptr<QListWidget> owner(&view);
    view.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view.viewport()->setObjectName("userGalleryViewport");
    view.viewport()->setAutoFillBackground(false);
    view.viewport()->setAttribute(Qt::WA_StyledBackground, true);
    view.setStyleSheet(AppStyle::loadQss(":/styles/base.qss")
                      + AppStyle::loadQss(":/styles/mainwindow.qss"));
    view.resize(650, 500);
    QPixmap pixmap(140, 140);
    pixmap.fill(Qt::blue);
    for (int row = 0; row < 200; ++row)
        new QListWidgetItem(QIcon(pixmap), QString(), &view);
    view.show();
    settle();
    verifyFirstVisible(view);
    for (int pass = 0; pass < 8; ++pass) {
        view.setUpdatesEnabled(false);
        for (int row = 0; row < view.count(); ++row)
            view.item(row)->setHidden(pass % 4 != 3 && (row < (pass + 1) * 7 || row % 3 == 0));
        const QSize gridSize = view.gridSize();
        view.setGridSize(QSize());
        view.setGridSize(gridSize);
        view.setUpdatesEnabled(true);
        view.doItemsLayout();
        view.verticalScrollBar()->setValue(0);
        settle();
        verifyFirstVisible(view);
        // Thumbnail workers may finish for rows hidden by the latest filter.
        for (int row = 0; row < view.count(); ++row) {
            QPixmap thumbnail(140, 140);
            thumbnail.fill(row % 2 ? Qt::green : Qt::blue);
            view.item(row)->setIcon(QIcon(thumbnail));
            view.item(row)->setData(Qt::UserRole, pass);
        }
        // Cover a model notification spanning both hidden and visible rows.
        view.model()->dataChanged(view.model()->index(0, 0),
                                  view.model()->index(view.count() - 1, 0), {Qt::DecorationRole});
        settle();
        verifyFirstVisible(view);
        view.resize(pass % 2 ? 480 : 810, 500);
        settle();
        verifyFirstVisible(view);
        view.verticalScrollBar()->setValue(320);
        settle();
        for (int row = 0; row < view.count(); ++row) {
            auto *item = view.item(row);
            if (!item->isHidden() && view.viewport()->rect().contains(view.visualItemRect(item))) {
                verifyItem(view, item);
                break;
            }
        }
    }
    for (int row = 0; row < view.count(); ++row) view.item(row)->setHidden(true);
    view.doItemsLayout();
    for (int row = 0; row < view.count(); ++row) view.item(row)->setIcon(QIcon(pixmap));
    settle();
    check(!view.itemAt(QPoint(75, 75)), "empty filtered gallery has no clickable hidden item");
    for (int row = 0; row < view.count(); ++row) view.item(row)->setHidden(false);
    view.doItemsLayout();
    view.verticalScrollBar()->setValue(0);
    settle();
    verifyFirstVisible(view);
    check(view.count() == 200 && view.item(0)->icon().cacheKey() != 0,
          "hidden thumbnail updates remain available when the filter is cleared");
}
}

int main(int argc, char **argv)
{
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        std::fprintf(stderr, "%s\n", message.toUtf8().constData());
    });
    QApplication app(argc, argv);
    testGallery();
    return failures == 0 ? 0 : 1;
}
