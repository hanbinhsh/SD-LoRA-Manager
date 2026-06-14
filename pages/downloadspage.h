#ifndef DOWNLOADSPAGE_WIDGET_H
#define DOWNLOADSPAGE_WIDGET_H

#include <QHash>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "downloadmodels.h"

class QComboBox;
class QPixmap;
class QPushButton;
class QTabWidget;
class QTableWidgetItem;
class QVBoxLayout;

namespace Ui {
class DownloadsPage;
}

class DownloadsPage : public QWidget
{
    Q_OBJECT

public:
    explicit DownloadsPage(QWidget *parent = nullptr);
    ~DownloadsPage() override;

    QComboBox *filterCombo() const;
    QTabWidget *statusTabs() const;

    QPushButton *checkSelectedButton() const;
    QPushButton *checkAllButton() const;
    QPushButton *downloadSelectedButton() const;
    QPushButton *retryButton() const;
    QPushButton *openFolderButton() const;
    QPushButton *clearCompletedButton() const;
    QPushButton *toggleCurrentTabButton() const;
    QPushButton *clearSelectionButton() const;
    QPushButton *ignoreSelectedButton() const;

    QVBoxLayout *cardsLayout(const QString &category) const;

    QString currentCategory() const;
    QStringList selectedFilePaths() const;
    QStringList filePathsForCategory(const QString &category) const;
    QStringList sortedFilePathsForCategory(const QString &category) const;
    QString categoryForStatus(const QString &status) const;
    QString cardStatusText(const QString &filePath) const;
    QString cardTargetPath(const QString &filePath) const;
    bool containsCard(const QString &filePath) const;
    void setStatusText(const QString &text);
    void setModelSelectionAvailability(bool hasCurrentModel, bool hasSelectedModels);
    void setUpdateCheckButtonsEnabled(bool enabled);
    void updateSelectionSummary();
    void initializeAppearance();
    void addOrUpdateCard(const ModelUpdateInfo &info, const QString &status, bool sourceAvailable);
    void updateCardStatus(const QString &filePath, const QString &status);
    void updateCardProgress(const QString &filePath, int percent, const QString &speedText);
    void updateCardTargetPath(const QString &filePath, const QString &targetPath);
    void setCardPreview(const QString &filePath, const QPixmap &pixmap);
    void setCardSelected(const QString &filePath, bool selected);
    void setCurrentTabSelection(bool checked);
    void clearAllCardSelection();
    void toggleCurrentTabSelection();
    bool hasErrorCards() const;
    void placeCardInCategory(const QString &filePath, const QString &category, bool deferSort = false);
    void sortCardsInCategory(const QString &category);
    void sortAllCards();
    void removeCard(const QString &filePath);

    void setMetadataScanRunning(bool running);
    void setMetadataScanItems(const QVector<MetadataScanItem> &items);
    QStringList checkedMetadataScanFilePaths() const;
    void updateMetadataScanItemStatus(const QString &filePath, const QString &status, const QString &category, const QString &lastSyncedAt = QString(), const QString &lastSyncedSource = QString());
    void setHealthCheckRunning(bool running);
    void setHealthIssues(const QVector<MetadataHealthIssue> &issues);
    void loadMetadataResultCache();
    void saveMetadataResultCache() const;

signals:
    void cardSelectionChanged();
    void sourceRequested(const QString &filePath);
    void civitaiRequested(const QString &filePath);
    void downloadRequested(const QString &filePath);
    void ignoreToggled(const QString &filePath);
    void metadataScanRequested();
    void metadataUpdateRequested(const QStringList &filePaths);
    void metadataCivArchiveRequested(const QStringList &filePaths);
    void metadataOpenModelRequested(const QString &filePath);
    void metadataOpenFolderRequested(const QString &filePath);
    void metadataSelectionChanged();
    void healthCheckRequested();
    void healthOpenModelRequested(const QString &filePath);
    void healthOpenFolderRequested(const QString &filePath);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateVersionActionButtons();
    void updateSelectionSummary(int selectedCurrent, int currentTotal, int selectedTotal);
    QString currentMetadataCategory() const;
    bool metadataItemMatchesCurrentCategory(const MetadataScanItem &item) const;
    void refreshMetadataScanTable();
    void updateMetadataActionButtons();
    void updateMetadataSelectionSummary();
    void updateSelectedMetadataIdentityLabel();
    void applyMetadataTableColumnLayout();
    void applyHealthTableColumnLayout();
    void updateHealthActionButtons();
    void setCurrentMetadataCategoryChecked(bool checked);
    void clearMetadataSelection();
    void copySelectedHealthIssues() const;
    QString metadataResultCachePath() const;

    Ui::DownloadsPage *ui = nullptr;
    QHash<QString, DownloadCardWidgets> m_cards;
    QVector<MetadataScanItem> m_metadataScanItems;
    QVector<MetadataHealthIssue> m_healthIssues;
    bool m_hasCurrentModel = false;
    bool m_hasSelectedModels = false;
    bool m_updateCheckBusy = false;
    bool m_metadataScanRunning = false;
    bool m_healthCheckRunning = false;
};

#endif // DOWNLOADSPAGE_WIDGET_H
