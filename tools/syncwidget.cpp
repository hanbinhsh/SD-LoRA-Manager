#include "utils/applanguage.h"
#include "syncwidget.h"
#include "ui_syncwidget.h"
#include "dialogs/foldersyncdeletedialog.h"
#include "styleconstants.h"
#include <QFileDialog>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDataStream>
#include <QMessageBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QTimer>
#include <QHostInfo>
#include <QNetworkInterface>
#include <QFile>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <QHBoxLayout>
#include <QUrl>
#include <QMetaObject>
#include <QPointer>
#include <QSignalBlocker>
#include <QtConcurrent/QtConcurrent>

SyncWidget::SyncWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::SyncWidget)
{
    ui->setupUi(this);
    setStyleSheet(AppStyle::loadToolPageQss());

    // 初始化网络和文件监控
    tcpServer = new QTcpServer(this);
    udpDiscoverySocket = new QUdpSocket(this);
    watcher = new QFileSystemWatcher(this);

    connect(tcpServer, &QTcpServer::newConnection, this, &SyncWidget::newClientConnected);
    connect(udpDiscoverySocket, &QUdpSocket::readyRead, this, &SyncWidget::onDiscoveryReadyRead);
    connect(watcher, &QFileSystemWatcher::directoryChanged, this, &SyncWidget::handleDirectoryChange);
    connect(ui->btnGetMobileApp, &QPushButton::clicked, this, [this]() {
        const QUrl url("https://github.com/hanbinhsh/SD_Image_Synchronizer_PE/releases/latest");
        if (QDesktopServices::openUrl(url)) {
            logMsg("已打开手机版同步 App 的最新发布页面。");
        } else {
            QMessageBox::warning(this, AppText::tr("打开失败"), AppText::tr("无法调用系统浏览器，请手动访问：\n") + url.toString());
            logMsg("打开手机版同步 App 发布页面失败。");
        }
    });

    // 调整 Splitter 比例
    ui->splitter->setSizes(QList<int>() << 300 << 250 << 400);

    // 初始化文件夹同步控制器与信号连接（全部耗时扫描/同步/对比/删除均走后台线程）
    m_folderPlanWatcher = new QFutureWatcher<FolderSyncPlan>(this);
    connect(m_folderPlanWatcher, &QFutureWatcher<FolderSyncPlan>::finished,
            this, &SyncWidget::onFolderPlanFinished);

    m_folderSyncWatcher = new QFutureWatcher<FolderSyncResult>(this);
    connect(m_folderSyncWatcher, &QFutureWatcher<FolderSyncResult>::finished,
            this, &SyncWidget::onFolderSyncFinished);

    m_folderCompareWatcher = new QFutureWatcher<FolderCompareResult>(this);
    connect(m_folderCompareWatcher, &QFutureWatcher<FolderCompareResult>::finished,
            this, &SyncWidget::onFolderCompareFinished);

    m_folderTrashWatcher = new QFutureWatcher<TrashDeleteResult>(this);
    connect(m_folderTrashWatcher, &QFutureWatcher<TrashDeleteResult>::finished,
            this, &SyncWidget::onFolderTrashFinished);

    m_folderAutoWatchScanWatcher = new QFutureWatcher<QStringList>(this);
    connect(m_folderAutoWatchScanWatcher, &QFutureWatcher<QStringList>::finished,
            this, &SyncWidget::onFolderAutoWatchScanFinished);

    m_folderAutoWatcher = new QFileSystemWatcher(this);
    connect(m_folderAutoWatcher, &QFileSystemWatcher::directoryChanged,
            this, &SyncWidget::onFolderAutoDirectoryChanged);

    m_folderLogFlushTimer = new QTimer(this);
    m_folderLogFlushTimer->setSingleShot(true);
    m_folderLogFlushTimer->setInterval(80);
    connect(m_folderLogFlushTimer, &QTimer::timeout, this, &SyncWidget::flushFolderSyncLogs);

    m_folderAutoDebounceTimer = new QTimer(this);
    m_folderAutoDebounceTimer->setSingleShot(true);
    m_folderAutoDebounceTimer->setInterval(1000);
    connect(m_folderAutoDebounceTimer, &QTimer::timeout, this, [this]() {
        if (ui->chkAutoFolderSync->isChecked() && !isFolderTaskRunning()) {
            startFolderPlanAsync(FolderTaskKind::SyncPlan, true);
        }
    });

    m_folderAutoCooldownTimer = new QTimer(this);
    m_folderAutoCooldownTimer->setSingleShot(true);
    m_folderAutoCooldownTimer->setInterval(500);
    connect(m_folderAutoCooldownTimer, &QTimer::timeout, this, [this]() {
        m_folderAutoSuppressWatchEvents = false;
        if (ui->chkAutoFolderSync->isChecked() && !isFolderTaskRunning()) {
            startFolderAutoWatchAsync(false, false);
        }
    });

    connect(ui->btnBrowseFolderA, &QPushButton::clicked, this, &SyncWidget::onBrowseFolderA);
    connect(ui->btnBrowseFolderB, &QPushButton::clicked, this, &SyncWidget::onBrowseFolderB);
    connect(ui->btnSwapFolders, &QPushButton::clicked, this, &SyncWidget::onSwapFolders);
    connect(ui->btnPreviewFolderSync, &QPushButton::clicked, this, &SyncWidget::onPreviewFolderSync);
    connect(ui->btnCompareFolders, &QPushButton::clicked, this, &SyncWidget::onCompareFolders);
    connect(ui->btnDeleteToTrash, &QPushButton::clicked, this, &SyncWidget::onDeleteToTrashClicked);
    connect(ui->btnStartFolderSync, &QPushButton::clicked, this, &SyncWidget::onStartFolderSync);
    connect(ui->btnCancelFolderSync, &QPushButton::clicked, this, &SyncWidget::onCancelFolderSync);
    connect(ui->btnClearFolderSyncLog, &QPushButton::clicked, this, &SyncWidget::onClearFolderSyncLog);

    connect(ui->radioSyncCopy, &QRadioButton::toggled, this, [this](bool checked) { if (checked) onFolderSyncModeChanged(); });
    connect(ui->radioSyncMove, &QRadioButton::toggled, this, [this](bool checked) { if (checked) onFolderSyncModeChanged(); });
    connect(ui->radioSyncTwoWay, &QRadioButton::toggled, this, [this](bool checked) { if (checked) onFolderSyncModeChanged(); });
    connect(ui->chkSyncSubdirs, &QCheckBox::toggled, this, [this](bool) {
        clearLastCompareResult();
        saveSettings();
        refreshFolderAutoWatchIfEnabled();
    });
    connect(ui->comboFolderTimeFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { onFolderTimeFilterChanged(); });
    connect(ui->dtFolderSyncStart, &QDateTimeEdit::dateTimeChanged,
            this, [this](const QDateTime &) { clearLastCompareResult(); saveSettings(); });
    connect(ui->dtFolderSyncEnd, &QDateTimeEdit::dateTimeChanged,
            this, [this](const QDateTime &) { clearLastCompareResult(); saveSettings(); });
    connect(ui->chkFolderSyncImagesOnly, &QCheckBox::toggled,
            this, [this](bool) { clearLastCompareResult(); saveSettings(); });
    connect(ui->chkFolderRequireMetadata, &QCheckBox::toggled,
            this, [this](bool) { clearLastCompareResult(); saveSettings(); });
    connect(ui->spinFolderMinSizeKB, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int) { clearLastCompareResult(); saveSettings(); });
    connect(ui->spinFolderMaxSizeKB, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int) { clearLastCompareResult(); saveSettings(); });
    connect(ui->comboFolderNameFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { onFolderNameFilterChanged(); });
    connect(ui->editFolderNameKeywords, &QLineEdit::editingFinished,
            this, [this]() { clearLastCompareResult(); saveSettings(); });
    connect(ui->chkAutoFolderSync, &QCheckBox::toggled, this, &SyncWidget::onAutoFolderSyncToggled);
    connect(ui->editFolderA, &QLineEdit::editingFinished, this, [this]() {
        clearLastCompareResult();
        saveSettings();
        refreshFolderAutoWatchIfEnabled();
    });
    connect(ui->editFolderB, &QLineEdit::editingFinished, this, [this]() {
        clearLastCompareResult();
        saveSettings();
        refreshFolderAutoWatchIfEnabled();
    });

    // 加载设置
    loadSettings();
    updateFolderTimeFilterUi();
    updateFolderNameFilterUi();
    updateFolderSyncModeHint();

    // 自动启动服务
    if (ui->chkAutoStart->isChecked()) {
        QTimer::singleShot(500, this, &SyncWidget::on_btnStart_clicked);
    }
    if (ui->chkAutoFolderSync->isChecked()) {
        QTimer::singleShot(500, this, [this]() {
            if (!ui->chkAutoFolderSync->isChecked()) return;
            const QString err = FolderSyncEngine::validateInputs(
                ui->editFolderA->text(), ui->editFolderB->text(), currentFolderSyncFilter());
            if (!err.isEmpty()) {
                const QSignalBlocker blocker(ui->chkAutoFolderSync);
                ui->chkAutoFolderSync->setChecked(false);
                saveSettings();
                logFolderSyncMsg(err);
                return;
            }
            startFolderAutoWatchAsync(true, false);
        });
    }
}

