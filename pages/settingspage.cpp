#include "settingspage.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QWidget>

template <typename T>
T *SettingsPage::child(const char *objectName) const
{
    return m_root ? m_root->findChild<T *>(QString::fromLatin1(objectName)) : nullptr;
}

SettingsPage::SettingsPage(QWidget *root, QObject *parent)
    : QObject(parent)
    , m_root(root)
{
    m_editLoraPath = child<QLineEdit>("editLoraPath");
    m_editGalleryPath = child<QLineEdit>("editGalleryPath");
    m_editTransPath = child<QLineEdit>("editTransPath");
    m_btnBrowseLora = child<QPushButton>("btnBrowseLora");
    m_btnBrowseGallery = child<QPushButton>("btnBrowseGallery");
    m_btnBrowseTrans = child<QPushButton>("btnBrowseTrans");
    m_btnClearGalleryCache = child<QPushButton>("btnClearGalleryCache");

    m_chkRecursiveLora = child<QCheckBox>("chkRecursiveLora");
    m_chkRecursiveGallery = child<QCheckBox>("chkRecursiveGallery");
    m_sliderBlur = child<QSlider>("sliderBlur");
    m_lblBlurValue = child<QLabel>("lblBlurValue");
    m_chkDownscaleBlur = child<QCheckBox>("chkDownscaleBlur");
    m_spinBlurWidth = child<QSpinBox>("spinBlurWidth");
    m_chkFilterNSFW = child<QCheckBox>("chkFilterNSFW");
    m_radioNSFWHide = child<QRadioButton>("radioNSFW_Hide");
    m_radioNSFWBlur = child<QRadioButton>("radioNSFW_Blur");
    m_spinNSFWLevel = child<QSpinBox>("spinNSFWLevel");
    m_spinRenderThreads = child<QSpinBox>("spinRenderThreads");
    m_chkRestoreTreeState = child<QCheckBox>("chkRestoreTreeState");
    m_chkSplitOnNewline = child<QCheckBox>("chkSplitOnNewline");
    m_editFilterTags = child<QLineEdit>("editFilterTags");
    m_btnResetFilterTags = child<QPushButton>("btnResetFilterTags");
    m_chkShowEmptyCollections = child<QCheckBox>("chkShowEmptyCollections");
    m_chkCollectionFolderTopLevel = child<QCheckBox>("chkCollectionFolderTopLevel");
    m_chkCollectionFolderSecondLevel = child<QCheckBox>("chkCollectionFolderSecondLevel");
    m_chkModelListFolderGrouping = child<QCheckBox>("chkModelListFolderGrouping");
    m_chkUseCustomUserAgent = child<QCheckBox>("chkUseCustomUserAgent");
    m_editUserAgent = child<QLineEdit>("editUserAgent");
    m_btnResetUA = child<QPushButton>("btnResetUA");
    m_chkUseCivitaiName = child<QCheckBox>("chkUseCivitaiName");
    m_chkSuppressLocalWarnings = child<QCheckBox>("chkSuppressLocalWarnings");
    m_chkAutoCheckUpdatesOnStartup = child<QCheckBox>("chkAutoCheckUpdatesOnStartup");
    m_editCivitaiApiKey = child<QLineEdit>("editCivitaiApiKey");
    m_btnToggleCivitaiApiKey = child<QPushButton>("btnToggleCivitaiApiKey");
    m_btnTestCivitaiApiKey = child<QPushButton>("btnTestCivitaiApiKey");
    m_lblCivitaiApiStatus = child<QLabel>("lblCivitaiApiStatus");
    m_comboModelUpdateDownloadPolicy = child<QComboBox>("comboModelUpdateDownloadPolicy");
    m_comboUserGalleryMatchMode = child<QComboBox>("comboUserGalleryMatchMode");
    m_spinUiScale = child<QDoubleSpinBox>("spinUiScale");

    if (m_btnBrowseLora) connect(m_btnBrowseLora, &QPushButton::clicked, this, &SettingsPage::loraPathsEditRequested);
    if (m_btnBrowseGallery) connect(m_btnBrowseGallery, &QPushButton::clicked, this, &SettingsPage::galleryPathsEditRequested);
    if (m_btnBrowseTrans) connect(m_btnBrowseTrans, &QPushButton::clicked, this, &SettingsPage::translationPathsEditRequested);
    if (m_btnClearGalleryCache) connect(m_btnClearGalleryCache, &QPushButton::clicked, this, &SettingsPage::clearGalleryCacheRequested);

    if (m_chkRecursiveLora) connect(m_chkRecursiveLora, &QCheckBox::toggled, this, &SettingsPage::emitStateChanged);
    if (m_chkRecursiveGallery) connect(m_chkRecursiveGallery, &QCheckBox::toggled, this, &SettingsPage::emitStateChanged);
    if (m_sliderBlur) {
        connect(m_sliderBlur, &QSlider::valueChanged, this, [this](int value) {
            if (m_updating) return;
            setBlurValue(value);
            emit blurChanged(value, false);
        });
        connect(m_sliderBlur, &QSlider::sliderReleased, this, [this]() {
            if (m_sliderBlur) emit blurChanged(m_sliderBlur->value(), true);
        });
    }
    if (m_chkDownscaleBlur) connect(m_chkDownscaleBlur, &QCheckBox::toggled, this, [this]() {
        updateDependentControls();
        emitStateChanged();
    });
    if (m_spinBlurWidth) connect(m_spinBlurWidth, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsPage::emitStateChanged);
    if (m_chkFilterNSFW) connect(m_chkFilterNSFW, &QCheckBox::toggled, this, [this]() {
        updateDependentControls();
        emitStateChanged();
    });
    if (m_radioNSFWHide) connect(m_radioNSFWHide, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) emitStateChanged();
    });
    if (m_radioNSFWBlur) connect(m_radioNSFWBlur, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) emitStateChanged();
    });
    if (m_spinNSFWLevel) connect(m_spinNSFWLevel, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsPage::emitStateChanged);
    if (m_spinRenderThreads) connect(m_spinRenderThreads, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsPage::emitStateChanged);
    if (m_chkRestoreTreeState) connect(m_chkRestoreTreeState, &QCheckBox::toggled, this, &SettingsPage::emitStateChanged);
    if (m_chkSplitOnNewline) connect(m_chkSplitOnNewline, &QCheckBox::toggled, this, &SettingsPage::emitStateChanged);
    if (m_editFilterTags) connect(m_editFilterTags, &QLineEdit::editingFinished, this, &SettingsPage::emitStateChanged);
    if (m_btnResetFilterTags) connect(m_btnResetFilterTags, &QPushButton::clicked, this, &SettingsPage::resetFilterTagsRequested);
    if (m_chkShowEmptyCollections) connect(m_chkShowEmptyCollections, &QCheckBox::toggled, this, &SettingsPage::emitStateChanged);
    if (m_chkCollectionFolderTopLevel) connect(m_chkCollectionFolderTopLevel, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && m_chkCollectionFolderSecondLevel) {
            QSignalBlocker blocker(m_chkCollectionFolderSecondLevel);
            m_chkCollectionFolderSecondLevel->setChecked(false);
        }
        emitStateChanged();
    });
    if (m_chkCollectionFolderSecondLevel) connect(m_chkCollectionFolderSecondLevel, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && m_chkCollectionFolderTopLevel) {
            QSignalBlocker blocker(m_chkCollectionFolderTopLevel);
            m_chkCollectionFolderTopLevel->setChecked(false);
        }
        emitStateChanged();
    });
    if (m_chkModelListFolderGrouping) connect(m_chkModelListFolderGrouping, &QCheckBox::toggled, this, &SettingsPage::emitStateChanged);
    if (m_chkUseCustomUserAgent) connect(m_chkUseCustomUserAgent, &QCheckBox::toggled, this, [this](bool checked) {
        updateDependentControls();
        if (checked && m_editUserAgent && m_editUserAgent->text().trimmed().isEmpty()) {
            emit randomUserAgentRequested();
            return;
        }
        emitStateChanged();
    });
    if (m_editUserAgent) connect(m_editUserAgent, &QLineEdit::editingFinished, this, &SettingsPage::emitStateChanged);
    if (m_btnResetUA) connect(m_btnResetUA, &QPushButton::clicked, this, &SettingsPage::randomUserAgentRequested);
    if (m_chkUseCivitaiName) connect(m_chkUseCivitaiName, &QCheckBox::toggled, this, &SettingsPage::emitStateChanged);
    if (m_chkSuppressLocalWarnings) connect(m_chkSuppressLocalWarnings, &QCheckBox::toggled, this, &SettingsPage::emitStateChanged);
    if (m_chkAutoCheckUpdatesOnStartup) connect(m_chkAutoCheckUpdatesOnStartup, &QCheckBox::toggled, this, &SettingsPage::emitStateChanged);
    if (m_editCivitaiApiKey) connect(m_editCivitaiApiKey, &QLineEdit::editingFinished, this, [this]() {
        setCivitaiApiStatus(m_editCivitaiApiKey && m_editCivitaiApiKey->text().trimmed().isEmpty() ? "API Key 未配置" : "API Key 未测试");
        emitStateChanged();
    });
    if (m_btnToggleCivitaiApiKey) connect(m_btnToggleCivitaiApiKey, &QPushButton::clicked, this, [this]() {
        if (!m_editCivitaiApiKey || !m_btnToggleCivitaiApiKey) return;
        const bool showing = m_editCivitaiApiKey->echoMode() == QLineEdit::Normal;
        m_editCivitaiApiKey->setEchoMode(showing ? QLineEdit::Password : QLineEdit::Normal);
        m_btnToggleCivitaiApiKey->setText(showing ? "显示" : "隐藏");
    });
    if (m_btnTestCivitaiApiKey) connect(m_btnTestCivitaiApiKey, &QPushButton::clicked, this, [this]() {
        emit testCivitaiApiKeyRequested(state().civitaiApiKey);
    });
    if (m_comboModelUpdateDownloadPolicy) connect(m_comboModelUpdateDownloadPolicy, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SettingsPage::emitStateChanged);
    if (m_comboUserGalleryMatchMode) connect(m_comboUserGalleryMatchMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SettingsPage::emitStateChanged);
    if (m_spinUiScale) connect(m_spinUiScale, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &SettingsPage::emitStateChanged);
}

