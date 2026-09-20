#include "modellistcache.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

namespace {
int failures = 0;
void check(bool ok, const char *message)
{
    if (!ok) {
        ++failures;
        qCritical() << "FAIL:" << message;
    }
}

void writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(), "write fixture");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory");
    if (!dir.isValid()) return 1;
    const QString model = dir.filePath("example.safetensors");
    const QString json = dir.filePath("example.json");
    const QString cachePath = dir.filePath("config/model_list_cache.json");
    writeFile(model, "model");
    writeFile(json, R"({"name":"original","images":[]})");
    const auto stamp = [&]() { return ModelListCache::fingerprint(QFileInfo(model), QFileInfo(json)); };
    const QJsonObject originalStamp = stamp();
    const QJsonObject summary{{"name", "original"}, {"modelId", 123}, {"previewState", 2}};
    QJsonObject restored;

    ModelListCache first(cachePath);
    check(!first.lookup(model, originalStamp, &restored), "cold scan misses");
    first.insert(model, originalStamp, summary);
    check(first.save(), "atomic save creates config directory");

    ModelListCache warm(cachePath);
    check(warm.lookup(model, originalStamp, &restored) && restored == summary, "warm scan restores summary");
    check(warm.save(), "unchanged cache can be reused");
    QFile original(json);
    check(original.open(QIODevice::ReadOnly) && original.readAll() == R"({"name":"original","images":[]})",
          "model metadata remains untouched");
    original.close();

    writeFile(json, R"({"name":"edited metadata","images":[],"trainedWords":["new"]})");
    check(!warm.lookup(model, stamp(), &restored), "metadata edit invalidates cached summary");
    writeFile(model, "replacement model content");
    check(!warm.lookup(model, stamp(), &restored), "model replacement invalidates cached summary");
    check(!warm.lookup(dir.filePath("different.safetensors"), originalStamp, &restored), "paths stay independent");

    check(QFile::remove(json), "remove metadata fixture");
    const QJsonObject missingStamp = stamp();
    check(missingStamp != originalStamp, "metadata removal invalidates stamp");
    ModelListCache bare(cachePath);
    bare.insert(model, missingStamp, QJsonObject{{"name", "bare model"}});
    check(bare.save(), "cache model without metadata");
    writeFile(json, "{}");
    ModelListCache newMetadata(cachePath);
    check(!newMetadata.lookup(model, stamp(), &restored), "new metadata invalidates bare model cache");

    ModelListCache emptyScan(cachePath);
    check(emptyScan.save(), "empty scan prunes old entries");
    ModelListCache pruned(cachePath);
    check(!pruned.lookup(model, missingStamp, &restored), "removed models do not persist in index");

    writeFile(cachePath, "{broken");
    ModelListCache corrupt(cachePath);
    check(!corrupt.lookup(model, stamp(), &restored), "corrupt cache falls back to parsing");
    corrupt.insert(model, stamp(), summary);
    check(corrupt.save(), "corrupt cache can be rebuilt");
    writeFile(cachePath, QJsonDocument(QJsonObject{{"version", 999}, {"entries", QJsonObject{}}}).toJson());
    ModelListCache unsupported(cachePath);
    check(!unsupported.lookup(model, stamp(), &restored), "unknown cache version ignored");

    qInfo() << (failures == 0 ? "All model list cache checks passed" : "Model list cache checks failed");
    return failures == 0 ? 0 : 1;
}