SyncWidget::~SyncWidget()
{
    saveSettings();
    stopFolderAutoWatch();
    if (m_folderLogFlushTimer) m_folderLogFlushTimer->stop();
    if (m_folderSyncCancelFlag) {
        m_folderSyncCancelFlag->store(true);
    }
    if (m_folderWatchScanCancelFlag) {
        m_folderWatchScanCancelFlag->store(true);
    }
    auto waitWatcher = [this](auto *w) {
        if (!w) return;
        w->disconnect(this);
        if (w->isRunning()) w->waitForFinished();
    };
    waitWatcher(m_folderPlanWatcher);
    waitWatcher(m_folderSyncWatcher);
    waitWatcher(m_folderCompareWatcher);
    waitWatcher(m_folderTrashWatcher);
    waitWatcher(m_folderAutoWatchScanWatcher);
    if (watcher) {
        watcher->disconnect(this);
        const QStringList watchedFiles = watcher->files();
        if (!watchedFiles.isEmpty()) watcher->removePaths(watchedFiles);
        const QStringList watchedDirs = watcher->directories();
        if (!watchedDirs.isEmpty()) watcher->removePaths(watchedDirs);
    }
    if (tcpServer) {
        tcpServer->disconnect(this);
        tcpServer->close();
    }
    if (udpDiscoverySocket) {
        udpDiscoverySocket->disconnect(this);
        udpDiscoverySocket->close();
    }
    const QList<QTcpSocket*> sockets = clients;
    for (QTcpSocket *client : sockets) {
        if (!client) continue;
        client->disconnect(this);
        client->abort();
        client->setParent(nullptr);
        delete client;
    }
    clients.clear();
    m_clientExpectedSizes.clear();
    m_authenticatedClients.clear();
    delete ui;
}

void SyncWidget::logMsg(const QString &msg) {
    QString timeStr = QDateTime::currentDateTime().toString("[HH:mm:ss] ");
    ui->logOutput->append(timeStr + msg);
}

// ======================= 设置存取 (立即保存) =======================
void SyncWidget::loadSettings() {
    m_isLoading = true; // 开启加载模式

    QSettings settings("IceRinne", "LoraManager");
    settings.beginGroup("SyncServer");

    ui->chkAutoStart->setChecked(settings.value("autoStart", false).toBool());
    ui->chkWhitelist->setChecked(settings.value("enableWhitelist", false).toBool());
    ui->editPort->setText(settings.value("port", "12345").toString());
    ui->editAesKey->setText(settings.value("aesKey", "").toString());

    QStringList whitelistArr = settings.value("whitelist").toStringList();
    for (const QString &id : whitelistArr) {
        m_whitelistedDevices.insert(id);
        addWhitelistItem(id);
    }

    m_rootPaths = settings.value("rootPaths").toStringList();
    for (const QString &path : m_rootPaths) {
        addFolderItem(path);
        addPathRecursive(path, path);
    }
    settings.endGroup();

    settings.beginGroup("FolderSync");
    ui->editFolderA->setText(settings.value("folderA", "").toString());
    ui->editFolderB->setText(settings.value("folderB", "").toString());
    const int savedMode = settings.value("mode", 0).toInt();
    if (savedMode == 1) {
        ui->radioSyncMove->setChecked(true);
    } else if (savedMode == 2) {
        ui->radioSyncTwoWay->setChecked(true);
    } else {
        ui->radioSyncCopy->setChecked(true);
    }
    ui->chkSyncSubdirs->setChecked(settings.value("includeSubdirs", true).toBool());
    ui->chkAutoFolderSync->setChecked(settings.value("autoSync", false).toBool());
    const int savedTimeMode = qBound(0, settings.value("timeFilterMode", 0).toInt(), 3);
    ui->comboFolderTimeFilter->setCurrentIndex(savedTimeMode);
    const QDateTime defaultStart(QDate::currentDate(), QTime(0, 0, 0));
    const QDateTime defaultEnd(QDate::currentDate(), QTime(23, 59, 59));
    const QDateTime savedStart = QDateTime::fromString(settings.value("timeStartIso").toString(), Qt::ISODate);
    const QDateTime savedEnd = QDateTime::fromString(settings.value("timeEndIso").toString(), Qt::ISODate);
    ui->dtFolderSyncStart->setDateTime(savedStart.isValid() ? savedStart : defaultStart);
    ui->dtFolderSyncEnd->setDateTime(savedEnd.isValid() ? savedEnd : defaultEnd);
    ui->chkFolderSyncImagesOnly->setChecked(settings.value("imagesOnly", false).toBool());
    ui->chkFolderRequireMetadata->setChecked(settings.value("requireMetadata", false).toBool());
    ui->spinFolderMinSizeKB->setValue(qMax(0, settings.value("minSizeKB", 0).toInt()));
    ui->spinFolderMaxSizeKB->setValue(qMax(0, settings.value("maxSizeKB", 0).toInt()));
    ui->comboFolderNameFilter->setCurrentIndex(qBound(0, settings.value("nameFilterMode", 0).toInt(), 2));
    ui->editFolderNameKeywords->setText(settings.value("nameKeywords", "").toString());
    settings.endGroup();

    m_isLoading = false; // 关闭加载模式
}

void SyncWidget::saveSettings() {
    if (m_isLoading) return; // 拦截：如果是正在读取配置时触发的改变，不执行保存！

    QSettings settings("IceRinne", "LoraManager");
    settings.beginGroup("SyncServer");
    settings.setValue("autoStart", ui->chkAutoStart->isChecked());
    settings.setValue("enableWhitelist", ui->chkWhitelist->isChecked());
    settings.setValue("port", ui->editPort->text());
    settings.setValue("aesKey", ui->editAesKey->text());
    settings.setValue("whitelist", QStringList(m_whitelistedDevices.begin(), m_whitelistedDevices.end()));
    settings.setValue("rootPaths", m_rootPaths);
    settings.endGroup();

    settings.beginGroup("FolderSync");
    settings.setValue("folderA", ui->editFolderA->text().trimmed());
    settings.setValue("folderB", ui->editFolderB->text().trimmed());
    settings.setValue("mode", static_cast<int>(currentFolderSyncMode()));
    settings.setValue("includeSubdirs", ui->chkSyncSubdirs->isChecked());
    settings.setValue("autoSync", ui->chkAutoFolderSync->isChecked());
    settings.setValue("timeFilterMode", ui->comboFolderTimeFilter->currentIndex());
    settings.setValue("timeStartIso", ui->dtFolderSyncStart->dateTime().toString(Qt::ISODate));
    settings.setValue("timeEndIso", ui->dtFolderSyncEnd->dateTime().toString(Qt::ISODate));
    settings.setValue("imagesOnly", ui->chkFolderSyncImagesOnly->isChecked());
    settings.setValue("requireMetadata", ui->chkFolderRequireMetadata->isChecked());
    settings.setValue("minSizeKB", ui->spinFolderMinSizeKB->value());
    settings.setValue("maxSizeKB", ui->spinFolderMaxSizeKB->value());
    settings.setValue("nameFilterMode", ui->comboFolderNameFilter->currentIndex());
    settings.setValue("nameKeywords", ui->editFolderNameKeywords->text().trimmed());
    settings.endGroup();
}

// UI 改变即刻触发保存
void SyncWidget::on_chkAutoStart_toggled(bool checked) { Q_UNUSED(checked); saveSettings(); }
void SyncWidget::on_chkWhitelist_toggled(bool checked) { Q_UNUSED(checked); saveSettings(); }
void SyncWidget::on_editPort_editingFinished() { saveSettings(); }
void SyncWidget::on_editAesKey_editingFinished() { saveSettings(); }

// ======================= UI 按钮响应 =======================
void SyncWidget::on_btnStart_clicked() {
    if (tcpServer->isListening()) {
        stopDiscoveryResponder();
        tcpServer->close();
        for (auto client : clients) client->disconnectFromHost();
        ui->btnStart->setText(AppText::tr("启动服务"));
        ui->btnStart->setStyleSheet("");
        logMsg("服务已停止。");
    } else {
        quint16 port = ui->editPort->text().toUShort();
        if (tcpServer->listen(QHostAddress::Any, port)) {
            startDiscoveryResponder(port);
            ui->btnStart->setText(AppText::tr("停止服务"));
            ui->btnStart->setStyleSheet(QString("background-color: %1; color: %2; font-weight: bold;")
                                             .arg(AppStyle::SyncStopRed(), AppStyle::WhiteText()));
            logMsg(QString("服务已启动，监听端口: %1").arg(port));
        } else {
            QMessageBox::critical(this, AppText::tr("错误"), AppText::tr("端口被占用或启动失败"));
        }
    }
}

void SyncWidget::startDiscoveryResponder(quint16 port) {
    stopDiscoveryResponder();

    const bool bound = udpDiscoverySocket->bind(
        QHostAddress::AnyIPv4,
        port,
        QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint
    );
    if (bound) {
        logMsg(QString("UDP 发现服务已启动，监听端口: %1").arg(port));
    } else {
        logMsg("⚠️ UDP 发现服务启动失败，手机端自动扫描可能不可用。");
    }
}

void SyncWidget::stopDiscoveryResponder() {
    if (udpDiscoverySocket && udpDiscoverySocket->state() != QAbstractSocket::UnconnectedState) {
        udpDiscoverySocket->close();
    }
}

QString SyncWidget::getLocalIPv4AddressForPeer(const QHostAddress &peerAddress) const {
    const QHostAddress peerV4(peerAddress.toIPv4Address());
    const quint32 peer = peerV4.toIPv4Address();

    for (const QNetworkInterface &networkInterface : QNetworkInterface::allInterfaces()) {
        if (!(networkInterface.flags() & QNetworkInterface::IsUp)
            || !(networkInterface.flags() & QNetworkInterface::IsRunning)
            || (networkInterface.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }

        for (const QNetworkAddressEntry &entry : networkInterface.addressEntries()) {
            const QHostAddress address = entry.ip();
            if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback()) {
                continue;
            }

            const quint32 ip = address.toIPv4Address();
            const quint32 mask = entry.netmask().toIPv4Address();
            if (mask != 0 && peer != 0 && (ip & mask) == (peer & mask)) {
                return address.toString();
            }
        }
    }

    for (const QHostAddress &address : QNetworkInterface::allAddresses()) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback()) {
            return address.toString();
        }
    }

    return QString();
}

