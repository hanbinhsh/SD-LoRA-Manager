#ifndef SYNCWIDGET_H
#define SYNCWIDGET_H

#include <QWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QFileSystemWatcher>
#include <QMap>
#include <QSet>
#include <QJsonObject>
#include <QFutureWatcher>
#include <atomic>
#include <memory>

#include "utils/foldersyncengine.h"

namespace Ui {
class SyncWidget;
}

class SyncWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SyncWidget(QWidget *parent = nullptr);
    ~SyncWidget();

private slots:
    void on_btnStart_clicked();
    void on_btnAddFolder_clicked();
    
    // 监听UI变化以实时保存设置
    void on_chkAutoStart_toggled(bool checked);
    void on_chkWhitelist_toggled(bool checked);
    void on_editPort_editingFinished();
    void on_editAesKey_editingFinished();

    // 网络与文件监控回调
    void newClientConnected();
    void onClientReadyRead();
    void onDiscoveryReadyRead();
    void handleDirectoryChange(const QString &path);

    // 文件夹同步槽函数
    void onBrowseFolderA();
    void onBrowseFolderB();
    void onSwapFolders();
    void onFolderSyncModeChanged();
    void onPreviewFolderSync();
    void onCompareFolders();
    void onDeleteToTrashClicked();
    void onStartFolderSync();
    void onCancelFolderSync();
    void onClearFolderSyncLog();
    void onFolderPlanFinished();
    void onFolderSyncFinished();
    void onFolderCompareFinished();
    void onFolderTrashFinished();
    void onFolderAutoWatchScanFinished();
    void onAutoFolderSyncToggled(bool checked);
    void onFolderAutoDirectoryChanged(const QString &path);
    void onFolderTimeFilterChanged();
    void onFolderNameFilterChanged();
    void onFolderExtFilterChanged();

private:
    Ui::SyncWidget *ui;

    // 网络与文件监听
    QTcpServer *tcpServer;
    QUdpSocket *udpDiscoverySocket;
    QFileSystemWatcher *watcher;
    QList<QTcpSocket*> clients;
    QMap<QTcpSocket*, quint32> m_clientExpectedSizes;
    QSet<QTcpSocket*> m_authenticatedClients;

    // 目录状态
    QStringList m_rootPaths;
    QMap<QString, QSet<QString>> m_watchedPathsMap;
    QMap<QString, QSet<QString>> m_dirFileState;
    QMap<QString, QSet<QString>> m_dirSubdirState;
    QSet<QString> m_whitelistedDevices;

    bool m_isLoading = false;

    // 文件夹同步状态
    enum class FolderTaskKind {
        Idle,
        Preview,
        SyncPlan,
        SyncExecute,
        Compare,
        TrashDelete
    };

    QFutureWatcher<FolderSyncPlan> *m_folderPlanWatcher = nullptr;
    QFutureWatcher<FolderSyncResult> *m_folderSyncWatcher = nullptr;
    QFutureWatcher<FolderCompareResult> *m_folderCompareWatcher = nullptr;
    QFutureWatcher<TrashDeleteResult> *m_folderTrashWatcher = nullptr;
    QFutureWatcher<QStringList> *m_folderAutoWatchScanWatcher = nullptr;
    QFileSystemWatcher *m_folderAutoWatcher = nullptr;
    QTimer *m_folderAutoDebounceTimer = nullptr;
    QTimer *m_folderAutoCooldownTimer = nullptr;
    QTimer *m_folderLogFlushTimer = nullptr;
    QStringList m_pendingFolderLogLines;
    std::shared_ptr<std::atomic_bool> m_folderSyncCancelFlag;
    std::shared_ptr<std::atomic_bool> m_folderWatchScanCancelFlag;
    FolderTaskKind m_folderTaskKind = FolderTaskKind::Idle;
    bool m_folderAutoSuppressWatchEvents = false;
    bool m_folderPendingPlanIsAuto = false;
    bool m_folderWatchScanTriggerSyncAfter = false;
    int m_folderWatchScanGeneration = 0;
    FolderCompareResult m_lastCompareResult;
    FolderDeleteMode m_lastDeleteMode = FolderDeleteMode::MoveToTrash;

    bool isFolderTaskRunning() const { return m_folderTaskKind != FolderTaskKind::Idle; }
    FolderSyncMode currentFolderSyncMode() const;
    FolderSyncFilter currentFolderSyncFilter() const;
    void updateFolderTimeFilterUi();
    void updateFolderNameFilterUi();
    void updateFolderExtFilterUi();
    void updateFolderSyncModeHint();
    void setFolderSyncUiBusy(bool busy);
    void logFolderSyncMsg(const QString &msg);
    void enqueueFolderSyncLog(const QString &msg);
    void flushFolderSyncLogs();
    void clearLastCompareResult();
    void startFolderPlanAsync(FolderTaskKind targetKind, bool isAuto);
    void executePreparedFolderPlan(const FolderSyncPlan &plan, bool isAuto);
    void startFolderAutoWatchAsync(bool triggerInitialSync = false, bool showWarningDialog = false);
    void stopFolderAutoWatch();
    void refreshFolderAutoWatchIfEnabled();

    // 动态生成带按钮的列表项的辅助函数
    void addFolderItem(const QString &path);
    void addPendingDeviceItem(const QString &deviceId, const QString &displayName);
    void addWhitelistItem(const QString &deviceId);
    void addActiveClientItem(QTcpSocket *client, const QString &displayName);

    // 内嵌按钮触发的槽函数
    void removeFolderByPath(const QString &path);
    void allowPendingDevice(const QString &deviceId, const QString &displayName);
    void removeWhitelistDevice(const QString &deviceId);
    void kickActiveClient(QTcpSocket *client);

    void handleClientDisconnected();

    // 核心功能函数
    QString settingsPath() const;
    void loadSettings();
    void saveSettings();
    void logMsg(const QString &msg);
    void startDiscoveryResponder(quint16 port);
    void stopDiscoveryResponder();
    QString getLocalIPv4AddressForPeer(const QHostAddress &peerAddress) const;
    void addPathRecursive(const QString &rootPath, const QString &path);
    void removePathRecursive(const QString &rootPath);
    void updateDirState(const QString &dirPath);
    QString findRootForPath(const QString &path);
    QString getRelativePathWithRoot(const QString &fullPath, const QString &rootPath);
    bool isImage(const QString &fileName);

    // 协议与通讯
    void sendPacket(QTcpSocket* client, const QJsonObject& metadata, const QByteArray& fileData = QByteArray());
    void processClientManifest(QTcpSocket* client, const QJsonObject& json);
    void sendFile(const QString &filePath, const QString &rootPath);
    void sendDeleteNotification(const QString &relativePath);
    void sendFolderDeleteNotification(const QString &relativePath);

    // AES-GCM (OpenSSL)
    QByteArray encryptAESGCM(const QByteArray &plaintext, const QByteArray &key, QByteArray &iv, QByteArray &tag);
    QByteArray decryptAESGCM(const QByteArray &ciphertext, const QByteArray &key, const QByteArray &iv, const QByteArray &tag, bool &success);
};

#endif // SYNCWIDGET_H
