#include "promptparserwidget.h"
#include "ui_promptparserwidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QSaveFile>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QtEndian>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

QString settingsPath()
{
    return qApp->applicationDirPath() + "/config/settings.json";
}

QPair<quint64, quint64> systemMemorySnapshot()
{
#ifdef Q_OS_WIN
    MEMORYSTATUSEX state;
    state.dwLength = sizeof(state);
    if (GlobalMemoryStatusEx(&state)) {
        return {state.ullTotalPhys, state.ullAvailPhys};
    }
#endif
    return {0, 0};
}

QString escapeParentheses(QString text)
{
    text.replace("(", "\\(");
    text.replace(")", "\\)");
    return text;
}

QString formatMemoryBytes(quint64 bytes)
{
    if (bytes == 0) return "--";
    return QString::number(double(bytes) / (1024.0 * 1024.0 * 1024.0), 'f', 1) + " GB";
}

Wd14TagScore parseScoreObject(const QJsonObject &obj)
{
    Wd14TagScore score;
    score.tag = obj.value("tag").toString();
    score.category = obj.value("category").toString();
    score.confidence = float(obj.value("confidence").toDouble());
    return score;
}

} // namespace

PromptParserWidget::PromptParserWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::PromptParserWidget)
    , m_translationMap(nullptr)
{
    ui->setupUi(this);

    setAcceptDrops(true);
    ui->lblImage->installEventFilter(this);
    ui->lblWd14Image->installEventFilter(this);

    posTagWidget = new TagFlowWidget(ui->scrollAreaWidgetContentsPos);
    negTagWidget = new TagFlowWidget(ui->scrollAreaWidgetContentsNeg);

    const bool showTrans = ui->btnTranslate->isChecked();
    posTagWidget->setShowTranslation(showTrans);
    negTagWidget->setShowTranslation(showTrans);

    ui->layoutTagsPos->addWidget(posTagWidget);
    ui->layoutTagsPos->addStretch();
    ui->layoutTagsNeg->addWidget(negTagWidget);
    ui->layoutTagsNeg->addStretch();

    connect(ui->btnTranslate, &QPushButton::toggled, this, [this](bool checked) {
        if (checked && (!m_translationMap || m_translationMap->isEmpty())) {
            QMessageBox::warning(this, "提示", "未加载翻译词表，请在设置中配置 CSV 文件。");
            QSignalBlocker blocker(ui->btnTranslate);
            ui->btnTranslate->setChecked(false);
            return;
        }
        posTagWidget->setShowTranslation(checked);
        negTagWidget->setShowTranslation(checked);
    });

    wd14Process = new QProcess(this);
    wd14Process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(wd14Process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        Q_UNUSED(exitStatus);
        setWd14Running(false);
        const Wd14InferenceResult result = parseWd14ProcessOutput(
            wd14Process->readAllStandardOutput(),
            wd14Process->readAllStandardError(),
            exitCode);
        ui->lblWd14Elapsed->setText(QString("用时: %1 sec.").arg(result.elapsedSec, 0, 'f', 2));
        updateWd14MemoryLabel(result.totalMemory, result.availableMemory);

        if (!result.ok) {
            ui->lblWd14Status->setText(result.error.isEmpty() ? "WD14 反推失败。" : result.error);
            return;
        }

        applyWd14Result(result);
        ui->lblWd14Status->setText(QString("WD14 反推完成，共 %1 个标签。").arg(result.tags.size()));
    });
    connect(wd14Process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        setWd14Running(false);
        ui->lblWd14Status->setText("WD14 Python 进程启动失败: " + wd14Process->errorString());
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
    connect(ui->btnWd14BrowseModel, &QPushButton::clicked, this, &PromptParserWidget::browseWd14ModelPath);
    connect(ui->btnWd14BrowsePython, &QPushButton::clicked, this, &PromptParserWidget::browseWd14PythonPath);
    connect(ui->btnWd14BrowseScript, &QPushButton::clicked, this, &PromptParserWidget::browseWd14ScriptPath);
    connect(ui->btnWd14SavePreset, &QPushButton::clicked, this, &PromptParserWidget::saveWd14Preset);
    connect(ui->comboWd14Preset, &QComboBox::textActivated, this, &PromptParserWidget::loadWd14Preset);
    connect(ui->sliderWd14Threshold, &QSlider::valueChanged, this, &PromptParserWidget::updateWd14ThresholdFromSlider);
    connect(ui->spinWd14Threshold, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &PromptParserWidget::updateWd14ThresholdFromSpin);

    const auto saveSettingsLater = [this]() { saveWd14Settings(); };
    connect(ui->editWd14ModelPath, &QLineEdit::editingFinished, this, saveSettingsLater);
    connect(ui->editWd14PythonPath, &QLineEdit::editingFinished, this, saveSettingsLater);
    connect(ui->editWd14ScriptPath, &QLineEdit::editingFinished, this, saveSettingsLater);
    connect(ui->editWd14AdditionalTags, &QLineEdit::editingFinished, this, saveSettingsLater);
    connect(ui->editWd14ExcludeTags, &QLineEdit::editingFinished, this, saveSettingsLater);
    connect(ui->editWd14DefaultExclude, &QLineEdit::editingFinished, this, saveSettingsLater);
    connect(ui->chkWd14SortAlphabetically, &QCheckBox::toggled, this, saveSettingsLater);
    connect(ui->chkWd14IncludeConfidence, &QCheckBox::toggled, this, saveSettingsLater);
    connect(ui->chkWd14ReplaceUnderscore, &QCheckBox::toggled, this, saveSettingsLater);
    connect(ui->chkWd14EscapeBrackets, &QCheckBox::toggled, this, saveSettingsLater);
    connect(ui->chkWd14UnloadAfterInference, &QCheckBox::toggled, this, saveSettingsLater);

    ui->btnWd14Copy->setEnabled(false);
    ui->treeWd14Ratings->setRootIsDecorated(false);
    ui->treeWd14Ratings->setAlternatingRowColors(true);
    ui->treeWd14Tags->setRootIsDecorated(false);
    ui->treeWd14Tags->setAlternatingRowColors(true);

    connect(ui->scrollAreaPos->verticalScrollBar(), &QScrollBar::valueChanged,
            ui->scrollAreaWidgetContentsPos, [this]() { ui->scrollAreaWidgetContentsPos->update(); });
    connect(ui->scrollAreaNeg->verticalScrollBar(), &QScrollBar::valueChanged,
            ui->scrollAreaWidgetContentsNeg, [this]() { ui->scrollAreaWidgetContentsNeg->update(); });

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
}

