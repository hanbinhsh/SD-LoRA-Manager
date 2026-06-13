#include "aboutpage.h"
#include "ui_aboutpage.h"

#include <QPushButton>

AboutPage::AboutPage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::AboutPage)
{
    ui->setupUi(this);
    connect(ui->btnCheckUpdate, &QPushButton::clicked, this, &AboutPage::checkUpdateRequested);
}

AboutPage::~AboutPage()
{
    delete ui;
}

void AboutPage::setVersionText(const QString &version)
{
    ui->lblAboutVersion->setText("Version " + version);
}

void AboutPage::setCheckingForUpdates(bool checking)
{
    ui->btnCheckUpdate->setText(checking ? "⏳ Checking..." : "🚀 检查更新 / Check for Updates");
    ui->btnCheckUpdate->setEnabled(!checking);
}
