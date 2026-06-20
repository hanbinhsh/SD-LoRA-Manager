#include "promptparserwidget.h"
#include "ui_promptparserwidget.h"
#include "tagutils.h"
#include "imagemetadataparser.h"
#include "tableviewstylehelper.h"
#include "styleconstants.h"

#include <QAbstractItemView>
#include <QAction>
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
#include <QKeySequence>
#include <QHeaderView>
#include <QMessageBox>
#include <QMimeData>
#include <QMenu>
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
#include <QTableWidgetItem>
#include <QTreeWidgetItem>
#include <QTreeWidget>
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

class Wd14ScoreItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override
    {
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        if (column == 1) {
            return data(column, Qt::UserRole).toFloat() < other.data(column, Qt::UserRole).toFloat();
        }
        return QString::localeAwareCompare(text(column), other.text(column)) < 0;
    }
};

} // namespace

PromptParserWidget::PromptParserWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::PromptParserWidget)
    , m_translationMap(nullptr)
{
    ui->setupUi(this);
    setStyleSheet(AppStyle::loadToolPageQss());

    setAcceptDrops(true);
    ui->lblImage->installEventFilter(this);
    ui->lblWd14Image->installEventFilter(this);
    ui->lblComparePreviewA->setAcceptDrops(true);
    ui->lblComparePreviewB->setAcceptDrops(true);
    ui->lblComparePreviewA->installEventFilter(this);
    ui->lblComparePreviewB->installEventFilter(this);

    applyUnifiedTableRowStyle(ui->tableCompareParams);

    posTagWidget = new TagFlowWidget(ui->scrollAreaWidgetContentsPos);
    negTagWidget = new TagFlowWidget(ui->scrollAreaWidgetContentsNeg);
    compareTagWidgetA = new TagFlowWidget(ui->compareTagsAContainer);
    compareTagWidgetB = new TagFlowWidget(ui->compareTagsBContainer);
    posTagWidget->setPixmapCacheEnabled(false);
    negTagWidget->setPixmapCacheEnabled(false);
    compareTagWidgetA->setPixmapCacheEnabled(false);
    compareTagWidgetB->setPixmapCacheEnabled(false);

    const bool showTrans = ui->btnTranslate->isChecked();
    posTagWidget->setShowTranslation(showTrans);
    negTagWidget->setShowTranslation(showTrans);
    compareTagWidgetA->setShowTranslation(ui->chkCompareTranslate->isChecked());
    compareTagWidgetB->setShowTranslation(ui->chkCompareTranslate->isChecked());

    ui->layoutTagsPos->addWidget(posTagWidget);
    ui->layoutTagsNeg->addWidget(negTagWidget);
    ui->layoutCompareTagsAContainer->addWidget(compareTagWidgetA);
    ui->layoutCompareTagsBContainer->addWidget(compareTagWidgetB);

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
    connect(ui->btnSelectAllTags, &QPushButton::clicked, this, [this]() {
        posTagWidget->selectAllVisibleTags();
        negTagWidget->selectAllVisibleTags();
    });
    connect(ui->btnClearTagSelection, &QPushButton::clicked, this, [this]() {
        posTagWidget->clearSelectedTags();
        negTagWidget->clearSelectedTags();
    });

    ui->tableCompareParams->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ui->tableCompareParams->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->tableCompareParams->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    ui->tableCompareParams->verticalHeader()->hide();
    ui->tableCompareParams->setShowGrid(false);
    ui->tableCompareParams->setFocusPolicy(Qt::NoFocus);
    ui->splitterCompareMain->setSizes({300, 280});
    ui->splitterCompareTags->setSizes({1, 1});
    ui->splitterCompareBottom->setSizes({1, 1});
    ui->listCompareOnlyA->setToolTip("仅图片 A 中存在的 Tag");
    ui->listCompareOnlyB->setToolTip("仅图片 B 中存在的 Tag");
    ui->listCompareCommon->setToolTip("两张图片共同存在的 Tag");
    connect(ui->chkCompareNegative, &QCheckBox::toggled, this, &PromptParserWidget::updateImageCompare);
    connect(ui->chkCompareTranslate, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && (!m_translationMap || m_translationMap->isEmpty())) {
            QMessageBox::warning(this, "提示", "未加载翻译词表，请在设置中配置 CSV 文件。");
            QSignalBlocker blocker(ui->chkCompareTranslate);
            ui->chkCompareTranslate->setChecked(false);
            checked = false;
        }
        compareTagWidgetA->setShowTranslation(checked);
        compareTagWidgetB->setShowTranslation(checked);
    });
    connect(ui->btnCopyOnlyA, &QPushButton::clicked, this, [this]() { copyCompareTags(compareOnlyATags, "仅图片 A"); });
    connect(ui->btnCopyOnlyB, &QPushButton::clicked, this, [this]() { copyCompareTags(compareOnlyBTags, "仅图片 B"); });
    connect(ui->btnCopyCommon, &QPushButton::clicked, this, [this]() { copyCompareTags(compareCommonTags, "共同 Tag"); });
    connect(ui->btnCopyCompareAll, &QPushButton::clicked, this, &PromptParserWidget::copyCompareAll);

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
    connect(ui->btnWd14DeletePreset, &QPushButton::clicked, this, &PromptParserWidget::deleteWd14Preset);
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

    ui->btnWd14Copy->setEnabled(false);
    ui->treeWd14Ratings->setRootIsDecorated(false);
    ui->treeWd14Ratings->setAlternatingRowColors(true);
    ui->treeWd14Ratings->setSortingEnabled(true);
    ui->treeWd14Ratings->header()->setSectionsClickable(true);
    ui->treeWd14Tags->setRootIsDecorated(false);
    ui->treeWd14Tags->setAlternatingRowColors(true);
    ui->treeWd14Tags->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->treeWd14Tags->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->treeWd14Tags->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->treeWd14Tags->setSortingEnabled(true);
    ui->treeWd14Tags->header()->setSectionsClickable(true);
    connect(ui->treeWd14Tags, &QWidget::customContextMenuRequested,
            this, &PromptParserWidget::showWd14TagContextMenu);
    auto *copyWd14SelectedTagsAction = new QAction("复制选中 Tag", ui->treeWd14Tags);
    copyWd14SelectedTagsAction->setShortcut(QKeySequence::Copy);
    copyWd14SelectedTagsAction->setShortcutContext(Qt::WidgetShortcut);
    ui->treeWd14Tags->addAction(copyWd14SelectedTagsAction);
    connect(copyWd14SelectedTagsAction, &QAction::triggered, this, &PromptParserWidget::copySelectedWd14Tags);

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
    if (compareTagWidgetA) compareTagWidgetA->setTranslationMap(map);
    if (compareTagWidgetB) compareTagWidgetB->setTranslationMap(map);
}

