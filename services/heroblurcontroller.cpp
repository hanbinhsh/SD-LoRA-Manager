#include "services/heroblurcontroller.h"

#include <QDebug>
#include <QEasingCurve>
#include <QEvent>
#include <QFile>
#include <QFrame>
#include <QImageReader>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QThreadPool>
#include <QTimer>
#include <QVariantAnimation>
#include <QtConcurrent/QtConcurrent>

#include "utils/imagepresentation.h"
#include "utils/styleconstants.h"

HeroBlurController::HeroBlurController(QFrame *heroFrame,
                                       QLabel *backgroundLabel,
                                       QWidget *scrollContents,
                                       QObject *parent)
    : QObject(parent)
    , m_heroFrame(heroFrame)
    , m_backgroundLabel(backgroundLabel)
    , m_scrollContents(scrollContents)
{
    m_imageLoadWatcher = new QFutureWatcher<HeroImageLoadResult>(this);
    connect(m_imageLoadWatcher, &QFutureWatcher<HeroImageLoadResult>::finished,
            this, &HeroBlurController::onImageLoadFinished);

    m_transitionAnim = new QVariantAnimation(this);
    m_transitionAnim->setStartValue(0.0f);
    m_transitionAnim->setEndValue(1.0f);
    m_transitionAnim->setDuration(250);
    m_transitionAnim->setEasingCurve(QEasingCurve::InOutQuad);
    connect(m_transitionAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &val) {
        m_transitionOpacity = val.toFloat();
        if (m_heroFrame) m_heroFrame->update();
        updateBackgroundDuringTransition();
    });
    connect(m_transitionAnim, &QVariantAnimation::finished, this, [this]() {
        m_currentHeroPixmap = m_nextHeroPixmap;
        m_currentBlurredBgPix = m_nextBlurredBgPix;
        m_nextHeroPixmap = QPixmap();
        m_nextBlurredBgPix = QPixmap();
        m_transitionOpacity = 0.0f;
        if (m_heroFrame) m_heroFrame->update();
        updateBackgroundDuringTransition();
    });

    m_bgResizeTimer = new QTimer(this);
    m_bgResizeTimer->setSingleShot(true);
    connect(m_bgResizeTimer, &QTimer::timeout, this, &HeroBlurController::updateBackgroundImage);

    if (m_heroFrame) {
        m_heroFrame->installEventFilter(this);
        m_heroFrame->setCursor(Qt::PointingHandCursor);
    }

    if (m_backgroundLabel && m_scrollContents) {
        m_scrollContents->installEventFilter(this);
        m_backgroundLabel->setScaledContents(false);
        m_backgroundLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        updateBackgroundImage();
    }
}

HeroBlurController::~HeroBlurController()
{
    shutdown();
}

void HeroBlurController::setThreadPool(QThreadPool *pool)
{
    m_threadPool = pool;
}

void HeroBlurController::setBlurOptions(bool downscaleBlur, int processWidth, int blurRadius)
{
    m_downscaleBlur = downscaleBlur;
    m_blurProcessWidth = processWidth;
    m_blurRadius = blurRadius;
}

void HeroBlurController::setNsfwBlurPredicate(std::function<bool(const QString &)> predicate)
{
    m_nsfwBlurPredicate = std::move(predicate);
}

void HeroBlurController::setResizeTrackingEnabled(bool enabled)
{
    m_resizeTrackingEnabled = enabled;
    if (!enabled && m_bgResizeTimer) {
        m_bgResizeTimer->stop();
    }
}

void HeroBlurController::clearCurrentPath()
{
    m_currentHeroPath.clear();
}

QString HeroBlurController::currentHeroPath() const
{
    return m_currentHeroPath;
}

void HeroBlurController::updateHeroFrame()
{
    if (m_heroFrame) {
        m_heroFrame->update();
    }
}

void HeroBlurController::shutdown()
{
    m_shuttingDown = true;
    if (m_bgResizeTimer) m_bgResizeTimer->stop();
    if (m_transitionAnim) m_transitionAnim->stop();
    if (m_imageLoadWatcher && m_imageLoadWatcher->isRunning()) {
        m_imageLoadWatcher->waitForFinished();
    }
}

HeroImageLoadResult HeroBlurController::processImageTask(const QString &path)
{
    HeroImageLoadResult result;
    result.path = path;

    QImageReader reader(path);
    reader.setAutoTransform(true);
    result.originalImg = reader.read();
    result.valid = !result.originalImg.isNull();
    return result;
}

