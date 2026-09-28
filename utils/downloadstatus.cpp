#include "downloadstatus.h"

namespace DownloadStatus {

QString category(const QString &status)
{
    if (status.contains("已忽略")) return "ignored";
    if (status.contains("旧版共存")) return "coexisting";
    if (status.contains("本地") || status.contains("跳过")) return "local";
    if (status.contains("失败") || status.contains("无法") || status.contains("错误") || status.contains("出错"))
        return "errors";
    if (status.contains("已是最新")) return "latest";
    return "updates";
}

bool isChecking(const QString &status)
{
    return status.startsWith("检查中") || status.startsWith("计算 Hash 中");
}

bool isDownloading(const QString &status)
{
    return status.contains("下载中") || status.contains("认证重试") || status.contains("队列")
           || status.startsWith("校验中");
}

bool isCheckFailure(const QString &status)
{
    return status.startsWith("检查失败:") || status.startsWith("无法从 Hash 匹配")
           || status.startsWith("无法计算 Hash");
}

bool isDownloadFailure(const QString &status)
{
    return status.startsWith("下载失败:") || status.startsWith("失败:");
}

CardAction cardAction(const QString &status, bool hasUpdate)
{
    if (isDownloading(status) || isChecking(status) || status.contains("完成")) return CardAction::Disabled;
    const QString group = category(status);
    if (!hasUpdate || group == "ignored" || group == "latest" || group == "errors" || group == "local")
        return CardAction::Check;
    return CardAction::Download;
}

QString persistedStatus(const QString &status, bool hasUpdate)
{
    if (isChecking(status)) return {};
    return isDownloading(status) ? (hasUpdate ? QStringLiteral("发现新版本") : QStringLiteral("已是最新")) : status;
}

QString preserveIgnored(const QString &previous, const QString &next)
{
    if (previous.contains("已忽略") && !next.contains("下载") && !next.contains("完成") && !next.contains("失败"))
        return QStringLiteral("已忽略更新");
    return next;
}

}
