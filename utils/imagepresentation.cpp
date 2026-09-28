#include "imagepresentation.h"

#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QGraphicsBlurEffect>
#include <QPainter>
#include <QPainterPath>

namespace ImagePresentation {
namespace {
QColor withAlpha(QColor color, int alpha)
{
    color.setAlpha(alpha);
    return color;
}
}

QIcon getSquareIcon(const QPixmap &srcPix)
{
    if (srcPix.isNull()) return QIcon();

    // 1. 计算裁剪区域 (短边裁剪)
    int side = qMin(srcPix.width(), srcPix.height());
    // X轴居中，Y轴顶端对齐 (适合人物)
    int x = (srcPix.width() - side) / 2;
    int y = 0;

    // 获取原始的正方形裁剪图
    QPixmap square = srcPix.copy(x, y, side, side);

    // 2. === 核心修改：增加透明内边距 ===
    // 设定输出图标的基础分辨率 (越高越清晰，64x64 对侧边栏足够)
    int fullSize = 64;

    // 设定内边距 (比如 8px，意味着图片四周都有 8px 的透明区域)
    // 这样图片实际显示大小就是 48x48，视觉上就分开了
    int padding = 8;
    int contentSize = fullSize - (padding * 2);

    // 创建透明底图
    QPixmap finalPix(fullSize, fullSize);
    finalPix.fill(Qt::transparent);

    QPainter painter(&finalPix);
    // 开启高质量抗锯齿
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // 将裁剪好的图缩放并画在中间
    painter.drawPixmap(padding, padding,
                       square.scaled(contentSize, contentSize,
                                     Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation));

    return QIcon(finalPix);
}

QIcon getFitIcon(const QString &path)
{
    QPixmap pix(path);
    if (pix.isNull()) return QIcon();

    // 目标尺寸 (根据你的图库按钮大小设定，这里是 100x150)
    QSize targetSize(100, 150);

    // 创建一个透明底的容器
    QPixmap base(targetSize);
    base.fill(Qt::transparent); // 或者使用 Qt::black

    QPainter painter(&base);
    // 开启抗锯齿
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);

    // 计算适应比例 (KeepAspectRatio)
    QPixmap scaled = pix.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    // 计算居中位置
    int x = (targetSize.width() - scaled.width()) / 2;
    int y = (targetSize.height() - scaled.height()) / 2;

    // 绘制图片
    painter.drawPixmap(x, y, scaled);

    return QIcon(base);
}

QPixmap applyRoundedMask(const QPixmap &src, int radius)
{
    if (src.isNull()) return QPixmap();
    if (radius <= 0) return src;

    QPixmap result(src.size());
    result.fill(Qt::transparent);

    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    // 创建圆角路径
    QPainterPath path;
    path.addRoundedRect(src.rect(), radius, radius);

    // 裁剪并绘制
    painter.setClipPath(path);
    painter.drawPixmap(0, 0, src);

    return result;
}

QPixmap applyNSFWBlur(const QPixmap &pix) {
    if (pix.isNull()) return pix;

    QGraphicsBlurEffect *blur = new QGraphicsBlurEffect;
    blur->setBlurRadius(40); // 强度大一点，确保看不清内容

    QGraphicsScene scene;
    QGraphicsPixmapItem *item = new QGraphicsPixmapItem(pix);
    item->setGraphicsEffect(blur);
    scene.addItem(item);

    QPixmap result(pix.size());
    result.fill(Qt::transparent);
    QPainter painter(&result);
    scene.render(&painter);
    return result;
}