bool PromptParserWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->lblImage && event->type() == QEvent::MouseButtonPress) {
        const QString filePath = QFileDialog::getOpenFileName(this, "选择图片", "", "Images (*.png *.jpg *.jpeg *.webp)");
        if (!filePath.isEmpty()) processImage(filePath);
        return true;
    }
    if (watched == ui->lblWd14Image && event->type() == QEvent::MouseButtonPress) {
        const QString filePath = QFileDialog::getOpenFileName(this, "选择图片", "", "Images (*.png *.jpg *.jpeg *.webp)");
        if (!filePath.isEmpty()) processWd14Image(filePath);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void PromptParserWidget::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        const QList<QUrl> urls = event->mimeData()->urls();
        if (!urls.isEmpty()) {
            const QString path = urls.first().toLocalFile().toLower();
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
    if (!mimeData->hasUrls()) return;

    const QString filePath = mimeData->urls().first().toLocalFile();
    if (ui->tabPromptParser->currentWidget() == ui->tabWd14) {
        processWd14Image(filePath);
    } else {
        processImage(filePath);
    }
    event->acceptProposedAction();
}

void PromptParserWidget::updateImageLabelPreview(QLabel *label, const QString &filePath, const QString &fallbackText)
{
    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull()) {
        label->clear();
        label->setText(fallbackText);
        return;
    }

    const QPixmap pixmap = QPixmap::fromImage(image).scaled(label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    label->setPixmap(pixmap);
    label->setText("");
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

    const QByteArray signature = file.read(8);
    const char pngSignature[] = {-119, 'P', 'N', 'G', 13, 10, 26, 10};
    if (signature != QByteArray::fromRawData(pngSignature, 8)) return "";

    while (!file.atEnd()) {
        const QByteArray lenData = file.read(4);
        if (lenData.size() < 4) break;
        const quint32 length = qFromBigEndian<quint32>(lenData.constData());
        const QByteArray type = file.read(4);
        if (type == "tEXt") {
            const QByteArray data = file.read(length);
            const int nullPos = data.indexOf('\0');
            if (nullPos != -1) {
                const QString keyword = QString::fromLatin1(data.left(nullPos));
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

QString PromptParserWidget::cleanTagText(QString text)
{
    text = text.trimmed();
    if (text.isEmpty()) return "";

    static const QSet<QString> emoticons = {":)", ":-)", ":(", ":-(", "^_^", "T_T", "o_o", "O_O"};
    if (emoticons.contains(text)) return text;

    static const QRegularExpression weightRegex(":[0-9.]+$");
    text.remove(weightRegex);
    static const QRegularExpression bracketRegex("[\\{\\}\\[\\]\\(\\)]");
    text.remove(bracketRegex);
    return text.trimmed();
}

QMap<QString, int> PromptParserWidget::parsePromptToMap(const QString &rawPrompt)
{
    QMap<QString, int> result;
    if (rawPrompt.isEmpty()) return result;

    QString processText = rawPrompt;
    processText.replace("\r\n", ",");
    processText.replace("\n", ",");
    processText.replace("\r", ",");
    const QStringList parts = processText.split(",", Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QString clean = cleanTagText(part);
        if (!clean.isEmpty()) result[clean]++;
    }
    return result;
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
        posTagWidget->setData({});
        negTagWidget->setData({});
        return;
    }

    QString positivePrompt;
    QString negativePrompt;
    QString params;
    const int stepsIndex = text.lastIndexOf("Steps: ");
    if (stepsIndex == -1) {
        positivePrompt = text.trimmed();
    } else {
        params = text.mid(stepsIndex).trimmed();
        const QString beforeParams = text.left(stepsIndex).trimmed();
        const int negativeIndex = beforeParams.indexOf("Negative prompt:");
        if (negativeIndex != -1) {
            positivePrompt = beforeParams.left(negativeIndex).trimmed();
            negativePrompt = beforeParams.mid(negativeIndex + 16).trimmed();
        } else {
            positivePrompt = beforeParams.trimmed();
        }
    }

    ui->txtParams->setText(params);
    posTagWidget->setData(parsePromptToMap(positivePrompt));
    negTagWidget->setData(parsePromptToMap(negativePrompt));
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
    QFile file(settingsPath());
    QJsonObject root;
    if (file.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(file.readAll()).object();
    }

    ui->editWd14ModelPath->setText(root.value("wd14_model_dir").toString());
    ui->editWd14PythonPath->setText(root.value("wd14_python_path").toString());
    ui->editWd14ScriptPath->setText(root.value("wd14_script_path").toString());
    ui->spinWd14Threshold->setValue(root.value("wd14_threshold").toDouble(0.35));
    ui->sliderWd14Threshold->setValue(qRound(ui->spinWd14Threshold->value() * 100.0));
    ui->editWd14AdditionalTags->setText(root.value("wd14_additional_tags").toString());
    ui->editWd14ExcludeTags->setText(root.value("wd14_exclude_tags").toString());
    ui->editWd14DefaultExclude->setText(root.value("wd14_default_exclude").toString(ui->editWd14DefaultExclude->text()));
    ui->chkWd14SortAlphabetically->setChecked(root.value("wd14_sort_alpha").toBool(false));
    ui->chkWd14IncludeConfidence->setChecked(root.value("wd14_include_confidence").toBool(false));
    ui->chkWd14ReplaceUnderscore->setChecked(root.value("wd14_replace_underscore").toBool(true));
    ui->chkWd14EscapeBrackets->setChecked(root.value("wd14_escape_brackets").toBool(false));
    ui->chkWd14UnloadAfterInference->setChecked(root.value("wd14_unload_after_inference").toBool(false));

    const QString presetDir = wd14PresetDirectory();
    QDir dir(presetDir);
    if (!dir.exists()) dir.mkpath(".");
    const QStringList presets = dir.entryList({"*.json"}, QDir::Files, QDir::Name);
    ui->comboWd14Preset->clear();
    if (presets.isEmpty()) {
        ui->comboWd14Preset->addItem("default.json");
    } else {
        ui->comboWd14Preset->addItems(presets);
    }
    const QString activePreset = root.value("wd14_active_preset").toString("default.json");
    const int presetIndex = ui->comboWd14Preset->findText(activePreset);
    if (presetIndex >= 0) ui->comboWd14Preset->setCurrentIndex(presetIndex);
}

void PromptParserWidget::saveWd14Settings() const
{
    const QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);

    QJsonObject root;
    QFile readFile(settingsPath());
    if (readFile.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(readFile.readAll()).object();
    }

    root["wd14_model_dir"] = ui->editWd14ModelPath->text().trimmed();
    root["wd14_python_path"] = ui->editWd14PythonPath->text().trimmed();
    root["wd14_script_path"] = ui->editWd14ScriptPath->text().trimmed();
    root["wd14_threshold"] = ui->spinWd14Threshold->value();
    root["wd14_additional_tags"] = ui->editWd14AdditionalTags->text().trimmed();
    root["wd14_exclude_tags"] = ui->editWd14ExcludeTags->text().trimmed();
    root["wd14_default_exclude"] = ui->editWd14DefaultExclude->text().trimmed();
    root["wd14_sort_alpha"] = ui->chkWd14SortAlphabetically->isChecked();
    root["wd14_include_confidence"] = ui->chkWd14IncludeConfidence->isChecked();
    root["wd14_replace_underscore"] = ui->chkWd14ReplaceUnderscore->isChecked();
    root["wd14_escape_brackets"] = ui->chkWd14EscapeBrackets->isChecked();
    root["wd14_unload_after_inference"] = ui->chkWd14UnloadAfterInference->isChecked();
    root["wd14_active_preset"] = ui->comboWd14Preset->currentText().trimmed().isEmpty()
        ? "default.json"
        : ui->comboWd14Preset->currentText().trimmed();

    QFile file(settingsPath());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson());
    }
}

QString PromptParserWidget::wd14PresetDirectory() const
{
    return qApp->applicationDirPath() + "/config/wd14_presets";
}

QString PromptParserWidget::extractedWd14ScriptPath() const
{
    const QString tempDir = QDir(QDir::tempPath()).filePath("SD_LoRA_Manager/scripts");
    QDir().mkpath(tempDir);

    const QString targetPath = QDir(tempDir).filePath("wd14_tagger.py");
    QFile resource(":/scripts/wd14_tagger.py");
    if (!resource.open(QIODevice::ReadOnly)) {
        return QString();
    }

    const QByteArray resourceBytes = resource.readAll();
    QFile existing(targetPath);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == resourceBytes) {
        existing.close();
        return QFileInfo(targetPath).absoluteFilePath();
    }

    QSaveFile out(targetPath);
    if (!out.open(QIODevice::WriteOnly)) {
        return QString();
    }
    out.write(resourceBytes);
    if (!out.commit()) {
        return QString();
    }
    return QFileInfo(targetPath).absoluteFilePath();
}