void SyncWidget::onDiscoveryReadyRead() {
    while (udpDiscoverySocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(int(udpDiscoverySocket->pendingDatagramSize()));

        QHostAddress senderAddress;
        quint16 senderPort = 0;
        udpDiscoverySocket->readDatagram(datagram.data(), datagram.size(), &senderAddress, &senderPort);

        const QJsonObject request = QJsonDocument::fromJson(datagram).object();
        if (request["cmd"].toString() != "SD_SYNC_DISCOVER") continue;
        if (request["version"].toInt() != 1) continue;
        if (!tcpServer->isListening()) continue;

        const quint16 port = tcpServer->serverPort();
        const QString localIp = getLocalIPv4AddressForPeer(senderAddress);

        QJsonObject response;
        response["cmd"] = "SD_SYNC_DISCOVER_RESPONSE";
        response["version"] = 1;
        response["app"] = "SD_LoRA_Manager";
        response["name"] = QHostInfo::localHostName();
        response["ip"] = localIp;
        response["port"] = int(port);

        const QByteArray payload = QJsonDocument(response).toJson(QJsonDocument::Compact);
        udpDiscoverySocket->writeDatagram(payload, senderAddress, senderPort);
    }
}

void SyncWidget::on_btnAddFolder_clicked() {
    QString dir = QFileDialog::getExistingDirectory(this, AppText::tr("选择 SD 输出文件夹"));
    if (dir.isEmpty() || m_rootPaths.contains(dir)) return;

    m_rootPaths.append(dir);
    addFolderItem(dir);
    addPathRecursive(dir, dir);
    saveSettings();

    // 立即向已连接客户端发送新图片
    QDirIterator it(dir, QStringList() << "*.png" << "*.jpg" << "*.jpeg" << "*.webp", QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) sendFile(it.next(), dir);
}

// ======================= AES-GCM 加解密 =======================
QByteArray SyncWidget::encryptAESGCM(const QByteArray &plaintext, const QByteArray &key, QByteArray &iv, QByteArray &tag) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return QByteArray();
    int len = 0;
    int ciphertext_len = 0;
    QByteArray ciphertext(plaintext.length() + 16, 0);

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return QByteArray();
    }
    QByteArray paddedKey = key.leftJustified(32, '\0', true);

    if (EVP_EncryptInit_ex(ctx, NULL, NULL,
                           reinterpret_cast<const unsigned char*>(paddedKey.constData()),
                           reinterpret_cast<const unsigned char*>(iv.constData())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return QByteArray();
    }
    if (EVP_EncryptUpdate(ctx,
                          reinterpret_cast<unsigned char*>(ciphertext.data()),
                          &len,
                          reinterpret_cast<const unsigned char*>(plaintext.constData()),
                          plaintext.length()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return QByteArray();
    }
    ciphertext_len = len;
    if (EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(ciphertext.data()) + ciphertext_len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return QByteArray();
    }
    ciphertext_len += len;

    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data());
    EVP_CIPHER_CTX_free(ctx);

    return ciphertext.left(ciphertext_len);
}

QByteArray SyncWidget::decryptAESGCM(const QByteArray &ciphertext, const QByteArray &key, const QByteArray &iv, const QByteArray &tag, bool &success) {
    success = false;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return QByteArray();
    int len = 0;
    int plaintext_len = 0;
    QByteArray plaintext(ciphertext.length() + 16, 0);

    QByteArray paddedKey = key.leftJustified(32, '\0', true);

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1
        || EVP_DecryptInit_ex(ctx, NULL, NULL,
                              reinterpret_cast<const unsigned char*>(paddedKey.constData()),
                              reinterpret_cast<const unsigned char*>(iv.constData())) != 1
        || EVP_DecryptUpdate(ctx,
                             reinterpret_cast<unsigned char*>(plaintext.data()),
                             &len,
                             reinterpret_cast<const unsigned char*>(ciphertext.constData()),
                             ciphertext.length()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return QByteArray();
    }
    plaintext_len = len;

    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<char*>(tag.constData()));
    int ret = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(plaintext.data()) + plaintext_len, &len);

    success = (ret > 0);
    EVP_CIPHER_CTX_free(ctx);

    if (!success) return QByteArray();
    return plaintext.left(plaintext_len + len);
}

// ======================= 通讯协议 =======================
void SyncWidget::sendPacket(QTcpSocket* client, const QJsonObject& metadata, const QByteArray& fileData) {
    if (!client || client->state() != QAbstractSocket::ConnectedState) return;

    QByteArray jsonBytes = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    quint32 jsonLen = jsonBytes.size();
    quint32 dataLen = fileData.size();
    quint32 totalLen = 4 + jsonLen + dataLen;

    QByteArray plainPacket;
    QDataStream plainOut(&plainPacket, QIODevice::WriteOnly);
    plainOut.setByteOrder(QDataStream::BigEndian);
    plainOut << totalLen << jsonLen;
    plainOut.writeRawData(jsonBytes.constData(), jsonLen);
    if (dataLen > 0) plainOut.writeRawData(fileData.constData(), dataLen);

    QByteArray aesKey = ui->editAesKey->text().toUtf8();
    QByteArray iv(12, 0); RAND_bytes((unsigned char*)iv.data(), 12);
    QByteArray tag(16, 0);

    QByteArray cipher = encryptAESGCM(plainPacket, aesKey, iv, tag);

    quint32 cipherPayloadLen = cipher.size() + 12 + 16;
    QByteArray finalPacket;
    QDataStream out(&finalPacket, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::BigEndian);
    
    out << cipherPayloadLen;
    out.writeRawData(iv.data(), 12);
    out.writeRawData(tag.data(), 16);
    out.writeRawData(cipher.data(), cipher.size());

    client->write(finalPacket);
    client->flush();
}

void SyncWidget::newClientConnected() {
    QTcpSocket *client = tcpServer->nextPendingConnection();
    clients << client;
    m_clientExpectedSizes[client] = 0;

    QString clientInfo = client->peerAddress().toString().remove("::ffff:") + ":" + QString::number(client->peerPort());
    logMsg("🔌 建立物理连接: " + clientInfo + " (等待认证...)");

    connect(client, &QTcpSocket::readyRead, this, &SyncWidget::onClientReadyRead);

    connect(client, &QTcpSocket::disconnected, this, &SyncWidget::handleClientDisconnected);
}

void SyncWidget::onClientReadyRead() {
    QTcpSocket *client = qobject_cast<QTcpSocket*>(sender());
    if (!client) return;

    QDataStream in(client);
    in.setByteOrder(QDataStream::BigEndian);

    while (true) {
        if (m_clientExpectedSizes[client] == 0) {
            if (client->bytesAvailable() < 4) return;
            in >> m_clientExpectedSizes[client];
        }

        quint32 expectedSize = m_clientExpectedSizes[client];
        if (expectedSize < 28 || expectedSize > 512u * 1024u * 1024u) {
            logMsg("⚠️ 警告: 收到异常同步数据包长度，已断开客户端。");
            m_clientExpectedSizes[client] = 0;
            client->disconnectFromHost();
            return;
        }
        if (client->bytesAvailable() < expectedSize) return;

        QByteArray iv = client->read(12);
        QByteArray tag = client->read(16);
        QByteArray ciphertext = client->read(expectedSize - 28);
        m_clientExpectedSizes[client] = 0;

        bool decSuccess = false;
        QByteArray aesKey = ui->editAesKey->text().toUtf8();
        QByteArray plaintext = decryptAESGCM(ciphertext, aesKey, iv, tag, decSuccess);

        if (!decSuccess) {
            logMsg("⚠️ 警告: 客户端加密认证失败，可能密钥错误，已断开！");
            client->disconnectFromHost();
            return;
        }

        if (plaintext.size() < 8) {
            logMsg("⚠️ 警告: 同步数据包明文头不完整，已断开客户端。");
            client->disconnectFromHost();
            return;
        }

        QDataStream plainIn(plaintext);
        plainIn.setByteOrder(QDataStream::BigEndian);
        quint32 plainTotalLen = 0;
        quint32 jsonLen = 0;
        plainIn >> plainTotalLen >> jsonLen;

        // plainTotalLen is encoded as sizeof(jsonLen) + JSON + optional data.
        const quint64 actualPayloadLength = static_cast<quint64>(plaintext.size() - 4);
        const quint64 jsonEnd = 8ull + static_cast<quint64>(jsonLen);
        if (plainIn.status() != QDataStream::Ok
            || static_cast<quint64>(plainTotalLen) != actualPayloadLength
            || jsonEnd > static_cast<quint64>(plaintext.size())) {
            logMsg("⚠️ 警告: 同步数据包长度字段无效，已断开客户端。");
            client->disconnectFromHost();
            return;
        }

        QByteArray jsonBytes = plaintext.mid(8, jsonLen);
        QJsonParseError jsonError;
        const QJsonDocument jsonDocument = QJsonDocument::fromJson(jsonBytes, &jsonError);
        if (jsonError.error != QJsonParseError::NoError || !jsonDocument.isObject()) {
            logMsg("⚠️ 警告: 同步数据包 JSON 无效，已断开客户端。");
            client->disconnectFromHost();
            return;
        }
        QJsonObject json = jsonDocument.object();
        QString cmd = json["cmd"].toString();

        if (cmd == "AUTH") {
            QString deviceId = json["deviceId"].toString();
            QString deviceName = json["deviceName"].toString();
            QString displayName = deviceName + " (" + deviceId + ")";

            // === 1. 白名单拦截逻辑 ===
            if (ui->chkWhitelist->isChecked() && !m_whitelistedDevices.contains(deviceId)) {
                logMsg("🚫 拦截未授权设备: " + displayName);
                // 【核心：添加到待审核 UI】
                addPendingDeviceItem(deviceId, displayName);
                client->disconnectFromHost();
                return;
            }

            // === 2. 认证成功逻辑 ===
            logMsg("✅ 设备认证成功: " + displayName);
            m_authenticatedClients.insert(client);

            // 【核心：添加到已连接 UI】
            QString ip = client->peerAddress().toString().remove("::ffff:");
            QString activeName = displayName + " [" + ip + "]";
            addActiveClientItem(client, activeName);

            // 发送文件夹列表
            QJsonArray folders;
            for(const QString &path : m_rootPaths) folders.append(QFileInfo(path).fileName());
            QJsonObject respJson; respJson["cmd"] = "FOLDER_LIST"; respJson["folders"] = folders;
            sendPacket(client, respJson);
        }
        else if (cmd == "CLIENT_MANIFEST") {
            if (!m_authenticatedClients.contains(client)) return;
            processClientManifest(client, json);
        }
    }
}

