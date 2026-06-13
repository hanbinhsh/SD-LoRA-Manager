#ifndef ABOUTPAGE_H
#define ABOUTPAGE_H

#include <QWidget>

namespace Ui {
class AboutPage;
}

class AboutPage : public QWidget
{
    Q_OBJECT

public:
    explicit AboutPage(QWidget *parent = nullptr);
    ~AboutPage() override;

    void setVersionText(const QString &version);
    void setCheckingForUpdates(bool checking);

signals:
    void checkUpdateRequested();

private:
    Ui::AboutPage *ui = nullptr;
};

#endif // ABOUTPAGE_H
