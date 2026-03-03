#include "promptparserwidget.h"
#include "ui_promptparserwidget.h"

#include <QFileDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
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

    // 初始化 TagFlowWidgets
    posTagWidget = new TagFlowWidget();
    negTagWidget = new TagFlowWidget();

    // 根据 UI 中按钮的默认状态设置 (XML中默认 checked=true)
    bool showTrans = ui->btnTranslate->isChecked();
    posTagWidget->setShowTranslation(showTrans);
    negTagWidget->setShowTranslation(showTrans);

    // 将其添加到对应的 ScrollArea 布局中
    ui->layoutTagsPos->addWidget(posTagWidget);
    ui->layoutTagsPos->addStretch();

    ui->layoutTagsNeg->addWidget(negTagWidget);
    ui->layoutTagsNeg->addStretch();

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
    });

    // 修复滚动条拖动时重绘问题 (防止 Tag 渲染残留)
    connect(ui->scrollAreaPos->verticalScrollBar(), &QScrollBar::valueChanged,
            ui->scrollAreaWidgetContentsPos, [this](){ ui->scrollAreaWidgetContentsPos->update(); });
    connect(ui->scrollAreaNeg->verticalScrollBar(), &QScrollBar::valueChanged,
            ui->scrollAreaWidgetContentsNeg, [this](){ ui->scrollAreaWidgetContentsNeg->update(); });
}

PromptParserWidget::~PromptParserWidget()
{
    delete ui;
}

void PromptParserWidget::setTranslationMap(const QHash<QString, QString> *map)
{
    m_translationMap = map;
    posTagWidget->setTranslationMap(map);
    negTagWidget->setTranslationMap(map);
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
        processImage(filePath);
        event->acceptProposedAction();
    }
}

void PromptParserWidget::updateImagePreview(const QString &filePath)
{
    QImageReader reader(filePath);
    reader.setAutoTransform(true); 
    QImage img = reader.read();
    if (!img.isNull()) {
        QPixmap pix = QPixmap::fromImage(img).scaled(ui->lblImage->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        ui->lblImage->setPixmap(pix);
        ui->lblImage->setText(""); 
    } else {
        ui->lblImage->clear();
        ui->lblImage->setText("图片加载失败\nFailed to load image");
    }
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
