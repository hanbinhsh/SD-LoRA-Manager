#include "services/translationdictionaryservice.h"

#include "utils/applanguage.h"
#include "utils/civitaiclient.h"
#include "utils/translationcsv.h"

#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>
#include <QThreadPool>
#include <QUrl>
#include <QtConcurrent/QtConcurrent>

TranslationDictionaryService::TranslationDictionaryService(QNetworkAccessManager *netManager,
                                                           QThreadPool *backgroundPool,
                                                           QObject *parent)
    : QObject(parent)
    , m_netManager(netManager)
    , m_backgroundPool(backgroundPool)
{
}

void TranslationDictionaryService::shutdown()
{
    m_isShuttingDown = true;
    ++m_loadToken;
}

void TranslationDictionaryService::downloadAiTagTranslate(const QString &targetPath, const QString &userAgent)
{
    if (!m_netManager || targetPath.isEmpty()) return;

    const QString directory = QFileInfo(targetPath).absolutePath();
    if (!QDir().mkpath(directory)) {
        emit aiTagDownloadFailed(AppText::tr("下载失败"), AppText::tr("无法创建词表目录：\n") + directory);
        return;
    }

    emit aiTagDownloadStateChanged(true, AppText::tr("正在下载 AITagTranslate 词表..."));
    QNetworkReply *reply = m_netManager->get(CivitaiClient::makeNetworkRequest(
        QUrl(QStringLiteral("https://raw.githubusercontent.com/hanbinhsh/AITagTranslate/main/autocomplete.txt")),
        userAgent,
        QString(),
        false));

    connect(reply, &QNetworkReply::finished, this, [this, reply, targetPath]() {
        const QByteArray data = reply->readAll();
        const QString errorText = reply->errorString();
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();

        if (m_isShuttingDown) return;

        if (!networkOk) {
            emit aiTagDownloadStateChanged(false, AppText::tr("下载失败：") + errorText);
            emit aiTagDownloadFailed(AppText::tr("下载失败"), AppText::tr("无法下载 AITagTranslate 词表：\n") + errorText);
            return;
        }

        if (data.trimmed().startsWith('<')) {
            emit aiTagDownloadStateChanged(false, AppText::tr("下载内容不是有效的 Tag 词表。"));
            emit aiTagDownloadFailed(AppText::tr("下载失败"),
                                     AppText::tr("GitHub 返回的内容不是有效的 AITagTranslate 词表，旧文件未被修改。"));
            return;
        }

        emit aiTagDownloadStateChanged(true, AppText::tr("下载完成，正在后台验证词表..."));
        auto *watcher = new QFutureWatcher<int>(this);
        connect(watcher, &QFutureWatcher<int>::finished, this, [this, watcher, data, targetPath]() {
            const int entryCount = watcher->result();
            watcher->deleteLater();
            if (m_isShuttingDown) return;

            if (entryCount < 10) {
                emit aiTagDownloadStateChanged(false, AppText::tr("下载内容不是有效的 Tag 词表。"));
                emit aiTagDownloadFailed(AppText::tr("下载失败"),
                                         AppText::tr("GitHub 返回的内容不是有效的 AITagTranslate 词表，旧文件未被修改。"));
                return;
            }

            QSaveFile output(targetPath);
            if (!output.open(QIODevice::WriteOnly)
                || output.write(data) != data.size()
                || !output.commit()) {
                const QString message = output.errorString();
                emit aiTagDownloadStateChanged(false, AppText::tr("保存失败：") + message);
                emit aiTagDownloadFailed(AppText::tr("保存失败"), AppText::tr("无法保存 AITagTranslate 词表：\n") + message);
                return;
            }

            emit aiTagDownloadStateChanged(
                false, AppText::tr("已加载 %1 条记录，词表优先级位于最底部。").arg(entryCount));
            emit aiTagDownloadSucceeded(targetPath, entryCount);
        });

        QThreadPool *pool = m_backgroundPool ? m_backgroundPool : QThreadPool::globalInstance();
        watcher->setFuture(QtConcurrent::run(pool, [data]() {
            return static_cast<int>(TranslationCsv::parseUtf8(data).size());
        }));
    });
}

void TranslationDictionaryService::reloadMaps(const QStringList &activePaths, bool notifyWidgets, bool asynchronous)
{
    const int token = ++m_loadToken;
    const auto readMaps = [activePaths]() {
        QHash<QString, QString> result;
        for (int i = activePaths.size() - 1; i >= 0; --i) {
            const QVector<TranslationCsvEntry> entries = TranslationCsv::readFile(activePaths.at(i));
            for (const TranslationCsvEntry &entry : entries) {
                const QString display = entry.displayValue().trimmed();
                if (!entry.tag.isEmpty() && !display.isEmpty()) {
                    result.insert(entry.tag, display);
                }
            }
        }
        return result;
    };

    if (!asynchronous) {
        if (!m_isShuttingDown && token == m_loadToken) {
            emit translationMapsLoaded(readMaps(), notifyWidgets, activePaths.size());
        }
        return;
    }

    auto *watcher = new QFutureWatcher<QHash<QString, QString>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, token, notifyWidgets, count = activePaths.size()]() {
        const QHash<QString, QString> result = watcher->result();
        watcher->deleteLater();
        if (m_isShuttingDown || token != m_loadToken) return;
        emit translationMapsLoaded(result, notifyWidgets, count);
    });

    QThreadPool *pool = m_backgroundPool ? m_backgroundPool : QThreadPool::globalInstance();
    watcher->setFuture(QtConcurrent::run(pool, readMaps));
}
