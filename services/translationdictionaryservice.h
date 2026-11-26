#ifndef TRANSLATIONDICTIONARYSERVICE_H
#define TRANSLATIONDICTIONARYSERVICE_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QThreadPool;

class TranslationDictionaryService : public QObject
{
    Q_OBJECT

public:
    explicit TranslationDictionaryService(QNetworkAccessManager *netManager,
                                          QThreadPool *backgroundPool,
                                          QObject *parent = nullptr);

    void downloadAiTagTranslate(const QString &targetPath, const QString &userAgent);
    void reloadMaps(const QStringList &activePaths, bool notifyWidgets = true, bool asynchronous = true);
    void shutdown();

signals:
    void aiTagDownloadStateChanged(bool downloading, const QString &statusText);
    void aiTagDownloadFailed(const QString &title, const QString &message);
    void aiTagDownloadSucceeded(const QString &targetPath, int entryCount);
    void translationMapsLoaded(const QHash<QString, QString> &map, bool notifyWidgets, int sourceFileCount);

private:
    QNetworkAccessManager *m_netManager = nullptr;
    QThreadPool *m_backgroundPool = nullptr;
    int m_loadToken = 0;
    bool m_isShuttingDown = false;
};

#endif // TRANSLATIONDICTIONARYSERVICE_H
