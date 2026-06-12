#include "settingspage.h"
#include "ui_settingspage.h"

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

SettingsPage::SettingsPage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::SettingsPage)
{
    ui->setupUi(this);

    if (ui->btnBrowseLora) connect(ui->btnBrowseLora, &QPushButton::clicked, this, &SettingsPage::loraPathsEditRequested);
    if (ui->btnBrowseGallery) connect(ui->btnBrowseGallery, &QPushButton::clicked, this, &SettingsPage::galleryPathsEditRequested);
    if (ui->btnBrowseTrans) connect(ui->btnBrowseTrans, &QPushButton::clicked, this, &SettingsPage::translationPathsEditRequested);
    if (ui->btnClearGalleryCache) connect(ui->btnClearGalleryCache, &QPushButton::clicked, this, &SettingsPage::clearGalleryCacheRequested);

    if (ui->chkRecursiveLora) connect(ui->chkRecursiveLora, &QCheckBox::toggled, this, &SettingsPage::emitStateChanged);
    if (ui->chkRecursiveGallery) connect(ui->chkRecursiveGallery, &QCheckBox::toggled, this, &SettingsPage::emitStateChanged);
    if (ui->sliderBlur) {
        connect(ui->sliderBlur, &QSlider::valueChanged, this, [this](int value) {
            if (m_updating) return;
            setBlurValue(value);
            emit blurChanged(value, false);
        });
        connect(ui->sliderBlur, &QSlider::sliderReleased, this, [this]() {
            if (ui->sliderBlur) emit blurChanged(ui->sliderBlur->value(), true);
        });
    }
    if (ui->chkDownscaleBlur) connect(ui->chkDownscaleBlur, &QCheckBox::toggled, this, [this]() {
        updateDependentControls();
        emitStateChanged();
    });
    if (ui->spinBlurWidth) connect(ui->spinBlurWidth, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsPage::emitStateChanged);
    if (ui->chkFilterNSFW) connect(ui->chkFilterNSFW, &QCheckBox::toggled, this, [this]() {
        updateDependentControls();
        emitStateChanged();
    });
    if (ui->radioNSFW_Hide) connect(ui->radioNSFW_Hide, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) emitStateChanged();
    });
    if (ui->radioNSFW_Blur) connect(ui->radioNSFW_Blur, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) emitStateChanged();
    });
    if (ui->spinNSFWLevel) connect(ui->spinNSFWLevel, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsPage::emitStateChanged);
    if (ui->spinRenderThreads) connect(ui->spinRenderThreads, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsPage::emitStateChanged);
    if (ui->chkRestoreTreeState) connect(ui->chkRestoreTreeState, &QCheckBox::toggled, this, &SettingsPage::emitStateChanged);
    if (ui->chkSplitOnNewline) connect(ui->chkSplitOnNewline, &QCheckBox::toggled, this, &SettingsPage::emitStateChanged);
    if (ui->editFilterTags) connect(ui->editFilterTags, &QLineEdit::editingFinished, this, &SettingsPage::emitStateChanged);
    if (ui->btnResetFilterTags) connect(ui->btnResetFilterTags, &QPushButton::clicked, this, &SettingsPage::resetFilterTagsRequested);
    if (ui->chkShowEmptyCollections) connect(ui->chkShowEmptyCollections, &QCheckBox::toggled, this, &SettingsPage::emitStateChanged);
    if (ui->chkCollectionFolderTopLevel) connect(ui->chkCollectionFolderTopLevel, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && ui->chkCollectionFolderSecondLevel) {
            QSignalBlocker blocker(ui->chkCollectionFolderSecondLevel);
            ui->chkCollectionFolderSecondLevel->setChecked(false);
        }
        emitStateChanged();
    });
    if (ui->chkCollectionFolderSecondLevel) connect(ui->chkCollectionFolderSecondLevel, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && ui->chkCollectionFolderTopLevel) {
            QSignalBlocker blocker(ui->chkCollectionFolderTopLevel);
            ui->chkCollectionFolderTopLevel->setChecked(false);
        }
        emitStateChanged();
    });
    if (ui->chkModelListFolderGrouping) connect(ui->chkModelListFolderGrouping, &QCheckBox::toggled, this, &SettingsPage::emitStateChanged);
    if (ui->chkUseCustomUserAgent) connect(ui->chkUseCustomUserAgent, &QCheckBox::toggled, this, [this](bool checked) {
        updateDependentControls();
        if (checked && ui->editUserAgent && ui->editUserAgent->text().trimmed().isEmpty()) {
            emit randomUserAgentRequested();
            return;
        }
        emitStateChanged();
    });
    if (ui->editUserAgent) connect(ui->editUserAgent, &QLineEdit::editingFinished, this, &SettingsPage::emitStateChanged);
    if (ui->btnResetUA) connect(ui->btnResetUA, &QPushButton::clicked, this, &SettingsPage::randomUserAgentRequested);
    if (ui->chkUseCivitaiName) connect(ui->chkUseCivitaiName, &QCheckBox::toggled, this, &SettingsPage::emitStateChanged);
    if (ui->chkSuppressLocalWarnings) connect(ui->chkSuppressLocalWarnings, &QCheckBox::toggled, this, &SettingsPage::emitStateChanged);
    if (ui->chkAutoCheckUpdatesOnStartup) connect(ui->chkAutoCheckUpdatesOnStartup, &QCheckBox::toggled, this, &SettingsPage::emitStateChanged);
    if (ui->editCivitaiApiKey) connect(ui->editCivitaiApiKey, &QLineEdit::editingFinished, this, [this]() {
        setCivitaiApiStatus(ui->editCivitaiApiKey && ui->editCivitaiApiKey->text().trimmed().isEmpty() ? "API Key 未配置" : "API Key 未测试");
        emitStateChanged();
    });
    if (ui->btnToggleCivitaiApiKey) connect(ui->btnToggleCivitaiApiKey, &QPushButton::clicked, this, [this]() {
        if (!ui->editCivitaiApiKey || !ui->btnToggleCivitaiApiKey) return;
        const bool showing = ui->editCivitaiApiKey->echoMode() == QLineEdit::Normal;
        ui->editCivitaiApiKey->setEchoMode(showing ? QLineEdit::Password : QLineEdit::Normal);
        ui->btnToggleCivitaiApiKey->setText(showing ? "显示" : "隐藏");
    });
    if (ui->btnTestCivitaiApiKey) connect(ui->btnTestCivitaiApiKey, &QPushButton::clicked, this, [this]() {
        emit testCivitaiApiKeyRequested(state().civitaiApiKey);
    });
    if (ui->comboModelUpdateDownloadPolicy) connect(ui->comboModelUpdateDownloadPolicy, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SettingsPage::emitStateChanged);
    if (ui->comboUserGalleryMatchMode) connect(ui->comboUserGalleryMatchMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SettingsPage::emitStateChanged);
    if (ui->spinUiScale) connect(ui->spinUiScale, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &SettingsPage::emitStateChanged);
}

