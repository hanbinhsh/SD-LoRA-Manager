#include "launcherimportdialog.h"
#include "ui_launcherimportdialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QStringList>

LauncherImportDialog::LauncherImportDialog(const LaunchScriptImportResult &result,
                                           const QString &currentScript,
                                           const QString &currentArguments,
                                           const QString &currentWorkdir,
                                           const QString &currentEnvironment,
                                           QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::LauncherImportDialog)
{
    ui->setupUi(this);

    QStringList details;
    details << "来源脚本：" + result.sourcePath;
    details << "";
    details << "当前启动入口：" + (currentScript.trimmed().isEmpty() ? "(空)" : currentScript);
    details << "检测启动入口：" + (result.scriptPath.trimmed().isEmpty() ? "(未识别)" : result.scriptPath);
    details << "";
    details << "当前附加参数：" + (currentArguments.trimmed().isEmpty() ? "(空)" : currentArguments);
    details << "可安全导入参数：" + (result.arguments.trimmed().isEmpty() ? "(无)" : result.arguments);
    if (!result.internalArguments.isEmpty()) {
        details << "脚本内部参数（仅供参考）：" + result.internalArguments;
    }
    details << "";
    details << "当前工作目录：" + (currentWorkdir.trimmed().isEmpty() ? "(空)" : currentWorkdir);
    details << "检测工作目录：" + (result.workingDirectory.trimmed().isEmpty() ? "(未识别)" : result.workingDirectory);
    details << "";
    details << QStringLiteral("当前环境变量：")
                   + (currentEnvironment.trimmed().isEmpty()
                          ? QStringLiteral("(空)")
                          : QStringLiteral("已配置"));
    details << "可安全导入环境变量：" + (result.environmentText.trimmed().isEmpty() ? "(无)" : result.environmentText);
    if (!result.notes.isEmpty()) {
        details << "" << "解析说明：";
        for (const QString &note : result.notes) details << "- " + note;
    }
    ui->textImportDetails->setPlainText(details.join('\n'));

    ui->chkImportScript->setEnabled(result.scriptSafe && !result.scriptPath.isEmpty());
    ui->chkImportScript->setChecked(ui->chkImportScript->isEnabled());
    ui->chkImportArguments->setEnabled(result.argumentsSafe);
    ui->chkImportArguments->setChecked(result.argumentsSafe && currentArguments.trimmed().isEmpty());
    ui->chkImportWorkdir->setEnabled(result.workingDirectorySafe && !result.workingDirectory.isEmpty());
    ui->chkImportWorkdir->setChecked(ui->chkImportWorkdir->isEnabled() && currentWorkdir.trimmed().isEmpty());
    ui->chkImportEnvironment->setEnabled(result.environmentSafe);
    ui->chkImportEnvironment->setChecked(result.environmentSafe && currentEnvironment.trimmed().isEmpty());

    connect(ui->buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

LauncherImportDialog::~LauncherImportDialog()
{
    delete ui;
}

bool LauncherImportDialog::applyScript() const { return ui->chkImportScript->isChecked(); }
bool LauncherImportDialog::applyArguments() const { return ui->chkImportArguments->isChecked(); }
bool LauncherImportDialog::applyWorkdir() const { return ui->chkImportWorkdir->isChecked(); }
bool LauncherImportDialog::applyEnvironment() const { return ui->chkImportEnvironment->isChecked(); }
