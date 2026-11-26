#ifndef USERGALLERYPAGE_H
#define USERGALLERYPAGE_H

#include "core/modeltypes.h"
#include "utils/modelimagematcher.h"

#include <QHash>
#include <QIcon>
#include <QImage>
#include <QMap>
#include <QPixmap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

class QListWidgetItem;
class QThreadPool;
class QTimer;
class QWheelEvent;
class FlowLayout;
class TagFlowWidget;

namespace Ui {
class UserGalleryPage;
}

struct UserGalleryOptions {
    bool filterNsfw = false;
    int nsfwMode = 1; // 0: hide, 1: blur
    QStringList nsfwRules;
    bool splitOnNewline = true;
    QStringList filterTags;
    bool recursive = true;
    int matchMode = 0;
    bool comfyModelNameFallback = true;
};

struct UserGalleryFilterState {
    int userTagSortMode = 0;
    bool userTagLoraOnly = false;
    bool userTagIncludeNegative = false;
    int galleryPromptSortMode = 0;
    bool galleryTagViewActive = false;
};

class UserGalleryPage : public QWidget
{
    Q_OBJECT

public:
    explicit UserGalleryPage(QWidget *parent = nullptr);
    ~UserGalleryPage() override;

    void setThreadPools(QThreadPool *thumbPool, QThreadPool *backgroundPool);
    void setTranslationMap(const QHash<QString, QString> *map);
    void setImageCache(const QMap<QString, UserImageInfo> *cache);
    void setOptions(const UserGalleryOptions &options);
    void setPlaceholderIcon(const QIcon &icon);
    void recolorPlaceholders(const QIcon &icon);

    void restoreFilterState(const UserGalleryFilterState &state);
    UserGalleryFilterState filterState() const;

    void invalidateAndReset();
    void shutdown();
    void clearGalleryView();
    void showInvalidFolderHint();
    void refreshNsfwDisplay();

    void startScan(const QStringList &validGalleryPaths,
                   bool isGlobalMode,
                   const QString &displayName,
                   const ModelImageMatcher::ModelUsageInput &modelInput);

    void filterBySingleTagOrLora(const QString &tagOrLora, bool isLora);
    void filterByMultiTagsAndLoras(const QStringList &tags, const QStringList &loras);

    int imageCount() const;
    QString currentImagePath() const;
    QStringList visibleImagePaths() const;
    QSet<QString> allImagePaths() const;
    bool isImageNsfwByPath(const QString &imagePath) const;

    bool isCursorInsideScrollAreas() const;
    bool isScrollChild(QObject *watched) const;
    void handleWheelScroll(QObject *watched, QWheelEvent *event);

    void applyExternalImageUpdate(const QString &filePath, const QPixmap &originalPix, const QIcon &originalIcon);

    Q_INVOKABLE void onIconLoaded(const QString &id, const QImage &image);

signals:
    void setSdFolderRequested();
    void rescanRequested();
    void openSettingsForTranslationRequested();
    void openGraphRequested(int scope, const QString &selectedTagFocus);
    void imageSelected(const QString &imagePath);
    void openImageViewerRequested(const QStringList &visiblePaths, const QString &currentPath);
    void showInFolderRequested(const QString &filePath);
    void showComfyWorkflowRequested(const QString &filePath);
    void showRawMetadataRequested(const QString &filePath);
    void jumpToModelRequested(const QString &name, const QString &hash);
    void statusMessage(const QString &text, int timeoutMs = 0);
    void cacheEntriesParsed(const QMap<QString, UserImageInfo> &newUpdates);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onUserImageClicked(QListWidgetItem *item);
    void onContextMenuRequested(const QPoint &pos);
    void onTagFilterChanged(const QSet<QString> &selectedTags);

private:
    void setupPromptToolbar();
    void setTagViewActive(bool tagView);
    void updatePromptTagView();
    void applyPromptSortMode();
    void clearPromptView();
    void resetThumbLoading();
    void scheduleVisibleThumbLoad();
    void dispatchVisibleThumbLoad();
    void refreshTagFlowStats(bool applyGalleryFilter = true, bool resetGalleryScrollToTop = true);
    void applyTagFilter(const QSet<QString> &selectedTags, bool resetScrollToTop = true);
    bool isImageNsfw(const QString &prompt, const QStringList &cleanTags) const;

    Ui::UserGalleryPage *ui = nullptr;
    QThreadPool *m_threadPool = nullptr;
    QThreadPool *m_backgroundThreadPool = nullptr;
    const QHash<QString, QString> *m_translationMap = nullptr;
    const QMap<QString, UserImageInfo> *m_imageCache = nullptr;
    UserGalleryOptions m_options;
    QIcon m_placeholderIcon;

    TagFlowWidget *m_tagFlowWidget = nullptr;
    TagFlowWidget *m_posTagWidget = nullptr;
    TagFlowWidget *m_negTagWidget = nullptr;
    FlowLayout *m_paramsChipLayout = nullptr;
    QTimer *m_tagSearchDebounceTimer = nullptr;
    QTimer *m_thumbLoadTimer = nullptr;

    QHash<QString, QListWidgetItem*> m_itemByPath;
    QSet<QString> m_queuedThumbPaths;
    QSet<QString> m_loadedThumbPaths;
    QHash<QString, int> m_failedThumbLoads;
    quint64 m_generation = 0;
    quint64 m_layoutGeneration = 0;

    QString m_currentImagePath;
    QString m_currentPrompt;
    QString m_currentNegative;
    QString m_currentParams;
    QStringList m_posTagOrder;
    QStringList m_negTagOrder;
    QStringList m_extraLoraFilters;
    bool m_tagViewActive = false;
    bool m_isShuttingDown = false;
};

#endif // USERGALLERYPAGE_H