QPixmap HeroBlurController::applyBlurToImage(const QImage &srcImg, const QSize &bgSize, const QSize &heroSize) const
{
    const ImagePresentation::BlurOptions options{
        m_downscaleBlur, m_blurProcessWidth, m_blurRadius, QColor(AppStyle::MainBackground())};
    return ImagePresentation::blurredDetailBackground(srcImg, bgSize, heroSize, options);
}

QSize HeroBlurController::resolveBackgroundTargetSize() const
{
    QSize targetSize;
    if (m_scrollContents) {
        targetSize = m_scrollContents->size();
        if (QWidget *viewport = m_scrollContents->parentWidget()) {
            targetSize.setWidth(qMax(targetSize.width(), viewport->width()));
            targetSize.setHeight(qMax(targetSize.height(), viewport->height()));
        }
    } else if (m_backgroundLabel) {
        targetSize = m_backgroundLabel->size();
    }
    if (targetSize.isEmpty()) {
        targetSize = QSize(1920, 1080);
    }
    return targetSize;
}

void HeroBlurController::transitionToImage(const QString &path)
{
    if (m_shuttingDown) return;
    if (path == m_currentHeroPath) {
        return;
    }

    m_currentHeroPath = path;

    if (m_transitionAnim && m_transitionAnim->state() == QAbstractAnimation::Running) {
        m_transitionAnim->stop();
        if (!m_nextHeroPixmap.isNull()) {
            m_currentHeroPixmap = m_nextHeroPixmap;
            m_currentBlurredBgPix = m_nextBlurredBgPix;
        }
    }

    if (m_imageLoadWatcher && m_imageLoadWatcher->isRunning()) {
        m_imageLoadWatcher->cancel();
    }

    m_nextHeroPixmap = QPixmap();
    m_nextBlurredBgPix = QPixmap();
    m_transitionOpacity = 0.0f;

    if (path.isEmpty()) {
        if (m_transitionAnim) m_transitionAnim->start();
    } else {
        QThreadPool *pool = m_threadPool ? m_threadPool : QThreadPool::globalInstance();
        QFuture<HeroImageLoadResult> future = QtConcurrent::run(pool, &HeroBlurController::processImageTask, path);
        if (m_imageLoadWatcher) m_imageLoadWatcher->setFuture(future);
    }
}

void HeroBlurController::onImageLoadFinished()
{
    if (m_shuttingDown || !m_imageLoadWatcher || m_imageLoadWatcher->isCanceled()) {
        return;
    }

    const HeroImageLoadResult result = m_imageLoadWatcher->result();
    if (result.path != m_currentHeroPath) {
        qDebug() << "Discarding obsolete image load:" << result.path;
        return;
    }

    if (!result.valid) {
        m_nextHeroPixmap = QPixmap();
        m_nextBlurredBgPix = QPixmap();
    } else {
        const QPixmap rawPix = QPixmap::fromImage(result.originalImg);
        const bool shouldBlur = m_nsfwBlurPredicate ? m_nsfwBlurPredicate(result.path) : false;
        m_nextHeroPixmap = shouldBlur ? ImagePresentation::applyNSFWBlur(rawPix) : rawPix;

        const QSize targetSize = resolveBackgroundTargetSize();
        if (m_backgroundLabel) {
            const QRect bgRect(QPoint(0, 0), targetSize);
            if (m_backgroundLabel->geometry() != bgRect) {
                m_backgroundLabel->setGeometry(bgRect);
            }
        }

        QSize heroSize = m_heroFrame ? m_heroFrame->size() : QSize();
        if (heroSize.isEmpty()) heroSize = QSize(targetSize.width(), 400);

        if (m_currentBlurredBgPix.isNull() && !m_currentHeroPixmap.isNull()) {
            m_currentBlurredBgPix = applyBlurToImage(m_currentHeroPixmap.toImage(), targetSize, heroSize);
        }
        m_nextBlurredBgPix = applyBlurToImage(result.originalImg, targetSize, heroSize);
    }

    m_transitionOpacity = 0.0f;
    if (m_transitionAnim) {
        if (m_transitionAnim->state() == QAbstractAnimation::Running) {
            m_transitionAnim->stop();
        }
        m_transitionAnim->start();
    }
}