bool PromptParserWidget::eventFilter(QObject *watched, QEvent *event)
{
    const auto imagePathFromEvent = [](QEvent *event) -> QString {
        const QMimeData *mimeData = nullptr;
        if (event->type() == QEvent::DragEnter) {
            mimeData = static_cast<QDragEnterEvent*>(event)->mimeData();
        } else if (event->type() == QEvent::Drop) {
            mimeData = static_cast<QDropEvent*>(event)->mimeData();
        }
        if (!mimeData || !mimeData->hasUrls() || mimeData->urls().isEmpty()) return QString();
        const QString filePath = mimeData->urls().first().toLocalFile();
        const QString lower = filePath.toLower();
        return (lower.endsWith(".png") || lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".webp"))
            ? filePath
            : QString();
    };
    if ((watched == ui->lblComparePreviewA || watched == ui->lblComparePreviewB) && event->type() == QEvent::DragEnter) {
        if (!imagePathFromEvent(event).isEmpty()) {
            static_cast<QDragEnterEvent*>(event)->acceptProposedAction();
            return true;
        }
    }
    if ((watched == ui->lblComparePreviewA || watched == ui->lblComparePreviewB) && event->type() == QEvent::Drop) {
        const QString filePath = imagePathFromEvent(event);
        if (!filePath.isEmpty()) {
            processCompareImage(watched == ui->lblComparePreviewA, filePath);
            static_cast<QDropEvent*>(event)->acceptProposedAction();
            return true;
        }
    }
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
    if ((watched == ui->lblComparePreviewA || watched == ui->lblComparePreviewB) && event->type() == QEvent::MouseButtonPress) {
        const bool imageA = watched == ui->lblComparePreviewA;
        const QString filePath = QFileDialog::getOpenFileName(this,
                                                              imageA ? "选择图片 A" : "选择图片 B",
                                                              "",
                                                              "Images (*.png *.jpg *.jpeg *.webp)");
        if (!filePath.isEmpty()) processCompareImage(imageA, filePath);
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
    } else if (ui->tabPromptParser->currentWidget() == ui->tabImageCompare) {
        const bool targetA = compareImagePathA.isEmpty() || !compareImagePathB.isEmpty();
        processCompareImage(targetA, filePath);
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

QMap<QString, int> PromptParserWidget::parsePromptToMap(const QString &rawPrompt)
{
    QMap<QString, int> result;
    const QStringList parts = TagUtils::splitPromptParts(rawPrompt, true);
    for (const QString &part : parts) {
        const QString clean = TagUtils::cleanPromptTag(part);
        if (!clean.isEmpty()) result[clean]++;
    }
    return result;
}

void PromptParserWidget::processImage(const QString &filePath)
{
    updateImagePreview(filePath);

    const ParsedImageMetadata parsed = parseImageMetadataFromFile(filePath);
    if (!parsed.hasContent()) {
        ui->txtParams->setText("未找到生成参数 / No generation parameters found.");
        posTagWidget->setData({});
        negTagWidget->setData({});
        return;
    }

    ui->txtParams->setText(parsed.parametersText);
    posTagWidget->setData(parsePromptToMap(parsed.positivePrompt));
    negTagWidget->setData(parsePromptToMap(parsed.negativePrompt));
}

void PromptParserWidget::processCompareImage(bool imageA, const QString &filePath)
{
    if (filePath.isEmpty() || !QFile::exists(filePath)) {
        ui->lblCompareStatus->setText(imageA ? "图片 A 不存在。" : "图片 B 不存在。");
        return;
    }

    ParsedImageMetadata parsed = parseImageMetadataFromFile(filePath);
    if (imageA) {
        compareImagePathA = filePath;
        compareMetaA = parsed;
        updateImageLabelPreview(ui->lblComparePreviewA, filePath, "图片 A 加载失败");
    } else {
        compareImagePathB = filePath;
        compareMetaB = parsed;
        updateImageLabelPreview(ui->lblComparePreviewB, filePath, "图片 B 加载失败");
    }
    updateImageCompare();
}

QString PromptParserWidget::normalizeCompareTag(QString tag) const
{
    tag = TagUtils::cleanPromptTag(tag).toCaseFolded().trimmed();
    tag.replace('_', ' ');
    static const QRegularExpression spaces("\\s+");
    tag.replace(spaces, " ");
    return tag;
}

void PromptParserWidget::fillCompareList(QListWidget *list, const QStringList &tags)
{
    if (!list) return;
    list->clear();
    list->addItems(tags);
}

QString PromptParserWidget::extractParameterLine(const QString &parameters, const QStringList &keys) const
{
    QString normalized = parameters;
    normalized.replace("\r\n", "\n");
    normalized.replace('\r', '\n');
    normalized.replace('\n', ",");
    const QStringList parts = normalized.split(',', Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QString trimmed = part.trimmed();
        for (const QString &key : keys) {
            if (trimmed.startsWith(key + ":", Qt::CaseInsensitive)) {
                return trimmed.mid(key.size() + 1).trimmed();
            }
        }
    }
    return QString();
}

QString PromptParserWidget::compareParamValue(const ParsedImageMetadata &meta, const QString &key) const
{
    if (key == "Source") return meta.sourceType;
    if (key == "Seed") return meta.seed.isEmpty() ? extractParameterLine(meta.parametersText, {"Seed"}) : meta.seed;
    if (key == "Steps") return meta.steps.isEmpty() ? extractParameterLine(meta.parametersText, {"Steps"}) : meta.steps;
    if (key == "CFG") return meta.cfg.isEmpty() ? extractParameterLine(meta.parametersText, {"CFG scale", "CFG"}) : meta.cfg;
    if (key == "Sampler") return meta.sampler.isEmpty() ? extractParameterLine(meta.parametersText, {"Sampler"}) : meta.sampler;
    if (key == "Scheduler") return meta.scheduler.isEmpty() ? extractParameterLine(meta.parametersText, {"Schedule type", "Scheduler"}) : meta.scheduler;
    if (key == "Model") return meta.checkpoint.isEmpty() ? extractParameterLine(meta.parametersText, {"Model"}) : meta.checkpoint;
    if (key == "LoRA") {
        if (!meta.loraDescriptions.isEmpty()) return meta.loraDescriptions.join(", ");
        QStringList loras;
        static const QRegularExpression loraRegex("<\\s*lora:([^>]+)>", QRegularExpression::CaseInsensitiveOption);
        auto it = loraRegex.globalMatch(meta.positivePrompt + ", " + meta.negativePrompt);
        while (it.hasNext()) loras << it.next().captured(1).trimmed();
        if (!loras.isEmpty()) return loras.join(", ");
        return extractParameterLine(meta.parametersText, {"Lora hashes", "LoRA", "ComfyUI LoRAs"});
    }
    return QString();
}

void PromptParserWidget::fillCompareParams()
{
    const QStringList keys = {"Source", "Seed", "Steps", "CFG", "Sampler", "Scheduler", "Model", "LoRA"};
    ui->tableCompareParams->setRowCount(0);
    for (const QString &key : keys) {
        const QString valueA = compareParamValue(compareMetaA, key);
        const QString valueB = compareParamValue(compareMetaB, key);
        if (valueA.isEmpty() && valueB.isEmpty()) continue;
        const int row = ui->tableCompareParams->rowCount();
        ui->tableCompareParams->insertRow(row);
        auto *keyItem = new QTableWidgetItem(key);
        auto *itemA = new QTableWidgetItem(valueA.isEmpty() ? "-" : valueA);
        auto *itemB = new QTableWidgetItem(valueB.isEmpty() ? "-" : valueB);
        if (QString::compare(valueA, valueB, Qt::CaseInsensitive) != 0) {
            itemA->setBackground(AppStyle::imageCompareOnlyA());
            itemB->setBackground(AppStyle::imageCompareOnlyB());
        }
        ui->tableCompareParams->setItem(row, 0, keyItem);
        ui->tableCompareParams->setItem(row, 1, itemA);
        ui->tableCompareParams->setItem(row, 2, itemB);
    }
}

void PromptParserWidget::updateImageCompare()
{
    const bool useNegative = ui->chkCompareNegative->isChecked();
    const QString promptA = useNegative ? compareMetaA.negativePrompt : compareMetaA.positivePrompt;
    const QString promptB = useNegative ? compareMetaB.negativePrompt : compareMetaB.positivePrompt;
    const QMap<QString, int> tagsA = parsePromptToMap(promptA);
    const QMap<QString, int> tagsB = parsePromptToMap(promptB);

    compareTagWidgetA->setData(tagsA);
    compareTagWidgetB->setData(tagsB);

    QHash<QString, QString> displayA;
    QHash<QString, QString> displayB;
    for (const QString &tag : tagsA.keys()) {
        const QString key = normalizeCompareTag(tag);
        if (!key.isEmpty() && !displayA.contains(key)) displayA.insert(key, tag);
    }
    for (const QString &tag : tagsB.keys()) {
        const QString key = normalizeCompareTag(tag);
        if (!key.isEmpty() && !displayB.contains(key)) displayB.insert(key, tag);
    }

    compareOnlyATags.clear();
    compareOnlyBTags.clear();
    compareCommonTags.clear();
    QHash<QString, TagFlowWidget::DiffState> statesA;
    QHash<QString, TagFlowWidget::DiffState> statesB;

    for (auto it = displayA.constBegin(); it != displayA.constEnd(); ++it) {
        if (displayB.contains(it.key())) {
            compareCommonTags << it.value();
            statesA.insert(it.value(), TagFlowWidget::DiffCommon);
            statesB.insert(displayB.value(it.key()), TagFlowWidget::DiffCommon);
        } else {
            compareOnlyATags << it.value();
            statesA.insert(it.value(), TagFlowWidget::DiffOnlyA);
        }
    }
    for (auto it = displayB.constBegin(); it != displayB.constEnd(); ++it) {
        if (!displayA.contains(it.key())) {
            compareOnlyBTags << it.value();
            statesB.insert(it.value(), TagFlowWidget::DiffOnlyB);
        }
    }
    auto sortTags = [](QStringList &tags) {
        std::sort(tags.begin(), tags.end(), [](const QString &a, const QString &b) {
            return QString::compare(a, b, Qt::CaseInsensitive) < 0;
        });
    };
    sortTags(compareOnlyATags);
    sortTags(compareOnlyBTags);
    sortTags(compareCommonTags);

    compareTagWidgetA->setTagDiffStates(statesA);
    compareTagWidgetB->setTagDiffStates(statesB);
    fillCompareList(ui->listCompareOnlyA, compareOnlyATags);
    fillCompareList(ui->listCompareOnlyB, compareOnlyBTags);
    fillCompareList(ui->listCompareCommon, compareCommonTags);
    fillCompareParams();

    const bool hasA = compareMetaA.hasContent();
    const bool hasB = compareMetaB.hasContent();
    if (!hasA && !hasB) {
        ui->lblCompareStatus->setText("请选择两张包含元数据的图片。");
    } else if (!hasA || !hasB) {
        ui->lblCompareStatus->setText(hasA ? "图片 B 未解析到可用元数据。" : "图片 A 未解析到可用元数据。");
    } else {
        ui->lblCompareStatus->setText(QString("%1对比：仅 A %2 个，仅 B %3 个，共同 %4 个。")
            .arg(useNegative ? "负面 Tag " : "正面 Tag ")
            .arg(compareOnlyATags.size())
            .arg(compareOnlyBTags.size())
            .arg(compareCommonTags.size()));
    }
}

void PromptParserWidget::copyCompareTags(const QStringList &tags, const QString &label)
{
    QApplication::clipboard()->setText(tags.join(", "));
    ui->lblCompareStatus->setText(QString("已复制 %1：%2 个 Tag。").arg(label).arg(tags.size()));
}

void PromptParserWidget::copyCompareAll()
{
    QStringList lines;
    lines << "Image Compare";
    lines << "Image A: " + (compareImagePathA.isEmpty() ? "-" : compareImagePathA);
    lines << "Image B: " + (compareImagePathB.isEmpty() ? "-" : compareImagePathB);
    lines << "Scope: " + QString(ui->chkCompareNegative->isChecked() ? "Negative" : "Positive");
    lines << "";
    lines << "Only A:";
    lines << compareOnlyATags.join(", ");
    lines << "";
    lines << "Only B:";
    lines << compareOnlyBTags.join(", ");
    lines << "";
    lines << "Common:";
    lines << compareCommonTags.join(", ");
    lines << "";
    lines << "Parameter differences:";
    for (int row = 0; row < ui->tableCompareParams->rowCount(); ++row) {
        const QString key = ui->tableCompareParams->item(row, 0) ? ui->tableCompareParams->item(row, 0)->text() : QString();
        const QString a = ui->tableCompareParams->item(row, 1) ? ui->tableCompareParams->item(row, 1)->text() : QString();
        const QString b = ui->tableCompareParams->item(row, 2) ? ui->tableCompareParams->item(row, 2)->text() : QString();
        if (a != b) lines << QString("%1: A=%2 | B=%3").arg(key, a, b);
    }
    QApplication::clipboard()->setText(lines.join("\n"));
    ui->lblCompareStatus->setText("已复制全部图片差异。");
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
    if (presetIndex >= 0 && QFile::exists(QDir(wd14PresetDirectory()).filePath(activePreset))) {
        applyWd14Preset(activePreset, false);
    }
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

void PromptParserWidget::deleteWd14Preset()
{
    QString presetName = ui->comboWd14Preset->currentText().trimmed();
    if (presetName.isEmpty()) {
        ui->lblWd14Status->setText("没有可删除的预设。");
        return;
    }
    if (!presetName.endsWith(".json", Qt::CaseInsensitive)) presetName += ".json";

    const QString presetPath = QDir(wd14PresetDirectory()).filePath(presetName);
    if (!QFile::exists(presetPath)) {
        ui->lblWd14Status->setText("预设文件不存在: " + presetName);
        return;
    }

    const auto reply = QMessageBox::question(
        this,
        "删除预设",
        QString("确定要删除预设 \"%1\" 吗？").arg(presetName),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    if (!QFile::remove(presetPath)) {
        ui->lblWd14Status->setText("预设删除失败: " + presetName);
        return;
    }

    QDir dir(wd14PresetDirectory());
    const QStringList presets = dir.entryList({"*.json"}, QDir::Files, QDir::Name);
    ui->comboWd14Preset->clear();
    if (presets.isEmpty()) {
        ui->comboWd14Preset->addItem("default.json");
    } else {
        ui->comboWd14Preset->addItems(presets);
    }

    const QString nextPreset = ui->comboWd14Preset->currentText().trimmed();
    if (!nextPreset.isEmpty() && QFile::exists(QDir(wd14PresetDirectory()).filePath(nextPreset))) {
        applyWd14Preset(nextPreset, true);
    } else {
        saveWd14Settings();
    }
    ui->lblWd14Status->setText("已删除预设: " + presetName);
}

void PromptParserWidget::loadWd14Preset(const QString &presetName)
{
    applyWd14Preset(presetName, true);
}

bool PromptParserWidget::applyWd14Preset(const QString &presetName, bool persistActivePreset)
{
    const QString path = QDir(wd14PresetDirectory()).filePath(presetName);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;

    const QJsonObject preset = QJsonDocument::fromJson(file.readAll()).object();
    if (preset.isEmpty()) return false;
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
    if (persistActivePreset) saveWd14Settings();
    ui->lblWd14Status->setText("已加载预设: " + presetName);
    return true;
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

void PromptParserWidget::copySelectedWd14Tags()
{
    QSet<QTreeWidgetItem*> selectedSet;
    for (QTreeWidgetItem *item : ui->treeWd14Tags->selectedItems()) {
        if (item) selectedSet.insert(item);
    }

    QStringList tags;
    for (int i = 0; i < ui->treeWd14Tags->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = ui->treeWd14Tags->topLevelItem(i);
        if (!selectedSet.contains(item)) continue;
        const QString tag = item->text(0).trimmed();
        if (!tag.isEmpty()) tags.append(tag);
    }

    if (tags.isEmpty()) {
        ui->lblWd14Status->setText("请先选择需要复制的 WD14 Tag。");
        return;
    }

    QApplication::clipboard()->setText(tags.join(", "));
    ui->lblWd14Status->setText(QString("已复制 %1 个 WD14 Tag。").arg(tags.size()));
}

void PromptParserWidget::showWd14TagContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *clickedItem = ui->treeWd14Tags->itemAt(pos);
    if (clickedItem && !clickedItem->isSelected()) {
        ui->treeWd14Tags->setCurrentItem(clickedItem);
    }

    QMenu menu(this);
    QAction *copyAction = menu.addAction("复制选中 Tag");
    copyAction->setEnabled(!ui->treeWd14Tags->selectedItems().isEmpty());
    connect(copyAction, &QAction::triggered, this, &PromptParserWidget::copySelectedWd14Tags);
    menu.exec(ui->treeWd14Tags->viewport()->mapToGlobal(pos));
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
    ui->btnWd14DeletePreset->setEnabled(!running);
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
        auto *item = new Wd14ScoreItem(ui->treeWd14Ratings);
        item->setText(0, formatWd14Tag(rating.tag));
        item->setText(1, QString::number(rating.confidence * 100.0f, 'f', 2) + "%");
        item->setData(1, Qt::UserRole, rating.confidence);
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

        auto *item = new Wd14ScoreItem(ui->treeWd14Tags);
        item->setText(0, formatted);
        item->setText(1, QString::number(score.confidence * 100.0f, 'f', 2) + "%");
        item->setText(2, score.translation);
        item->setData(1, Qt::UserRole, score.confidence);
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
    ui->treeWd14Ratings->sortByColumn(1, Qt::DescendingOrder);
    ui->treeWd14Tags->sortByColumn(1, Qt::DescendingOrder);
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
