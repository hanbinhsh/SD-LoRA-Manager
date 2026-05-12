#include "promptparserwidget.h"
#include "ui_promptparserwidget.h"

#include <QFileDialog>
#include <QApplication>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QImageReader>
#include <QFileInfo>
#include <QRegularExpression>
#include <QtEndian>
#include <QScrollBar>
#include <QMessageBox>

PromptParserWidget::PromptParserWidget(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::PromptParserWidget),
    m_translationMap(nullptr)
{
    ui->setupUi(this);

    this->setAcceptDrops(true);
    ui->lblImage->installEventFilter(this);
    ui->lblWd14Image->installEventFilter(this);

    // 初始化 TagFlowWidgets
    posTagWidget = new TagFlowWidget(ui->scrollAreaWidgetContentsPos);
    negTagWidget = new TagFlowWidget(ui->scrollAreaWidgetContentsNeg);
    wd14TagWidget = new TagFlowWidget(ui->scrollAreaWidgetContentsWd14);

    // 根据 UI 中按钮的默认状态设置 (XML中默认 checked=true)
    bool showTrans = ui->btnTranslate->isChecked();
    posTagWidget->setShowTranslation(showTrans);
    negTagWidget->setShowTranslation(showTrans);
    wd14TagWidget->setShowTranslation(showTrans);

    // 将其添加到对应的 ScrollArea 布局中
    ui->layoutTagsPos->addWidget(posTagWidget);
    ui->layoutTagsPos->addStretch();

    ui->layoutTagsNeg->addWidget(negTagWidget);
    ui->layoutTagsNeg->addStretch();

    ui->layoutTagsWd14->addWidget(wd14TagWidget);
    ui->layoutTagsWd14->addStretch();

    // 绑定翻译按钮信号
    connect(ui->btnTranslate, &QPushButton::toggled, this, [this](bool checked){
        if (checked && (!m_translationMap || m_translationMap->isEmpty())) {
            // 如果尝试开启但没有字典，临时阻断并提示
            QMessageBox::warning(this, "提示", "未加载翻译词表，请在设置中配置 CSV 文件。");
            ui->btnTranslate->blockSignals(true);
            ui->btnTranslate->setChecked(false);
            ui->btnTranslate->blockSignals(false);
            return;
        }
        posTagWidget->setShowTranslation(checked);
        negTagWidget->setShowTranslation(checked);
        wd14TagWidget->setShowTranslation(checked);
    });

    wd14Process = new QProcess(this);
    connect(wd14Process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &PromptParserWidget::onWd14Finished);
    connect(wd14Process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        Q_UNUSED(error);
        setWd14Running(false);
        ui->lblWd14Status->setText("WD14 反推启动失败: " + wd14Process->errorString());
    });
    connect(ui->btnWd14Run, &QPushButton::clicked, this, &PromptParserWidget::runWd14Tagger);
    connect(ui->btnWd14Copy, &QPushButton::clicked, this, [this]() {
        if (wd14LastTagsText.trimmed().isEmpty()) {
            ui->lblWd14Status->setText("暂无可复制的 WD14 Tag。");
            return;
        }
        QApplication::clipboard()->setText(wd14LastTagsText);
        ui->lblWd14Status->setText("已复制 WD14 Tag。");
    });
    connect(ui->editWd14Command, &QLineEdit::editingFinished, this, &PromptParserWidget::saveWd14Settings);
    ui->btnWd14Copy->setEnabled(false);

    // 修复滚动条拖动时重绘问题 (防止 Tag 渲染残留)
    connect(ui->scrollAreaPos->verticalScrollBar(), &QScrollBar::valueChanged,
            ui->scrollAreaWidgetContentsPos, [this](){ ui->scrollAreaWidgetContentsPos->update(); });
    connect(ui->scrollAreaNeg->verticalScrollBar(), &QScrollBar::valueChanged,
            ui->scrollAreaWidgetContentsNeg, [this](){ ui->scrollAreaWidgetContentsNeg->update(); });
    connect(ui->scrollAreaWd14->verticalScrollBar(), &QScrollBar::valueChanged,
            ui->scrollAreaWidgetContentsWd14, [this](){ ui->scrollAreaWidgetContentsWd14->update(); });

    loadWd14Settings();
}

PromptParserWidget::~PromptParserWidget()
{
    if (wd14Process && wd14Process->state() != QProcess::NotRunning) {
        wd14Process->kill();
        wd14Process->waitForFinished(1000);
    }
    delete ui;
}

void PromptParserWidget::setTranslationMap(const QHash<QString, QString> *map)
{
    m_translationMap = map;
    posTagWidget->setTranslationMap(map);
    negTagWidget->setTranslationMap(map);
    wd14TagWidget->setTranslationMap(map);
}

