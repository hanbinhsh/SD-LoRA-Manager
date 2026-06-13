#ifndef IMAGELOADER_H
#define IMAGELOADER_H

#include <QObject>
#include <QRunnable>
#include <QString>
#include <QImage>
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

        // 3. 加载图片
        QImage srcImg(m_path);

        // === 核心逻辑：文件不存在 或 加载失败 ===
        if (srcImg.isNull()) {
            // 1. 填充深灰背景
            painter.fillRect(QRect(QPoint(0, 0), targetSize), QColor("#25282f"));

            // 2. 画边框
            QPen pen(QColor("#3d4450"));
            pen.setWidth(2);
            painter.setPen(pen);

            if (!m_isFitMode)
                painter.drawRoundedRect(1, 1, targetSize.width() - 2, targetSize.height() - 2, m_radius, m_radius);
            else
                painter.drawRect(1, 1, targetSize.width() - 2, targetSize.height() - 2);

            // 3. 画一个灰色的 "X" 代替文字 (线程安全)
            painter.setPen(QPen(QColor("#3d4450"), 2));
            painter.drawLine(targetSize.width() * 0.3, targetSize.height() * 0.3,
                             targetSize.width() * 0.7, targetSize.height() * 0.7);
            painter.drawLine(targetSize.width() * 0.7, targetSize.height() * 0.3,
                             targetSize.width() * 0.3, targetSize.height() * 0.7);
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
