#ifndef HOMEGALLERYPAGE_H
#define HOMEGALLERYPAGE_H

#include "core/modeltypes.h"

#include <QHash>
#include <QIcon>
#include <QImage>
#include <QList>
#include <QPixmap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QListWidgetItem;
class QThreadPool;
class QTimer;

namespace Ui {
class HomeGalleryPage;
}

struct HomeFilterTagInfo {
    QString tag;
    int count = 0;
    bool fromModel = false;
    bool fromUser = false;

    QString sourceName() const {
        if (fromModel && fromUser) return QStringLiteral("mixed");
        if (fromUser) return QStringLiteral("user");
        return QStringLiteral("model");
    }
};

struct HomeGalleryOptions {
    bool filterNsfw = false;
    int nsfwMode = 1; // 0: hide, 1: blur
    int nsfwLevel = 1;
};

struct HomeGalleryFilterState {
    QString authorFilter;
    QSet<QString> tagFilters;
    bool filterExpanded = false;
    bool tagsSortByCount = true;
};

struct HomeGalleryEntry {
    const QListWidgetItem *sourceItem = nullptr;
    QString tooltip;
};

class HomeGalleryPage : public QWidget
{
    Q_OBJECT

public:
    explicit HomeGalleryPage(QWidget *parent = nullptr);
    ~HomeGalleryPage() override;

    void setThreadPools(QThreadPool *thumbPool, QThreadPool *backgroundPool);
    void setOptions(const HomeGalleryOptions &options);
    void setPlaceholderIcons(const QIcon &placeholderIcon, const QIcon &noPreviewIcon);
    void recolorPlaceholders(const QIcon &placeholderIcon, const QIcon &noPreviewIcon);

    void restoreFilterState(const HomeGalleryFilterState &state);
    HomeGalleryFilterState filterState() const;
    QString authorFilter() const;
    QSet<QString> tagFilters() const;
    bool isTagsSortByCount() const;

    void setAuthorFilter(const QString &author);
    void addTagFilter(const QString &tag);
    void clearFilters();

    void refreshCollectionsBar(const QStringList &collectionNames,
                               const QString &currentFilter,
                               const QString &uncategorizedKey);
    void refreshFilterChips(const QList<HomeFilterTagInfo> &availableTags);
    void setModelEntries(const QVector<HomeGalleryEntry> &entries);
    void clearItems();

    QListWidgetItem *itemForPath(const QString &filePath) const;
    void updateDownloadedCoverPreview(const QString &localBaseName,
                                      const QString &downloadedDirectory,
                                      const QString &savePath);
    void syncPreviewState(const QString &filePath, ModelPreviewState previewState);
    void invalidateIconCacheForPath(const QString &previewPath);
    void clearIconCache();
    void applyExternalImageUpdate(const QString &filePath,
                                  const QPixmap &originalPix,
                                  const QIcon &originalIcon);
    void shutdown();

    Q_INVOKABLE void onIconLoaded(const QString &id, const QImage &image);

signals:
    void createCollectionRequested();
    void collectionFilterRequested(const QString &collectionName);
    void renameCollectionRequested(const QString &collectionName);
    void deleteCollectionRequested(const QString &collectionName);
    void filtersChanged();
    void filterChipsRefreshRequested();
    void modelClicked(const QString &filePath);
    void contextMenuRequested(const QList<QListWidgetItem*> &selectedItems, const QPoint &globalPos);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onGalleryItemClicked(QListWidgetItem *item);
    void onGalleryContextMenu(const QPoint &pos);

private:
    void scheduleVisibleThumbLoad();
    void dispatchVisibleThumbLoad();
    void applyItemIcon(QListWidgetItem *item,
                       const QPixmap &originalPix,
                       const QIcon &originalIcon,
                       QPixmap &blurredPix);

    Ui::HomeGalleryPage *ui = nullptr;
    QThreadPool *m_thumbPool = nullptr;
    QThreadPool *m_backgroundPool = nullptr;
    HomeGalleryOptions m_options;
    QIcon m_placeholderIcon;
    QIcon m_noPreviewIcon;

    QString m_authorFilter;
    QSet<QString> m_tagFilters;
    bool m_filterExpanded = false;
    bool m_tagsSortByCount = true;

    QHash<QString, QIcon> m_iconCache;
    QSet<QString> m_queuedThumbPaths;
    QSet<QString> m_loadedThumbPaths;
    QHash<QString, QListWidgetItem*> m_itemByPath;
    QTimer *m_authorFilterDebounceTimer = nullptr;
    QTimer *m_thumbLoadTimer = nullptr;
    bool m_isShuttingDown = false;
};

#endif // HOMEGALLERYPAGE_H
