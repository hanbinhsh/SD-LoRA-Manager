#ifndef SYNCWIDGET_H
#define SYNCWIDGET_H

#include <QWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QFileSystemWatcher>
#include <QSettings>
#include <QMap>
#include <QSet>
#include <QJsonObject>

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
    void handleDirectoryChange(const QString &path);

private:
    Ui::SyncWidget *ui;

    // 网络与文件监听
    QTcpServer *tcpServer;
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
    void loadSettings();
    void saveSettings();
    void logMsg(const QString &msg);
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
