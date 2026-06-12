#ifndef DOWNLOADSPAGE_WIDGET_H
#define DOWNLOADSPAGE_WIDGET_H

#include <QHash>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "downloadmodels.h"

class QLabel;
class QComboBox;
class QPixmap;
class QPushButton;
class QScrollArea;
class QTabWidget;
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

    QLabel *statusLabel() const { return m_statusLabel; }
    QLabel *selectedLabel() const { return m_selectedLabel; }
    QComboBox *filterCombo() const { return m_filterCombo; }
    QTabWidget *statusTabs() const { return m_statusTabs; }

    QPushButton *checkCurrentButton() const { return m_checkCurrentButton; }
    QPushButton *checkSelectedButton() const { return m_checkSelectedButton; }
    QPushButton *checkAllButton() const { return m_checkAllButton; }
    QPushButton *downloadSelectedButton() const { return m_downloadSelectedButton; }
    QPushButton *cancelButton() const { return m_cancelButton; }
    QPushButton *retryButton() const { return m_retryButton; }
    QPushButton *openFolderButton() const { return m_openFolderButton; }
    QPushButton *clearCompletedButton() const { return m_clearCompletedButton; }
    QPushButton *toggleCurrentTabButton() const { return m_toggleCurrentTabButton; }

    QWidget *defaultCardsContainer() const { return m_defaultCardsContainer; }
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
    void toggleCurrentTabSelection();
    void placeCardInCategory(const QString &filePath, const QString &category, bool deferSort = false);
    void sortCardsInCategory(const QString &category);
    void sortAllCards();
    void removeCard(const QString &filePath);

signals:
    void cardSelectionChanged();
    void sourceRequested(const QString &filePath);
    void civitaiRequested(const QString &filePath);
    void downloadRequested(const QString &filePath);
    void ignoreToggled(const QString &filePath);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateSelectionSummary(int selectedCurrent, int currentTotal, int selectedTotal);

    Ui::DownloadsPage *ui = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_selectedLabel = nullptr;
    QLabel *m_filterLabel = nullptr;
    QComboBox *m_filterCombo = nullptr;
    QTabWidget *m_statusTabs = nullptr;

    QPushButton *m_checkCurrentButton = nullptr;
    QPushButton *m_checkSelectedButton = nullptr;
    QPushButton *m_checkAllButton = nullptr;
    QPushButton *m_downloadSelectedButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_retryButton = nullptr;
    QPushButton *m_openFolderButton = nullptr;
    QPushButton *m_clearCompletedButton = nullptr;
    QPushButton *m_toggleCurrentTabButton = nullptr;

    QWidget *m_defaultCardsContainer = nullptr;
    QScrollArea *m_scrollUpdates = nullptr;
    QScrollArea *m_scrollCoexisting = nullptr;
    QScrollArea *m_scrollIgnored = nullptr;
    QScrollArea *m_scrollLatest = nullptr;
    QScrollArea *m_scrollErrors = nullptr;
    QScrollArea *m_scrollLocal = nullptr;

    QVBoxLayout *m_layoutUpdates = nullptr;
    QVBoxLayout *m_layoutCoexisting = nullptr;
    QVBoxLayout *m_layoutIgnored = nullptr;
    QVBoxLayout *m_layoutLatest = nullptr;
    QVBoxLayout *m_layoutErrors = nullptr;
    QVBoxLayout *m_layoutLocal = nullptr;
    QHash<QString, DownloadCardWidgets> m_cards;
};

#endif // DOWNLOADSPAGE_WIDGET_H
