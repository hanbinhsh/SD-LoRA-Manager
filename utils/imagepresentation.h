#pragma once

#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QColor>

namespace ImagePresentation {

// QPixmap/QGraphicsScene rendering stays on the GUI thread.
QIcon getSquareIcon(const QPixmap &source);
QIcon getFitIcon(const QString &path);
QPixmap applyRoundedMask(const QPixmap &source, int radius);
QPixmap applyNSFWBlur(const QPixmap &source);

struct BlurOptions {
    bool downscale = true;
    int processWidth = 500;
    int radius = 30;
    QColor background;
};
QPixmap blurredDetailBackground(const QImage &source, const QSize &backgroundSize,
                                const QSize &heroSize, const BlurOptions &options);
QIcon modelPlaceholder(bool small, bool knownNoPreview,
                       const QColor &background, const QColor &foreground);

}