QString PromptParserWidget::defaultWd14ScriptPath() const
{
    const QString extractedScript = extractedWd14ScriptPath();
    if (!extractedScript.isEmpty() && QFile::exists(extractedScript)) return extractedScript;

    const QString appScript = QDir(qApp->applicationDirPath()).filePath("scripts/wd14_tagger.py");
    if (QFile::exists(appScript)) return appScript;
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../../scripts/wd14_tagger.py");
}

QString PromptParserWidget::selectedWd14ScriptPath() const
{
    const QString explicitPath = ui->editWd14ScriptPath->text().trimmed();
    if (!explicitPath.isEmpty() && QFile::exists(explicitPath)) return explicitPath;
    return defaultWd14ScriptPath();
}

QString PromptParserWidget::selectedPythonPath() const
{
    const QString explicitPath = ui->editWd14PythonPath->text().trimmed();
    if (!explicitPath.isEmpty()) return explicitPath;
    const QString python = QStandardPaths::findExecutable("python");
    if (!python.isEmpty()) return python;
    const QString python3 = QStandardPaths::findExecutable("python3");
    return python3.isEmpty() ? QStringLiteral("python") : python3;
}

void PromptParserWidget::saveWd14Preset()
{
    QString presetName = ui->comboWd14Preset->currentText().trimmed();
    if (presetName.isEmpty()) presetName = "default.json";
    if (!presetName.endsWith(".json", Qt::CaseInsensitive)) presetName += ".json";

    QJsonObject preset;
    preset["model_dir"] = ui->editWd14ModelPath->text().trimmed();
    preset["python_path"] = ui->editWd14PythonPath->text().trimmed();
    preset["script_path"] = ui->editWd14ScriptPath->text().trimmed();
    preset["threshold"] = ui->spinWd14Threshold->value();
    preset["additional_tags"] = ui->editWd14AdditionalTags->text().trimmed();
    preset["exclude_tags"] = ui->editWd14ExcludeTags->text().trimmed();
    preset["default_exclude"] = ui->editWd14DefaultExclude->text().trimmed();
    preset["sort_alpha"] = ui->chkWd14SortAlphabetically->isChecked();
    preset["include_confidence"] = ui->chkWd14IncludeConfidence->isChecked();
    preset["replace_underscore"] = ui->chkWd14ReplaceUnderscore->isChecked();
    preset["escape_brackets"] = ui->chkWd14EscapeBrackets->isChecked();
    preset["unload_after_inference"] = ui->chkWd14UnloadAfterInference->isChecked();

    QDir().mkpath(wd14PresetDirectory());
    QFile file(QDir(wd14PresetDirectory()).filePath(presetName));
    if (!file.open(QIODevice::WriteOnly)) {
        ui->lblWd14Status->setText("预设保存失败。");
        return;
    }
    file.write(QJsonDocument(preset).toJson());
    if (ui->comboWd14Preset->findText(presetName) < 0) {
        ui->comboWd14Preset->addItem(presetName);
    }
    ui->comboWd14Preset->setCurrentText(presetName);
    saveWd14Settings();
    ui->lblWd14Status->setText("已保存预设: " + presetName);
}