bool PromptParserWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->lblImage && event->type() == QEvent::MouseButtonPress) {
        QString filePath = QFileDialog::getOpenFileName(this, "选择图片", "", "Images (*.png *.jpg *.jpeg *.webp)");
        if (!filePath.isEmpty()) {
            processImage(filePath);
        }
        return true;
    }
    if (watched == ui->lblWd14Image && event->type() == QEvent::MouseButtonPress) {
        QString filePath = QFileDialog::getOpenFileName(this, "选择图片", "", "Images (*.png *.jpg *.jpeg *.webp)");
        if (!filePath.isEmpty()) {
            processWd14Image(filePath);
        }
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void PromptParserWidget::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        QList<QUrl> urls = event->mimeData()->urls();
        if (!urls.isEmpty()) {
            QString path = urls.first().toLocalFile().toLower();
            if (path.endsWith(".png") || path.endsWith(".jpg") || path.endsWith(".jpeg") || path.endsWith(".webp")) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    event->ignore();
}

void PromptParserWidget::dropEvent(QDropEvent *event)
{
    const QMimeData *mimeData = event->mimeData();
    if (mimeData->hasUrls()) {
        QString filePath = mimeData->urls().first().toLocalFile();
        if (ui->tabPromptParser->currentWidget() == ui->tabWd14) {
            processWd14Image(filePath);
        } else {
            processImage(filePath);
        }
        event->acceptProposedAction();
    }
}

void PromptParserWidget::updateImageLabelPreview(QLabel *label, const QString &filePath, const QString &fallbackText)
{
    QImageReader reader(filePath);
    reader.setAutoTransform(true); 
    QImage img = reader.read();
    if (!img.isNull()) {
        QPixmap pix = QPixmap::fromImage(img).scaled(label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        label->setPixmap(pix);
        label->setText("");
    } else {
        label->clear();
        label->setText(fallbackText);
    }
}

void PromptParserWidget::updateImagePreview(const QString &filePath)
{
    updateImageLabelPreview(ui->lblImage, filePath, "图片加载失败\nFailed to load image");
}

void PromptParserWidget::updateWd14ImagePreview(const QString &filePath)
{
    updateImageLabelPreview(ui->lblWd14Image, filePath, "图片加载失败\nFailed to load image");
}

QString PromptParserWidget::extractPngParameters(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return "";

    QByteArray signature = file.read(8);
    const char pngSignature[] = {-119, 'P', 'N', 'G', 13, 10, 26, 10};
    if (signature != QByteArray::fromRawData(pngSignature, 8)) return "";

    while (!file.atEnd()) {
        QByteArray lenData = file.read(4);
        if (lenData.size() < 4) break;
        quint32 length = qFromBigEndian<quint32>(lenData.constData());
        QByteArray type = file.read(4);

        if (type == "tEXt") {
            QByteArray data = file.read(length);
            int nullPos = data.indexOf('\0');
            if (nullPos != -1) {
                QString keyword = QString::fromLatin1(data.left(nullPos));
                if (keyword == "parameters") {
                    return QString::fromUtf8(data.mid(nullPos + 1));
                }
            }
        } else {
            file.seek(file.pos() + length);
        }
        file.seek(file.pos() + 4);
    }
    return "";
}

// 辅助函数：清洗 Tag（去除权重和括号）
QString PromptParserWidget::cleanTagText(QString t) {
    t = t.trimmed();
    if (t.isEmpty()) return "";

    static const QSet<QString> emoticons = {":)", ":-)", ":(", ":-(", "^_^", "T_T", "o_o", "O_O"};
    if (emoticons.contains(t)) return t;

    static QRegularExpression weightRegex(":[0-9.]+$");
    t.remove(weightRegex);

    static QRegularExpression bracketRegex("[\\{\\}\\[\\]\\(\\)]");
    t.remove(bracketRegex);

    return t.trimmed();
}

// 解析文本为 Map 以供 TagFlowWidget 使用
QMap<QString, int> PromptParserWidget::parsePromptToMap(const QString &rawPrompt) {
    QMap<QString, int> map;
    if (rawPrompt.isEmpty()) return map;

    // 默认按照换行和逗号分割
    QString processText = rawPrompt;
    processText.replace("\r\n", ",");
    processText.replace("\n", ",");
    processText.replace("\r", ",");

    QStringList parts = processText.split(",", Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        QString clean = cleanTagText(part);
        if (!clean.isEmpty()) {
            map[clean]++;
        }
    }
    return map;
}

void PromptParserWidget::processImage(const QString &filePath)
{
    updateImagePreview(filePath);

    QString text = extractPngParameters(filePath);
    if (text.isEmpty()) {
        QImageReader reader(filePath);
        if (reader.canRead()) {
            text = reader.text("parameters");
            if (text.isEmpty()) text = reader.text("prompt");
        }
    }

    if (text.isEmpty()) {
        ui->txtParams->setText("未找到生成参数 / No generation parameters found.");
        posTagWidget->setData(QMap<QString, int>());
        negTagWidget->setData(QMap<QString, int>());
        return;
    }

    QString posPrompt, negPrompt, params;
    int stepsIndex = text.lastIndexOf("Steps: ");

    if (stepsIndex == -1) {
        posPrompt = text.trimmed();
    } else {
        params = text.mid(stepsIndex).trimmed();
        QString beforeParams = text.left(stepsIndex).trimmed();
        int negIndex = beforeParams.indexOf("Negative prompt:");

        if (negIndex != -1) {
            posPrompt = beforeParams.left(negIndex).trimmed();
            negPrompt = beforeParams.mid(negIndex + 16).trimmed();
        } else {
            posPrompt = beforeParams.trimmed();
        }
    }

    ui->txtParams->setText(params);

    // 将解析出的提示词转换为 QMap 送入控件
    posTagWidget->setData(parsePromptToMap(posPrompt));
    negTagWidget->setData(parsePromptToMap(negPrompt));
}

void PromptParserWidget::processWd14Image(const QString &filePath)
{
    if (filePath.isEmpty() || !QFile::exists(filePath)) {
        ui->lblWd14Status->setText("图片不存在。");
        return;
    }

    wd14ImagePath = filePath;
    updateWd14ImagePreview(filePath);
    ui->lblWd14Status->setText("已选择图片: " + QFileInfo(filePath).fileName());
}

void PromptParserWidget::loadWd14Settings()
{
    const QString configPath = qApp->applicationDirPath() + "/config/settings.json";
    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly)) return;

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    ui->editWd14Command->setText(root.value("wd14_command_template").toString());
}

