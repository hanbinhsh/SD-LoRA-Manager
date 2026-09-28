#include "tools/tagbrowserwidget.h"
#include "utils/translationcsv.h"
#include "widgets/tagsearchproxymodel.h"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <functional>
#include <cstdio>

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
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    return done();
}

void settle(TagSearchProxyModel &proxy)
{
    proxy.applySearchNow();
    check(waitUntil([&]() { return !proxy.isSearching(); }), "search completes");
}

QList<QStandardItem*> row(const QString &tag, const QString &translation, int count = 0)
{
    auto *number = new QStandardItem(QString::number(count));
    number->setData(count, Qt::UserRole);
    auto *tagItem = new QStandardItem(tag);
    tagItem->setData(tag, Qt::UserRole);
    return {tagItem, new QStandardItem("category_only"), new QStandardItem(translation), number};
}

void testSearchAndEdits()
{
    QStandardItemModel model;
    TagSearchProxyModel proxy;
    proxy.setSearchColumns(0, 2);
    proxy.setSourceModel(&model);
    model.appendRow(row("spoken_heart", QString::fromUtf8("\xe7\x88\xb1\xe5\xbf\x83"), 999));
    model.appendRow(row("heart-shaped", "shape", 1000));
    model.appendRow(row("heartwarming", "warm", 10));
    model.appendRow(row("broken_heart", "sad", 2));
    check(proxy.rowCount() == 4, "new rows visible before searching");
    proxy.setSearchText("SPOKEN heart");
    settle(proxy);
    check(proxy.rowCount() == 1 && proxy.index(0, 0).data() == "spoken_heart", "case and underscore equivalent");
    proxy.setSearchText(QString::fromUtf8("\xe7\x88\xb1\xe5\xbf\x83"));
    settle(proxy);
    check(proxy.rowCount() == 1, "translation search");
    proxy.setSearchText("category_only");
    settle(proxy);
    check(proxy.rowCount() == 0, "category excluded from search");
    proxy.setSearchText("999");
    settle(proxy);
    check(proxy.rowCount() == 0, "numeric fields excluded from search");
    proxy.setSearchText("heart");
    proxy.setMatchMode(TagSearch::WordMatch);
    settle(proxy);
    check(proxy.rowCount() == 3, "whole word excludes heartwarming");
    proxy.setMatchMode(TagSearch::ExactMatch);
    settle(proxy);
    check(proxy.rowCount() == 0, "exact is per-cell not concatenated fields");
    proxy.setSearchText("spoken heart");
    settle(proxy);
    check(proxy.rowCount() == 1, "exact normalized phrase");
    check(proxy.setData(proxy.index(0, 0), "edited_tag", Qt::EditRole), "proxy remains editable");
    settle(proxy);
    check(proxy.rowCount() == 0 && model.item(0, 0)->text() == "edited_tag", "edit updates index and source");
    model.item(0, 2)->setText("spoken heart");
    settle(proxy);
    check(proxy.rowCount() == 1, "translation edit is indexed");
    model.insertRow(0, row("spoken_heart", "new"));
    settle(proxy);
    check(proxy.rowCount() == 2, "insertion keeps source row alignment");
    model.removeRow(0);
    settle(proxy);
    check(proxy.rowCount() == 1 && proxy.mapToSource(proxy.index(0, 0)).row() == 0, "removal keeps source row alignment");
    proxy.setSearchText("");
    settle(proxy);
    proxy.sort(3, Qt::DescendingOrder);
    check(proxy.index(0, 3).data().toInt() == 1000, "numeric sort retained");
    proxy.sort(-1);
    check(proxy.index(0, 0).data() == "edited_tag", "CSV source order retained");
    proxy.setSearchText("edited_tag");
    settle(proxy);
    model.sort(0, Qt::DescendingOrder);
    settle(proxy);
    check(proxy.rowCount() == 1 && proxy.index(0, 0).data() == "edited_tag", "source layout changes rebuild index");
    model.clear();
    model.appendRow(row("other", "spoken heart"));
    proxy.setSearchText("spoken heart");
    settle(proxy);
    check(proxy.rowCount() == 1 && proxy.index(0, 0).data() == "other", "reset replaces old index");

    QStandardItemModel userModel;
    TagSearchProxyModel userProxy;
    userProxy.setSearchColumns(0, 3);
    userProxy.setSourceModel(&userModel);
    userModel.appendRow({new QStandardItem("tag"), new QStandardItem("positive"), new QStandardItem("hidden category"),
                         new QStandardItem("translation"), new QStandardItem("99"), new QStandardItem("123")});
    userProxy.setSearchText("translation");
    settle(userProxy);
    check(userProxy.rowCount() == 1, "six-column user table uses configured translation column");
    userProxy.setSearchText("positive");
    settle(userProxy);
    check(userProxy.rowCount() == 0, "user type excluded");
}

class LargeTable : public QAbstractTableModel
{
public:
    mutable int dataReads = 0;
    mutable int headerReads = 0;
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 100000; }
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 4; }
    QVariant data(const QModelIndex &index, int role) const override {
        check(QThread::currentThread() == thread(), "source model is never accessed by worker");
        ++dataReads;
        if (role != Qt::DisplayRole || !index.isValid()) return {};
        return index.column() == 0 ? QString("tag_%1").arg(index.row()) : QStringLiteral("translation");
    }
    QVariant headerData(int, Qt::Orientation, int) const override { ++headerReads; return {}; }
};

