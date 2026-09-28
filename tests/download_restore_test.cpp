#include "pages/downloadmanager.h"
#include "pages/downloadspage.h"
#include "utils/styleconstants.h"

#include <QApplication>
#include <QDebug>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QLineEdit>
#include <QPushButton>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>

namespace {
int failures = 0;
void check(bool condition, const char *message)
{
    if (!condition) {
        ++failures;
        qCritical() << "FAIL:" << message;
    }
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "read fixture");
    return file.readAll();
}

bool waitUntil(const std::function<bool()> &done)
{
    if (done()) return true;
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() { if (done()) loop.quit(); });
    poll.start(1);
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    return done();
}

QVector<DownloadCache::Entry> fixture(const QTemporaryDir &dir)
{
    QVector<DownloadCache::Entry> entries;
    for (int i = 63; i >= 0; --i) {
        ModelUpdateInfo info;
        info.filePath = dir.filePath(QString("model%1.safetensors").arg(i));
        info.modelDir = dir.path();
        info.displayName = QString("Model %1").arg(i);
        info.hasUpdate = true;
        info.previewState = ModelPreviewState::KnownNoPreview;
        info.latestVersionJson = {{"images", QJsonArray{QJsonObject{{"meta", QJsonObject{{"prompt", "keep me"}}}}}}};
        entries.append({info, "发现新版本", false});
    }
    return entries;
}

void testRestore(const QTemporaryDir &dir)
{
    const QString path = dir.filePath("downloads.json");
    const auto entries = fixture(dir);
    check(DownloadCache::save(path, entries), "save restore fixture");
    const auto original = readFile(path);
    QThreadPool pool;
    pool.setMaxThreadCount(1);
    QSemaphore releaseReader;
    pool.start([&]() { releaseReader.acquire(); });
    DownloadsPage page;
    page.setStyleSheet(AppStyle::loadQss(":/styles/base.qss") + AppStyle::loadQss(":/styles/mainwindow.qss"));
    DownloadManager manager(&page, nullptr, &pool, nullptr, path);
    int readyCount = 0;
    QObject::connect(&manager, &DownloadManager::cacheReady, &page, [&]() { ++readyCount; });
    manager.ensureCacheLoaded();
    manager.ensureCacheLoaded();
    check(!manager.cacheLoaded() && page.filePathsForCategory("updates").isEmpty(), "cache read never blocks caller");
    check(!page.checkAllButton()->isEnabled(), "actions disabled during restoration");
    manager.saveCache();
    check(readFile(path) == original, "save during pending read does not truncate cache");

    auto live = entries.first().info;
    live.displayName = "Live result";
    manager.addOrUpdateCard(live, "已忽略更新", false);
    const QString removedPath = entries.at(1).info.filePath;
    manager.removeCard(removedPath);
    int responsiveTicks = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &page, [&]() {
        if (!manager.cacheLoaded() && !page.filePathsForCategory("updates").isEmpty()) ++responsiveTicks;
    });
    heartbeat.start(1);
    releaseReader.release();
    check(waitUntil([&]() { return manager.cacheLoaded(); }), "async restore completes");
    heartbeat.stop();
    check(readyCount == 1 && responsiveTicks > 0, "one restoration with event-loop opportunities between batches");
    check(page.filePathsForCategory("updates").size() == entries.size() - 2, "all cached cards restored exactly once");
    check(page.cardStatusText(live.filePath) == "已忽略更新" && manager.info(live.filePath).displayName == "Live result",
          "live update wins over older cache");
    check(!page.containsCard(removedPath), "removed item does not return from pending cache");
    check(page.checkAllButton()->isEnabled() && !page.checkSelectedButton()->isEnabled(), "toolbar recovers without phantom selection");
    const auto ordered = page.sortedFilePathsForCategory("updates");
    const auto *layout = page.cardsLayout("updates");
    for (int i = 0; i < ordered.size(); ++i) {
        check(layout->itemAt(i)->widget()->property("downloadFilePath").toString() == ordered.at(i), "card layout matches deterministic order");
    }
    check(ordered.indexOf(dir.filePath("model2.safetensors")) < ordered.indexOf(dir.filePath("model10.safetensors")), "natural model order");
    auto *search = page.findChild<QLineEdit*>("editDownloadsSearch");
    check(search != nullptr, "search control exists");
    if (search) search->setText("Model 10");
    page.setCurrentTabSelection(true);
    check(page.selectedFilePaths() == QStringList{dir.filePath("model10.safetensors")}, "select all respects search");
    check(page.checkSelectedButton()->isEnabled() && page.downloadSelectedButton()->isEnabled(), "actions enabled by cards, not sidebar selection");
    page.clearAllCardSelection();
    check(!page.checkSelectedButton()->isEnabled(), "clearing selection updates buttons");
    manager.ensureCacheLoaded();
    check(readyCount == 1, "returning to page does not restore twice");
    manager.shutdown();
    pool.waitForDone();
    check(DownloadCache::load(path).entries.size() == entries.size() - 1, "full cache saved after restoration");
}

void testInterruptedRestore(const QTemporaryDir &dir)
{
    const QString path = dir.filePath("interrupted.json");
    check(DownloadCache::save(path, fixture(dir)), "save interrupted fixture");
    const auto original = readFile(path);
    QThreadPool pool;
    DownloadsPage page;
    DownloadManager manager(&page, nullptr, &pool, nullptr, path);
    bool stopped = false;
    QObject::connect(&manager, &DownloadManager::statusMessageChanged, &page, [&](const QString &) {
        if (!stopped && !manager.cacheLoaded() && !page.filePathsForCategory("updates").isEmpty()) {
            stopped = true;
            manager.saveCache();
            manager.shutdown();
        }
    });
    manager.ensureCacheLoaded();
    check(waitUntil([&]() { return stopped; }), "interrupt after first partial batch");
    pool.waitForDone();
    check(!manager.cacheLoaded() && readFile(path) == original, "partial restoration never overwrites original cache");
}

void testInvalidAndEmptyCache(const QTemporaryDir &dir)
{
    for (bool invalid : {true, false}) {
        const QString path = dir.filePath(invalid ? "invalid.json" : "absent.json");
        if (invalid) {
            QFile file(path);
            check(file.open(QIODevice::WriteOnly) && file.write("{broken") == 7, "invalid fixture");
        }
        QThreadPool pool;
        DownloadsPage page;
        DownloadManager manager(&page, nullptr, &pool, nullptr, path);
        manager.ensureCacheLoaded();
        check(waitUntil([&]() { return manager.cacheLoaded(); }), "invalid/missing cache releases busy state");
        check(page.checkAllButton()->isEnabled(), "can still check models after missing/invalid cache");
        manager.shutdown();
        pool.waitForDone();
        if (invalid) check(readFile(path) == "{broken", "invalid original preserved for recovery");
        else check(DownloadCache::load(path).error.isEmpty(), "empty cache round-trip");
    }
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) return 1;
    testRestore(dir);
    testInterruptedRestore(dir);
    testInvalidAndEmptyCache(dir);
    qInfo() << "Download restore" << (failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
