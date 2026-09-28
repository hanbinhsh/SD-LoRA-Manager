#pragma once

#include <QString>

namespace DownloadStatus {

enum class CardAction { Check, Download, Disabled };

QString category(const QString &status);
bool isChecking(const QString &status);
bool isDownloading(const QString &status);
bool isCheckFailure(const QString &status);
bool isDownloadFailure(const QString &status);
CardAction cardAction(const QString &status, bool hasUpdate);
QString persistedStatus(const QString &status, bool hasUpdate);
QString preserveIgnored(const QString &previous, const QString &next);

}
