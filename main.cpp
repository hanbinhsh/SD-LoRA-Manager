#include "mainwindow.h"

#include <QApplication>
#include <QStyleHints>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    a.setWindowIcon(QIcon("://logo.ico"));
    MainWindow w;
    w.show();
    return a.exec();
}