QPixmap blurredDetailBackground(const QImage &srcImg, const QSize &bgSize, const QSize &heroSize,
                                const BlurOptions &options)
{
    if (srcImg.isNull() || bgSize.isEmpty()) return QPixmap();

    QPixmap tempPix;

    // === 修改点：根据设置决定是否缩小 ===
    if (options.downscale) {
        // 使用配置的缩小尺寸
        tempPix = QPixmap::fromImage(srcImg.scaledToWidth(qMax(1, options.processWidth), Qt::SmoothTransformation));
    } else {
        // 不缩小，直接使用原图（注意：这在模糊半径较大时非常耗时）
        tempPix = QPixmap::fromImage(srcImg);
    }

    // 2. 高斯模糊
    QGraphicsBlurEffect *blur = new QGraphicsBlurEffect;
    blur->setBlurRadius(options.radius);
    blur->setBlurHints(QGraphicsBlurEffect::PerformanceHint);
    QGraphicsScene scene;
    QGraphicsPixmapItem *item = new QGraphicsPixmapItem(tempPix);
    item->setGraphicsEffect(blur);
    scene.addItem(item);
    QPixmap blurredResult(tempPix.size());
    blurredResult.fill(Qt::transparent);
    QPainter ptr(&blurredResult);
    scene.render(&ptr);

    // 3. 合成最终背景
    QPixmap finalBg(bgSize);
    finalBg.fill(options.background); // 填充底色
    QPainter painter(&finalBg);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);

    // === 核心修复：使用 heroSize 进行计算 ===
    // 这样算法就和 eventFilter 里的 Hero 绘制逻辑完全一致了

    // 保底：防止 heroSize 为空导致除以0
    int heroW = heroSize.width() > 0 ? heroSize.width() : bgSize.width();
    int heroH = heroSize.height() > 0 ? heroSize.height() : 400;

    double scaleW = (double)heroW / blurredResult.width();
    double scaleH = (double)heroH / blurredResult.height();
    double scale = qMax(scaleW, scaleH); // Cover 模式

    int newW = blurredResult.width() * scale;
    int newH = blurredResult.height() * scale;

    // 使用 heroH 来计算 Y 轴偏移
    int offsetX = (heroW - newW) / 2;
    int offsetY = (heroH - newH) / 4;

    // 绘制图片
    painter.drawPixmap(QRect(offsetX, offsetY, newW, newH), blurredResult);

    // 4. 绘制渐变遮罩 (自然融合到底部背景色)
    // Keep fade positions in content coordinates. This avoids a visible jump
    // when trigger words change the total detail-page height.
    QLinearGradient gradient(0, 0, 0, bgSize.height());
    gradient.setColorAt(0.0, withAlpha(options.background, 120)); // 顶部半透

    const double bgH = qMax(1, bgSize.height());
    const double imgBottomY = offsetY + newH;
    const double fadeStartY = qMax(0.0, imgBottomY - qMax(120.0, heroH * 0.25));
    const double fadeEndY = qMax(fadeStartY + 1.0, imgBottomY);
    gradient.setColorAt(qBound(0.0, fadeStartY / bgH, 1.0), withAlpha(options.background, 210));
    gradient.setColorAt(qBound(0.0, fadeEndY / bgH, 1.0), options.background);
    gradient.setColorAt(1.0, options.background);

    painter.fillRect(finalBg.rect(), gradient);
    painter.end();

    return finalBg;
}

QIcon modelPlaceholder(bool small, bool knownNoPreview,
                       const QColor &background, const QColor &foreground)
{
    const int fullSize = small ? 64 : 180;
    const int padding = small ? 8 : 0;
    const int contentSize = fullSize - padding * 2;
    QPixmap pixmap(fullSize, fullSize);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRect rect(padding, padding, contentSize, contentSize);
    painter.setBrush(background);
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(rect, small ? 6 : 12, small ? 6 : 12);
    QPen pen(foreground);
    pen.setWidth(small ? 3 : 5);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    if (knownNoPreview) {
        const qreal diameter = contentSize * 0.36;
        const QPointF center = QRectF(rect).center();
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QRectF(center.x() - diameter / 2.0,
                                   center.y() - diameter / 2.0, diameter, diameter));
    } else {
        const int margin = small ? qRound(contentSize * 0.28) : 54;
        painter.drawLine(rect.left() + margin, rect.top() + margin,
                         rect.right() - margin, rect.bottom() - margin);
        painter.drawLine(rect.right() - margin, rect.top() + margin,
                         rect.left() + margin, rect.bottom() - margin);
    }
    return QIcon(pixmap);
}

}
