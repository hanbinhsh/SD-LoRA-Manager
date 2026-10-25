#include "utils/applanguage.h"
#include "utils/downloadstatus.h"
#include "pages/settingspage.h"
#include "pages/downloadspage.h"
#include "ui_mainwindow.h"
#include "ui_promptparserwidget.h"
#include "ui_prompttemplatelibrarywidget.h"
#include "ui_llmpromptwidget.h"
#include "ui_tagbrowserwidget.h"
#include "ui_syncwidget.h"
#include "ui_usageanalysiswidget.h"
#include "ui_launcherwidget.h"
#include "ui_aboutpage.h"
#include "ui_modelnotedialog.h"
#include "ui_pathlistdialog.h"
#include "ui_imageviewerdialog.h"
#include "ui_launcherimportdialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTabWidget>
#include <QXmlStreamReader>
#include <cstdio>

namespace {
int failures = 0;
void check(bool condition, const char *message)
{
    if (!condition) { ++failures; qCritical() << "FAIL:" << message; }
}

bool hasChinese(const QString &text)
{
    static const QRegularExpression chinese("[\\x{3400}-\\x{9fff}]");
    return text.contains(chinese);
}

template<typename Form, typename Widget = QWidget>
void checkForm()
{
    Widget widget;
    Form form;
    form.setupUi(&widget);
    for (auto *label : widget.template findChildren<QLabel*>())
        check(!hasChinese(label->text()), "English fixed labels do not contain Chinese");
    for (auto *button : widget.template findChildren<QAbstractButton*>())
        check(!hasChinese(button->text()), "English fixed buttons do not contain Chinese");
    for (auto *tabs : widget.template findChildren<QTabWidget*>())
        for (int i = 0; i < tabs->count(); ++i)
            check(!hasChinese(tabs->tabText(i)), "English fixed tab titles do not contain Chinese");
}

void checkAllUiSources()
{
    int checked = 0;
    for (const QString &folder : {"windows", "pages", "tools", "dialogs"}) {
        QDirIterator files(QStringLiteral(TEST_SOURCE_DIR) + "/" + folder, {"*.ui"}, QDir::Files);
        while (files.hasNext()) {
            QFile file(files.next());
            check(file.open(QIODevice::ReadOnly), "read source UI");
            QXmlStreamReader xml(&file);
            QString context;
            while (!xml.atEnd()) {
                xml.readNext();
                if (!xml.isStartElement()) continue;
                if (xml.name() == QLatin1String("class") && context.isEmpty()) context = xml.readElementText();
                else if (xml.name() == QLatin1String("string")) {
                    const bool translate = xml.attributes().value("notr") != QLatin1String("true");
                    const QString source = xml.readElementText();
                    if (!translate || !hasChinese(source)) continue;
                    const QByteArray key = source.toUtf8();
                    const QByteArray owner = context.toUtf8();
                    const QString translated = QCoreApplication::translate(owner.constData(), key.constData());
                    if (hasChinese(translated)) qCritical() << "Untranslated UI:" << file.fileName() << source;
                    check(!hasChinese(translated), "all translatable Chinese UI text has English coverage");
                    ++checked;
                }
            }
            check(!xml.hasError(), "UI remains valid XML");
        }
    }
    check(checked > 700, "coverage includes every UI file, not just primary navigation");
}

void checkSettings()
{
    SettingsState state = SettingsState::fromJson({}, "BREAK");
    check(state.uiLanguage == "zh_CN", "old configs default to Chinese");
    check(!state.imageViewerNewWindow, "old configs reuse the image viewer by default");
    check(state.civitaiBrowserHost == "civitai.red", "old configs default model browser links to civitai.red");
    state.uiLanguage = "en-US";
    state.imageViewerNewWindow = true;
    state.civitaiBrowserHost = "civitai.com";
    QJsonObject config{{"unrelated", 123}};
    state.writeToJson(config);
    check(config.value("ui_language") == "en_US" && config.value("unrelated") == 123,
          "language persists canonically without overwriting other settings");
    check(config.value("civitai_browser_host") == "civitai.com", "model browser preference persists");
    check(config.value("image_viewer_new_window").toBool(), "image viewer mode persists");
    QString changedHost;
    SettingsPage page;
    page.setState(SettingsState::fromJson(config, "BREAK"));
    auto *viewerMode = page.findChild<QComboBox*>("comboImageViewerOpenMode");
    check(viewerMode && viewerMode->currentIndex() == 1 && page.state().imageViewerNewWindow,
          "image viewer mode restores from config");
    bool modeChanged = false;
    const auto modeConnection = QObject::connect(&page, &SettingsPage::stateChanged, &page,
        [&](const SettingsState &changed) { modeChanged = !changed.imageViewerNewWindow; });
    if (viewerMode) viewerMode->setCurrentIndex(0);
    check(modeChanged && !page.state().imageViewerNewWindow, "image viewer mode changes immediately");
    QObject::disconnect(modeConnection);
    auto *languages = page.findChild<QComboBox*>("comboUiLanguage");
    check(languages && languages->currentData() == "en_US", "language control restores English");
    check(page.state().uiLanguage == "en_US", "settings state reads language by stable ID");
    check(languages->itemText(0) == QString::fromUtf8("简体中文") && languages->itemText(1) == "English",
          "language names stay recognizable in either language");
    auto *sites = page.findChild<QComboBox*>("comboCivitaiBrowserHost");
    check(sites && sites->currentData() == "civitai.com" && page.state().civitaiBrowserHost == "civitai.com",
          "settings restore the selected website using stable host values");
    if (sites) {
        QObject::connect(&page, &SettingsPage::stateChanged, &page, [&](const SettingsState &changed) {
            changedHost = changed.civitaiBrowserHost;
        });
        sites->setCurrentIndex(0);
        check(changedHost == "civitai.red" && page.state().civitaiBrowserHost == "civitai.red",
              "changing the website immediately exposes the new preference");
    }
}

void checkDownloadState()
{
    DownloadsPage page;
    ModelUpdateInfo info;
    info.filePath = "model.safetensors";
    info.displayName = QString::fromUtf8("用户的模型");
    info.hasUpdate = false;
    const QString rawStatus = QString::fromUtf8("检查失败: HTTP 429");
    page.addOrUpdateCard(info, rawStatus, true);
    check(page.cardStatusText(info.filePath) == rawStatus && page.filePathsForCategory("errors").contains(info.filePath),
          "translated card preserves raw state and error classification");
    check(page.failedUpdateCheckFilePaths().contains(info.filePath), "English UI preserves retry classification");
    bool translated = false, preservedName = false;
    for (auto *label : page.findChildren<QLabel*>()) {
        translated |= label->text() == "Check failed: HTTP 429";
        preservedName |= label->text().contains(info.displayName);
    }
    check(translated && preservedName, "status is English while the user model name is preserved");
    page.updateCardStatus(info.filePath, QString::fromUtf8("已是最新"));
    check(page.filePathsForCategory("latest").contains(info.filePath), "translated status does not change category keys");
    page.findChild<QLineEdit*>("editDownloadsSearch")->setText("up to date");
    check(page.visibleFilePathsForCategory("latest").contains(info.filePath), "search matches the displayed English status");
}
}

