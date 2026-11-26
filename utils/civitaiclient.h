#ifndef CIVITAICLIENT_H
#define CIVITAICLIENT_H

#include <QNetworkRequest>
#include <QString>
#include <QUrl>

class QNetworkReply;

namespace CivitaiClient {

bool isCivitaiUrl(const QUrl &url);
bool shouldUseBearerAuth(const QUrl &url);
QUrl urlWithToken(const QUrl &url, const QString &apiKey);
QNetworkRequest makeNetworkRequest(const QUrl &url,
                                   const QString &userAgent,
                                   const QString &apiKey,
                                   bool allowCivitaiAuth = true);
QString networkErrorMessage(QNetworkReply *reply);
QString randomUserAgent();

} // namespace CivitaiClient

#endif // CIVITAICLIENT_H