SettingsState SettingsPage::state() const
{
    SettingsState s;
    s.loraRecursive = m_chkRecursiveLora && m_chkRecursiveLora->isChecked();
    s.galleryRecursive = m_chkRecursiveGallery && m_chkRecursiveGallery->isChecked();
    s.blurRadius = m_sliderBlur ? m_sliderBlur->value() : 30;
    s.downscaleBlur = !m_chkDownscaleBlur || m_chkDownscaleBlur->isChecked();
    s.blurProcessWidth = m_spinBlurWidth ? m_spinBlurWidth->value() : 500;
    s.filterNSFW = m_chkFilterNSFW && m_chkFilterNSFW->isChecked();
    s.nsfwMode = (m_radioNSFWHide && m_radioNSFWHide->isChecked()) ? 0 : 1;
    s.nsfwLevel = m_spinNSFWLevel ? m_spinNSFWLevel->value() : 1;
    s.renderThreadCount = m_spinRenderThreads ? m_spinRenderThreads->value() : 4;
    s.restoreTreeState = !m_chkRestoreTreeState || m_chkRestoreTreeState->isChecked();
    s.splitOnNewline = !m_chkSplitOnNewline || m_chkSplitOnNewline->isChecked();
    s.filterTagsText = m_editFilterTags ? m_editFilterTags->text() : QString();
    s.showEmptyCollections = m_chkShowEmptyCollections && m_chkShowEmptyCollections->isChecked();
    s.collectionFolderTopLevel = m_chkCollectionFolderTopLevel && m_chkCollectionFolderTopLevel->isChecked();
    s.collectionFolderSecondLevel = m_chkCollectionFolderSecondLevel && m_chkCollectionFolderSecondLevel->isChecked();
    s.modelListFolderGrouping = m_chkModelListFolderGrouping && m_chkModelListFolderGrouping->isChecked();
    s.useCustomUserAgent = m_chkUseCustomUserAgent && m_chkUseCustomUserAgent->isChecked();
    s.customUserAgent = m_editUserAgent ? m_editUserAgent->text().trimmed() : QString();
    s.civitaiApiKey = m_editCivitaiApiKey ? m_editCivitaiApiKey->text().trimmed() : QString();
    s.modelUpdateDownloadPolicy = m_comboModelUpdateDownloadPolicy ? m_comboModelUpdateDownloadPolicy->currentIndex() : 0;
    s.autoCheckUpdatesOnStartup = !m_chkAutoCheckUpdatesOnStartup || m_chkAutoCheckUpdatesOnStartup->isChecked();
    s.useCivitaiName = m_chkUseCivitaiName && m_chkUseCivitaiName->isChecked();
    s.suppressLocalWarnings = m_chkSuppressLocalWarnings && m_chkSuppressLocalWarnings->isChecked();
    s.userGalleryMatchMode = m_comboUserGalleryMatchMode ? m_comboUserGalleryMatchMode->currentIndex() : 0;
    s.uiScale = m_spinUiScale ? m_spinUiScale->value() : 1.0;
    return s;
}

