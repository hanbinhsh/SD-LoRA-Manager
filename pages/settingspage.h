#ifndef SETTINGSPAGE_H
#define SETTINGSPAGE_H

#include <QObject>
#include <QString>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSlider;
class QSpinBox;
class QWidget;

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
};

class SettingsPage : public QObject
{
    Q_OBJECT

public:
    explicit SettingsPage(QWidget *root, QObject *parent = nullptr);

    QWidget *widget() const { return m_root; }

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
    template <typename T>
    T *child(const char *objectName) const;

    void emitStateChanged();
    void updateDependentControls();

    QWidget *m_root = nullptr;
    bool m_updating = false;

    QLineEdit *m_editLoraPath = nullptr;
    QLineEdit *m_editGalleryPath = nullptr;
    QLineEdit *m_editTransPath = nullptr;
    QPushButton *m_btnBrowseLora = nullptr;
    QPushButton *m_btnBrowseGallery = nullptr;
    QPushButton *m_btnBrowseTrans = nullptr;
    QPushButton *m_btnClearGalleryCache = nullptr;

    QCheckBox *m_chkRecursiveLora = nullptr;
    QCheckBox *m_chkRecursiveGallery = nullptr;
    QSlider *m_sliderBlur = nullptr;
    QLabel *m_lblBlurValue = nullptr;
    QCheckBox *m_chkDownscaleBlur = nullptr;
    QSpinBox *m_spinBlurWidth = nullptr;
    QCheckBox *m_chkFilterNSFW = nullptr;
    QRadioButton *m_radioNSFWHide = nullptr;
    QRadioButton *m_radioNSFWBlur = nullptr;
    QSpinBox *m_spinNSFWLevel = nullptr;
    QSpinBox *m_spinRenderThreads = nullptr;
    QCheckBox *m_chkRestoreTreeState = nullptr;
    QCheckBox *m_chkSplitOnNewline = nullptr;
    QLineEdit *m_editFilterTags = nullptr;
    QPushButton *m_btnResetFilterTags = nullptr;
    QCheckBox *m_chkShowEmptyCollections = nullptr;
    QCheckBox *m_chkCollectionFolderTopLevel = nullptr;
    QCheckBox *m_chkCollectionFolderSecondLevel = nullptr;
    QCheckBox *m_chkModelListFolderGrouping = nullptr;
    QCheckBox *m_chkUseCustomUserAgent = nullptr;
    QLineEdit *m_editUserAgent = nullptr;
    QPushButton *m_btnResetUA = nullptr;
    QCheckBox *m_chkUseCivitaiName = nullptr;
    QCheckBox *m_chkSuppressLocalWarnings = nullptr;
    QCheckBox *m_chkAutoCheckUpdatesOnStartup = nullptr;
    QLineEdit *m_editCivitaiApiKey = nullptr;
    QPushButton *m_btnToggleCivitaiApiKey = nullptr;
    QPushButton *m_btnTestCivitaiApiKey = nullptr;
    QLabel *m_lblCivitaiApiStatus = nullptr;
    QComboBox *m_comboModelUpdateDownloadPolicy = nullptr;
    QComboBox *m_comboUserGalleryMatchMode = nullptr;
    QDoubleSpinBox *m_spinUiScale = nullptr;
};

#endif // SETTINGSPAGE_H
