#ifndef MODELUPDATECOORDINATOR_H
#define MODELUPDATECOORDINATOR_H

#include "core/metadatascantypes.h"
#include "core/modelupdateinfo.h"

#include <QJsonObject>
#include <QList>
#include <QNetworkRequest>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QThreadPool;

class ModelUpdateCoordinator : public QObject
{
    Q_OBJECT

public:
    using RequestFactory = std::function<QNetworkRequest(const QUrl &, bool allowCivitaiAuth)>;
    using CoexistenceCheck = std::function<bool(int modelId, int versionId)>;

    ModelUpdateCoordinator(QNetworkAccessManager *netManager,
                           QThreadPool *backgroundThreadPool,
                           RequestFactory requestFactory,
                           QObject *parent = nullptr);

    void startChecks(const QList<UpdateCheckSnapshot> &snapshots, bool detailPrompt);
    void cancelAll();

    static ModelUpdateInfo parseModelUpdateInfo(const UpdateCheckSnapshot &snapshot,
                                                const QJsonObject &modelRoot,
                                                const CoexistenceCheck &hasLocalVersion);

signals:
    void statusChanged(const QString &text);
    void buttonsEnabledChanged(bool enabled);
    void cardUpdated(const ModelUpdateInfo &info, const QString &status);
    void modelRootReceived(const QString &filePath,
                           int matchedVersionId,
                           const QJsonObject &modelRoot,
                           const ModelUpdateInfo &fallbackInfo);
    void allChecksCompleted(int totalCount, bool detailPrompt, const QString &detailFilePath);

private:
    void checkSnapshot(const UpdateCheckSnapshot &snapshot);
    void dispatchQueuedChecks();
    void enqueueHashCheck(const UpdateCheckSnapshot &snapshot);
    void dispatchHashChecks();
    void markCheckFinished();
    void handleReply(QNetworkReply *reply,
                     const UpdateCheckSnapshot &snapshot,
                     int token,
                     bool byHash);

    QNetworkAccessManager *m_netManager = nullptr;
    QThreadPool *m_backgroundThreadPool = nullptr;
    RequestFactory m_requestFactory;

    QQueue<UpdateCheckSnapshot> m_pendingChecksQueue;
    QQueue<UpdateCheckSnapshot> m_pendingHashChecks;
    int m_activeNetworkChecks = 0;
    int m_activeHashChecks = 0;
    int m_pendingChecks = 0;
    int m_completedChecks = 0;
    int m_token = 0;
    QString m_detailFilePath;
    bool m_detailPrompt = false;
};

#endif // MODELUPDATECOORDINATOR_H