void SettingsPage::setState(const SettingsState &state)
{
    m_updating = true;
    const auto guard = qScopeGuard([this]() {
        m_updating = false;
        updateDependentControls();
    });

    if (m_chkRecursiveLora) m_chkRecursiveLora->setChecked(state.loraRecursive);
    if (m_chkRecursiveGallery) m_chkRecursiveGallery->setChecked(state.galleryRecursive);
    if (m_sliderBlur) m_sliderBlur->setValue(state.blurRadius);
    setBlurValue(state.blurRadius);
    if (m_chkDownscaleBlur) m_chkDownscaleBlur->setChecked(state.downscaleBlur);
    if (m_spinBlurWidth) m_spinBlurWidth->setValue(state.blurProcessWidth);
    if (m_chkFilterNSFW) m_chkFilterNSFW->setChecked(state.filterNSFW);
    if (m_radioNSFWHide) m_radioNSFWHide->setChecked(state.nsfwMode == 0);
    if (m_radioNSFWBlur) m_radioNSFWBlur->setChecked(state.nsfwMode != 0);
    if (m_spinNSFWLevel) m_spinNSFWLevel->setValue(state.nsfwLevel);
    if (m_spinRenderThreads) m_spinRenderThreads->setValue(state.renderThreadCount);
    if (m_chkRestoreTreeState) m_chkRestoreTreeState->setChecked(state.restoreTreeState);
    if (m_chkSplitOnNewline) m_chkSplitOnNewline->setChecked(state.splitOnNewline);
    if (m_editFilterTags) m_editFilterTags->setText(state.filterTagsText);
    if (m_chkShowEmptyCollections) m_chkShowEmptyCollections->setChecked(state.showEmptyCollections);
    if (m_chkCollectionFolderTopLevel) m_chkCollectionFolderTopLevel->setChecked(state.collectionFolderTopLevel);
    if (m_chkCollectionFolderSecondLevel) m_chkCollectionFolderSecondLevel->setChecked(state.collectionFolderSecondLevel);
    if (m_chkModelListFolderGrouping) m_chkModelListFolderGrouping->setChecked(state.modelListFolderGrouping);
    if (m_chkUseCustomUserAgent) m_chkUseCustomUserAgent->setChecked(state.useCustomUserAgent);
    if (m_editUserAgent) m_editUserAgent->setText(state.customUserAgent);
    if (m_editCivitaiApiKey) m_editCivitaiApiKey->setText(state.civitaiApiKey);
    if (m_comboModelUpdateDownloadPolicy) m_comboModelUpdateDownloadPolicy->setCurrentIndex(qBound(0, state.modelUpdateDownloadPolicy, 2));
    if (m_chkAutoCheckUpdatesOnStartup) m_chkAutoCheckUpdatesOnStartup->setChecked(state.autoCheckUpdatesOnStartup);
    if (m_chkUseCivitaiName) m_chkUseCivitaiName->setChecked(state.useCivitaiName);
    if (m_chkSuppressLocalWarnings) m_chkSuppressLocalWarnings->setChecked(state.suppressLocalWarnings);
    if (m_comboUserGalleryMatchMode) m_comboUserGalleryMatchMode->setCurrentIndex(qBound(0, state.userGalleryMatchMode, 2));
    if (m_spinUiScale) m_spinUiScale->setValue(state.uiScale);
}