void PromptParserWidget::saveWd14Settings() const
{
    const QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);
    const QString configPath = configDir + "/settings.json";

    QJsonObject root;
    QFile readFile(configPath);
    if (readFile.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(readFile.readAll()).object();
        readFile.close();
    }

    root["wd14_command_template"] = ui->editWd14Command->text().trimmed();

    QFile file(configPath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson());
    }
}

QString PromptParserWidget::buildWd14Command() const
{
    QString command = ui->editWd14Command->text().trimmed();
    if (command.isEmpty() || wd14ImagePath.isEmpty()) return QString();

    QString imagePath = QDir::toNativeSeparators(wd14ImagePath);
    imagePath.replace("\"", "\\\"");

    if (command.contains("{image}")) {
        command.replace("{image}", imagePath);
    } else {
        command += QString(" \"%1\"").arg(imagePath);
    }
    return command;
}

void PromptParserWidget::setWd14Running(bool running)
{
    ui->btnWd14Run->setEnabled(!running);
    ui->editWd14Command->setEnabled(!running);
    ui->lblWd14Image->setEnabled(!running);
}

void PromptParserWidget::runWd14Tagger()
{
    if (wd14Process->state() != QProcess::NotRunning) return;

    if (wd14ImagePath.isEmpty() || !QFile::exists(wd14ImagePath)) {
        ui->lblWd14Status->setText("请先选择需要反推的图片。");
        return;
    }

    const QString command = buildWd14Command();
    if (command.isEmpty()) {
        ui->lblWd14Status->setText("请先填写 WD14 命令模板。");
        return;
    }

    saveWd14Settings();
    setWd14Running(true);
    ui->lblWd14Status->setText("WD14 反推运行中...");
    wd14Process->setProcessChannelMode(QProcess::SeparateChannels);
    wd14Process->startCommand(command);
}

void PromptParserWidget::onWd14Finished(int exitCode, QProcess::ExitStatus exitStatus)
{
    setWd14Running(false);

    const QString stdoutText = QString::fromUtf8(wd14Process->readAllStandardOutput()).trimmed();
    const QString stderrText = QString::fromUtf8(wd14Process->readAllStandardError()).trimmed();

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        QString message = QString("WD14 反推失败 (exit %1)").arg(exitCode);
        if (!stderrText.isEmpty()) message += ": " + stderrText.left(500);
        ui->lblWd14Status->setText(message);
        return;
    }

    if (stdoutText.isEmpty()) {
        ui->lblWd14Status->setText("WD14 命令执行完成，但没有输出 Tag。");
        return;
    }

    wd14LastTagsText = stdoutText;
    wd14TagWidget->setData(parsePromptToMap(stdoutText));
    ui->btnWd14Copy->setEnabled(true);
    ui->lblWd14Status->setText(QString("WD14 反推完成，共 %1 个 Tag。").arg(parsePromptToMap(stdoutText).size()));
}
