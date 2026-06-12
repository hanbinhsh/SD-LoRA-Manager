#ifndef SETTINGSPAGE_WIDGET_H
#define SETTINGSPAGE_WIDGET_H

#include <QString>
#include <QStringList>
#include <QWidget>

class QJsonObject;

namespace Ui {
class SettingsPage;
}

struct SettingsState {
    bool loraRecursive = false;
    bool galleryRecursive = false;
    int blurRadius = 30;
    bool downscaleBlur = true;
    int blurProcessWidth = 500;
    bool filterNSFW = false;
    int nsfwMode = 1;
    int nsfwLevel = 1;
    int renderThreadCount = 4;
    bool restoreTreeState = true;
    bool splitOnNewline = true;
    QString filterTagsText;
    bool showEmptyCollections = false;
    bool collectionFolderTopLevel = false;
    bool collectionFolderSecondLevel = false;
    bool modelListFolderGrouping = false;
    bool useCustomUserAgent = false;
    QString customUserAgent;
    QString civitaiApiKey;
    int modelUpdateDownloadPolicy = 0;
    bool autoCheckUpdatesOnStartup = true;
    bool useCivitaiName = false;
    bool suppressLocalWarnings = false;
    int userGalleryMatchMode = 0;
    double uiScale = 1.0;

    static SettingsState fromJson(const QJsonObject &root, const QString &defaultFilterTags);
    void writeToJson(QJsonObject &root) const;
    void normalize();
    QStringList filterTags() const;
};

class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPage(QWidget *parent = nullptr);
    ~SettingsPage() override;

    QWidget *widget() { return this; }

    SettingsState state() const;
    void setState(const SettingsState &state);
    void setPathSummaries(const QString &lora, const QString &gallery, const QString &translation);
    void setCivitaiApiStatus(const QString &text);
    void setCivitaiApiTesting(bool testing);
    void setBlurValue(int value);
    void setFilterTagsText(const QString &text);
    void setUserAgentText(const QString &text);
    void focusTranslationPath();

signals:
    void loraPathsEditRequested();
    void galleryPathsEditRequested();
    void translationPathsEditRequested();
    void clearGalleryCacheRequested();
    void testCivitaiApiKeyRequested(const QString &key);
    void stateChanged(const SettingsState &state);
    void blurChanged(int value, bool finalSave);
    void resetFilterTagsRequested();
    void randomUserAgentRequested();

private:
    void emitStateChanged();
    void updateDependentControls();

    Ui::SettingsPage *ui = nullptr;
    bool m_updating = false;
};

#endif // SETTINGSPAGE_WIDGET_H