int main(int argc, char **argv)
{
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        std::fprintf(stderr, "%s\n", message.toUtf8().constData());
    });
    QApplication app(argc, argv);
    check(AppLanguage::initialize(app, "zh_CN"), "load embedded Chinese catalog");
    check(QCoreApplication::translate("MainWindow", "TRIGGER WORDS / 触发词") == QString::fromUtf8("触发词"),
          "Chinese caption contains only Chinese, not a bilingual slash caption");
    check(AppText::tr("关闭 / Close") == QString::fromUtf8("关闭"), "Chinese dynamic dialogs are localized");
    {
        QDialogButtonBox buttons(QDialogButtonBox::Cancel);
        check(buttons.button(QDialogButtonBox::Cancel)->text().contains(QString::fromUtf8("取消")),
              "Qt standard dialog buttons are included in the Chinese catalog");
    }
    check(AppLanguage::initialize(app, "en_US"), "load embedded English catalog");
    check(QCoreApplication::translate("MainWindow", "TRIGGER WORDS / 触发词") == "TRIGGER WORDS", "translate generated UI context");
    check(AppText::tr("关闭 / Close") == "Close", "translate dynamic dialog text");
    {
        QDialogButtonBox buttons(QDialogButtonBox::Cancel);
        check(buttons.button(QDialogButtonBox::Cancel)->text().remove('&') == "Cancel", "Qt dialog buttons return to English");
    }
    check(AppText::display(QString::fromUtf8("检查失败: HTTP 429")) == "Check failed: HTTP 429", "translate a formatted stored status");
    check(AppText::display(QString::fromUtf8("已从 我的收藏 移除 3 个模型")) == QString::fromUtf8("Removed 3 models from 我的收藏"),
          "status placeholders can reorder without translating user data");
    check(AppText::display("plain user data %1") == "plain user data %1", "unmatched user content is unchanged");
    checkSettings();
    checkAllUiSources();
    checkForm<Ui::MainWindow, QMainWindow>();
    checkForm<Ui::PromptParserWidget>();
    checkForm<Ui::PromptTemplateLibraryWidget>();
    checkForm<Ui::LlmPromptWidget>();
    checkForm<Ui::TagBrowserWidget>();
    checkForm<Ui::SyncWidget>();
    checkForm<Ui::UsageAnalysisWidget>();
    checkForm<Ui::LauncherWidget>();
    checkForm<Ui::AboutPage>();
    checkForm<Ui::ModelNoteDialog, QDialog>();
    checkForm<Ui::PathListDialog, QDialog>();
    checkForm<Ui::ImageViewerDialog, QDialog>();
    checkForm<Ui::LauncherImportDialog, QDialog>();
    checkDownloadState();
    qInfo() << (failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