void PromptParserWidget::loadWd14Preset(const QString &presetName)
{
    const QString path = QDir(wd14PresetDirectory()).filePath(presetName);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;

    const QJsonObject preset = QJsonDocument::fromJson(file.readAll()).object();
    ui->editWd14ModelPath->setText(preset.value("model_dir").toString());
    ui->editWd14PythonPath->setText(preset.value("python_path").toString());
    ui->editWd14ScriptPath->setText(preset.value("script_path").toString());
    ui->spinWd14Threshold->setValue(preset.value("threshold").toDouble(0.35));
    ui->editWd14AdditionalTags->setText(preset.value("additional_tags").toString());
    ui->editWd14ExcludeTags->setText(preset.value("exclude_tags").toString());
    ui->editWd14DefaultExclude->setText(preset.value("default_exclude").toString(ui->editWd14DefaultExclude->text()));
    ui->chkWd14SortAlphabetically->setChecked(preset.value("sort_alpha").toBool(false));
    ui->chkWd14IncludeConfidence->setChecked(preset.value("include_confidence").toBool(false));
    ui->chkWd14ReplaceUnderscore->setChecked(preset.value("replace_underscore").toBool(true));
    ui->chkWd14EscapeBrackets->setChecked(preset.value("escape_brackets").toBool(false));
    ui->chkWd14UnloadAfterInference->setChecked(preset.value("unload_after_inference").toBool(false));
    saveWd14Settings();
    ui->lblWd14Status->setText("已加载预设: " + presetName);
}