void SettingsPage::setPathSummaries(const QString &lora, const QString &gallery, const QString &translation)
{
    if (m_editLoraPath) m_editLoraPath->setText(lora);
    if (m_editGalleryPath) m_editGalleryPath->setText(gallery);
    if (m_editTransPath) m_editTransPath->setText(translation);
}

void SettingsPage::setCivitaiApiStatus(const QString &text)
{
    if (m_lblCivitaiApiStatus) m_lblCivitaiApiStatus->setText(text);
}

void SettingsPage::setCivitaiApiTesting(bool testing)
{
    if (m_btnTestCivitaiApiKey) m_btnTestCivitaiApiKey->setEnabled(!testing);
}

void SettingsPage::setBlurValue(int value)
{
    if (m_lblBlurValue) m_lblBlurValue->setText(QString::number(value) + "px");
}

void SettingsPage::setFilterTagsText(const QString &text)
{
    if (!m_editFilterTags) return;
    QSignalBlocker blocker(m_editFilterTags);
    m_editFilterTags->setText(text);
}

void SettingsPage::setUserAgentText(const QString &text)
{
    if (!m_editUserAgent) return;
    QSignalBlocker blocker(m_editUserAgent);
    m_editUserAgent->setText(text);
}

void SettingsPage::focusTranslationPath()
{
    if (m_editTransPath) m_editTransPath->setFocus();
}

void SettingsPage::emitStateChanged()
{
    if (m_updating) return;
    emit stateChanged(state());
}

void SettingsPage::updateDependentControls()
{
    const bool downscale = m_chkDownscaleBlur && m_chkDownscaleBlur->isChecked();
    if (m_spinBlurWidth) m_spinBlurWidth->setEnabled(downscale);

    const bool nsfwEnabled = m_chkFilterNSFW && m_chkFilterNSFW->isChecked();
    if (m_radioNSFWHide) m_radioNSFWHide->setEnabled(nsfwEnabled);
    if (m_radioNSFWBlur) m_radioNSFWBlur->setEnabled(nsfwEnabled);
    if (m_spinNSFWLevel) m_spinNSFWLevel->setEnabled(nsfwEnabled);

    const bool customUa = m_chkUseCustomUserAgent && m_chkUseCustomUserAgent->isChecked();
    if (m_editUserAgent) m_editUserAgent->setEnabled(customUa);
}
