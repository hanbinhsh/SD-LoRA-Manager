#include "civitaiclient.h"

#include <QNetworkReply>
#include <QRandomGenerator>
#include <QStringList>
#include <QUrlQuery>

namespace CivitaiClient {

bool isCivitaiUrl(const QUrl &url)
{
    const QString host = url.host().toLower();
    return host == QLatin1String("civitai.com") || host.endsWith(QLatin1String(".civitai.com"));
}

bool shouldUseBearerAuth(const QUrl &url)
{
    if (!isCivitaiUrl(url)) return false;
    const QString host = url.host().toLower();

    // 图片/CDN 链接通常使用 token query 参数认证，Bearer 在这些 host 上容易触发 401。
    if (host == QLatin1String("image.civitai.com") || host == QLatin1String("imagecache.civitai.com")) return false;
    if (host.startsWith(QLatin1String("image.")) || host.contains(QLatin1String("imagecache"))) return false;

    return true;
}

QUrl urlWithToken(const QUrl &url, const QString &apiKey)
{
    const QString key = apiKey.trimmed();
    if (key.isEmpty() || !isCivitaiUrl(url)) return url;

    QUrl out(url);
    QUrlQuery query(out);
    if (query.hasQueryItem(QStringLiteral("token"))) return out;
    query.addQueryItem(QStringLiteral("token"), key);
    out.setQuery(query);
    return out;
}

QNetworkRequest makeNetworkRequest(const QUrl &url,
                                   const QString &userAgent,
                                   const QString &apiKey,
                                   bool allowCivitaiAuth)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(30000); // 30秒超时，防止异常挂起导致队列卡死
#endif
    if (allowCivitaiAuth && isCivitaiUrl(url) && shouldUseBearerAuth(url)) {
        const QString key = apiKey.trimmed();
        if (!key.isEmpty()) {
            request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(key).toUtf8());
        }
    }
    return request;
}

QString networkErrorMessage(QNetworkReply *reply)
{
    if (!reply) return QStringLiteral("未知网络错误");
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString text = reply->errorString();
    if ((status == 401 || status == 403) && isCivitaiUrl(reply->url())) {
        text += QStringLiteral("。请检查 Civitai API Key，或确认该资源是否需要登录权限。");
    }
    if (status > 0) text += QStringLiteral(" (HTTP %1)").arg(status);
    return text;
}

QString randomUserAgent()
{
    static const QStringList uas = {
        // Chrome Win10/11
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/132.0.6834.83 Safari/537.36"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/133.0.6943.50 Safari/537.36"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36"),
        QStringLiteral("Mozilla/5.0 (Windows NT 11.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"),
        // Edge Win10/11
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36 Edg/120.0.0.0"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36 Edg/121.0.0.0"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36 Edg/131.0.0.0"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/132.0.0.0 Safari/537.36 Edg/132.0.0.0"),
        QStringLiteral("Mozilla/5.0 (Windows NT 11.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36 Edg/131.0.2903.99"),
        // Windows Firefox
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:132.0) Gecko/20100101 Firefox/132.0"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:133.0) Gecko/20100101 Firefox/133.0"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:134.0) Gecko/20100101 Firefox/134.0"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:123.0) Gecko/20100101 Firefox/123.0"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:122.0) Gecko/20100101 Firefox/122.0"),
        // macOS
        QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"),
        QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/132.0.0.0 Safari/537.36"),
        QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 14_7_1) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.6 Safari/605.1.15"),
        QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 15_1) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.1 Safari/605.1.15"),
        QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 14.7; rv:132.0) Gecko/20100101 Firefox/132.0"),
        // Linux
        QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"),
        QStringLiteral("Mozilla/5.0 (X11; Linux x86_64; rv:132.0) Gecko/20100101 Firefox/132.0"),
        QStringLiteral("Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:133.0) Gecko/20100101 Firefox/133.0"),
    };
    return uas.at(QRandomGenerator::global()->bounded(uas.size()));
}

} // namespace CivitaiClient