void PromptParserWidget::browseWd14ModelPath()
{
    const QString dir = QFileDialog::getExistingDirectory(this, "选择 WD14 模型目录", ui->editWd14ModelPath->text().trimmed());
    if (dir.isEmpty()) return;
    ui->editWd14ModelPath->setText(dir);
    saveWd14Settings();
}

void PromptParserWidget::browseWd14PythonPath()
{
    const QString file = QFileDialog::getOpenFileName(
        this,
        "选择 Python",
        QFileInfo(ui->editWd14PythonPath->text().trimmed()).absolutePath(),
        "Python (python.exe python);;Executable Files (*.exe);;All Files (*)");
    if (file.isEmpty()) return;
    ui->editWd14PythonPath->setText(file);
    saveWd14Settings();
}

void PromptParserWidget::browseWd14ScriptPath()
{
    const QString file = QFileDialog::getOpenFileName(
        this,
        "选择 WD14 Python 脚本",
        QFileInfo(selectedWd14ScriptPath()).absolutePath(),
        "Python Scripts (*.py);;All Files (*)");
    if (file.isEmpty()) return;
    ui->editWd14ScriptPath->setText(file);
    saveWd14Settings();
}

void PromptParserWidget::updateWd14ThresholdFromSlider(int value)
{
    QSignalBlocker blocker(ui->spinWd14Threshold);
    ui->spinWd14Threshold->setValue(double(value) / 100.0);
    saveWd14Settings();
}

