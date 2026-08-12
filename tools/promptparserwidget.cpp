#include "promptparserwidget.h"
#include "ui_promptparserwidget.h"
#include "tagutils.h"
#include "imagemetadataparser.h"
#include "tableviewstylehelper.h"
#include "styleconstants.h"
#include "fileutils.h"

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
#include <QItemSelectionModel>
#include <QListView>
#include <QMessageBox>
#include <QMimeData>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProcess>
#include <QStackedWidget>
#include <QScrollArea>
#include <QBoxLayout>
#include <QProgressBar>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QTableWidgetItem>
#include <QTreeWidgetItem>
#include <QTreeWidget>
#include <QUrl>
#include <QUuid>
#include <QtEndian>
#include <QtConcurrent/QtConcurrent>

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

struct Wd14TranslationInfo
{
    QString category;
    QString translation;
    QString priority;
};

QString normalizedWd14TagKey(QString tag)
{
    tag = TagUtils::cleanPromptTag(tag, false).toCaseFolded().trimmed();
    tag.replace(' ', '_');
    static const QRegularExpression separators("_+");
    tag.replace(separators, "_");
    return tag;
}

QString translationValueForTag(const QString &tag, const QHash<QString, QString> *translations)
{
    if (!translations) return QString();
    if (translations->contains(tag)) return translations->value(tag);

    QString swapped = tag;
    swapped.replace(' ', '_');
    if (translations->contains(swapped)) return translations->value(swapped);

    swapped = tag;
    swapped.replace('_', ' ');
    if (translations->contains(swapped)) return translations->value(swapped);
    return QString();
}

QString stripBalancedCsvQuotes(QString text)
{
    text = text.trimmed();
    if (text.size() >= 2 && text.startsWith('"') && text.endsWith('"')) {
        text = text.mid(1, text.size() - 2);
        text.replace("\"\"", "\"");
    }
    return text.trimmed();
}

Wd14TranslationInfo parseWd14TranslationInfo(const QString &tag, QString raw)
{
    Wd14TranslationInfo info;
    raw = raw.trimmed();
    if (raw.isEmpty()) return info;

    // ComfyUI-Custom-Scripts autocomplete CSV 会被全局翻译映射保存为：
    // "tag 类别-翻译",优先级。这里保留优先级并拆开展示字段。
    const int lastComma = raw.lastIndexOf(',');
    if (lastComma > 0) {
        const QString tail = raw.mid(lastComma + 1).trimmed();
        bool numeric = !tail.isEmpty();
        for (const QChar ch : tail) {
            if (!ch.isDigit()) {
                numeric = false;
                break;
            }
        }
        if (numeric) {
            info.priority = tail;
            raw = raw.left(lastComma).trimmed();
        }
    }
    raw = stripBalancedCsvQuotes(raw);

    QStringList prefixes{tag};
    QString spaced = tag;
    spaced.replace('_', ' ');
    if (!prefixes.contains(spaced)) prefixes.append(spaced);
    QString underscored = tag;
    underscored.replace(' ', '_');
    if (!prefixes.contains(underscored)) prefixes.append(underscored);
    std::sort(prefixes.begin(), prefixes.end(), [](const QString &a, const QString &b) {
        return a.size() > b.size();
    });
    for (const QString &prefix : prefixes) {
        if (raw.size() > prefix.size()
            && raw.startsWith(prefix, Qt::CaseInsensitive)
            && raw.at(prefix.size()).isSpace()) {
            raw = raw.mid(prefix.size()).trimmed();
            break;
        }
    }

    int dash = raw.indexOf('-');
    if (dash < 0) dash = raw.indexOf(QChar(0xFF0D));
    if (dash < 0) dash = raw.indexOf(QChar(0x2014));
    if (dash < 0) dash = raw.indexOf(QChar(0x2013));
    if (dash > 0) {
        info.category = raw.left(dash).trimmed();
        info.translation = raw.mid(dash + 1).trimmed();
    } else {
        info.translation = raw.trimmed();
    }
    return info;
}

