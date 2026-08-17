#ifndef LAUNCHERIMPORTDIALOG_H
#define LAUNCHERIMPORTDIALOG_H

#include <QDialog>

#include "launchscriptparser.h"

namespace Ui {
class LauncherImportDialog;
}

class LauncherImportDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LauncherImportDialog(const LaunchScriptImportResult &result,
                                  const QString &currentScript,
                                  const QString &currentArguments,
                                  const QString &currentWorkdir,
                                  const QString &currentEnvironment,
                                  QWidget *parent = nullptr);
    ~LauncherImportDialog() override;

    bool applyScript() const;
    bool applyArguments() const;
    bool applyWorkdir() const;
    bool applyEnvironment() const;

private:
    Ui::LauncherImportDialog *ui = nullptr;
};

#endif // LAUNCHERIMPORTDIALOG_H