void HeroBlurController::updateBackgroundImage()
{
    if (m_shuttingDown || !m_backgroundLabel || !m_heroFrame || !m_scrollContents) {
        return;
    }

    const QSize targetSize = resolveBackgroundTargetSize();
    const QRect bgRect(QPoint(0, 0), targetSize);
    if (m_backgroundLabel->geometry() != bgRect) {
        m_backgroundLabel->setGeometry(bgRect);
    }

    if (m_transitionAnim && m_transitionAnim->state() == QAbstractAnimation::Running) {
        return;
    }

    QSize heroSize = m_heroFrame->size();
    if (heroSize.isEmpty()) heroSize = QSize(targetSize.width(), 400);

    if (!m_currentHeroPixmap.isNull()) {
        m_currentBlurredBgPix = applyBlurToImage(m_currentHeroPixmap.toImage(), targetSize, heroSize);
        m_backgroundLabel->setPixmap(m_currentBlurredBgPix);
    } else if (!m_currentHeroPath.isEmpty() && QFile::exists(m_currentHeroPath)) {
        const QImage img(m_currentHeroPath);
        m_currentBlurredBgPix = applyBlurToImage(img, targetSize, heroSize);
        m_backgroundLabel->setPixmap(m_currentBlurredBgPix);
    } else {
        m_backgroundLabel->clear();
        QPixmap empty(targetSize);
        empty.fill(QColor(AppStyle::MainBackground()));
        m_backgroundLabel->setPixmap(empty);
    }
}

void HeroBlurController::updateBackgroundDuringTransition()
{
    if (!m_backgroundLabel) return;
    const QSize bgSize = m_backgroundLabel->size();
    if (bgSize.isEmpty()) return;

    QPixmap canvas(bgSize);
    canvas.fill(QColor(AppStyle::MainBackground()));

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    if (!m_nextBlurredBgPix.isNull()) {
        if (!m_currentBlurredBgPix.isNull()) {
            painter.setOpacity(1.0 - m_transitionOpacity);
            painter.drawPixmap(0, 0, m_currentBlurredBgPix);
        }
        painter.setOpacity(m_transitionOpacity);
        painter.drawPixmap(0, 0, m_nextBlurredBgPix);
    } else if (!m_currentBlurredBgPix.isNull()) {
        qreal alpha = 1.0 - m_transitionOpacity;
        if (alpha < 0.0) alpha = 0.0;
        painter.setOpacity(alpha);
        painter.drawPixmap(0, 0, m_currentBlurredBgPix);
    }

    painter.end();
    m_backgroundLabel->setPixmap(canvas);
}

bool HeroBlurController::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_heroFrame && m_heroFrame) {
        if (event->type() == QEvent::Paint) {
            QPainter painter(m_heroFrame);

            const bool isLightTheme = AppStyle::color("windowBg").lightness() > 128;
            const QColor heroFallbackBg = isLightTheme ? AppStyle::color("headerBg") : QColor(Qt::black);
            painter.fillRect(m_heroFrame->rect(), heroFallbackBg);

            auto drawPix = [&](const QPixmap &pix, qreal opacity) {
                if (pix.isNull()) return;
                const QSize widgetSize = m_heroFrame->size();
                const QSize imgSize = pix.size();
                if (imgSize.isEmpty()) return;

                const double scaleW = static_cast<double>(widgetSize.width()) / imgSize.width();
                const double scaleH = static_cast<double>(widgetSize.height()) / imgSize.height();
                const double scale = qMax(scaleW, scaleH);

                const double newW = imgSize.width() * scale;
                const double newH = imgSize.height() * scale;
                const double offsetX = (widgetSize.width() - newW) / 2.0;
                const double offsetY = (widgetSize.height() - newH) / 4.0;

                painter.setOpacity(opacity);
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.drawPixmap(QRectF(offsetX, offsetY, newW, newH), pix, pix.rect());
            };

            if (!m_nextHeroPixmap.isNull()) {
                drawPix(m_currentHeroPixmap, 1.0);
                drawPix(m_nextHeroPixmap, m_transitionOpacity);
            } else if (m_transitionAnim && m_transitionAnim->state() == QAbstractAnimation::Running) {
                drawPix(m_currentHeroPixmap, 1.0 - m_transitionOpacity);
            } else {
                drawPix(m_currentHeroPixmap, 1.0);
            }

            return true;
        }

        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                if (!m_currentHeroPath.isEmpty() && QFile::exists(m_currentHeroPath)) {
                    emit heroClicked(m_currentHeroPath);
                    return true;
                }
            }
        }
    }

    if (watched == m_scrollContents && event->type() == QEvent::Resize) {
        if (m_resizeTrackingEnabled && m_backgroundLabel && m_bgResizeTimer) {
            m_bgResizeTimer->start(0);
        }
    }

    return QObject::eventFilter(watched, event);
}