QHash<QString, int> readWd14TagUsageCountsWorker(const QString &cachePath)
{
    QHash<QString, int> counts;
    QFile file(cachePath);
    if (!file.open(QIODevice::ReadOnly)) return counts;

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        if (it.key().startsWith("__") || !it.value().isObject()) continue;
        const QJsonObject image = it.value().toObject();
        QSet<QString> tagsInImage;
        const auto collect = [&tagsInImage](QString prompt) {
            prompt.replace("\r\n", ",");
            prompt.replace('\n', ',');
            prompt.replace('\r', ',');
            for (const QString &part : prompt.split(',', Qt::SkipEmptyParts)) {
                const QString key = normalizedWd14TagKey(part);
                if (!key.isEmpty()) tagsInImage.insert(key);
            }
        };
        collect(image.value("p").toString());
        collect(image.value("np").toString());
        for (const QString &key : tagsInImage) counts[key] += 1;
    }
    return counts;
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
        if (column == 4 || column == 5) {
            return data(column, Qt::UserRole).toLongLong() < other.data(column, Qt::UserRole).toLongLong();
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
    connect(ui->btnOriginalOrder, &QPushButton::toggled, this, [this](bool) {
        applyTagSortMode();
    });
    connect(ui->btnSelectAllTags, &QPushButton::clicked, this, [this]() {
        posTagWidget->selectAllVisibleTags();
        negTagWidget->selectAllVisibleTags();
    });
    connect(ui->btnClearTagSelection, &QPushButton::clicked, this, [this]() {
        posTagWidget->clearSelectedTags();
        negTagWidget->clearSelectedTags();
    });

    // 把正/负面 tagflow 滚动区包成“文本/标签”双视图：文本视图可编辑/复制/粘贴。
    setupPromptTextToggle();
    connect(ui->btnToggleTagText, &QPushButton::toggled, this, [this](bool tagView) {
        setTagViewActive(tagView);
    });
    // 刷新：用图片原始提示词覆盖当前（可能已被编辑的）文本。
    connect(ui->btnRefreshPrompt, &QPushButton::clicked, this, [this]() {
        if (m_posEdit) m_posEdit->setPlainText(m_lastParsedPositive);
        if (m_negEdit) m_negEdit->setPlainText(m_lastParsedNegative);
        if (m_tagViewActive) refreshTagFlowsFromText();
    });
    setTagViewActive(m_tagViewActive); // 同步初始可见性/页面（默认标签视图）

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

        applyWd14Result(result, &m_activeWd14Settings);
        ui->lblWd14Status->setText(QString("WD14 反推完成，共 %1 个标签。").arg(result.tags.size()));
        appendWd14History(result, m_activeWd14Settings);
    });
    connect(wd14Process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        setWd14Running(false);
        ui->lblWd14Status->setText("WD14 Python 进程启动失败: " + wd14Process->errorString());
    });

    m_wd14BatchProcess = new QProcess(this);
    m_wd14BatchProcess->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_wd14BatchProcess, &QProcess::readyReadStandardOutput,
            this, &PromptParserWidget::processWd14BatchOutput);
    connect(m_wd14BatchProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) {
        processWd14BatchOutput();
        const QString stderrText = QString::fromUtf8(m_wd14BatchProcess->readAllStandardError()).trimmed();
        for (Wd14BatchItem &item : m_wd14BatchModel->items()) {
            if (item.status == Wd14BatchStatus::Running || item.status == Wd14BatchStatus::Waiting) {
                item.status = m_wd14BatchStopRequested ? Wd14BatchStatus::Stopped : Wd14BatchStatus::Failed;
                if (!m_wd14BatchStopRequested && item.error.isEmpty())
                    item.error = stderrText.isEmpty() ? QString("批量进程异常结束 (exit %1)。").arg(exitCode) : stderrText.left(1000);
            }
        }
        m_wd14BatchModel->notifyAll();
        setWd14BatchRunning(false);
        updateWd14BatchCounts();
        if (m_wd14BatchStopRequested) ui->lblWd14BatchStatus->setText("批量任务已停止，可继续失败/未完成项。");
        else if (!m_wd14BatchFatalError.isEmpty()) ui->lblWd14BatchStatus->setText(m_wd14BatchFatalError);
        else if (exitCode != 0 && !stderrText.isEmpty()) ui->lblWd14BatchStatus->setText(stderrText.left(1000));
        else if (exitCode != 0) ui->lblWd14BatchStatus->setText(QString("批量进程异常结束 (exit %1)。").arg(exitCode));
        else ui->lblWd14BatchStatus->setText("批量打标完成。");
        if (!m_wd14BatchManifestPath.isEmpty()) QFile::remove(m_wd14BatchManifestPath);
        m_wd14BatchManifestPath.clear();
        m_wd14BatchFatalError.clear();
        m_wd14BatchStopRequested = false;
    });
    connect(m_wd14BatchProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        const QString message = "WD14 批量 Python 进程启动失败: " + m_wd14BatchProcess->errorString();
        if (error == QProcess::FailedToStart) {
            m_wd14BatchFatalError = message;
            for (Wd14BatchItem &item : m_wd14BatchModel->items()) {
                if (item.status == Wd14BatchStatus::Waiting || item.status == Wd14BatchStatus::Running) {
                    item.status = Wd14BatchStatus::Failed;
                    item.error = message;
                }
            }
            m_wd14BatchModel->notifyAll();
            if (!m_wd14BatchManifestPath.isEmpty()) QFile::remove(m_wd14BatchManifestPath);
            m_wd14BatchManifestPath.clear();
            updateWd14BatchCounts();
        }
        setWd14BatchRunning(false);
        ui->lblWd14BatchStatus->setText(message);
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

    const auto saveSettingsLater = [this]() {
        saveWd14Settings();
        updateWd14BatchSettingsSummary();
    };
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
    ui->treeWd14Tags->header()->setStretchLastSection(false);
    ui->treeWd14Tags->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    ui->treeWd14Tags->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    ui->treeWd14Tags->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    ui->treeWd14Tags->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    ui->treeWd14Tags->header()->setSectionResizeMode(4, QHeaderView::Fixed);
    ui->treeWd14Tags->header()->setSectionResizeMode(5, QHeaderView::Fixed);
    ui->treeWd14Tags->header()->resizeSection(0, 210);
    ui->treeWd14Tags->header()->resizeSection(1, 86);
    ui->treeWd14Tags->header()->resizeSection(2, 90);
    ui->treeWd14Tags->header()->resizeSection(4, 82);
    ui->treeWd14Tags->header()->resizeSection(5, 88);
    ui->treeWd14Tags->header()->setSortIndicatorShown(true);
    connect(ui->treeWd14Tags, &QWidget::customContextMenuRequested,
            this, &PromptParserWidget::showWd14TagContextMenu);
    auto *copyWd14SelectedTagsAction = new QAction("复制选中 Tag", ui->treeWd14Tags);
    copyWd14SelectedTagsAction->setShortcut(QKeySequence::Copy);
    copyWd14SelectedTagsAction->setShortcutContext(Qt::WidgetShortcut);
    ui->treeWd14Tags->addAction(copyWd14SelectedTagsAction);
    connect(copyWd14SelectedTagsAction, &QAction::triggered, this, &PromptParserWidget::copySelectedWd14Tags);

    m_wd14HistoryModel = new Wd14HistoryModel(this);
    ui->listWd14History->setModel(m_wd14HistoryModel);
    ui->listWd14History->setItemDelegate(new Wd14HistoryDelegate(ui->listWd14History));
    ui->btnWd14HistoryRestore->setEnabled(false);
    ui->btnWd14HistoryApplySettings->setEnabled(false);
    ui->btnWd14HistoryDelete->setEnabled(false);
    ui->btnWd14HistoryClear->setEnabled(false);
    connect(ui->tabWd14Left, &QTabWidget::currentChanged, this, [this](int index) {
        if (ui->tabWd14Left->widget(index) == ui->tabWd14History) loadWd14History();
    });
    connect(ui->editWd14HistorySearch, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_wd14HistoryModel->setSearchText(text);
        updateWd14HistoryActions();
    });
    connect(ui->comboWd14HistorySort, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_wd14HistoryModel->setNewestFirst(index == 0);
        updateWd14HistoryActions();
    });
    connect(ui->listWd14History->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this]() { updateWd14HistoryActions(); });
    connect(ui->listWd14History, &QListView::clicked,
            this, &PromptParserWidget::previewWd14HistoryEntry);
    connect(ui->listWd14History, &QListView::doubleClicked, this, [this](const QModelIndex &index) {
        previewWd14HistoryEntry(index);
        restoreWd14HistoryEntry();
    });
    connect(ui->btnWd14HistoryRestore, &QPushButton::clicked,
            this, &PromptParserWidget::restoreWd14HistoryEntry);
    connect(ui->btnWd14HistoryApplySettings, &QPushButton::clicked,
            this, &PromptParserWidget::applyWd14HistorySettings);
    connect(ui->btnWd14HistoryDelete, &QPushButton::clicked,
            this, &PromptParserWidget::deleteSelectedWd14History);
    connect(ui->btnWd14HistoryClear, &QPushButton::clicked,
            this, &PromptParserWidget::clearWd14History);

    m_wd14BatchModel = new Wd14BatchModel(this);
    ui->listWd14Batch->setModel(m_wd14BatchModel);
    ui->listWd14Batch->setItemDelegate(new Wd14BatchDelegate(ui->listWd14Batch));
    ui->splitterWd14Batch->setSizes({760, 300});
    connect(ui->btnWd14BatchBrowse, &QPushButton::clicked, this, &PromptParserWidget::browseWd14BatchFolder);
    connect(ui->btnWd14BatchScan, &QPushButton::clicked, this, &PromptParserWidget::scanWd14BatchFolder);
    connect(ui->btnWd14BatchStart, &QPushButton::clicked, this, [this]() { startWd14Batch(false); });
    connect(ui->btnWd14BatchRetry, &QPushButton::clicked, this, [this]() { startWd14Batch(true); });
    connect(ui->btnWd14BatchStop, &QPushButton::clicked, this, &PromptParserWidget::stopWd14Batch);
    connect(ui->btnWd14BatchClear, &QPushButton::clicked, this, &PromptParserWidget::clearWd14Batch);
    connect(ui->btnWd14BatchEditSettings, &QPushButton::clicked, this, [this]() {
        ui->tabPromptParser->setCurrentWidget(ui->tabWd14);
        ui->tabWd14Left->setCurrentWidget(ui->tabWd14Settings);
    });
    connect(ui->editWd14BatchPrefix, &QLineEdit::textChanged,
            this, &PromptParserWidget::updateWd14BatchCaptionPreview);
    connect(ui->editWd14BatchSuffix, &QLineEdit::textChanged,
            this, &PromptParserWidget::updateWd14BatchCaptionPreview);
    connect(ui->comboWd14BatchExistingPolicy, &QComboBox::currentIndexChanged,
            this, [this]() { applyWd14BatchExistingPolicy(); });
    connect(ui->listWd14Batch->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this]() { updateWd14BatchSelection(); });
    connect(ui->btnWd14BatchOpenImage, &QPushButton::clicked, this, [this]() {
        const Wd14BatchItem *item = m_wd14BatchModel->itemAt(ui->listWd14Batch->currentIndex().row());
        if (item) FileUtils::showFileInFolder(item->imagePath, this);
    });
    connect(ui->btnWd14BatchOpenTxt, &QPushButton::clicked, this, [this]() {
        const Wd14BatchItem *item = m_wd14BatchModel->itemAt(ui->listWd14Batch->currentIndex().row());
        if (item) FileUtils::showFileInFolder(QFile::exists(item->txtPath) ? item->txtPath : item->imagePath, this);
    });
    connect(ui->tabPromptParser, &QTabWidget::currentChanged, this, [this](int) {
        if (ui->tabPromptParser->currentWidget() == ui->tabWd14Batch) updateWd14BatchSettingsSummary();
    });
    updateWd14BatchCaptionPreview();
    setWd14BatchRunning(false);
    updateWd14BatchCounts();
    updateWd14BatchSelection();

    connect(ui->scrollAreaPos->verticalScrollBar(), &QScrollBar::valueChanged,
            ui->scrollAreaWidgetContentsPos, [this]() { ui->scrollAreaWidgetContentsPos->update(); });
    connect(ui->scrollAreaNeg->verticalScrollBar(), &QScrollBar::valueChanged,
            ui->scrollAreaWidgetContentsNeg, [this]() { ui->scrollAreaWidgetContentsNeg->update(); });

    loadWd14Settings();
    updateWd14BatchSettingsSummary();
    loadWd14TagUsageCounts();
}