// ======================= 文件与目录管理 =======================
QString SyncWidget::findRootForPath(const QString &path) {
    const QString normalizedPath = QDir::cleanPath(
        QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()));
    QString bestMatch;
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity pathCaseSensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity pathCaseSensitivity = Qt::CaseSensitive;
#endif
    for (const QString &root : m_rootPaths) {
        const QString normalizedRoot = QDir::cleanPath(
            QDir::fromNativeSeparators(QFileInfo(root).absoluteFilePath()));
        if (normalizedRoot.isEmpty()) continue;
        const bool exactMatch = normalizedPath.compare(normalizedRoot, pathCaseSensitivity) == 0;
        const bool childMatch = normalizedPath.startsWith(normalizedRoot + '/', pathCaseSensitivity);
        if ((exactMatch || childMatch) && normalizedRoot.size() > bestMatch.size()) {
            bestMatch = root;
        }
    }
    return bestMatch;
}
QString SyncWidget::getRelativePathWithRoot(const QString &fullPath, const QString &rootPath) {
    QDir rootDir(rootPath);
    QString rootFolderName = QFileInfo(rootPath).fileName();
    return rootFolderName + "/" + rootDir.relativeFilePath(fullPath).replace("\\", "/");
}
bool SyncWidget::isImage(const QString &fileName) {
    static QStringList filters = {"*.png", "*.jpg", "*.jpeg", "*.webp"};
    for (const QString &f : filters) if (QDir::match(f, fileName.toLower())) return true;
    return false;
}

void SyncWidget::sendFile(const QString &filePath, const QString &rootPath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return;
    QByteArray data = file.readAll();
    file.close();

    QJsonObject json; json["cmd"] = "SYNC";
    json["path"] = getRelativePathWithRoot(filePath, rootPath);
    json["size"] = data.size();

    for (QTcpSocket *client : m_authenticatedClients) sendPacket(client, json, data);
}
void SyncWidget::sendDeleteNotification(const QString &relativePath) {
    QJsonObject json; json["cmd"] = "DELETE"; json["path"] = relativePath;
    for (QTcpSocket *client : m_authenticatedClients) sendPacket(client, json);
}
void SyncWidget::sendFolderDeleteNotification(const QString &relativePath) {
    QJsonObject json; json["cmd"] = "DELETE_FOLDER"; json["path"] = relativePath;
    for (QTcpSocket *client : m_authenticatedClients) sendPacket(client, json);
}

void SyncWidget::processClientManifest(QTcpSocket* client, const QJsonObject& json) {
    QJsonObject filesObj = json["files"].toObject();
    logMsg(QString("[Sync] 收到客户端 Manifest，文件数: %1").arg(filesObj.size()));
    int sentCount = 0;
    for (const QString &rootPath : m_rootPaths) {
        QDirIterator it(rootPath, QStringList() << "*.png" << "*.jpg" << "*.jpeg" << "*.webp", QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString fullPath = it.next();
            QString relativePath = getRelativePathWithRoot(fullPath, rootPath);
            bool needSend = !filesObj.contains(relativePath) || filesObj[relativePath].toVariant().toLongLong() != QFileInfo(fullPath).size();
            if (needSend) {
                sendFile(fullPath, rootPath);
                sentCount++;
                if (sentCount % 10 == 0) client->flush();
            }
        }
    }
    logMsg(QString("[Sync] 差异比对完成，共发送 %1 个更新文件。").arg(sentCount));
}

void SyncWidget::addPathRecursive(const QString &rootPath, const QString &path) {
    if (!m_watchedPathsMap.contains(rootPath)) m_watchedPathsMap[rootPath] = QSet<QString>();
    QDirIterator it(path, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    if(!m_watchedPathsMap[rootPath].contains(path)) {
        if (watcher->addPath(path)) {
            m_watchedPathsMap[rootPath].insert(path);
            updateDirState(path);
        } else {
            logMsg("⚠️ 无法监控目录: " + path);
        }
    }
    while (it.hasNext()) {
        QString subDir = it.next();
        if(!m_watchedPathsMap[rootPath].contains(subDir)) {
            if (watcher->addPath(subDir)) {
                m_watchedPathsMap[rootPath].insert(subDir);
                updateDirState(subDir);
            } else {
                logMsg("⚠️ 无法监控目录: " + subDir);
            }
        }
    }
}
void SyncWidget::removePathRecursive(const QString &rootPath) {
    if (!m_watchedPathsMap.contains(rootPath)) return;
    for (const QString &path : m_watchedPathsMap[rootPath]) watcher->removePath(path);
    m_watchedPathsMap.remove(rootPath);
}
void SyncWidget::updateDirState(const QString &dirPath) {
    QDir dir(dirPath);
    dir.setFilter(QDir::Files | QDir::NoDotAndDotDot);
    QStringList fileList = dir.entryList();
    m_dirFileState[dirPath] = QSet<QString>(fileList.begin(), fileList.end());

    dir.setFilter(QDir::Dirs | QDir::NoDotAndDotDot);
    QStringList subDirList = dir.entryList();
    m_dirSubdirState[dirPath] = QSet<QString>(subDirList.begin(), subDirList.end());
}
void SyncWidget::handleDirectoryChange(const QString &path) {
    QString rootPath = findRootForPath(path);
    if (rootPath.isEmpty()) return;
    QDir dir(path);

    // 处理文件
    dir.setFilter(QDir::Files | QDir::NoDotAndDotDot);
    QStringList currentFileList = dir.entryList();
    QSet<QString> currentFileSet(currentFileList.begin(), currentFileList.end());
    if (!m_dirFileState.contains(path)) {
        m_dirFileState[path] = currentFileSet;
        updateDirState(path);
        return;
    }
    QSet<QString> oldFileSet = m_dirFileState.value(path);
    
    QSet<QString> addedFiles = currentFileSet - oldFileSet;
    for (const QString &fileName : addedFiles) {
        QString fullPath = dir.absoluteFilePath(fileName);
        if (isImage(fileName) && QFileInfo(fullPath).size() > 0) sendFile(fullPath, rootPath);
    }
    QSet<QString> deletedFiles = oldFileSet - currentFileSet;
    for (const QString &fileName : deletedFiles) {
        if (isImage(fileName)) {
            sendDeleteNotification(getRelativePathWithRoot(path + "/" + fileName, rootPath));
        }
    }
    m_dirFileState[path] = currentFileSet;

    // 处理文件夹
    dir.setFilter(QDir::Dirs | QDir::NoDotAndDotDot);
    QStringList currentSubDirList = dir.entryList();
    QSet<QString> currentSubdirSet(currentSubDirList.begin(), currentSubDirList.end());
    QSet<QString> oldSubdirSet = m_dirSubdirState.value(path);
    
    QSet<QString> addedSubdirs = currentSubdirSet - oldSubdirSet;
    for (const QString &subdirName : addedSubdirs) {
        QString fullPath = path + "/" + subdirName;
        if (!m_watchedPathsMap[rootPath].contains(fullPath)) addPathRecursive(rootPath, fullPath);
    }
    QSet<QString> deletedSubdirs = oldSubdirSet - currentSubdirSet;
    for (const QString &subdirName : deletedSubdirs) {
        sendFolderDeleteNotification(getRelativePathWithRoot(path + "/" + subdirName, rootPath));
    }
    m_dirSubdirState[path] = currentSubdirSet;
}

// ===========================列表创建===============================
// 1. 文件夹项
void SyncWidget::addFolderItem(const QString &path) {
    QListWidgetItem *item = new QListWidgetItem(ui->listFolders);
    item->setSizeHint(QSize(0, 42)); // 保持行高

    QWidget *widget = new QWidget();
    widget->setObjectName("rowWidget"); // 【关键新增】：绑定 QSS 样式

    QHBoxLayout *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(10, 0, 10, 0); // 左右留10，上下靠 item 大小撑起即可
    layout->setSpacing(10);

    QLabel *lblPath = new QLabel(path);
    lblPath->setToolTip(path);
    lblPath->setStyleSheet(QString("color: %1; background-color: transparent;").arg(AppStyle::MutedText()));
    lblPath->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    QPushButton *btnRemove = new QPushButton(AppText::tr("X"));
    btnRemove->setCursor(Qt::PointingHandCursor);
    btnRemove->setStyleSheet(AppStyle::dangerButtonStyle());

    layout->addWidget(lblPath, 1);
    layout->addWidget(btnRemove, 0);

    ui->listFolders->setItemWidget(item, widget);
    item->setData(Qt::UserRole, path);

    connect(btnRemove, &QPushButton::clicked, this, [this, path, item]() {
        removeFolderByPath(path);
        delete ui->listFolders->takeItem(ui->listFolders->row(item));
    });
}

// 2. 待审核设备项
void SyncWidget::addPendingDeviceItem(const QString &deviceId, const QString &displayName) {
    for (int i = 0; i < ui->listDevices->count(); ++i) {
        if (ui->listDevices->item(i)->data(Qt::UserRole).toString() == deviceId) return;
    }

    QListWidgetItem *item = new QListWidgetItem(ui->listDevices);
    item->setSizeHint(QSize(0, 42));

    QWidget *widget = new QWidget();
    widget->setObjectName("rowWidget"); // 【关键新增】

    QHBoxLayout *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(10, 0, 10, 0);
    layout->setSpacing(10);

    QLabel *lblName = new QLabel(displayName);
    lblName->setToolTip(displayName);
    lblName->setStyleSheet(QString("color: %1; font-weight: bold; background-color: transparent;").arg(AppStyle::WarningYellow()));
    lblName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    QPushButton *btnAllow = new QPushButton(AppText::tr("允许"));
    btnAllow->setCursor(Qt::PointingHandCursor);
    btnAllow->setStyleSheet(AppStyle::allowButtonStyle());

    layout->addWidget(lblName, 1);
    layout->addWidget(btnAllow, 0);

    ui->listDevices->setItemWidget(item, widget);
    item->setData(Qt::UserRole, deviceId);

    connect(btnAllow, &QPushButton::clicked, this, [this, deviceId, displayName, item]() {
        allowPendingDevice(deviceId, displayName);
        delete ui->listDevices->takeItem(ui->listDevices->row(item));
    });
}

// 3. 白名单项
void SyncWidget::addWhitelistItem(const QString &deviceId) {
    QListWidgetItem *item = new QListWidgetItem(ui->listWhitelist);
    item->setSizeHint(QSize(0, 42));

    QWidget *widget = new QWidget();
    widget->setObjectName("rowWidget"); // 【关键新增】

    QHBoxLayout *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(10, 0, 10, 0);
    layout->setSpacing(10);

    QLabel *lblId = new QLabel(deviceId);
    lblId->setToolTip(deviceId);
    lblId->setStyleSheet(QString("color: %1; background-color: transparent;").arg(AppStyle::MutedText()));
    lblId->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    QPushButton *btnRemove = new QPushButton(AppText::tr("X"));
    btnRemove->setCursor(Qt::PointingHandCursor);
    btnRemove->setStyleSheet(AppStyle::dangerButtonStyle());

    layout->addWidget(lblId, 1);
    layout->addWidget(btnRemove, 0);

    ui->listWhitelist->setItemWidget(item, widget);
    item->setData(Qt::UserRole, deviceId);

    connect(btnRemove, &QPushButton::clicked, this, [this, deviceId, item]() {
        removeWhitelistDevice(deviceId);
        delete ui->listWhitelist->takeItem(ui->listWhitelist->row(item));
    });
}

// 4. 已连接设备项
void SyncWidget::addActiveClientItem(QTcpSocket *client, const QString &displayName) {
    QListWidgetItem *item = new QListWidgetItem(ui->listActive);
    item->setSizeHint(QSize(0, 42));

    QWidget *widget = new QWidget();
    widget->setObjectName("rowWidget"); // 【关键新增】

    QHBoxLayout *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(10, 0, 10, 0);
    layout->setSpacing(10);

    QLabel *lblName = new QLabel(displayName);
    lblName->setToolTip(displayName);
    lblName->setStyleSheet(QString("color: %1; font-weight: bold; background-color: transparent;").arg(AppStyle::AccentBlue()));
    lblName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    QPushButton *btnKick = new QPushButton(AppText::tr("断开"));
    btnKick->setCursor(Qt::PointingHandCursor);
    btnKick->setStyleSheet(AppStyle::dangerButtonStyle());

    layout->addWidget(lblName, 1);
    layout->addWidget(btnKick, 0);

    ui->listActive->setItemWidget(item, widget);
    item->setData(Qt::UserRole, QVariant::fromValue((void*)client));

    connect(btnKick, &QPushButton::clicked, this, [this, client]() {
        kickActiveClient(client);
        if(QPushButton* btn = qobject_cast<QPushButton*>(sender())) btn->setEnabled(false);
    });
}
// ============按钮点击==============
void SyncWidget::removeFolderByPath(const QString &path) {
    m_rootPaths.removeAll(path);
    removePathRecursive(path);
    saveSettings();
    logMsg("已移除监控文件夹: " + path);
}

void SyncWidget::allowPendingDevice(const QString &deviceId, const QString &displayName) {
    if (!m_whitelistedDevices.contains(deviceId)) {
        m_whitelistedDevices.insert(deviceId);
        addWhitelistItem(deviceId); // 也可以把 displayName 一起存下来显示
        saveSettings();
        logMsg("🛡️ 已添加设备到白名单: " + displayName);
    }
}

void SyncWidget::removeWhitelistDevice(const QString &deviceId) {
    m_whitelistedDevices.remove(deviceId);
    saveSettings();
    logMsg("🛡️ 已移除白名单设备: " + deviceId);

    // 可选：如果该设备当前正连着，是否强制踢出？
    for (QTcpSocket *client : m_authenticatedClients) {
        // 如果你需要踢出，需要在 authenticated 列表里找到对应 socket 并 disconnect
        // 因为目前 socket 没有直接绑定 deviceId，你可能需要在 onClientReadyRead 里额外记录
    }
}

void SyncWidget::kickActiveClient(QTcpSocket *client) {
    if (client && client->state() == QAbstractSocket::ConnectedState) {
        logMsg("🔌 强制断开客户端连接...");
        client->disconnectFromHost();
    }
}

void SyncWidget::handleClientDisconnected() {
    QTcpSocket *client = qobject_cast<QTcpSocket*>(sender());
    if (!client) return;

    // 遍历【已连接列表】，找到对应的 socket 并从 UI 删除
    for(int i = 0; i < ui->listActive->count(); ++i) {
        if (ui->listActive->item(i)->data(Qt::UserRole).value<void*>() == client) {
            delete ui->listActive->takeItem(i);
            break;
        }
    }

    m_authenticatedClients.remove(client);
    clients.removeAll(client);
    client->deleteLater();
    logMsg("🔌 客户端已断开连接");
}

// ======================= 文件夹同步 =======================
namespace {
QString formatSyncByteSize(qint64 bytes)
{
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024LL * 1024LL) return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024LL * 1024LL) return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
    return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

constexpr int kMaxAutoWatchDirs = 1500;
} // namespace

