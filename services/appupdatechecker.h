#ifndef APPUPDATECHECKER_H
#define APPUPDATECHECKER_H

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

class AppUpdateChecker : public QObject
{
    Q_OBJECT

public:
    explicit AppUpdateChecker(QNetworkAccessManager *netManager, QObject *parent = nullptr);

    void checkForUpdates(const QString &apiUrl,
                         const QString &currentVersion,
                         const QString &userAgent,
                         bool silentIfLatest);

    static bool isRemoteVersionNewer(QString remoteTag, const QString &currentVersion);

signals:
    void checkingChanged(bool checking);
    void statusMessage(const QString &message, int timeoutMs);
    void updateAvailable(const QString &remoteTag, const QString &htmlUrl, const QString &changelog);
    void alreadyUpToDate(const QString &currentVersion, bool silent);
    void checkFailed(const QString &errorMessage, bool parseError, bool silent);

private:
    void handleReply(QNetworkReply *reply, const QString &currentVersion, bool silentIfLatest);

    QNetworkAccessManager *m_netManager = nullptr;
};

#endif // APPUPDATECHECKER_H
