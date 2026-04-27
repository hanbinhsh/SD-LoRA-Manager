#include "syncwidget.h"
#include "ui_syncwidget.h"
#include <QFileDialog>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDataStream>
#include <QMessageBox>
#include <QDateTime>
#include <QTimer>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <QHBoxLayout>

SyncWidget::SyncWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::SyncWidget)
{
    ui->setupUi(this);

    // 初始化网络和文件监控
    tcpServer = new QTcpServer(this);
    watcher = new QFileSystemWatcher(this);

    connect(tcpServer, &QTcpServer::newConnection, this, &SyncWidget::newClientConnected);
    connect(watcher, &QFileSystemWatcher::directoryChanged, this, &SyncWidget::handleDirectoryChange);

    // 调整 Splitter 比例
    ui->splitter->setSizes(QList<int>() << 300 << 250 << 400);

    // 加载设置
    loadSettings();

    // 自动启动服务
    if (ui->chkAutoStart->isChecked()) {
        QTimer::singleShot(500, this, &SyncWidget::on_btnStart_clicked);
    }
}

SyncWidget::~SyncWidget()
{
    saveSettings();
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
}

// UI 改变即刻触发保存
void SyncWidget::on_chkAutoStart_toggled(bool checked) { Q_UNUSED(checked); saveSettings(); }
void SyncWidget::on_chkWhitelist_toggled(bool checked) { Q_UNUSED(checked); saveSettings(); }
void SyncWidget::on_editPort_editingFinished() { saveSettings(); }
void SyncWidget::on_editAesKey_editingFinished() { saveSettings(); }

// ======================= UI 按钮响应 =======================
void SyncWidget::on_btnStart_clicked() {
    if (tcpServer->isListening()) {
        tcpServer->close();
        for (auto client : clients) client->disconnectFromHost();
        ui->btnStart->setText("启动服务 / Start");
        ui->btnStart->setStyleSheet("");
        logMsg("服务已停止。");
    } else {
        quint16 port = ui->editPort->text().toUShort();
        if (tcpServer->listen(QHostAddress::Any, port)) {
            ui->btnStart->setText("停止服务 / Stop");
            ui->btnStart->setStyleSheet("background-color: #aa3333; color: white; font-weight: bold;");
            logMsg(QString("服务已启动，监听端口: %1").arg(port));
        } else {
            QMessageBox::critical(this, "错误", "端口被占用或启动失败");
        }
    }
}

void SyncWidget::on_btnAddFolder_clicked() {
    QString dir = QFileDialog::getExistingDirectory(this, "选择 SD 输出文件夹");
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

        QDataStream plainIn(plaintext);
        plainIn.setByteOrder(QDataStream::BigEndian);
        quint32 plainTotalLen, jsonLen;
        plainIn >> plainTotalLen >> jsonLen;

        QByteArray jsonBytes = plaintext.mid(8, jsonLen);
        QJsonObject json = QJsonDocument::fromJson(jsonBytes).object();
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
    for (const QString &root : m_rootPaths) {
        if (path.startsWith(root)) return root;
    }
    return QString();
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
        watcher->addPath(path);
        m_watchedPathsMap[rootPath].insert(path);
        updateDirState(path);
    }
    while (it.hasNext()) {
        QString subDir = it.next();
        if(!m_watchedPathsMap[rootPath].contains(subDir)) {
            watcher->addPath(subDir);
            m_watchedPathsMap[rootPath].insert(subDir);
            updateDirState(subDir);
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
    lblPath->setStyleSheet("color: #acb2b8; background-color: transparent;");
    lblPath->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    QPushButton *btnRemove = new QPushButton("X");
    btnRemove->setCursor(Qt::PointingHandCursor);
    btnRemove->setStyleSheet("QPushButton { background-color: #3c1919; color: #ff4c4c; border: none; padding: 4px 8px; border-radius: 2px; }"
                             "QPushButton:hover { background-color: #ff4c4c; color: #fff; }");

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
    lblName->setStyleSheet("color: #ffcc00; font-weight: bold; background-color: transparent;");
    lblName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    QPushButton *btnAllow = new QPushButton("允许");
    btnAllow->setCursor(Qt::PointingHandCursor);
    btnAllow->setStyleSheet("QPushButton { background-color: #1b4b2a; color: #8fbc8f; border: none; padding: 4px 8px; border-radius: 2px; }"
                            "QPushButton:hover { background-color: #2e8b57; color: #fff; }");

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
    lblId->setStyleSheet("color: #acb2b8; background-color: transparent;");
    lblId->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    QPushButton *btnRemove = new QPushButton("X");
    btnRemove->setCursor(Qt::PointingHandCursor);
    btnRemove->setStyleSheet("QPushButton { background-color: #3c1919; color: #ff4c4c; border: none; padding: 4px 8px; border-radius: 2px; }"
                             "QPushButton:hover { background-color: #ff4c4c; color: #fff; }");

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
    lblName->setStyleSheet("color: #66c0f4; font-weight: bold; background-color: transparent;");
    lblName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    QPushButton *btnKick = new QPushButton("断开");
    btnKick->setCursor(Qt::PointingHandCursor);
    btnKick->setStyleSheet("QPushButton { background-color: #3c1919; color: #ff4c4c; border: none; padding: 4px 8px; border-radius: 2px; }"
                           "QPushButton:hover { background-color: #ff4c4c; color: #fff; }");

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
    sendFolderDeleteNotification(QFileInfo(path).fileName());
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