void PromptParserWidget::updateWd14ThresholdFromSpin(double value)
{
    QSignalBlocker blocker(ui->sliderWd14Threshold);
    ui->sliderWd14Threshold->setValue(qRound(value * 100.0));
    saveWd14Settings();
}

void PromptParserWidget::setWd14Running(bool running)
{
    ui->btnWd14Run->setEnabled(!running);
    ui->btnWd14BrowseModel->setEnabled(!running);
    ui->btnWd14BrowsePython->setEnabled(!running);
    ui->btnWd14BrowseScript->setEnabled(!running);
    ui->editWd14PythonPath->setEnabled(!running);
    ui->editWd14ScriptPath->setEnabled(!running);
    ui->btnWd14SavePreset->setEnabled(!running);
    ui->comboWd14Preset->setEnabled(!running);
    ui->lblWd14Image->setEnabled(!running);
}

void PromptParserWidget::runWd14Tagger()
{
    if (wd14Process->state() != QProcess::NotRunning) return;
    if (wd14ImagePath.isEmpty() || !QFile::exists(wd14ImagePath)) {
        ui->lblWd14Status->setText("请先选择需要反推的图片。");
        return;
    }

    const QString modelDir = ui->editWd14ModelPath->text().trimmed();
    if (modelDir.isEmpty()) {
        ui->lblWd14Status->setText("请先选择 WD14 模型目录。");
        return;
    }

    const QString scriptPath = selectedWd14ScriptPath();
    if (!QFile::exists(scriptPath)) {
        ui->lblWd14Status->setText("未找到 WD14 脚本: " + scriptPath);
        return;
    }

    QStringList args;
    args << scriptPath
         << "--image" << wd14ImagePath
         << "--model-dir" << modelDir
         << "--threshold" << QString::number(ui->spinWd14Threshold->value(), 'f', 4);

    saveWd14Settings();
    setWd14Running(true);
    ui->lblWd14Status->setText("WD14 Python 反推运行中...");
    ui->lblWd14Elapsed->setText("用时: 计算中...");
    wd14Process->start(selectedPythonPath(), args);
}