FolderSyncMode SyncWidget::currentFolderSyncMode() const
{
    if (ui->radioSyncMove->isChecked()) return FolderSyncMode::OneWayMove;
    if (ui->radioSyncTwoWay->isChecked()) return FolderSyncMode::TwoWaySync;
    return FolderSyncMode::OneWayCopy;
}

FolderSyncFilter SyncWidget::currentFolderSyncFilter() const
{
    FolderSyncFilter filter;
    const int idx = ui->comboFolderTimeFilter->currentIndex();
    if (idx == 1) filter.timeMode = FolderSyncTimeFilterMode::Before;
    else if (idx == 2) filter.timeMode = FolderSyncTimeFilterMode::After;
    else if (idx == 3) filter.timeMode = FolderSyncTimeFilterMode::Between;
    else filter.timeMode = FolderSyncTimeFilterMode::All;

    filter.startTime = ui->dtFolderSyncStart->dateTime();
    filter.endTime = ui->dtFolderSyncEnd->dateTime();
    filter.imagesOnly = ui->chkFolderSyncImagesOnly->isChecked();
    filter.requireImageMetadata = ui->chkFolderRequireMetadata->isChecked();
    filter.minSizeKB = ui->spinFolderMinSizeKB->value();
    filter.maxSizeKB = ui->spinFolderMaxSizeKB->value();

    const int nameIdx = ui->comboFolderNameFilter->currentIndex();
    if (nameIdx == 1) filter.nameMode = FolderSyncNameFilterMode::ExcludeContains;
    else if (nameIdx == 2) filter.nameMode = FolderSyncNameFilterMode::IncludeContains;
    else filter.nameMode = FolderSyncNameFilterMode::None;
    filter.nameKeywords = FolderSyncEngine::parseNameKeywords(ui->editFolderNameKeywords->text());

    return filter;
}

void SyncWidget::updateFolderTimeFilterUi()
{
    const int idx = ui->comboFolderTimeFilter->currentIndex();
    const bool showStart = (idx == 2 || idx == 3);
    const bool showEnd = (idx == 1 || idx == 3);
    ui->lblFolderTimeStart->setVisible(showStart);
    ui->dtFolderSyncStart->setVisible(showStart);
    ui->lblFolderTimeEnd->setVisible(showEnd);
    ui->dtFolderSyncEnd->setVisible(showEnd);
}

void SyncWidget::updateFolderNameFilterUi()
{
    const bool enabled = (ui->comboFolderNameFilter->currentIndex() != 0);
    ui->editFolderNameKeywords->setEnabled(enabled && !isFolderTaskRunning());
}

void SyncWidget::onFolderTimeFilterChanged()
{
    updateFolderTimeFilterUi();
    clearLastCompareResult();
    saveSettings();
}

void SyncWidget::onFolderNameFilterChanged()
{
    updateFolderNameFilterUi();
    clearLastCompareResult();
    saveSettings();
}

void SyncWidget::updateFolderSyncModeHint()
{
    const FolderSyncMode mode = currentFolderSyncMode();
    if (mode == FolderSyncMode::OneWayCopy) {
        ui->lblFolderSyncModeHint->setText(
            AppText::tr("将文件夹 A 中新增或更新的文件复制到文件夹 B，保留 A 中的原始文件。"));
    } else if (mode == FolderSyncMode::OneWayMove) {
        ui->lblFolderSyncModeHint->setText(
            AppText::tr("将文件夹 A 中的文件移动到文件夹 B（移动成功后移除 A 中的源文件并清理空子目录）。"));
    } else {
        ui->lblFolderSyncModeHint->setText(
            AppText::tr("双向对比文件夹 A 与文件夹 B：自动补齐缺失文件，并以修改时间较新的一侧更新另一侧。"));
    }
}

void SyncWidget::clearLastCompareResult()
{
    m_lastCompareResult = FolderCompareResult{};
    if (ui && ui->btnDeleteToTrash) {
        ui->btnDeleteToTrash->setEnabled(false);
    }
}

void SyncWidget::setFolderSyncUiBusy(bool busy)
{
    ui->editFolderA->setEnabled(!busy);
    ui->editFolderB->setEnabled(!busy);
    ui->btnBrowseFolderA->setEnabled(!busy);
    ui->btnBrowseFolderB->setEnabled(!busy);
    ui->btnSwapFolders->setEnabled(!busy);
    ui->radioSyncCopy->setEnabled(!busy);
    ui->radioSyncMove->setEnabled(!busy);
    ui->radioSyncTwoWay->setEnabled(!busy);
    ui->chkSyncSubdirs->setEnabled(!busy);
    ui->comboFolderTimeFilter->setEnabled(!busy);
    ui->dtFolderSyncStart->setEnabled(!busy);
    ui->dtFolderSyncEnd->setEnabled(!busy);
    ui->chkFolderSyncImagesOnly->setEnabled(!busy);
    ui->chkFolderRequireMetadata->setEnabled(!busy);
    ui->spinFolderMinSizeKB->setEnabled(!busy);
    ui->spinFolderMaxSizeKB->setEnabled(!busy);
    ui->comboFolderNameFilter->setEnabled(!busy);
    ui->editFolderNameKeywords->setEnabled(!busy && ui->comboFolderNameFilter->currentIndex() != 0);
    ui->btnPreviewFolderSync->setEnabled(!busy);
    ui->btnCompareFolders->setEnabled(!busy);
    ui->btnDeleteToTrash->setEnabled(!busy && m_lastCompareResult.deletableCandidateCount() > 0);
    ui->btnStartFolderSync->setEnabled(!busy);
    ui->btnCancelFolderSync->setEnabled(busy);
}