SettingsPage::~SettingsPage()
{
    delete ui;
}

SettingsState SettingsPage::state() const
{
    SettingsState s;
    s.loraRecursive = ui->chkRecursiveLora && ui->chkRecursiveLora->isChecked();
    s.galleryRecursive = ui->chkRecursiveGallery && ui->chkRecursiveGallery->isChecked();
    s.blurRadius = ui->sliderBlur ? ui->sliderBlur->value() : 30;
    s.downscaleBlur = !ui->chkDownscaleBlur || ui->chkDownscaleBlur->isChecked();
    s.blurProcessWidth = ui->spinBlurWidth ? ui->spinBlurWidth->value() : 500;
    s.filterNSFW = ui->chkFilterNSFW && ui->chkFilterNSFW->isChecked();
    s.nsfwMode = (ui->radioNSFW_Hide && ui->radioNSFW_Hide->isChecked()) ? 0 : 1;
    s.nsfwLevel = ui->spinNSFWLevel ? ui->spinNSFWLevel->value() : 1;
    s.renderThreadCount = ui->spinRenderThreads ? ui->spinRenderThreads->value() : 4;
    s.restoreTreeState = !ui->chkRestoreTreeState || ui->chkRestoreTreeState->isChecked();
    s.splitOnNewline = !ui->chkSplitOnNewline || ui->chkSplitOnNewline->isChecked();
    s.filterTagsText = ui->editFilterTags ? ui->editFilterTags->text() : QString();
    s.showEmptyCollections = ui->chkShowEmptyCollections && ui->chkShowEmptyCollections->isChecked();
    s.collectionFolderTopLevel = ui->chkCollectionFolderTopLevel && ui->chkCollectionFolderTopLevel->isChecked();
    s.collectionFolderSecondLevel = ui->chkCollectionFolderSecondLevel && ui->chkCollectionFolderSecondLevel->isChecked();
    s.modelListFolderGrouping = ui->chkModelListFolderGrouping && ui->chkModelListFolderGrouping->isChecked();
    s.useCustomUserAgent = ui->chkUseCustomUserAgent && ui->chkUseCustomUserAgent->isChecked();
    s.customUserAgent = ui->editUserAgent ? ui->editUserAgent->text().trimmed() : QString();
    s.civitaiApiKey = ui->editCivitaiApiKey ? ui->editCivitaiApiKey->text().trimmed() : QString();
    s.modelUpdateDownloadPolicy = ui->comboModelUpdateDownloadPolicy ? ui->comboModelUpdateDownloadPolicy->currentIndex() : 0;
    s.autoCheckUpdatesOnStartup = !ui->chkAutoCheckUpdatesOnStartup || ui->chkAutoCheckUpdatesOnStartup->isChecked();
    s.useCivitaiName = ui->chkUseCivitaiName && ui->chkUseCivitaiName->isChecked();
    s.suppressLocalWarnings = ui->chkSuppressLocalWarnings && ui->chkSuppressLocalWarnings->isChecked();
    s.userGalleryMatchMode = ui->comboUserGalleryMatchMode ? ui->comboUserGalleryMatchMode->currentIndex() : 0;
    s.uiScale = ui->spinUiScale ? ui->spinUiScale->value() : 1.0;
    return s;
}

