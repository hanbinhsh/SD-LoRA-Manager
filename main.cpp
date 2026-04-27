#include "mainwindow.h"

#include <QApplication>
#include <QStyleHints>
#include <QIcon>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

int main(int argc, char *argv[])
{
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF
                   | _CRTDBG_LEAK_CHECK_DF
                   | _CRTDBG_CHECK_ALWAYS_DF);
#endif

    QApplication a(argc, argv);
    a.styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    a.setWindowIcon(QIcon("://logo.ico"));
    MainWindow w;
    w.show();
    return a.exec();
}