void SyncWidget::logFolderSyncMsg(const QString &msg)
{
    enqueueFolderSyncLog(msg);
    flushFolderSyncLogs();
}

void SyncWidget::enqueueFolderSyncLog(const QString &msg)
{
    const QString timeStr = QDateTime::currentDateTime().toString("[HH:mm:ss] ");
    m_pendingFolderLogLines.append(timeStr + msg);
    if (m_folderLogFlushTimer && !m_folderLogFlushTimer->isActive()) {
        m_folderLogFlushTimer->start();
    }
}

void SyncWidget::flushFolderSyncLogs()
{
    if (m_pendingFolderLogLines.isEmpty() || !ui || !ui->folderSyncLogOutput) return;
    if (m_folderLogFlushTimer) m_folderLogFlushTimer->stop();
    const QString batch = m_pendingFolderLogLines.join(QLatin1Char('\n'));
    m_pendingFolderLogLines.clear();
    ui->folderSyncLogOutput->append(batch);
}

void SyncWidget::onBrowseFolderA()
{
    const QString current = ui->editFolderA->text().trimmed();
    const QString dir = QFileDialog::getExistingDirectory(this, AppText::tr("选择文件夹 A（源目录）"), current);
    if (dir.isEmpty()) return;
    ui->editFolderA->setText(dir);
    clearLastCompareResult();
    saveSettings();
    refreshFolderAutoWatchIfEnabled();
}

void SyncWidget::onBrowseFolderB()
{
    const QString current = ui->editFolderB->text().trimmed();
    const QString dir = QFileDialog::getExistingDirectory(this, AppText::tr("选择文件夹 B（目标目录）"), current);
    if (dir.isEmpty()) return;
    ui->editFolderB->setText(dir);
    clearLastCompareResult();
    saveSettings();
    refreshFolderAutoWatchIfEnabled();
}

void SyncWidget::onSwapFolders()
{
    const QString a = ui->editFolderA->text();
    const QString b = ui->editFolderB->text();
    ui->editFolderA->setText(b);
    ui->editFolderB->setText(a);
    clearLastCompareResult();
    saveSettings();
    refreshFolderAutoWatchIfEnabled();
}

void SyncWidget::onFolderSyncModeChanged()
{
    updateFolderSyncModeHint();
    saveSettings();
    refreshFolderAutoWatchIfEnabled();
}

void SyncWidget::onClearFolderSyncLog()
{
    m_pendingFolderLogLines.clear();
    if (m_folderLogFlushTimer) m_folderLogFlushTimer->stop();
    ui->folderSyncLogOutput->clear();
    if (!isFolderTaskRunning()) {
        ui->progressFolderSync->setRange(0, 100);
        ui->progressFolderSync->setValue(0);
        ui->lblFolderSyncSummary->setText(AppText::tr("就绪"));
    }
}

void SyncWidget::onPreviewFolderSync()
{
    startFolderPlanAsync(FolderTaskKind::Preview, false);
}

void SyncWidget::onStartFolderSync()
{
    startFolderPlanAsync(FolderTaskKind::SyncPlan, false);
}

void SyncWidget::startFolderPlanAsync(FolderTaskKind targetKind, bool isAuto)
{
    if (isFolderTaskRunning()) return;

    const QString folderA = ui->editFolderA->text();
    const QString folderB = ui->editFolderB->text();
    const FolderSyncMode mode = currentFolderSyncMode();
    const bool includeSubdirs = ui->chkSyncSubdirs->isChecked();
    const FolderSyncFilter filter = currentFolderSyncFilter();

    const QString validationErr = FolderSyncEngine::validateInputs(folderA, folderB, filter);
    if (!validationErr.isEmpty()) {
        if (!isAuto) {
            QMessageBox::warning(this, AppText::tr("参数无效"), validationErr);
        }
        logFolderSyncMsg(validationErr);
        return;
    }

    if (!isAuto) {
        saveSettings();
    }

    m_folderTaskKind = targetKind;
    m_folderPendingPlanIsAuto = isAuto;
    setFolderSyncUiBusy(true);
    ui->progressFolderSync->setRange(0, 0); // indeterminate progress while scanning
    ui->lblFolderSyncSummary->setText(
        targetKind == FolderTaskKind::Preview
            ? AppText::tr("正在扫描并预览变更...")
            : AppText::tr("正在扫描并分析差异..."));
    if (!isAuto) {
        logFolderSyncMsg(targetKind == FolderTaskKind::Preview
                             ? AppText::tr("开始扫描目录以预览变更...")
                             : AppText::tr("开始扫描目录以计算同步计划..."));
    }

    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    m_folderSyncCancelFlag = cancelFlag;
    QPointer<SyncWidget> self(this);

    auto progressCb = [self](int done, int total, const QString &status) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, done, total, status]() {
            if (!self || !self->isFolderTaskRunning()) return;
            if (total > 0) {
                self->ui->progressFolderSync->setRange(0, total);
                self->ui->progressFolderSync->setValue(done);
            }
            self->ui->lblFolderSyncSummary->setText(status);
        }, Qt::QueuedConnection);
    };

    QFuture<FolderSyncPlan> future = QtConcurrent::run(
        [folderA, folderB, mode, includeSubdirs, filter, progressCb, cancelFlag]() {
            return FolderSyncEngine::planSync(
                folderA, folderB, mode, includeSubdirs, filter, progressCb, cancelFlag.get());
        });
    m_folderPlanWatcher->setFuture(future);
}

void SyncWidget::onFolderPlanFinished()
{
    const FolderSyncPlan plan = m_folderPlanWatcher->result();
    const FolderTaskKind kind = m_folderTaskKind;
    const bool isAuto = m_folderPendingPlanIsAuto;
    m_folderTaskKind = FolderTaskKind::Idle;

    if (plan.canceled) {
        setFolderSyncUiBusy(false);
        ui->progressFolderSync->setRange(0, 100);
        ui->progressFolderSync->setValue(0);
        const QString msg = AppText::tr("扫描已取消。");
        ui->lblFolderSyncSummary->setText(msg);
        logFolderSyncMsg(msg);
        return;
    }

    if (!plan.valid) {
        setFolderSyncUiBusy(false);
        ui->progressFolderSync->setRange(0, 100);
        ui->progressFolderSync->setValue(0);
        if (!isAuto) {
            QMessageBox::warning(this, AppText::tr("参数无效"), plan.errorMessage);
        }
        ui->lblFolderSyncSummary->setText(plan.errorMessage);
        logFolderSyncMsg(plan.errorMessage);
        return;
    }

    if (kind == FolderTaskKind::Preview) {
        setFolderSyncUiBusy(false);
        ui->progressFolderSync->setRange(0, 100);
        ui->progressFolderSync->setValue(100);

        const int pending = plan.pendingTransferCount();
        QString summary = AppText::tr("预览完成：A→B 复制 %1，A→B 移动 %2，B→A 复制 %3，跳过 %4（待传输 %5）")
                              .arg(plan.copyAtoBCount)
                              .arg(plan.moveAtoBCount)
                              .arg(plan.copyBtoACount)
                              .arg(plan.skipCount)
                              .arg(formatSyncByteSize(plan.totalTransferBytes));
        if (plan.filteredOutCount > 0) {
            summary += AppText::tr("，筛选排除 %1 个").arg(plan.filteredOutCount);
        }
        ui->lblFolderSyncSummary->setText(summary);
        enqueueFolderSyncLog(summary);

        if (pending == 0) {
            enqueueFolderSyncLog(AppText::tr("两个目录已一致，无需传输任何文件。"));
            flushFolderSyncLogs();
            return;
        }

        constexpr int kMaxPreviewLines = 200;
        int printed = 0;
        for (const FolderSyncAction &action : plan.actions) {
            if (action.type == FolderSyncActionType::Skip) continue;
            if (printed >= kMaxPreviewLines) {
                enqueueFolderSyncLog(AppText::tr("... 其余 %1 项已省略，点击“开始同步”将全部执行。")
                                         .arg(pending - printed));
                break;
            }
            QString tag;
            if (action.type == FolderSyncActionType::CopyAtoB) tag = QStringLiteral("[A → B 复制]");
            else if (action.type == FolderSyncActionType::MoveAtoB) tag = QStringLiteral("[A → B 移动]");
            else if (action.type == FolderSyncActionType::CopyBtoA) tag = QStringLiteral("[B → A 复制]");
            enqueueFolderSyncLog(QStringLiteral("  %1 %2 (%3, %4)")
                                     .arg(tag, action.relativePath, formatSyncByteSize(action.fileSize), action.reason));
            ++printed;
        }
        flushFolderSyncLogs();
        return;
    }

    // SyncPlan -> proceed to execution
    executePreparedFolderPlan(plan, isAuto);
}

