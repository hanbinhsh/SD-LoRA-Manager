#include "appupdatechecker.h"
#include "utils/applanguage.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

AppUpdateChecker::AppUpdateChecker(QNetworkAccessManager *netManager, QObject *parent)
    : QObject(parent)
    , m_netManager(netManager)
{
}

bool AppUpdateChecker::isRemoteVersionNewer(QString remoteTag, const QString &currentVersion)
{
    remoteTag = remoteTag.trimmed();
    if (remoteTag.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        remoteTag = remoteTag.mid(1);
    }
    if (remoteTag.isEmpty() || remoteTag == currentVersion) {
        return false;
    }

    const QStringList remoteParts = remoteTag.split(QLatin1Char('.'));
    const QStringList localParts = currentVersion.split(QLatin1Char('.'));
    const int len = qMax(remoteParts.size(), localParts.size());
    for (int i = 0; i < len; ++i) {
        const int r = (i < remoteParts.size()) ? remoteParts[i].toInt() : 0;
        const int l = (i < localParts.size()) ? localParts[i].toInt() : 0;
        if (r > l) return true;
        if (r < l) return false;
    }
    return false;
}

void AppUpdateChecker::checkForUpdates(const QString &apiUrl,
                                       const QString &currentVersion,
                                       const QString &userAgent,
                                       bool silentIfLatest)
{
    if (!m_netManager) return;

    emit statusMessage(AppText::tr("正在连接 GitHub 检查更新..."), 3000);
    emit checkingChanged(true);

    QNetworkRequest request((QUrl(apiUrl)));
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent);

    QNetworkReply *reply = m_netManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, currentVersion, silentIfLatest]() {
        handleReply(reply, currentVersion, silentIfLatest);
    });
}

void AppUpdateChecker::handleReply(QNetworkReply *reply,
                                   const QString &currentVersion,
                                   bool silentIfLatest)
{
    emit checkingChanged(false);
    if (!reply) return;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit checkFailed(reply->errorString(), false, silentIfLatest);
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
    QString remoteTag = root.value(QStringLiteral("tag_name")).toString();
    const QString htmlUrl = root.value(QStringLiteral("html_url")).toString();
    const QString body = root.value(QStringLiteral("body")).toString();

    if (remoteTag.isEmpty()) {
        emit checkFailed(QString(), true, silentIfLatest);
        return;
    }

    if (remoteTag.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        remoteTag = remoteTag.mid(1);
    }

    if (isRemoteVersionNewer(remoteTag, currentVersion)) {
        emit updateAvailable(remoteTag, htmlUrl, body);
    } else {
        emit alreadyUpToDate(currentVersion, silentIfLatest);
    }
}
