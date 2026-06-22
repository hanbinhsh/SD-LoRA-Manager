#ifndef IMAGELOADER_H
#define IMAGELOADER_H

#include <QObject>
#include <QRunnable>
#include <QString>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointer>
#include <QMetaObject>

class IconLoaderTask : public QObject, public QRunnable {
    Q_OBJECT
public:
    IconLoaderTask(const QString &path, int size, int radius, QObject *receiver, const QString &id, bool isFitMode = false)
        : m_path(path), m_size(size), m_radius(radius), m_receiver(receiver), m_id(id), m_isFitMode(isFitMode) {}

    void run() override {
        // 1. 检查接收者是否还活着 (快速检查)
        if (m_receiver.isNull()) return;

        // 2. 准备画布
        QSize targetSize = m_isFitMode ? QSize(100, 150) : QSize(m_size, m_size);
        QImage finalImg(targetSize, QImage::Format_ARGB32_Premultiplied);
        finalImg.fill(Qt::transparent);

        QPainter painter(&finalImg);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);

        // 裁切路径 (仅主页模式)
        if (!m_isFitMode) {
            QPainterPath pathObj;
            pathObj.addRoundedRect(0, 0, m_size, m_size, m_radius, m_radius);
            painter.setClipPath(pathObj);
        }

        // 3. 加载图片（按需缩放解码，避免把大图整张读进内存再缩小）
        QImageReader reader(m_path);
        QImage srcImg;
        if (reader.canRead()) {
            const QSize orig = reader.size();
            if (orig.isValid() && !orig.isEmpty()) {
                QSize decode = orig;
                if (m_isFitMode) {
                    // 适配模式：解码到刚好覆盖目标框即可
                    decode = orig.scaled(targetSize, Qt::KeepAspectRatio);
                } else {
                    // 方形裁切模式：让较短边缩到 m_size，裁切与缩放结果不变
                    const int shortSide = qMin(orig.width(), orig.height());
                    if (shortSide > m_size) {
                        const double f = double(m_size) / double(shortSide);
                        decode = QSize(qMax(1, qRound(orig.width() * f)),
                                       qMax(1, qRound(orig.height() * f)));
                    }
                }
                if (decode.isValid() && !decode.isEmpty()
                    && (decode.width() < orig.width() || decode.height() < orig.height())) {
                    reader.setScaledSize(decode);
                }
            }
            srcImg = reader.read();
        }

        // === 核心逻辑：文件不存在 或 加载失败 ===
        if (srcImg.isNull()) {
            // 返回空图：后台线程不画占位（颜色读不到当前主题）。改由主线程保留
            // 主题化的 placeholderIcon（缺失占位X），切主题时随之重染。
            painter.end();
            if (!m_receiver.isNull()) {
                QMetaObject::invokeMethod(m_receiver, "onIconLoaded",
                                          Qt::QueuedConnection,
                                          Q_ARG(QString, m_id),
                                          Q_ARG(QImage, QImage()));
            }
            return;
        }
        else {
            // 图片存在，正常绘制
            if (m_isFitMode) {
                QImage scaled = srcImg.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                int x = (targetSize.width() - scaled.width()) / 2;
                int y = (targetSize.height() - scaled.height()) / 2;
                painter.drawImage(x, y, scaled);
            } else {
                int side = qMin(srcImg.width(), srcImg.height());
                int x = (srcImg.width() - side) / 2;
                int y = 0;
                QImage square = srcImg.copy(x, y, side, side);
                QImage scaled = square.scaled(m_size, m_size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                painter.drawImage(0, 0, scaled);

                QPen pen(QColor(255, 255, 255, 30));
                pen.setWidth(2);
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(1, 1, m_size - 2, m_size - 2, m_radius, m_radius);
            }
        }
        painter.end();

        // 4. 最终检查并回调
        // 这一步是防止崩溃的最后一道防线
        if (!m_receiver.isNull()) {
            QMetaObject::invokeMethod(m_receiver, "onIconLoaded",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, m_id),
                                      Q_ARG(QImage, finalImg));
        }
    }

private:
    QString m_path;
    int m_size;
    int m_radius;
    QPointer<QObject> m_receiver;
    QString m_id;
    bool m_isFitMode;
};

#endif // IMAGELOADER_H
