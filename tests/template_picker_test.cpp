#include "tools/prompttemplatelibrarywidget.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QPushButton>
#include <QSemaphore>
#include <QTableWidget>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent>
#include <cstdio>
#include <functional>

namespace {
int failures = 0;
void check(bool ok, const char *message)
{
    if (!ok) { ++failures; qCritical() << "FAIL:" << message; }
}

bool waitUntil(const std::function<bool()> &done)
{
    if (done()) return true;
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() { if (done()) loop.quit(); });
    poll.start(1);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    return done();
}

void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write picker fixture");
}

void testPickers(const QString &cachePath)
{
    PromptTemplateLibraryWidget widget;
    auto *generateScope = widget.findChild<QComboBox*>("comboTagScope");
    auto *templateScope = widget.findChild<QComboBox*>("comboTemplateTagScope");
    auto *generateTable = widget.findChild<QTableWidget*>("tableTagPicker");
    auto *templateTable = widget.findChild<QTableWidget*>("tableTemplateTagPicker");
    auto *reload = widget.findChild<QPushButton*>("btnRefreshTags");
    check(generateScope && templateScope && generateTable && templateTable && reload, "fixed picker UI is available");
    if (!generateScope || !templateScope || !generateTable || !templateTable || !reload) return;

    QSemaphore entered, release;
    auto blocker = QtConcurrent::run([&]() { entered.release(); release.acquire(); });
    entered.acquire();
    generateScope->setCurrentIndex(1);
    templateScope->setCurrentIndex(2);
    generateScope->setCurrentIndex(0);
    release.release();
    blocker.waitForFinished();
    check(waitUntil([&]() { return generateTable->rowCount() == 2 && templateTable->rowCount() == 3; }),
          "both waiting pickers refresh with their latest, independent scope");
    check(generateTable->item(0, 4) && generateTable->item(0, 4)->text() == "1", "repeated tag counts once");
    generateTable->selectRow(0);
    generateScope->setCurrentIndex(1);
    check(generateTable->rowCount() == 1 && generateTable->item(0, 0)->text() == "blur"
          && templateTable->rowCount() == 3, "scope switch uses cached data without changing the other picker");
    check(generateTable->selectedItems().isEmpty(), "filtering does not silently select a different tag at the old row");

    writeFile(cachePath, "{}");
    generateScope->setCurrentIndex(0);
    check(generateTable->rowCount() == 2, "switching scope does not reread the cache");
    reload->click();
    check(waitUntil([&]() { return generateTable->rowCount() == 0 && templateTable->rowCount() == 0; }),
          "manual refresh updates all previously opened pickers, including empty results");
    writeFile(cachePath, R"({"a.png":{"p":"new tag"}})");
    generateScope->setCurrentIndex(1);
    generateScope->setCurrentIndex(0);
    check(generateTable->rowCount() == 0, "an empty successful result is cached too");
    reload->click();
    check(waitUntil([&]() { return generateTable->rowCount() == 1 && templateTable->rowCount() == 1; }),
          "manual refresh can reload after an empty cache");
}

void testClosingDuringLoad()
{
    auto *widget = new PromptTemplateLibraryWidget;
    QSemaphore entered, release;
    auto blocker = QtConcurrent::run([&]() { entered.release(); release.acquire(); });
    entered.acquire();
    widget->findChild<QComboBox*>("comboTagScope")->setCurrentIndex(1);
    QElapsedTimer elapsed;
    elapsed.start();
    delete widget;
    check(elapsed.elapsed() < 1000, "destroying a picker does not wait for queued file I/O");
    release.release();
    blocker.waitForFinished();
    QThreadPool::globalInstance()->waitForDone();
    QCoreApplication::processEvents();
}
}

int main(int argc, char **argv)
{
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        std::fprintf(stderr, "%s\n", message.toUtf8().constData());
    });
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    // This executable has its own test-only output directory, never the app's config.
    const QString dir = app.applicationDirPath() + "/config";
    const QString cache = dir + "/user_gallery_cache.json";
    const QString library = dir + "/prompt_templates.json";
    if (QFile::exists(cache) || QFile::exists(library)) {
        qCritical() << "Refusing to overwrite pre-existing test fixtures in" << dir;
        return 1;
    }
    if (!QDir().mkpath(dir)) return 1;
    writeFile(cache, R"({"a.png":{"p":"alpha, alpha, beta","np":"blur"}})");
    writeFile(library, "{}");
    QThreadPool::globalInstance()->setMaxThreadCount(1);
    testPickers(cache);
    testClosingDuringLoad();
    QFile::remove(cache);
    QFile::remove(library);
    qInfo() << (failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
