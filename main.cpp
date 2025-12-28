#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setWindowIcon(QIcon("://logo.ico"));
    MainWindow w;
    w.show();
    return a.exec();
}