void testLargeIndex()
{
    LargeTable model;
    TagSearchProxyModel proxy;
    proxy.setSearchColumns(0, 2);
    proxy.setSourceModel(&model);
    check(proxy.rowCount() == 100000, "large fixture loaded");
    model.dataReads = model.headerReads = 0;
    QElapsedTimer timer;
    timer.start();
    proxy.setSearchText("t");
    proxy.setSearchText("ta");
    proxy.setSearchText("tag_99999");
    const qint64 inputUs = timer.nsecsElapsed() / 1000;
    check(proxy.isSearching() && proxy.rowCount() == 100000, "typing is deferred and consecutive input is coalesced");
    check(model.dataReads == 0 && model.headerReads == 0, "typing does not rescan model or headers");
    int heartbeat = 0;
    QTimer pulse;
    QObject::connect(&pulse, &QTimer::timeout, &proxy, [&]() { ++heartbeat; });
    pulse.start(1);
    check(waitUntil([&]() { return !proxy.isSearching(); }), "debounced search completes");
    pulse.stop();
    check(heartbeat > 0 && proxy.rowCount() == 1, "event loop responsive and final query wins");
    check(model.dataReads == 0 && model.headerReads == 0, "filtering uses only cached normalized text");
    qInfo() << "100k rows: three input changes took" << inputUs << "us; total with debounce" << timer.elapsed() << "ms";
    proxy.setSearchText("not_found");
    proxy.applySearchNow();
    proxy.setSearchText("tag_43210");
    proxy.applySearchNow();
    settle(proxy);
    check(proxy.rowCount() == 1 && proxy.mapToSource(proxy.index(0, 0)).row() == 43210, "stale worker cannot replace latest search");
    proxy.setSearchText("tag");
    proxy.applySearchNow();
    proxy.setSourceModel(nullptr);
    settle(proxy);
    check(proxy.rowCount() == 0, "source replacement rejects outstanding results");
    auto *closing = new TagSearchProxyModel;
    closing->setSearchColumns(0, 2);
    closing->setSourceModel(&model);
    closing->setSearchText("tag");
    closing->applySearchNow();
    delete closing;
    QThreadPool::globalInstance()->waitForDone();
}

void writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(), "write CSV fixture");
}

void testBrowserSave(const QTemporaryDir &dir)
{
    const QString a = dir.filePath("a.csv"), b = dir.filePath("b.csv");
    writeFile(a, "shared_tag,preferred\nalpha,first\n");
    writeFile(b, "shared_tag,shadowed\nbeta,second\n");
    TagBrowserWidget browser;
    browser.setTranslationSources({{a, true}, {b, true}});
    QMetaObject::invokeMethod(&browser, "onReloadClicked");
    auto *table = browser.findChild<QTableView*>("tableTags");
    auto *search = browser.findChild<QLineEdit*>("editSearch");
    auto *save = browser.findChild<QPushButton*>("btnSave");
    auto *status = browser.findChild<QLabel*>("lblStatus");
    check(table && search && save && status, "browser controls present");
    if (!table || !search || !save || !status) return;
    auto *proxy = qobject_cast<TagSearchProxyModel*>(table->model());
    check(waitUntil([&]() { return save->isEnabled() && proxy->rowCount() == 3; }), "merged fixture loads asynchronously");
    search->setText("shared tag");
    settle(*proxy);
    check(proxy->rowCount() == 1 && proxy->index(0, 2).data() == "preferred", "merged search preserves top source priority");
    proxy->setData(proxy->index(0, 2), "edited", Qt::EditRole);
    check(status->text().contains("1 "), "one edited row tracked");
    proxy->setData(proxy->index(0, 2), "preferred", Qt::EditRole);
    check(!status->text().contains(QString::fromUtf8("\xe6\x9c\xaa\xe4\xbf\x9d\xe5\xad\x98")), "reverting edit clears dirty state");
    proxy->setData(proxy->index(0, 2), "edited", Qt::EditRole);
    search->setText("beta");
    settle(*proxy);
    check(proxy->rowCount() == 1, "search after edit");
    proxy->setData(proxy->index(0, 2), "second edited", Qt::EditRole);
    QMetaObject::invokeMethod(&browser, "onSaveClicked");
    const auto aRows = TranslationCsv::readFile(a), bRows = TranslationCsv::readFile(b);
    check(aRows.size() == 2 && aRows.at(0).translation == "edited", "merged edit writes winning source");
    check(bRows.size() == 2 && bRows.at(0).translation == "shadowed" && bRows.at(1).translation == "second edited",
          "other source edited without touching overridden row");
    const QString value = "a,b\"c";
    check(TranslationCsv::parseLine(TranslationCsv::escapeField(value)).value(0) == value, "shared CSV escaping round-trip");
}
}

int main(int argc, char **argv)
{
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        const QByteArray bytes = message.toUtf8();
        std::fprintf(stderr, "%s\n", bytes.constData());
    });
    QApplication app(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) return 1;
    testSearchAndEdits();
    testLargeIndex();
    testBrowserSave(dir);
    qInfo() << "Tag browser" << (failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