void SyncWidget::executePreparedFolderPlan(const FolderSyncPlan &plan, bool isAuto)
{
    if (!isAuto && plan.mode == FolderSyncMode::OneWayMove && plan.moveAtoBCount > 0) {
        setFolderSyncUiBusy(false);
        ui->progressFolderSync->setRange(0, 100);
        ui->progressFolderSync->setValue(0);
        const auto answer = QMessageBox::question(
            this,
            AppText::tr("确认移动同步"),
            AppText::tr("单向移动同步会将文件夹 A 中的 %1 个文件移动到文件夹 B，并在成功后移除 A 中的源文件。\n确定继续吗？")
                .arg(plan.moveAtoBCount),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            ui->lblFolderSyncSummary->setText(AppText::tr("已取消同步"));
            return;
        }
    }

    const int pending = plan.pendingTransferCount();
    if (pending == 0) {
        setFolderSyncUiBusy(false);
        ui->progressFolderSync->setRange(0, 100);
        ui->progressFolderSync->setValue(100);
        if (!isAuto) {
            QString msg = AppText::tr("两个目录已一致（跳过 %1 个文件），无需同步。").arg(plan.skipCount);
            if (plan.filteredOutCount > 0) {
                msg += AppText::tr("，筛选排除 %1 个").arg(plan.filteredOutCount);
            }
            ui->lblFolderSyncSummary->setText(msg);
            logFolderSyncMsg(msg);
        } else {
            ui->lblFolderSyncSummary->setText(AppText::tr("自动同步监听中（无新增差异）"));
        }
        return;
    }

    clearLastCompareResult();
    m_folderTaskKind = FolderTaskKind::SyncExecute;
    m_folderAutoSuppressWatchEvents = true;
    if (m_folderAutoDebounceTimer) m_folderAutoDebounceTimer->stop();
    if (m_folderAutoCooldownTimer) m_folderAutoCooldownTimer->stop();

    setFolderSyncUiBusy(true);
    ui->progressFolderSync->setRange(0, pending);
    ui->progressFolderSync->setValue(0);
    ui->lblFolderSyncSummary->setText(AppText::tr("正在同步 0 / %1 ...").arg(pending));
    logFolderSyncMsg(isAuto
                         ? AppText::tr("[自动同步] 检测到变更：%1 -> %2（共 %3 个待处理文件，%4）")
                               .arg(plan.folderA, plan.folderB)
                               .arg(pending)
                               .arg(formatSyncByteSize(plan.totalTransferBytes))
                         : AppText::tr("开始同步：%1 -> %2（共 %3 个待处理文件，%4）")
                               .arg(plan.folderA, plan.folderB)
                               .arg(pending)
                               .arg(formatSyncByteSize(plan.totalTransferBytes)));

    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    m_folderSyncCancelFlag = cancelFlag;
    QPointer<SyncWidget> self(this);

    auto progressCb = [self](int done, int total, const QString &line) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, done, total, line]() {
            if (!self) return;
            self->ui->progressFolderSync->setRange(0, qMax(1, total));
            self->ui->progressFolderSync->setValue(done);
            self->ui->lblFolderSyncSummary->setText(
                AppText::tr("正在同步 %1 / %2 ...").arg(done).arg(total));
            self->enqueueFolderSyncLog(line);
        }, Qt::QueuedConnection);
    };

    QFuture<FolderSyncResult> future = QtConcurrent::run([plan, progressCb, cancelFlag]() {
        return FolderSyncEngine::executeSync(plan, progressCb, cancelFlag.get());
    });
    m_folderSyncWatcher->setFuture(future);
}

void SyncWidget::onCompareFolders()
{
    if (isFolderTaskRunning()) return;

    const QString folderA = ui->editFolderA->text();
    const QString folderB = ui->editFolderB->text();
    const bool includeSubdirs = ui->chkSyncSubdirs->isChecked();
    const FolderSyncFilter filter = currentFolderSyncFilter();

    const QString validationErr = FolderSyncEngine::validateInputs(folderA, folderB, filter);
    if (!validationErr.isEmpty()) {
        QMessageBox::warning(this, AppText::tr("参数无效"), validationErr);
        logFolderSyncMsg(validationErr);
        return;
    }

    saveSettings();
    clearLastCompareResult();
    m_folderTaskKind = FolderTaskKind::Compare;
    setFolderSyncUiBusy(true);
    ui->progressFolderSync->setRange(0, 0);
    ui->lblFolderSyncSummary->setText(AppText::tr("正在对比两个文件夹..."));
    logFolderSyncMsg(AppText::tr("开始对比文件夹 A 与文件夹 B（按当前时间与筛选条件）..."));

    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    m_folderSyncCancelFlag = cancelFlag;
    QPointer<SyncWidget> self(this);

    auto progressCb = [self](int done, int total, const QString &status) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, done, total, status]() {
            if (!self || !self->isFolderTaskRunning()) return;
            if (total > 0) {
                self->ui->progressFolderSync->setRange(0, total);
                self->ui->progressFolderSync->setValue(done);
            }
            self->ui->lblFolderSyncSummary->setText(status);
        }, Qt::QueuedConnection);
    };

    QFuture<FolderCompareResult> future = QtConcurrent::run(
        [folderA, folderB, includeSubdirs, filter, progressCb, cancelFlag]() {
            return FolderSyncEngine::compareFolders(
                folderA, folderB, includeSubdirs, filter, progressCb, cancelFlag.get());
        });
    m_folderCompareWatcher->setFuture(future);
}

void SyncWidget::onFolderCompareFinished()
{
    const FolderCompareResult res = m_folderCompareWatcher->result();
    m_folderTaskKind = FolderTaskKind::Idle;

    if (res.canceled) {
        setFolderSyncUiBusy(false);
        ui->progressFolderSync->setRange(0, 100);
        ui->progressFolderSync->setValue(0);
        const QString msg = AppText::tr("文件对比已取消。");
        ui->lblFolderSyncSummary->setText(msg);
        logFolderSyncMsg(msg);
        return;
    }

    if (!res.valid) {
        setFolderSyncUiBusy(false);
        ui->progressFolderSync->setRange(0, 100);
        ui->progressFolderSync->setValue(0);
        QMessageBox::warning(this, AppText::tr("参数无效"), res.errorMessage);
        ui->lblFolderSyncSummary->setText(res.errorMessage);
        logFolderSyncMsg(res.errorMessage);
        return;
    }

    m_lastCompareResult = res;
    setFolderSyncUiBusy(false);
    ui->progressFolderSync->setRange(0, 100);
    ui->progressFolderSync->setValue(100);

    const QString verdict = res.isFilteredExactMatch()
        ? AppText::tr("筛选后完全一致（无多余、无缺失）")
        : AppText::tr("存在差异");
    const QString summary = AppText::tr("对比完成：%1 | 一致 %2，差异 %3，仅在 A %4，仅在 B %5，A 排除 %6，B 排除 %7")
                                .arg(verdict)
                                .arg(res.matchedCount)
                                .arg(res.differentCount)
                                .arg(res.onlyInACount)
                                .arg(res.onlyInBCount)
                                .arg(res.excludedInACount)
                                .arg(res.excludedInBCount);
    ui->lblFolderSyncSummary->setText(summary);
    enqueueFolderSyncLog(summary);

    constexpr int kMaxCompareLogLines = 200;
    int printed = 0;
    for (const FolderCompareEntry &entry : res.entries) {
        if (printed >= kMaxCompareLogLines) {
            enqueueFolderSyncLog(AppText::tr("... 其余 %1 项已省略，可点击“删除到回收站”查看完整清单。")
                                     .arg(res.entries.size() - printed));
            break;
        }
        enqueueFolderSyncLog(QStringLiteral("  [%1] %2 | %3 (%4, %5)")
                                 .arg(entry.sideLabel, entry.relativePath,
                                      QDir::toNativeSeparators(entry.parentDir),
                                      formatSyncByteSize(entry.fileSize),
                                      entry.reason));
        ++printed;
    }
    if (res.deletableCandidateCount() > 0) {
        enqueueFolderSyncLog(AppText::tr("提示：发现 %1 个多余或被筛选排除的文件，可点击“删除到回收站”进行二次确认清理。")
                                 .arg(res.deletableCandidateCount()));
    }
    flushFolderSyncLogs();
}

void SyncWidget::onDeleteToTrashClicked()
{
    if (isFolderTaskRunning()) return;
    if (m_lastCompareResult.deletableCandidateCount() == 0) {
        QMessageBox::information(this, AppText::tr("无需清理"),
                                 AppText::tr("当前对比结果中没有可删除的多余或排除文件，请先点击“文件对比”。"));
        return;
    }

    FolderSyncDeleteDialog dialog(m_lastCompareResult, this);
    if (dialog.exec() != QDialog::Accepted) return;

    const QStringList paths = dialog.selectedFilePaths();
    if (paths.isEmpty()) return;

    m_folderTaskKind = FolderTaskKind::TrashDelete;
    m_folderAutoSuppressWatchEvents = true;
    if (m_folderAutoDebounceTimer) m_folderAutoDebounceTimer->stop();
    if (m_folderAutoCooldownTimer) m_folderAutoCooldownTimer->stop();

    setFolderSyncUiBusy(true);
    ui->progressFolderSync->setRange(0, paths.size());
    ui->progressFolderSync->setValue(0);
    ui->lblFolderSyncSummary->setText(AppText::tr("正在移入回收站 0 / %1 ...").arg(paths.size()));
    logFolderSyncMsg(AppText::tr("开始将选中的 %1 个文件移入系统回收站...").arg(paths.size()));

    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    m_folderSyncCancelFlag = cancelFlag;
    QPointer<SyncWidget> self(this);

    auto progressCb = [self](int done, int total, const QString &line) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, done, total, line]() {
            if (!self) return;
            self->ui->progressFolderSync->setRange(0, qMax(1, total));
            self->ui->progressFolderSync->setValue(done);
            self->ui->lblFolderSyncSummary->setText(
                AppText::tr("正在移入回收站 %1 / %2 ...").arg(done).arg(total));
            self->enqueueFolderSyncLog(line);
        }, Qt::QueuedConnection);
    };

    QFuture<TrashDeleteResult> future = QtConcurrent::run([paths, progressCb, cancelFlag]() {
        return FolderSyncEngine::moveFilesToTrash(paths, progressCb, cancelFlag.get());
    });
    m_folderTrashWatcher->setFuture(future);
}

