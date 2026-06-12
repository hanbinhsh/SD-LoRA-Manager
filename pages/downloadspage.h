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

    QPushButton *checkCurrentButton() const;
    QPushButton *checkSelectedButton() const;
    QPushButton *checkAllButton() const;
    QPushButton *downloadSelectedButton() const;
    QPushButton *cancelButton() const;
    QPushButton *retryButton() const;
    QPushButton *openFolderButton() const;
    QPushButton *clearCompletedButton() const;
    QPushButton *toggleCurrentTabButton() const;

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
    QHash<QString, DownloadCardWidgets> m_cards;
};

#endif // DOWNLOADSPAGE_WIDGET_H
