#ifndef HEROBLURCONTROLLER_H
#define HEROBLURCONTROLLER_H

#include <QFutureWatcher>
#include <QImage>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QString>
#include <functional>

class QFrame;
class QLabel;
class QThreadPool;
class QTimer;
class QVariantAnimation;
class QWidget;

struct HeroImageLoadResult {
    QString path;
    QImage originalImg;
    bool valid = false;
};

class HeroBlurController : public QObject
{
    Q_OBJECT

public:
    explicit HeroBlurController(QFrame *heroFrame,
                                QLabel *backgroundLabel,
                                QWidget *scrollContents,
                                QObject *parent = nullptr);
    ~HeroBlurController() override;

    void setThreadPool(QThreadPool *pool);
    void setBlurOptions(bool downscaleBlur, int processWidth, int blurRadius);
    void setNsfwBlurPredicate(std::function<bool(const QString &)> predicate);
    void setResizeTrackingEnabled(bool enabled);

    void transitionToImage(const QString &path);
    void updateBackgroundImage();
    void updateHeroFrame();
    void clearCurrentPath();
    QString currentHeroPath() const;
    void shutdown();

signals:
    void heroClicked(const QString &imagePath);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    static HeroImageLoadResult processImageTask(const QString &path);
    QPixmap applyBlurToImage(const QImage &srcImg, const QSize &bgSize, const QSize &heroSize) const;
    QSize resolveBackgroundTargetSize() const;
    void onImageLoadFinished();
    void updateBackgroundDuringTransition();

    QPointer<QFrame> m_heroFrame;
    QPointer<QLabel> m_backgroundLabel;
    QPointer<QWidget> m_scrollContents;
    QThreadPool *m_threadPool = nullptr;

    QFutureWatcher<HeroImageLoadResult> *m_imageLoadWatcher = nullptr;
    QVariantAnimation *m_transitionAnim = nullptr;
    QTimer *m_bgResizeTimer = nullptr;

    QString m_currentHeroPath;
    QPixmap m_currentHeroPixmap;
    QPixmap m_nextHeroPixmap;
    QPixmap m_currentBlurredBgPix;
    QPixmap m_nextBlurredBgPix;
    float m_transitionOpacity = 0.0f;

    bool m_downscaleBlur = true;
    int m_blurProcessWidth = 500;
    int m_blurRadius = 30;
    bool m_resizeTrackingEnabled = true;
    bool m_shuttingDown = false;
    std::function<bool(const QString &)> m_nsfwBlurPredicate;
};

#endif // HEROBLURCONTROLLER_H
