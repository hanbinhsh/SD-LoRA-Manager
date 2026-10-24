#pragma once

#include <QString>
#include <QUrl>

namespace ModelBrowser {

inline QString normalizedCivitaiHost(QString host)
{
    host = host.trimmed().toLower();
    return host == "civitai.com" ? host : QStringLiteral("civitai.red");
}

inline bool isCivitaiModelPage(const QUrl &url)
{
    const QString host = url.host().toLower();
    const bool civitai = host == "civitai.com" || host == "www.civitai.com"
        || host == "civitai.red" || host == "www.civitai.red";
    const bool modelPage = url.path() == "/models" || url.path().startsWith("/models/");
    return url.isValid() && (url.scheme() == "https" || url.scheme() == "http") && civitai && modelPage;
}

// Resolve a browser destination without modifying metadata or API URLs.
inline QUrl preferredUrl(QUrl url, const QString &civitaiHost)
{
    if (isCivitaiModelPage(url)) {
        url.setScheme("https");
        url.setHost(normalizedCivitaiHost(civitaiHost));
        url.setPort(-1);
        url.setUserInfo(QString());
    }
    return url;
}

}