void SettingsPage::setState(const SettingsState &state)
{
    m_updating = true;
    const auto guard = qScopeGuard([this]() {
        m_updating = false;
        updateDependentControls();
    });

    if (ui->chkRecursiveLora) ui->chkRecursiveLora->setChecked(state.loraRecursive);
    if (ui->chkRecursiveGallery) ui->chkRecursiveGallery->setChecked(state.galleryRecursive);
    if (ui->sliderBlur) ui->sliderBlur->setValue(state.blurRadius);
    setBlurValue(state.blurRadius);
    if (ui->chkDownscaleBlur) ui->chkDownscaleBlur->setChecked(state.downscaleBlur);
    if (ui->spinBlurWidth) ui->spinBlurWidth->setValue(state.blurProcessWidth);
    if (ui->chkFilterNSFW) ui->chkFilterNSFW->setChecked(state.filterNSFW);
    if (ui->radioNSFW_Hide) ui->radioNSFW_Hide->setChecked(state.nsfwMode == 0);
    if (ui->radioNSFW_Blur) ui->radioNSFW_Blur->setChecked(state.nsfwMode != 0);
    if (ui->spinNSFWLevel) ui->spinNSFWLevel->setValue(state.nsfwLevel);
    if (ui->spinRenderThreads) ui->spinRenderThreads->setValue(state.renderThreadCount);
    if (ui->chkRestoreTreeState) ui->chkRestoreTreeState->setChecked(state.restoreTreeState);
    if (ui->chkSplitOnNewline) ui->chkSplitOnNewline->setChecked(state.splitOnNewline);
    if (ui->editFilterTags) ui->editFilterTags->setText(state.filterTagsText);
    if (ui->chkShowEmptyCollections) ui->chkShowEmptyCollections->setChecked(state.showEmptyCollections);
    if (ui->chkCollectionFolderTopLevel) ui->chkCollectionFolderTopLevel->setChecked(state.collectionFolderTopLevel);
    if (ui->chkCollectionFolderSecondLevel) ui->chkCollectionFolderSecondLevel->setChecked(state.collectionFolderSecondLevel);
    if (ui->chkModelListFolderGrouping) ui->chkModelListFolderGrouping->setChecked(state.modelListFolderGrouping);
    if (ui->chkUseCustomUserAgent) ui->chkUseCustomUserAgent->setChecked(state.useCustomUserAgent);
    if (ui->editUserAgent) ui->editUserAgent->setText(state.customUserAgent);
    if (ui->editCivitaiApiKey) ui->editCivitaiApiKey->setText(state.civitaiApiKey);
    if (ui->comboModelUpdateDownloadPolicy) ui->comboModelUpdateDownloadPolicy->setCurrentIndex(qBound(0, state.modelUpdateDownloadPolicy, 2));
    if (ui->chkAutoCheckUpdatesOnStartup) ui->chkAutoCheckUpdatesOnStartup->setChecked(state.autoCheckUpdatesOnStartup);
    if (ui->chkUseCivitaiName) ui->chkUseCivitaiName->setChecked(state.useCivitaiName);
    if (ui->chkSuppressLocalWarnings) ui->chkSuppressLocalWarnings->setChecked(state.suppressLocalWarnings);
    if (ui->comboUserGalleryMatchMode) ui->comboUserGalleryMatchMode->setCurrentIndex(qBound(0, state.userGalleryMatchMode, 2));
    if (ui->spinUiScale) ui->spinUiScale->setValue(state.uiScale);
}