void PromptParserWidget::onWd14Finished()
{
    // Kept for moc compatibility with older builds; QProcess finished is handled by a lambda.
}

Wd14InferenceResult PromptParserWidget::parseWd14ProcessOutput(const QByteArray &stdoutBytes, const QByteArray &stderrBytes, int exitCode) const
{
    Wd14InferenceResult result;
    const auto memory = systemMemorySnapshot();
    result.totalMemory = memory.first;
    result.availableMemory = memory.second;

    QByteArray jsonBytes = stdoutBytes.trimmed();
    const int firstBrace = jsonBytes.indexOf('{');
    const int lastBrace = jsonBytes.lastIndexOf('}');
    if (firstBrace >= 0 && lastBrace > firstBrace) {
        jsonBytes = jsonBytes.mid(firstBrace, lastBrace - firstBrace + 1);
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        QString message = QString("WD14 Python 输出不是有效 JSON (exit %1): %2").arg(exitCode).arg(parseError.errorString());
        const QString stderrText = QString::fromUtf8(stderrBytes).trimmed();
        if (!stderrText.isEmpty()) message += "\n" + stderrText.left(1000);
        const QString stdoutText = QString::fromUtf8(stdoutBytes).trimmed();
        if (!stdoutText.isEmpty()) message += "\nstdout:\n" + stdoutText.left(1000);
        result.error = message;
        return result;
    }

    const QJsonObject obj = doc.object();
    result.ok = obj.value("ok").toBool(false) && exitCode == 0;
    result.error = obj.value("error").toString();
    result.elapsedSec = obj.value("elapsed_sec").toDouble(0.0);

    const QJsonArray ratings = obj.value("ratings").toArray();
    for (const QJsonValue &value : ratings) {
        const Wd14TagScore score = parseScoreObject(value.toObject());
        if (!score.tag.isEmpty()) result.ratings.append(score);
    }

    const QJsonArray tags = obj.value("tags").toArray();
    for (const QJsonValue &value : tags) {
        const Wd14TagScore score = parseScoreObject(value.toObject());
        if (!score.tag.isEmpty()) result.tags.append(score);
    }

    if (!result.ok && result.error.isEmpty()) {
        result.error = QString::fromUtf8(stderrBytes).trimmed();
        if (result.error.isEmpty()) result.error = QString("WD14 Python 反推失败 (exit %1)。").arg(exitCode);
    }
    return result;
}

QStringList PromptParserWidget::splitWd14TagList(const QString &text) const
{
    QString normalized = text;
    normalized.replace("\r\n", ",");
    normalized.replace("\n", ",");
    normalized.replace("\r", ",");
    QStringList result;
    const QStringList parts = normalized.split(",", Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QString cleaned = part.trimmed();
        if (!cleaned.isEmpty()) result.append(cleaned);
    }
    return result;
}

QString PromptParserWidget::formatWd14Tag(const QString &tag) const
{
    QString formatted = tag.trimmed();
    if (ui->chkWd14ReplaceUnderscore->isChecked()) {
        formatted.replace("_", " ");
    }
    if (ui->chkWd14EscapeBrackets->isChecked()) {
        formatted = escapeParentheses(formatted);
    }
    return formatted;
}

QString PromptParserWidget::translateTag(const QString &tag) const
{
    if (!m_translationMap) return "";
    if (m_translationMap->contains(tag)) return m_translationMap->value(tag);
    QString swapped = tag;
    swapped.replace(' ', '_');
    if (m_translationMap->contains(swapped)) return m_translationMap->value(swapped);
    swapped = tag;
    swapped.replace('_', ' ');
    if (m_translationMap->contains(swapped)) return m_translationMap->value(swapped);
    return "";
}