PromptParserWidget::~PromptParserWidget()
{
    if (m_wd14BatchProcess && m_wd14BatchProcess->state() != QProcess::NotRunning) {
        m_wd14BatchProcess->kill();
        m_wd14BatchProcess->waitForFinished(1000);
    }
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


    // 翻译表可能在工具页打开后被重新排序/保存，立即刷新已有 WD14 行。
    for (int i = 0; i < ui->treeWd14Tags->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = ui->treeWd14Tags->topLevelItem(i);
        const QString sourceTag = item->data(0, Qt::UserRole).toString();
        const Wd14TranslationInfo info = parseWd14TranslationInfo(
            sourceTag, translationValueForTag(sourceTag, m_translationMap));
        item->setText(2, info.category);
        item->setText(3, info.translation);
        item->setText(4, info.priority);
        item->setData(4, Qt::UserRole, info.priority.toLongLong());
    }
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

QStringList PromptParserWidget::parsePromptOrder(const QString &rawPrompt) const
{
    QStringList order;
    QSet<QString> seen;
    const QStringList parts = TagUtils::splitPromptParts(rawPrompt, true);
    for (const QString &part : parts) {
        const QString clean = TagUtils::cleanPromptTag(part);
        if (clean.isEmpty() || seen.contains(clean)) continue;
        seen.insert(clean);
        order << clean;
    }
    return order;
}

void PromptParserWidget::applyTagSortMode()
{
    const bool original = ui->btnOriginalOrder && ui->btnOriginalOrder->isChecked();
    if (original) {
        posTagWidget->setGivenOrder(m_posTagOrder);
        negTagWidget->setGivenOrder(m_negTagOrder);
        posTagWidget->setSortMode(TagFlowWidget::SortByGivenOrder);
        negTagWidget->setSortMode(TagFlowWidget::SortByGivenOrder);
    } else {
        posTagWidget->setSortMode(TagFlowWidget::SortByCount);
        negTagWidget->setSortMode(TagFlowWidget::SortByCount);
    }
}

// 把一个 tagflow 滚动区在其所在布局里替换为 QStackedWidget：第 0 页为可编辑文本框，第 1 页为原滚动区。
static QStackedWidget *wrapScrollWithEditor(QBoxLayout *layout, QScrollArea *area, QPlainTextEdit *&editOut)
{
    if (!layout || !area) return nullptr;
    auto *stack = new QStackedWidget(area->parentWidget());
    stack->setSizePolicy(area->sizePolicy());

    auto *edit = new QPlainTextEdit(stack);
    edit->setSizePolicy(area->sizePolicy());
    edit->setPlaceholderText("提示词文本（可编辑 / 复制 / 粘贴），切到“标签”视图可解析并翻译");

    // 用 stack 接管滚动区在布局中的位置，再把滚动区重新父化进 stack。
    delete layout->replaceWidget(area, stack);
    stack->addWidget(edit);   // page 0: 文本
    stack->addWidget(area);   // page 1: tagflow
    stack->setCurrentIndex(1);
    editOut = edit;
    return stack;
}

void PromptParserWidget::setupPromptTextToggle()
{
    m_posStack = wrapScrollWithEditor(ui->layoutRight, ui->scrollAreaPos, m_posEdit);
    m_negStack = wrapScrollWithEditor(ui->layoutRight, ui->scrollAreaNeg, m_negEdit);
}

void PromptParserWidget::setTagViewActive(bool tagView)
{
    m_tagViewActive = tagView;
    if (m_posStack) m_posStack->setCurrentIndex(tagView ? 1 : 0);
    if (m_negStack) m_negStack->setCurrentIndex(tagView ? 1 : 0);
    // 标签相关按钮只在标签视图下有意义。
    ui->btnTranslate->setVisible(tagView);
    ui->btnSelectAllTags->setVisible(tagView);
    ui->btnClearTagSelection->setVisible(tagView);
    ui->btnOriginalOrder->setVisible(tagView);
    ui->btnToggleTagText->setText(tagView ? "文本" : "标签");
    if (tagView) refreshTagFlowsFromText(); // 切回标签时按文本框最新内容重新解析（支持手动编辑/粘贴）
}

void PromptParserWidget::refreshTagFlowsFromText()
{
    if (!m_posEdit || !m_negEdit) return;
    const QString posText = m_posEdit->toPlainText();
    const QString negText = m_negEdit->toPlainText();
    m_posTagOrder = parsePromptOrder(posText);
    m_negTagOrder = parsePromptOrder(negText);
    posTagWidget->setData(parsePromptToMap(posText));
    negTagWidget->setData(parsePromptToMap(negText));
    applyTagSortMode();
}

// 让参数“每项单独成行”：先按已有换行拆，再把每行按“不在引号内的逗号”拆开。
// A1111 的参数是一整行逗号分隔（且 Lora hashes 的值里带引号包裹的逗号，需跳过）；
// ComfyUI 本就每行一个参数，二次拆分对其无影响。与图库参数展示的处理保持一致。
static QString formatParamsPerLine(const QString &params)
{
    auto splitTopLevelCommas = [](const QString &s) {
        QStringList out;
        QString cur;
        bool inQuotes = false;
        for (const QChar c : s) {
            if (c == '"') { inQuotes = !inQuotes; cur += c; }
            else if (c == ',' && !inQuotes) { out << cur; cur.clear(); }
            else cur += c;
        }
        out << cur;
        return out;
    };
    QStringList lines;
    const QStringList raw = params.split('\n');
    for (const QString &line : raw) {
        for (const QString &seg : splitTopLevelCommas(line)) {
            const QString t = seg.trimmed();
            if (!t.isEmpty()) lines << t;
        }
    }
    return lines.join('\n');
}

void PromptParserWidget::processImage(const QString &filePath)
{
    updateImagePreview(filePath);

    const ParsedImageMetadata parsed = parseImageMetadataFromFile(filePath);
    if (!parsed.hasContent()) {
        ui->txtParams->setText("未找到生成参数 / No generation parameters found.");
        m_lastParsedPositive.clear();
        m_lastParsedNegative.clear();
        if (m_posEdit) m_posEdit->clear();
        if (m_negEdit) m_negEdit->clear();
        m_posTagOrder.clear();
        m_negTagOrder.clear();
        posTagWidget->setData({});
        negTagWidget->setData({});
        return;
    }

    // 分辨率放在第一行：直接读图片实际尺寸（ComfyUI 的元信息里可能没有分辨率），与图库一致。
    QString paramsText = formatParamsPerLine(parsed.parametersText);
    {
        QImageReader reader(filePath);
        const QSize sz = reader.size();
        if (sz.isValid()) {
            const QString resLine = QString("Resolution / 分辨率: %1 × %2").arg(sz.width()).arg(sz.height());
            paramsText = paramsText.isEmpty() ? resLine : resLine + '\n' + paramsText;
        }
    }
    ui->txtParams->setPlainText(paramsText);
    // 记住原始提示词，供“刷新”按钮覆盖回填。
    m_lastParsedPositive = parsed.positivePrompt;
    m_lastParsedNegative = parsed.negativePrompt;
    // 文本框是数据源：填入解析出的提示词文本（用户可编辑/复制/粘贴），tagflow 从文本框派生。
    if (m_posEdit) m_posEdit->setPlainText(parsed.positivePrompt);
    if (m_negEdit) m_negEdit->setPlainText(parsed.negativePrompt);
    if (m_tagViewActive) refreshTagFlowsFromText();
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
    if (m_wd14BatchModel) {
        const bool batchIdle = m_wd14BatchProcess->state() == QProcess::NotRunning;
        bool hasWaiting = false;
        bool hasRetryable = false;
        for (const Wd14BatchItem &item : m_wd14BatchModel->items()) {
            hasWaiting = hasWaiting || item.status == Wd14BatchStatus::Waiting;
            hasRetryable = hasRetryable || item.status == Wd14BatchStatus::Waiting
                || item.status == Wd14BatchStatus::Failed
                || item.status == Wd14BatchStatus::Stopped;
        }
        ui->btnWd14BatchStart->setEnabled(!running && batchIdle && hasWaiting);
        ui->btnWd14BatchRetry->setEnabled(!running && batchIdle && hasRetryable);
        ui->btnWd14BatchEditSettings->setEnabled(!running && batchIdle);
    }
}

void PromptParserWidget::runWd14Tagger()
{
    if (wd14Process->state() != QProcess::NotRunning) return;
    if (m_wd14BatchProcess && m_wd14BatchProcess->state() != QProcess::NotRunning) {
        ui->lblWd14Status->setText("批量 WD14 正在运行，请先停止或等待完成。");
        return;
    }
    loadWd14TagUsageCounts();
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
    m_activeWd14Settings = currentWd14Settings();
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

QString PromptParserWidget::formatWd14Tag(const QString &tag, const Wd14RenderSettings *settings) const
{
    QString formatted = tag.trimmed();
    const bool replaceUnderscore = settings ? settings->replaceUnderscore : ui->chkWd14ReplaceUnderscore->isChecked();
    const bool escapeBrackets = settings ? settings->escapeBrackets : ui->chkWd14EscapeBrackets->isChecked();
    if (replaceUnderscore) {
        formatted.replace("_", " ");
    }
    if (escapeBrackets) {
        formatted = escapeParentheses(formatted);
    }
    return formatted;
}

void PromptParserWidget::loadWd14TagUsageCounts()
{
    const QString cachePath = qApp->applicationDirPath() + "/config/user_gallery_cache.json";
    const qint64 modified = QFileInfo(cachePath).lastModified().toMSecsSinceEpoch();
    if (m_wd14UsageWatcher || modified == m_wd14UsageCacheModified) return;

    m_wd14UsageCacheModified = modified;
    m_wd14UsageWatcher = new QFutureWatcher<QHash<QString, int>>(this);
    connect(m_wd14UsageWatcher, &QFutureWatcher<QHash<QString, int>>::finished, this, [this]() {
        if (!m_wd14UsageWatcher) return;
        m_wd14TagUsageCounts = m_wd14UsageWatcher->result();
        m_wd14UsageWatcher->deleteLater();
        m_wd14UsageWatcher = nullptr;
        updateWd14TagUsageColumn();
    });
    m_wd14UsageWatcher->setFuture(QtConcurrent::run([cachePath]() {
        return readWd14TagUsageCountsWorker(cachePath);
    }));
}

void PromptParserWidget::updateWd14TagUsageColumn()
{
    for (int i = 0; i < ui->treeWd14Tags->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = ui->treeWd14Tags->topLevelItem(i);
        const QString sourceTag = item->data(0, Qt::UserRole).toString();
        const int count = m_wd14TagUsageCounts.value(normalizedWd14TagKey(sourceTag));
        item->setText(5, QString::number(count));
        item->setData(5, Qt::UserRole, count);
    }
}

void PromptParserWidget::applyWd14Result(const Wd14InferenceResult &result, const Wd14RenderSettings *settings)
{
    const Wd14RenderSettings effectiveSettings = settings ? *settings : currentWd14Settings();
    ui->treeWd14Ratings->clear();
    ui->treeWd14Tags->clear();

    for (Wd14TagScore rating : result.ratings) {
        auto *item = new Wd14ScoreItem(ui->treeWd14Ratings);
        item->setText(0, formatWd14Tag(rating.tag, settings));
        item->setText(1, QString::number(rating.confidence * 100.0f, 'f', 2) + "%");
        item->setData(1, Qt::UserRole, rating.confidence);
    }

    QSet<QString> excluded;
    for (const QString &tag : splitWd14TagList(effectiveSettings.excludeTags))
        excluded.insert(normalizedWd14TagKey(tag));
    for (const QString &tag : splitWd14TagList(effectiveSettings.defaultExclude))
        excluded.insert(normalizedWd14TagKey(tag));

    QVector<Wd14TagScore> visibleTags;
    visibleTags.reserve(result.tags.size());
    for (Wd14TagScore score : result.tags) {
        if (excluded.contains(normalizedWd14TagKey(score.tag))) continue;
        const Wd14TranslationInfo info = parseWd14TranslationInfo(
            score.tag, translationValueForTag(score.tag, m_translationMap));
        score.category = info.category;
        score.translation = info.translation;
        score.priority = info.priority;
        score.usageCount = m_wd14TagUsageCounts.value(normalizedWd14TagKey(score.tag));
        visibleTags.append(score);
    }

    const bool sortAlphabetically = effectiveSettings.sortAlphabetically;
    if (sortAlphabetically) {
        std::sort(visibleTags.begin(), visibleTags.end(), [](const Wd14TagScore &a, const Wd14TagScore &b) {
            return QString::compare(a.tag, b.tag, Qt::CaseInsensitive) < 0;
        });
    }

    QSet<QString> seen;
    for (const Wd14TagScore &score : visibleTags) {
        const QString formatted = formatWd14Tag(score.tag, settings);
        const QString key = normalizedWd14TagKey(formatted);
        if (formatted.isEmpty() || seen.contains(key)) continue;
        seen.insert(key);

        auto *item = new Wd14ScoreItem(ui->treeWd14Tags);
        item->setText(0, formatted);
        item->setText(1, QString::number(score.confidence * 100.0f, 'f', 2) + "%");
        item->setText(2, score.category);
        item->setText(3, score.translation);
        item->setText(4, score.priority);
        item->setText(5, QString::number(score.usageCount));
        item->setData(0, Qt::UserRole, score.tag);
        item->setData(1, Qt::UserRole, score.confidence);
        item->setData(4, Qt::UserRole, score.priority.toLongLong());
        item->setData(5, Qt::UserRole, score.usageCount);
    }

    wd14LastTagsText = renderWd14TagText(result, effectiveSettings);
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

Wd14RenderSettings PromptParserWidget::currentWd14Settings() const
{
    Wd14RenderSettings settings;
    settings.modelDir = ui->editWd14ModelPath->text().trimmed();
    settings.presetName = ui->comboWd14Preset->currentText().trimmed();
    settings.threshold = ui->spinWd14Threshold->value();
    settings.additionalTags = ui->editWd14AdditionalTags->text().trimmed();
    settings.excludeTags = ui->editWd14ExcludeTags->text().trimmed();
    settings.defaultExclude = ui->editWd14DefaultExclude->text().trimmed();
    settings.sortAlphabetically = ui->chkWd14SortAlphabetically->isChecked();
    settings.includeConfidence = ui->chkWd14IncludeConfidence->isChecked();
    settings.replaceUnderscore = ui->chkWd14ReplaceUnderscore->isChecked();
    settings.escapeBrackets = ui->chkWd14EscapeBrackets->isChecked();
    return settings;
}

QString PromptParserWidget::wd14HistoryPath() const
{
    const QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);
    return configDir + "/wd14_history.jsonl";
}

void PromptParserWidget::loadWd14History()
{
    if (m_wd14HistoryLoaded || m_wd14HistoryWatcher) return;
    ui->lblWd14HistoryStatus->setText("正在加载 WD14 反推历史...");
    m_wd14HistoryWatcher = new QFutureWatcher<QVector<Wd14HistoryEntry>>(this);
    connect(m_wd14HistoryWatcher, &QFutureWatcher<QVector<Wd14HistoryEntry>>::finished, this, [this]() {
        if (!m_wd14HistoryWatcher) return;
        QVector<Wd14HistoryEntry> entries = m_wd14HistoryWatcher->result();
        QSet<QString> ids;
        for (const Wd14HistoryEntry &entry : std::as_const(entries)) ids.insert(entry.id);
        for (const Wd14HistoryEntry &entry : std::as_const(m_pendingWd14HistoryEntries)) {
            if (!ids.contains(entry.id)) entries.append(entry);
        }
        m_pendingWd14HistoryEntries.clear();
        m_wd14HistoryWatcher->deleteLater();
        m_wd14HistoryWatcher = nullptr;
        m_wd14HistoryLoaded = true;
        m_wd14HistoryModel->setEntries(std::move(entries));
        ui->listWd14History->setCurrentIndex(QModelIndex());
        updateWd14HistoryActions();
    });
    const QString path = wd14HistoryPath();
    m_wd14HistoryWatcher->setFuture(QtConcurrent::run([path]() {
        return loadWd14HistoryFile(path);
    }));
}

void PromptParserWidget::appendWd14History(const Wd14InferenceResult &result,
                                           const Wd14RenderSettings &settings)
{
    Wd14HistoryEntry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    entry.createdAt = QDateTime::currentDateTimeUtc();
    entry.imagePath = QFileInfo(wd14ImagePath).absoluteFilePath();
    entry.settings = settings;
    entry.result = result;
    entry.finalTags = wd14LastTagsText;
    entry.result.finalTags = entry.finalTags;

    QFile file(wd14HistoryPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        ui->lblWd14Status->setText("WD14 反推完成，但历史记录保存失败: " + file.errorString());
        return;
    }
    file.write(QJsonDocument(wd14HistoryEntryToJson(entry)).toJson(QJsonDocument::Compact));
    file.write("\n");
    file.close();

    if (m_wd14HistoryLoaded) {
        m_wd14HistoryModel->prependEntry(entry);
    } else if (m_wd14HistoryWatcher) {
        m_pendingWd14HistoryEntries.append(entry);
    }
    updateWd14HistoryActions();
}

bool PromptParserWidget::rewriteWd14History() const
{
    QSaveFile file(wd14HistoryPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    for (const Wd14HistoryEntry &entry : m_wd14HistoryModel->allEntries()) {
        file.write(QJsonDocument(wd14HistoryEntryToJson(entry)).toJson(QJsonDocument::Compact));
        file.write("\n");
    }
    return file.commit();
}

const Wd14HistoryEntry *PromptParserWidget::currentWd14HistoryEntry() const
{
    return m_wd14HistoryModel->entryAt(ui->listWd14History->currentIndex().row());
}

void PromptParserWidget::updateWd14HistoryActions()
{
    const int visibleCount = m_wd14HistoryModel ? m_wd14HistoryModel->rowCount() : 0;
    const int totalCount = m_wd14HistoryModel ? m_wd14HistoryModel->allEntries().size() : 0;
    const int selectedCount = ui->listWd14History->selectionModel()
        ? ui->listWd14History->selectionModel()->selectedRows().size() : 0;
    const bool hasCurrent = currentWd14HistoryEntry() != nullptr;
    ui->btnWd14HistoryRestore->setEnabled(hasCurrent);
    ui->btnWd14HistoryApplySettings->setEnabled(hasCurrent);
    ui->btnWd14HistoryDelete->setEnabled(selectedCount > 0);
    ui->btnWd14HistoryClear->setEnabled(totalCount > 0);
    if (!m_wd14HistoryLoaded && m_wd14HistoryWatcher) return;
    ui->lblWd14HistoryStatus->setText(totalCount == 0
        ? "暂无 WD14 反推历史。"
        : QString("显示 %1 / %2 条历史，已选择 %3 条。").arg(visibleCount).arg(totalCount).arg(selectedCount));
}

void PromptParserWidget::previewWd14HistoryEntry(const QModelIndex &index)
{
    const Wd14HistoryEntry *entry = m_wd14HistoryModel->entryAt(index.row());
    if (!entry) return;
    applyWd14Result(entry->result, &entry->settings);
    ui->lblWd14Elapsed->setText(QString("用时: %1 sec.").arg(entry->result.elapsedSec, 0, 'f', 2));
    updateWd14MemoryLabel(entry->result.totalMemory, entry->result.availableMemory);
    ui->lblWd14Status->setText(QString("正在预览 %1 的历史结果。").arg(QFileInfo(entry->imagePath).fileName()));
    updateWd14HistoryActions();
}

void PromptParserWidget::restoreWd14HistoryEntry()
{
    const Wd14HistoryEntry *entry = currentWd14HistoryEntry();
    if (!entry) return;
    applyWd14Result(entry->result, &entry->settings);
    ui->lblWd14Elapsed->setText(QString("用时: %1 sec.").arg(entry->result.elapsedSec, 0, 'f', 2));
    updateWd14MemoryLabel(entry->result.totalMemory, entry->result.availableMemory);
    if (QFile::exists(entry->imagePath)) {
        wd14ImagePath = entry->imagePath;
        updateWd14ImagePreview(entry->imagePath);
        ui->lblWd14Status->setText("已恢复历史结果，不会重新运行反推。");
    } else {
        wd14ImagePath.clear();
        ui->lblWd14Image->clear();
        ui->lblWd14Image->setText("历史图片已移动或删除\n请重新选择图片");
        ui->lblWd14Status->setText("已恢复历史结果，但原图片不存在。");
    }
    ui->tabWd14Left->setCurrentWidget(ui->tabWd14Image);
}

void PromptParserWidget::applyWd14HistorySettings()
{
    const Wd14HistoryEntry *entry = currentWd14HistoryEntry();
    if (!entry) return;
    const Wd14RenderSettings &settings = entry->settings;
    const QSignalBlocker thresholdBlocker(ui->spinWd14Threshold);
    const QSignalBlocker sliderBlocker(ui->sliderWd14Threshold);
    const QSignalBlocker sortBlocker(ui->chkWd14SortAlphabetically);
    const QSignalBlocker confidenceBlocker(ui->chkWd14IncludeConfidence);
    const QSignalBlocker underscoreBlocker(ui->chkWd14ReplaceUnderscore);
    const QSignalBlocker bracketBlocker(ui->chkWd14EscapeBrackets);
    ui->editWd14ModelPath->setText(settings.modelDir);
    ui->spinWd14Threshold->setValue(settings.threshold);
    ui->sliderWd14Threshold->setValue(qRound(settings.threshold * 100.0));
    ui->editWd14AdditionalTags->setText(settings.additionalTags);
    ui->editWd14ExcludeTags->setText(settings.excludeTags);
    ui->editWd14DefaultExclude->setText(settings.defaultExclude);
    ui->chkWd14SortAlphabetically->setChecked(settings.sortAlphabetically);
    ui->chkWd14IncludeConfidence->setChecked(settings.includeConfidence);
    ui->chkWd14ReplaceUnderscore->setChecked(settings.replaceUnderscore);
    ui->chkWd14EscapeBrackets->setChecked(settings.escapeBrackets);
    const int presetIndex = ui->comboWd14Preset->findText(settings.presetName);
    if (presetIndex >= 0) {
        const QSignalBlocker presetBlocker(ui->comboWd14Preset);
        ui->comboWd14Preset->setCurrentIndex(presetIndex);
    }
    saveWd14Settings();
    ui->lblWd14HistoryStatus->setText("已应用该历史记录的模型与反推设置。");
}

void PromptParserWidget::deleteSelectedWd14History()
{
    const QModelIndexList rows = ui->listWd14History->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;
    if (QMessageBox::question(this, "删除反推历史",
                              QString("确定删除选中的 %1 条 WD14 反推历史吗？").arg(rows.size()))
        != QMessageBox::Yes) return;
    QSet<QString> ids;
    for (const QModelIndex &index : rows) ids.insert(index.data(Wd14HistoryModel::EntryIdRole).toString());
    const QVector<Wd14HistoryEntry> previousEntries = m_wd14HistoryModel->allEntries();
    m_wd14HistoryModel->removeIds(ids);
    if (!rewriteWd14History()) {
        m_wd14HistoryModel->setEntries(previousEntries);
        QMessageBox::warning(this, "保存失败", "无法写入 wd14_history.jsonl，历史记录未删除。");
    }
    updateWd14HistoryActions();
}

void PromptParserWidget::clearWd14History()
{
    if (!m_wd14HistoryModel || m_wd14HistoryModel->allEntries().isEmpty()) return;
    if (QMessageBox::question(this, "清空反推历史", "确定清空全部 WD14 反推历史吗？此操作无法撤销。")
        != QMessageBox::Yes) return;
    const QVector<Wd14HistoryEntry> previousEntries = m_wd14HistoryModel->allEntries();
    m_wd14HistoryModel->clear();
    if (!rewriteWd14History()) {
        m_wd14HistoryModel->setEntries(previousEntries);
        QMessageBox::warning(this, "保存失败", "无法清空历史文件，历史记录未删除。");
    }
    updateWd14HistoryActions();
}

void PromptParserWidget::browseWd14BatchFolder()
{
    const QString folder = QFileDialog::getExistingDirectory(
        this, "选择批量 WD14 图片文件夹", ui->editWd14BatchFolder->text());
    if (folder.isEmpty()) return;
    ui->editWd14BatchFolder->setText(QFileInfo(folder).absoluteFilePath());
    scanWd14BatchFolder();
}

void PromptParserWidget::scanWd14BatchFolder()
{
    if (m_wd14BatchScanWatcher || m_wd14BatchProcess->state() != QProcess::NotRunning) return;
    const QString folder = ui->editWd14BatchFolder->text().trimmed();
    if (!QDir(folder).exists()) {
        ui->lblWd14BatchStatus->setText("请选择有效的图片文件夹。");
        return;
    }
    ui->btnWd14BatchScan->setEnabled(false);
    ui->btnWd14BatchStart->setEnabled(false);
    ui->lblWd14BatchStatus->setText("正在后台扫描图片...");
    const bool recursive = ui->chkWd14BatchRecursive->isChecked();
    m_wd14BatchScanWatcher = new QFutureWatcher<QVector<Wd14BatchItem>>(this);
    setWd14BatchRunning(false);
    connect(m_wd14BatchScanWatcher, &QFutureWatcher<QVector<Wd14BatchItem>>::finished, this, [this]() {
        if (!m_wd14BatchScanWatcher) return;
        m_wd14BatchModel->setItems(m_wd14BatchScanWatcher->result());
        m_wd14BatchScanWatcher->deleteLater();
        m_wd14BatchScanWatcher = nullptr;
        applyWd14BatchExistingPolicy();
        setWd14BatchRunning(false);
        updateWd14BatchCounts();
        ui->lblWd14BatchStatus->setText(QString("扫描完成，共找到 %1 张图片。").arg(m_wd14BatchModel->rowCount()));
    });
    m_wd14BatchScanWatcher->setFuture(QtConcurrent::run([folder, recursive]() {
        return ::scanWd14BatchFolder(folder, recursive);
    }));
}

void PromptParserWidget::applyWd14BatchExistingPolicy()
{
    if (!m_wd14BatchModel || m_wd14BatchProcess->state() != QProcess::NotRunning) return;
    const bool skipExisting = ui->comboWd14BatchExistingPolicy->currentIndex() == 0;
    for (Wd14BatchItem &item : m_wd14BatchModel->items()) {
        if (item.status == Wd14BatchStatus::Conflict || item.status == Wd14BatchStatus::Success) continue;
        if (skipExisting && QFile::exists(item.txtPath)) {
            item.status = Wd14BatchStatus::Skipped;
            item.error.clear();
        } else if (item.status == Wd14BatchStatus::Skipped) {
            item.status = Wd14BatchStatus::Waiting;
        }
    }
    m_wd14BatchModel->notifyAll();
    updateWd14BatchCounts();
}

void PromptParserWidget::startWd14Batch(bool retryOnly)
{
    if (m_wd14BatchProcess->state() != QProcess::NotRunning || wd14Process->state() != QProcess::NotRunning) {
        ui->lblWd14BatchStatus->setText("单图或批量 WD14 正在运行，请等待或先停止。");
        return;
    }
    if (m_wd14BatchModel->items().isEmpty()) {
        ui->lblWd14BatchStatus->setText("请先扫描图片文件夹。");
        return;
    }
    const QString modelDir = ui->editWd14ModelPath->text().trimmed();
    const QString scriptPath = selectedWd14ScriptPath();
    if (modelDir.isEmpty() || !QFile::exists(QDir(modelDir).filePath("model.onnx"))) {
        ui->lblWd14BatchStatus->setText("请先在 WD14 设置中选择有效模型目录。");
        return;
    }
    if (!QFile::exists(scriptPath)) {
        ui->lblWd14BatchStatus->setText("未找到 WD14 脚本: " + scriptPath);
        return;
    }

    applyWd14BatchExistingPolicy();
    QStringList paths;
    for (Wd14BatchItem &item : m_wd14BatchModel->items()) {
        const bool retryCandidate = item.status == Wd14BatchStatus::Failed
            || item.status == Wd14BatchStatus::Stopped
            || item.status == Wd14BatchStatus::Waiting;
        const bool startCandidate = item.status == Wd14BatchStatus::Waiting;
        if ((retryOnly && retryCandidate) || (!retryOnly && startCandidate)) {
            item.status = Wd14BatchStatus::Waiting;
            item.error.clear();
            item.finalTags.clear();
            item.tagCount = 0;
            item.elapsedSec = 0.0;
            paths.append(item.imagePath);
        }
    }
    if (paths.isEmpty()) {
        ui->lblWd14BatchStatus->setText("没有需要处理的图片。");
        updateWd14BatchCounts();
        return;
    }

    QTemporaryFile manifest(QDir::tempPath() + "/sdlm_wd14_batch_XXXXXX.txt");
    manifest.setAutoRemove(false);
    if (!manifest.open()) {
        ui->lblWd14BatchStatus->setText("无法创建批量图片清单: " + manifest.errorString());
        return;
    }
    for (const QString &path : paths) {
        manifest.write(path.toUtf8());
        manifest.write("\n");
    }
    m_wd14BatchManifestPath = manifest.fileName();
    manifest.close();

    m_activeWd14BatchSettings = currentWd14Settings();
    m_activeWd14BatchPrefix = ui->editWd14BatchPrefix->text();
    m_activeWd14BatchSuffix = ui->editWd14BatchSuffix->text();
    m_activeWd14BatchExistingPolicy = ui->comboWd14BatchExistingPolicy->currentIndex();
    m_wd14BatchStdoutBuffer.clear();
    m_wd14BatchFatalError.clear();
    m_wd14BatchStopRequested = false;
    setWd14BatchRunning(true);
    m_wd14BatchModel->notifyAll();
    updateWd14BatchCounts();
    ui->lblWd14BatchStatus->setText(QString("正在启动批量打标，共 %1 张图片...").arg(paths.size()));
    QStringList arguments;
    arguments << scriptPath
              << "--manifest" << m_wd14BatchManifestPath
              << "--model-dir" << modelDir
              << "--threshold" << QString::number(m_activeWd14BatchSettings.threshold, 'f', 4);
    m_wd14BatchProcess->start(selectedPythonPath(), arguments);
}

void PromptParserWidget::stopWd14Batch()
{
    if (m_wd14BatchProcess->state() == QProcess::NotRunning) return;
    m_wd14BatchStopRequested = true;
    ui->lblWd14BatchStatus->setText("正在停止批量任务...");
    m_wd14BatchProcess->kill();
}

void PromptParserWidget::clearWd14Batch()
{
    if (m_wd14BatchProcess->state() != QProcess::NotRunning || m_wd14BatchScanWatcher) return;
    m_wd14BatchModel->clear();
    ui->listWd14Batch->setCurrentIndex(QModelIndex());
    updateWd14BatchCounts();
    updateWd14BatchSelection();
    ui->lblWd14BatchStatus->setText("任务列表已清空。");
}

void PromptParserWidget::setWd14BatchRunning(bool running)
{
    const bool hasItems = m_wd14BatchModel && !m_wd14BatchModel->items().isEmpty();
    bool hasWaiting = false;
    bool hasRetryable = false;
    if (m_wd14BatchModel) {
        for (const Wd14BatchItem &item : m_wd14BatchModel->items()) {
            hasWaiting = hasWaiting || item.status == Wd14BatchStatus::Waiting;
            hasRetryable = hasRetryable || item.status == Wd14BatchStatus::Waiting
                || item.status == Wd14BatchStatus::Failed
                || item.status == Wd14BatchStatus::Stopped;
        }
    }
    const bool scanning = m_wd14BatchScanWatcher != nullptr;
    const bool idle = !running && !scanning;
    ui->btnWd14BatchBrowse->setEnabled(idle);
    ui->btnWd14BatchScan->setEnabled(idle);
    ui->btnWd14BatchStart->setEnabled(idle && hasWaiting);
    ui->btnWd14BatchRetry->setEnabled(idle && hasRetryable);
    ui->btnWd14BatchStop->setEnabled(running);
    ui->btnWd14BatchClear->setEnabled(idle && hasItems);
    ui->btnWd14BatchEditSettings->setEnabled(idle);
    ui->comboWd14BatchExistingPolicy->setEnabled(idle);
    ui->chkWd14BatchRecursive->setEnabled(idle);
    ui->editWd14BatchPrefix->setEnabled(idle);
    ui->editWd14BatchSuffix->setEnabled(idle);
    ui->btnWd14Run->setEnabled(!running && wd14Process->state() == QProcess::NotRunning);
}

void PromptParserWidget::updateWd14BatchSettingsSummary()
{
    const Wd14RenderSettings settings = currentWd14Settings();
    const QString model = QFileInfo(settings.modelDir).fileName();
    ui->lblWd14BatchSettingsSummary->setText(
        QString("模型: %1 | 预设: %2 | 阈值: %3 | 附加: %4 | 排除: %5 | %6 / %7 / %8 / %9")
            .arg(model.isEmpty() ? "未设置" : model,
                 settings.presetName.isEmpty() ? "-" : settings.presetName)
            .arg(settings.threshold, 0, 'f', 2)
            .arg(settings.additionalTags.isEmpty() ? "无" : settings.additionalTags)
            .arg(settings.excludeTags.isEmpty() ? "无" : settings.excludeTags)
            .arg(settings.sortAlphabetically ? "字母排序" : "置信度顺序")
            .arg(settings.includeConfidence ? "包含置信度" : "仅标签")
            .arg(settings.replaceUnderscore ? "空格化" : "保留下划线")
            .arg(settings.escapeBrackets ? "括号转义" : "括号原样"));
    ui->lblWd14BatchSettingsSummary->setToolTip(settings.modelDir);
}

void PromptParserWidget::updateWd14BatchCaptionPreview()
{
    ui->editWd14BatchCaptionPreview->setText(
        ui->editWd14BatchPrefix->text() + "1girl, solo, sample_tag" + ui->editWd14BatchSuffix->text());
}

void PromptParserWidget::updateWd14BatchCounts()
{
    int success = 0, skipped = 0, failed = 0, remaining = 0, completed = 0;
    for (const Wd14BatchItem &item : m_wd14BatchModel->items()) {
        switch (item.status) {
        case Wd14BatchStatus::Success: ++success; ++completed; break;
        case Wd14BatchStatus::Skipped: ++skipped; ++completed; break;
        case Wd14BatchStatus::Conflict: ++skipped; ++completed; break;
        case Wd14BatchStatus::Failed: ++failed; ++completed; break;
        default: ++remaining; break;
        }
    }
    ui->lblWd14BatchCounts->setText(
        QString("成功 %1 | 跳过 %2 | 失败 %3 | 未处理 %4").arg(success).arg(skipped).arg(failed).arg(remaining));
    ui->progressWd14Batch->setRange(0, qMax(1, m_wd14BatchModel->rowCount()));
    ui->progressWd14Batch->setValue(completed);
    setWd14BatchRunning(m_wd14BatchProcess && m_wd14BatchProcess->state() != QProcess::NotRunning);
}

void PromptParserWidget::updateWd14BatchSelection()
{
    const Wd14BatchItem *item = m_wd14BatchModel->itemAt(ui->listWd14Batch->currentIndex().row());
    ui->btnWd14BatchOpenImage->setEnabled(item != nullptr);
    ui->btnWd14BatchOpenTxt->setEnabled(item != nullptr);
    if (!item) {
        ui->lblWd14BatchSelectedPath->setText("未选择任务");
        ui->textWd14BatchSelectedTags->clear();
        return;
    }
    ui->lblWd14BatchSelectedPath->setText(item->imagePath + "\n" + item->txtPath);
    ui->textWd14BatchSelectedTags->setPlainText(item->error.isEmpty() ? item->finalTags : item->error);
}

QString PromptParserWidget::renderWd14TagText(const Wd14InferenceResult &result,
                                               const Wd14RenderSettings &settings) const
{
    QSet<QString> excluded;
    for (const QString &tag : splitWd14TagList(settings.excludeTags)) excluded.insert(normalizedWd14TagKey(tag));
    for (const QString &tag : splitWd14TagList(settings.defaultExclude)) excluded.insert(normalizedWd14TagKey(tag));
    QVector<Wd14TagScore> tags;
    for (const Wd14TagScore &tag : result.tags) {
        if (!excluded.contains(normalizedWd14TagKey(tag.tag))) tags.append(tag);
    }
    if (settings.sortAlphabetically) {
        std::sort(tags.begin(), tags.end(), [](const Wd14TagScore &a, const Wd14TagScore &b) {
            return QString::compare(a.tag, b.tag, Qt::CaseInsensitive) < 0;
        });
    }
    QStringList output;
    QSet<QString> seen;
    for (const Wd14TagScore &tag : tags) {
        const QString formatted = formatWd14Tag(tag.tag, &settings);
        const QString key = normalizedWd14TagKey(formatted);
        if (formatted.isEmpty() || seen.contains(key)) continue;
        seen.insert(key);
        output.append(settings.includeConfidence
            ? QString("%1 (%2%)").arg(formatted).arg(tag.confidence * 100.0f, 0, 'f', 2)
            : formatted);
    }
    for (const QString &extra : splitWd14TagList(settings.additionalTags)) {
        if (excluded.contains(normalizedWd14TagKey(extra))) continue;
        const QString formatted = formatWd14Tag(extra, &settings);
        const QString key = normalizedWd14TagKey(formatted);
        if (!formatted.isEmpty() && !seen.contains(key)) {
            seen.insert(key);
            output.append(formatted);
        }
    }
    return output.join(", ");
}

QString PromptParserWidget::buildWd14BatchCaption(const QString &newTags, const QString &existingText) const
{
    const QString &prefix = m_activeWd14BatchPrefix;
    const QString &suffix = m_activeWd14BatchSuffix;
    QString middle = existingText;
    if (!prefix.isEmpty() && middle.startsWith(prefix)) middle.remove(0, prefix.size());
    if (!suffix.isEmpty() && middle.endsWith(suffix)) middle.chop(suffix.size());
    QStringList combined;
    QSet<QString> seen;
    const auto appendParts = [&](const QString &text) {
        for (const QString &part : text.split(',', Qt::SkipEmptyParts)) {
            const QString tag = part.trimmed();
            QString key = normalizedWd14TagKey(tag);
            if (tag.isEmpty() || seen.contains(key)) continue;
            seen.insert(key);
            combined.append(tag);
        }
    };
    appendParts(middle);
    appendParts(newTags);
    return prefix + combined.join(", ") + suffix;
}

bool PromptParserWidget::writeWd14BatchTxt(Wd14BatchItem &item, const QString &newTags, QString *error)
{
    QString existing;
    if (m_activeWd14BatchExistingPolicy == 2 && QFile::exists(item.txtPath)) {
        QFile input(item.txtPath);
        if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) {
            if (error) *error = "无法读取已有 TXT: " + input.errorString();
            return false;
        }
        existing = QString::fromUtf8(input.readAll());
        while (existing.endsWith('\n') || existing.endsWith('\r')) existing.chop(1);
    }
    const QString caption = buildWd14BatchCaption(newTags, existing);
    QSaveFile output(item.txtPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = "无法写入 TXT: " + output.errorString();
        return false;
    }
    output.write(caption.toUtf8());
    output.write("\n");
    if (!output.commit()) {
        if (error) *error = "提交 TXT 失败: " + output.errorString();
        return false;
    }
    item.finalTags = caption;
    return true;
}

void PromptParserWidget::processWd14BatchOutput()
{
    m_wd14BatchStdoutBuffer += m_wd14BatchProcess->readAllStandardOutput();
    qsizetype newline = -1;
    while ((newline = m_wd14BatchStdoutBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_wd14BatchStdoutBuffer.left(newline).trimmed();
        m_wd14BatchStdoutBuffer.remove(0, newline + 1);
        if (line.isEmpty()) continue;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        if (error.error == QJsonParseError::NoError && document.isObject())
            processWd14BatchEvent(document.object());
    }
}

void PromptParserWidget::processWd14BatchEvent(const QJsonObject &event)
{
    const QString type = event.value("event").toString();
    if (type == "started") {
        for (Wd14BatchItem &item : m_wd14BatchModel->items()) {
            if (item.status == Wd14BatchStatus::Waiting) item.status = Wd14BatchStatus::Running;
        }
        m_wd14BatchModel->notifyAll();
        ui->lblWd14BatchStatus->setText(QString("模型已加载，正在处理 %1 张图片...").arg(event.value("total").toInt()));
        return;
    }
    if (type == "fatal") {
        m_wd14BatchFatalError = event.value("error").toString("WD14 批量初始化失败。");
        ui->lblWd14BatchStatus->setText(m_wd14BatchFatalError);
        return;
    }
    if (type == "done") {
        ui->lblWd14BatchStatus->setText("Python 推理完成，正在整理结果...");
        return;
    }
    if (type != "item") return;
    const QString path = QFileInfo(event.value("image").toString()).absoluteFilePath();
    const int row = m_wd14BatchModel->rowForPath(path);
    if (row < 0) return;
    Wd14BatchItem &item = m_wd14BatchModel->items()[row];
    item.elapsedSec = event.value("elapsed_sec").toDouble();
    if (!event.value("ok").toBool(false)) {
        item.status = Wd14BatchStatus::Failed;
        item.error = event.value("error").toString("图片推理失败。");
    } else {
        Wd14InferenceResult result;
        result.ok = true;
        result.elapsedSec = item.elapsedSec;
        for (const QJsonValue &value : event.value("tags").toArray()) {
            const Wd14TagScore score = parseScoreObject(value.toObject());
            if (!score.tag.isEmpty()) result.tags.append(score);
        }
        const QString tags = renderWd14TagText(result, m_activeWd14BatchSettings);
        QString writeError;
        if (writeWd14BatchTxt(item, tags, &writeError)) {
            item.status = Wd14BatchStatus::Success;
            item.tagCount = tags.split(',', Qt::SkipEmptyParts).size();
            item.error.clear();
        } else {
            item.status = Wd14BatchStatus::Failed;
            item.error = writeError;
        }
    }
    m_wd14BatchModel->notifyRow(row);
    updateWd14BatchCounts();
    if (ui->listWd14Batch->currentIndex().row() == row) updateWd14BatchSelection();
}