void SettingsPage::setPathSummaries(const QString &lora, const QString &gallery, const QString &translation)
{
    if (ui->editLoraPath) ui->editLoraPath->setText(lora);
    if (ui->editGalleryPath) ui->editGalleryPath->setText(gallery);
    if (ui->editTransPath) ui->editTransPath->setText(translation);
}

void SettingsPage::setCivitaiApiStatus(const QString &text)
{
    if (ui->lblCivitaiApiStatus) ui->lblCivitaiApiStatus->setText(text);
}

void SettingsPage::setCivitaiApiTesting(bool testing)
{
    if (ui->btnTestCivitaiApiKey) ui->btnTestCivitaiApiKey->setEnabled(!testing);
}

void SettingsPage::setBlurValue(int value)
{
    if (ui->lblBlurValue) ui->lblBlurValue->setText(QString::number(value) + "px");
}

void SettingsPage::setFilterTagsText(const QString &text)
{
    if (!ui->editFilterTags) return;
    QSignalBlocker blocker(ui->editFilterTags);
    ui->editFilterTags->setText(text);
}

void SettingsPage::setUserAgentText(const QString &text)
{
    if (!ui->editUserAgent) return;
    QSignalBlocker blocker(ui->editUserAgent);
    ui->editUserAgent->setText(text);
}

void SettingsPage::focusTranslationPath()
{
    if (ui->editTransPath) ui->editTransPath->setFocus();
}

void SettingsPage::emitStateChanged()
{
    if (m_updating) return;
    emit stateChanged(state());
}

void SettingsPage::updateDependentControls()
{
    const bool downscale = ui->chkDownscaleBlur && ui->chkDownscaleBlur->isChecked();
    if (ui->spinBlurWidth) ui->spinBlurWidth->setEnabled(downscale);

    const bool nsfwEnabled = ui->chkFilterNSFW && ui->chkFilterNSFW->isChecked();
    if (ui->radioNSFW_Hide) ui->radioNSFW_Hide->setEnabled(nsfwEnabled);
    if (ui->radioNSFW_Blur) ui->radioNSFW_Blur->setEnabled(nsfwEnabled);
    if (ui->spinNSFWLevel) ui->spinNSFWLevel->setEnabled(nsfwEnabled);

    const bool customUa = ui->chkUseCustomUserAgent && ui->chkUseCustomUserAgent->isChecked();
    if (ui->editUserAgent) ui->editUserAgent->setEnabled(customUa);
}