void PromptParserWidget::applyWd14Result(const Wd14InferenceResult &result)
{
    ui->treeWd14Ratings->clear();
    ui->treeWd14Tags->clear();

    for (Wd14TagScore rating : result.ratings) {
        auto *item = new QTreeWidgetItem(ui->treeWd14Ratings);
        item->setText(0, formatWd14Tag(rating.tag));
        item->setText(1, QString::number(rating.confidence * 100.0f, 'f', 2) + "%");
    }

    QSet<QString> excluded;
    for (const QString &tag : splitWd14TagList(ui->editWd14ExcludeTags->text())) excluded.insert(tag);
    for (const QString &tag : splitWd14TagList(ui->editWd14DefaultExclude->text())) excluded.insert(tag);

    QVector<Wd14TagScore> visibleTags;
    visibleTags.reserve(result.tags.size());
    for (Wd14TagScore score : result.tags) {
        if (excluded.contains(score.tag)) continue;
        score.translation = translateTag(score.tag);
        visibleTags.append(score);
    }

    if (ui->chkWd14SortAlphabetically->isChecked()) {
        std::sort(visibleTags.begin(), visibleTags.end(), [](const Wd14TagScore &a, const Wd14TagScore &b) {
            return QString::compare(a.tag, b.tag, Qt::CaseInsensitive) < 0;
        });
    }

    QStringList finalTags;
    QSet<QString> seen;
    for (const Wd14TagScore &score : visibleTags) {
        const QString formatted = formatWd14Tag(score.tag);
        if (formatted.isEmpty() || seen.contains(formatted)) continue;
        seen.insert(formatted);
        if (ui->chkWd14IncludeConfidence->isChecked()) {
            finalTags.append(QString("%1 (%2%)").arg(formatted).arg(score.confidence * 100.0f, 0, 'f', 2));
        } else {
            finalTags.append(formatted);
        }

        auto *item = new QTreeWidgetItem(ui->treeWd14Tags);
        item->setText(0, formatted);
        item->setText(1, QString::number(score.confidence * 100.0f, 'f', 2) + "%");
        item->setText(2, score.translation);
    }

    for (const QString &extraTag : splitWd14TagList(ui->editWd14AdditionalTags->text())) {
        if (excluded.contains(extraTag)) continue;
        const QString formatted = formatWd14Tag(extraTag);
        if (formatted.isEmpty() || seen.contains(formatted)) continue;
        seen.insert(formatted);
        finalTags.append(formatted);
    }

    wd14LastTagsText = finalTags.join(", ");
    ui->txtWd14FinalTags->setPlainText(wd14LastTagsText);
    ui->btnWd14Copy->setEnabled(!wd14LastTagsText.isEmpty());
    ui->treeWd14Ratings->resizeColumnToContents(0);
    ui->treeWd14Ratings->resizeColumnToContents(1);
    ui->treeWd14Tags->resizeColumnToContents(0);
    ui->treeWd14Tags->resizeColumnToContents(1);
}

void PromptParserWidget::updateWd14MemoryLabel(quint64 totalBytes, quint64 availableBytes)
{
    if (totalBytes == 0) {
        ui->lblWd14Memory->setText("Sys: --");
        return;
    }

    const quint64 usedBytes = totalBytes > availableBytes ? totalBytes - availableBytes : 0;
    const double usage = totalBytes == 0 ? 0.0 : double(usedBytes) * 100.0 / double(totalBytes);
    ui->lblWd14Memory->setText(
        QString("Sys: %1/%2 (%3%)")
            .arg(formatMemoryBytes(usedBytes))
            .arg(formatMemoryBytes(totalBytes))
            .arg(usage, 0, 'f', 1));
}
