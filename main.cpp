#include "mainwindow.h"

#include <QApplication>
#include <QStyleHints>
#include <QIcon>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QDir>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

// 定义一个结构体存放初始配置
struct StartupConfig {
    QString uiScale = "1.0";
    int width = 1300;
    int height = 900;
};

// 在 QApplication 创建前读取配置
StartupConfig readStartupConfig(int argc, char *argv[]) {
    StartupConfig config;

    // 安全获取程序所在目录（替代 qApp->applicationDirPath()）
    QString appPath = ".";
    if (argc > 0) {
        appPath = QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath();
    }

    QString configPath = appPath + "/config/settings.json";
    QFile file(configPath);
    if (file.open(QIODevice::ReadOnly)) {
        QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();

        if (root.contains("ui_scale")) {
            config.uiScale = QString::number(root["ui_scale"].toDouble(1.0), 'f', 2);
        }
        if (root.contains("window_width")) {
            config.width = root["window_width"].toInt(1300);
        }
        if (root.contains("window_height")) {
            config.height = root["window_height"].toInt(900);
        }
    }
    return config;
}

int main(int argc, char *argv[])
{
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF
                   | _CRTDBG_LEAK_CHECK_DF
                   | _CRTDBG_CHECK_ALWAYS_DF);
#endif

    // 1. 读取初始配置
    StartupConfig config = readStartupConfig(argc, argv);

    // 2. 必须在 QApplication 实例化前设置缩放环境变量
    if (config.uiScale != "1.0") {
        qputenv("QT_SCALE_FACTOR", config.uiScale.toUtf8());
    }

    // 允许非整数倍缩放 (例如 1.25, 1.5)，Qt 5.14+ 支持
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif

    // 3. 实例化 QApplication
    QApplication a(argc, argv);
    a.styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    a.setWindowIcon(QIcon("://logo.ico"));

    // 4. 创建主窗口并应用分辨率
    MainWindow w;
    w.resize(config.width, config.height);
    w.show();

    return a.exec();
}