void SyncWidget::onFolderTrashFinished()
{
    const TrashDeleteResult res = m_folderTrashWatcher->result();
    m_folderTaskKind = FolderTaskKind::Idle;
    clearLastCompareResult();
    setFolderSyncUiBusy(false);
    flushFolderSyncLogs();

    ui->progressFolderSync->setRange(0, 100);
    ui->progressFolderSync->setValue(res.canceled ? 0 : 100);
    const QString summary = res.canceled
        ? AppText::tr("回收站清理已取消：已移入回收站 %1 个，失败 %2 个").arg(res.deletedCount).arg(res.failedCount)
        : AppText::tr("回收站清理完成：已移入回收站 %1 个，失败 %2 个").arg(res.deletedCount).arg(res.failedCount);
    ui->lblFolderSyncSummary->setText(summary);
    logFolderSyncMsg(summary);

    if (ui->chkAutoFolderSync->isChecked()) {
        m_folderAutoCooldownTimer->start();
    } else {
        m_folderAutoSuppressWatchEvents = false;
    }
}

void SyncWidget::onCancelFolderSync()
{
    if (!isFolderTaskRunning() || !m_folderSyncCancelFlag) return;
    m_folderSyncCancelFlag->store(true);
    ui->btnCancelFolderSync->setEnabled(false);
    ui->lblFolderSyncSummary->setText(AppText::tr("正在取消当前任务..."));
    logFolderSyncMsg(AppText::tr("已请求取消当前任务，等待当前文件处理完成..."));
}

void SyncWidget::onFolderSyncFinished()
{
    const FolderSyncResult result = m_folderSyncWatcher->result();
    m_folderTaskKind = FolderTaskKind::Idle;
    setFolderSyncUiBusy(false);
    flushFolderSyncLogs();

    if (result.canceled) {
        const QString msg = AppText::tr("同步已取消：已复制 %1，已移动 %2，跳过 %3，失败 %4")
                                .arg(result.copiedCount)
                                .arg(result.movedCount)
                                .arg(result.skippedCount)
                                .arg(result.failedCount);
        ui->lblFolderSyncSummary->setText(msg);
        logFolderSyncMsg(msg);
    } else {
        QString summary = AppText::tr("同步完成：复制 %1，移动 %2，跳过 %3，失败 %4（传输 %5）")
                              .arg(result.copiedCount)
                              .arg(result.movedCount)
                              .arg(result.skippedCount)
                              .arg(result.failedCount)
                              .arg(formatSyncByteSize(result.transferredBytes));
        if (result.removedEmptyDirs > 0) {
            summary += AppText::tr("，清理空目录 %1 个").arg(result.removedEmptyDirs);
        }

        ui->progressFolderSync->setRange(0, 100);
        ui->progressFolderSync->setValue(100);
        ui->lblFolderSyncSummary->setText(summary);
        logFolderSyncMsg(summary);
    }

    if (ui->chkAutoFolderSync->isChecked()) {
        m_folderAutoCooldownTimer->start();
    } else {
        m_folderAutoSuppressWatchEvents = false;
    }
}

void SyncWidget::onAutoFolderSyncToggled(bool checked)
{
    if (m_isLoading) return;

    if (!checked) {
        stopFolderAutoWatch();
        saveSettings();
        logFolderSyncMsg(AppText::tr("已关闭自动同步。"));
        return;
    }

    const QString validationErr = FolderSyncEngine::validateInputs(
        ui->editFolderA->text(), ui->editFolderB->text(), currentFolderSyncFilter());
    if (!validationErr.isEmpty()) {
        QMessageBox::warning(this, AppText::tr("参数无效"), validationErr);
        logFolderSyncMsg(validationErr);
        const QSignalBlocker blocker(ui->chkAutoFolderSync);
        ui->chkAutoFolderSync->setChecked(false);
        saveSettings();
        return;
    }

    const FolderSyncMode mode = currentFolderSyncMode();
    if (mode == FolderSyncMode::OneWayMove) {
        const auto answer = QMessageBox::question(
            this,
            AppText::tr("确认开启自动移动同步"),
            AppText::tr("开启“单向移动”自动同步后，文件夹 A 中新增或变动的文件将自动移动到文件夹 B 并移除源文件。\n确定开启吗？"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            const QSignalBlocker blocker(ui->chkAutoFolderSync);
            ui->chkAutoFolderSync->setChecked(false);
            return;
        }
    }

    saveSettings();

    // If a sync/scan task is currently active, defer attaching watch until it finishes so UI never freezes.
    if (isFolderTaskRunning()) {
        logFolderSyncMsg(AppText::tr("已开启自动同步，当前任务完成后将自动启动监听。"));
        return;
    }

    startFolderAutoWatchAsync(true, true);
}

void SyncWidget::startFolderAutoWatchAsync(bool triggerInitialSync, bool showWarningDialog)
{
    stopFolderAutoWatch();

    const QString folderA = ui->editFolderA->text();
    const QString folderB = ui->editFolderB->text();
    const FolderSyncMode mode = currentFolderSyncMode();
    const bool includeSubdirs = ui->chkSyncSubdirs->isChecked();

    const QString validationErr = FolderSyncEngine::validateInputs(folderA, folderB, currentFolderSyncFilter());
    if (!validationErr.isEmpty()) {
        if (showWarningDialog) {
            QMessageBox::warning(this, AppText::tr("参数无效"), validationErr);
        }
        logFolderSyncMsg(validationErr);
        const QSignalBlocker blocker(ui->chkAutoFolderSync);
        ui->chkAutoFolderSync->setChecked(false);
        saveSettings();
        return;
    }

    const QString normA = FolderSyncEngine::normalizeDirPath(folderA);
    const QString normB = FolderSyncEngine::normalizeDirPath(folderB);
    QStringList roots{normA};
    if (mode == FolderSyncMode::TwoWaySync) {
        QDir dirB(normB);
        if (!dirB.exists()) dirB.mkpath(QStringLiteral("."));
        roots.append(normB);
    }

    m_folderWatchScanTriggerSyncAfter = triggerInitialSync;
    const int gen = ++m_folderWatchScanGeneration;
    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    m_folderWatchScanCancelFlag = cancelFlag;

    QFuture<QStringList> future = QtConcurrent::run([roots, includeSubdirs, cancelFlag]() {
        return FolderSyncEngine::collectWatchDirectories(roots, includeSubdirs, kMaxAutoWatchDirs, cancelFlag.get());
    });
    m_folderAutoWatchScanWatcher->setProperty("scanGen", gen);
    m_folderAutoWatchScanWatcher->setFuture(future);
}

void SyncWidget::onFolderAutoWatchScanFinished()
{
    if (!ui->chkAutoFolderSync->isChecked()) return;
    if (m_folderAutoWatchScanWatcher->property("scanGen").toInt() != m_folderWatchScanGeneration) return;

    const QStringList dirs = m_folderAutoWatchScanWatcher->result();
    if (!dirs.isEmpty() && m_folderAutoWatcher) {
        m_folderAutoWatcher->addPaths(dirs);
    }

    const FolderSyncMode mode = currentFolderSyncMode();
    const QString normA = FolderSyncEngine::normalizeDirPath(ui->editFolderA->text());
    const QString normB = FolderSyncEngine::normalizeDirPath(ui->editFolderB->text());
    logFolderSyncMsg(mode == FolderSyncMode::TwoWaySync
                         ? AppText::tr("已开启自动同步监听（双向，共监听 %1 个目录）：%2 ⇄ %3").arg(dirs.size()).arg(normA, normB)
                         : AppText::tr("已开启自动同步监听（共监听 %1 个目录）：%2 → %3").arg(dirs.size()).arg(normA, normB));
    if (dirs.size() >= kMaxAutoWatchDirs) {
        logFolderSyncMsg(AppText::tr("提示：子目录数量超过 %1，为防止卡顿已限制监听目录上限。").arg(kMaxAutoWatchDirs));
    }

    if (m_folderWatchScanTriggerSyncAfter && !isFolderTaskRunning()) {
        m_folderWatchScanTriggerSyncAfter = false;
        startFolderPlanAsync(FolderTaskKind::SyncPlan, true);
    }
}

void SyncWidget::stopFolderAutoWatch()
{
    ++m_folderWatchScanGeneration;
    if (m_folderWatchScanCancelFlag) m_folderWatchScanCancelFlag->store(true);
    if (m_folderAutoDebounceTimer) m_folderAutoDebounceTimer->stop();
    if (m_folderAutoCooldownTimer) m_folderAutoCooldownTimer->stop();
    if (m_folderAutoWatcher) {
        const QStringList dirs = m_folderAutoWatcher->directories();
        if (!dirs.isEmpty()) m_folderAutoWatcher->removePaths(dirs);
        const QStringList files = m_folderAutoWatcher->files();
        if (!files.isEmpty()) m_folderAutoWatcher->removePaths(files);
    }
}

void SyncWidget::refreshFolderAutoWatchIfEnabled()
{
    if (m_isLoading || !ui->chkAutoFolderSync->isChecked() || isFolderTaskRunning()) return;
    startFolderAutoWatchAsync(false, false);
}

void SyncWidget::onFolderAutoDirectoryChanged(const QString &path)
{
    if (!ui->chkAutoFolderSync->isChecked()) return;
    if (m_folderAutoSuppressWatchEvents || isFolderTaskRunning()) return;

    // Only check immediate 1-level new subdirectories here (fast, O(immediate children)), never deep recursive scan on UI thread.
    if (ui->chkSyncSubdirs->isChecked() && m_folderAutoWatcher && m_folderAutoWatcher->directories().size() < kMaxAutoWatchDirs) {
        QDir dir(path);
        const QFileInfoList subDirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (!subDirs.isEmpty()) {
            const QStringList current = m_folderAutoWatcher->directories();
            const QSet<QString> currentSet(current.cbegin(), current.cend());
            QStringList newlyAdded;
            for (const QFileInfo &fi : subDirs) {
                const QString clean = FolderSyncEngine::normalizeDirPath(fi.absoluteFilePath());
                if (!currentSet.contains(clean)) newlyAdded.append(clean);
            }
            if (!newlyAdded.isEmpty()) {
                m_folderAutoWatcher->addPaths(newlyAdded);
            }
        }
    }

    if (m_folderAutoDebounceTimer) {
        m_folderAutoDebounceTimer->start();
    }
}
