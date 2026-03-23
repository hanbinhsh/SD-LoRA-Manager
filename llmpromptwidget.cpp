#include "llmpromptwidget.h"
#include "ui_llmpromptwidget.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QProcess>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTimer>
#include <algorithm>
#include <limits>

namespace {
const QString kTaskCharacterReplace = "character_replace";
const QString kTaskOutfitReplace = "outfit_replace";
const QString kTaskPreferenceGenerate = "preference_generate";
const QString kTaskImageAdjust = "image_adjust";

struct ParsedThinkingBlocks {
    QString visibleText;
    QString thinkingText;
};

ParsedThinkingBlocks splitThinkingBlocks(const QString &text)
{
    ParsedThinkingBlocks parsed;
    QString remaining = text;
    const QString openTag = "<think>";
    const QString closeTag = "</think>";

    while (!remaining.isEmpty()) {
        int openIndex = remaining.indexOf(openTag, 0, Qt::CaseInsensitive);
        if (openIndex < 0) {
            parsed.visibleText += remaining;
            break;
        }

        parsed.visibleText += remaining.left(openIndex);
        remaining.remove(0, openIndex + openTag.size());

        int closeIndex = remaining.indexOf(closeTag, 0, Qt::CaseInsensitive);
        if (closeIndex < 0) {
            parsed.thinkingText += remaining;
            break;
        }

        parsed.thinkingText += remaining.left(closeIndex);
        remaining.remove(0, closeIndex + closeTag.size());
    }

    return parsed;
}

QString normalizeStreamPiece(QString chunk)
{
    chunk.replace("\r\n", "\n");
    chunk.replace('\r', '\n');
    return chunk;
}

void appendStreamPiece(QString &target, const QString &chunk)
{
    if (chunk.isEmpty()) return;
    target += normalizeStreamPiece(chunk);
}

QString combineThinkingText(const QString &dedicatedThinking, const QString &embeddedThinking)
{
    QString result = dedicatedThinking.trimmed();
    QString extra = embeddedThinking.trimmed();
    if (!extra.isEmpty() && !result.contains(extra)) {
        if (!result.isEmpty()) result += "\n\n";
        result += extra;
    }
    return result.trimmed();
}

void scrollPlainTextEditToBottom(QPlainTextEdit *edit)
{
    if (!edit) return;
    QScrollBar *scrollBar = edit->verticalScrollBar();
    if (!scrollBar) return;
    scrollBar->setValue(scrollBar->maximum());
}
}

LlmPromptWidget::LlmPromptWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::LlmPromptWidget)
    , m_netManager(new QNetworkAccessManager(this))
{
    ui->setupUi(this);

    connect(ui->btnFetchModels, &QPushButton::clicked, this, &LlmPromptWidget::onFetchModelsClicked);
    connect(ui->btnRefreshCandidates, &QPushButton::clicked, this, &LlmPromptWidget::onRefreshCandidatesClicked);
    connect(ui->btnAnalyzePreference, &QPushButton::clicked, this, &LlmPromptWidget::onAnalyzePreferenceClicked);
    connect(ui->btnGenerate, &QPushButton::clicked, this, &LlmPromptWidget::onGenerateClicked);
    connect(ui->btnStopGenerate, &QPushButton::clicked, this, &LlmPromptWidget::onStopGenerateClicked);
    connect(ui->btnCopyPrompt, &QPushButton::clicked, this, &LlmPromptWidget::onCopyPromptClicked);
    connect(ui->btnCopyResult, &QPushButton::clicked, this, &LlmPromptWidget::onCopyResultClicked);
    connect(ui->btnAddManualImages, &QPushButton::clicked, this, &LlmPromptWidget::onAddManualImagesClicked);
    connect(ui->btnResetPromptTemplate, &QPushButton::clicked, this, &LlmPromptWidget::onResetPromptTemplateClicked);
    connect(ui->comboTaskType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LlmPromptWidget::onTaskTypeChanged);
    connect(ui->comboTemplateTaskType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &LlmPromptWidget::onTemplateTaskTypeChanged);
    connect(ui->textPromptTemplate, &QPlainTextEdit::textChanged, this, &LlmPromptWidget::onPromptTemplateEdited);
    connect(ui->textTaskGuidance, &QPlainTextEdit::textChanged, this, &LlmPromptWidget::onTaskGuidanceEdited);
    connect(ui->textImageAttachmentNote, &QPlainTextEdit::textChanged, this, &LlmPromptWidget::onImageAttachmentNoteEdited);
    connect(ui->btnToggleThinking, &QToolButton::toggled, this, &LlmPromptWidget::onThinkingToggled);
    connect(ui->listLoraCandidates, &QListWidget::itemChanged, this, &LlmPromptWidget::onCandidateItemChanged);
    connect(ui->listImageCandidates, &QListWidget::itemChanged, this, &LlmPromptWidget::onCandidateItemChanged);

    ui->listLoraCandidates->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->listImageCandidates->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->btnToggleThinking->setEnabled(false);
    ui->textPlaceholderHelp->setHtml(
        "<b>占位符说明</b><br>"
        "当前编辑的是所选任务类型的专属模板。切换任务后会自动切换到对应模板。<br>"
        "<code>{task_type}</code> 当前任务类型，例如人物替换。<br>"
        "<code>{instruction}</code> 用户输入的自然语言指令。<br>"
        "<code>{source_prompt}</code> 原始提示词。<br>"
        "<code>{parsed_old_target}</code> 从指令中解析出的旧目标。<br>"
        "<code>{parsed_new_target}</code> 从指令中解析出的新目标。<br>"
        "<code>{conservative_prompt}</code> 基于原 prompt 计算出的保守替换版本。<br>"
        "<code>{preference_summary}</code> 历史图库统计出的用户偏好摘要。<br>"
        "<code>{lora_context}</code> 选中的 LoRA 上下文，含触发词/预览提示词。<br>"
        "<code>{image_context}</code> 选中的参考图片和对应 prompt。<br>"
        "<code>{manual_trigger_words}</code> 手动输入的触发词。<br>"
        "<code>{manual_lora_prompts}</code> 手动输入的 LoRA 参考提示词。<br>"
        "<code>{task_guidance}</code> 针对任务类型的额外规则。<br>"
        "<code>{image_attachment_note}</code> 是否附带图片给模型的说明。"
    );

    loadSettings();
    onRefreshCandidatesClicked();
    updateContextSelectionSummary();
}

LlmPromptWidget::~LlmPromptWidget()
{
    saveSettings();
    delete ui;
}

void LlmPromptWidget::setLibraryPaths(const QStringList &loraPaths, const QStringList &galleryPaths)
{
    m_loraPaths = loraPaths;
    m_galleryPaths = galleryPaths;
    onRefreshCandidatesClicked();
}

QString LlmPromptWidget::settingsPath() const
{
    QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);
    return configDir + "/settings.json";
}

QString LlmPromptWidget::endpointBaseUrl() const
{
    QString endpoint = ui->editEndpoint->text().trimmed();
    if (endpoint.endsWith('/')) endpoint.chop(1);
    return endpoint;
}

QString LlmPromptWidget::taskKeyForIndex(int index) const
{
    switch (index) {
    case 0: return kTaskCharacterReplace;
    case 1: return kTaskOutfitReplace;
    case 2: return kTaskPreferenceGenerate;
    case 3: return kTaskImageAdjust;
    default: return kTaskCharacterReplace;
    }
}

QString LlmPromptWidget::currentTaskKey() const
{
    return taskKeyForIndex(ui->comboTaskType->currentIndex());
}

QString LlmPromptWidget::taskLabelForKey(const QString &taskKey) const
{
    if (taskKey == kTaskCharacterReplace) return "人物替换";
    if (taskKey == kTaskOutfitReplace) return "服装替换";
    if (taskKey == kTaskPreferenceGenerate) return "偏好提示词生成";
    if (taskKey == kTaskImageAdjust) return "图片调整";
    return "人物替换";
}

bool LlmPromptWidget::isReplacementTask(const QString &taskKey) const
{
    return taskKey == kTaskCharacterReplace || taskKey == kTaskOutfitReplace;
}

void LlmPromptWidget::loadPromptTemplateForTask(const QString &taskKey)
{
    m_syncingPromptTemplateEditor = true;
    ui->textPromptTemplate->setPlainText(m_taskPromptTemplates.value(taskKey, defaultPromptTemplate(taskKey)));
    ui->lblPromptTemplateTask->setText("当前模板任务");
    m_syncingPromptTemplateEditor = false;
}

void LlmPromptWidget::persistCurrentPromptTemplate()
{
    if (m_syncingPromptTemplateEditor) return;
    m_taskPromptTemplates.insert(currentTaskKey(), ui->textPromptTemplate->toPlainText());
}

void LlmPromptWidget::loadTaskPromptFieldsForTask(const QString &taskKey)
{
    m_syncingTaskPromptFields = true;
    ui->textTaskGuidance->setPlainText(m_taskGuidances.value(taskKey, defaultTaskGuidance(taskKey)));
    ui->textImageAttachmentNote->setPlainText(m_taskImageAttachmentNotes.value(taskKey, defaultImageAttachmentNote(taskKey)));
    m_syncingTaskPromptFields = false;
}

void LlmPromptWidget::persistCurrentTaskPromptFields()
{
    if (m_syncingTaskPromptFields) return;
    const QString taskKey = currentTaskKey();
    m_taskGuidances.insert(taskKey, ui->textTaskGuidance->toPlainText());
    m_taskImageAttachmentNotes.insert(taskKey, ui->textImageAttachmentNote->toPlainText());
}

void LlmPromptWidget::updateContextSelectionSummary()
{
    int loraChecked = 0;
    int imageChecked = 0;

    for (int i = 0; i < ui->listLoraCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listLoraCandidates->item(i);
        if (item && item->checkState() == Qt::Checked) ++loraChecked;
    }

    for (int i = 0; i < ui->listImageCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listImageCandidates->item(i);
        if (item && item->checkState() == Qt::Checked) ++imageChecked;
    }

    ui->lblLoraSelectionSummary->setText(QString("已勾选 %1 / %2 个 LoRA").arg(loraChecked).arg(ui->listLoraCandidates->count()));
    ui->lblImageSelectionSummary->setText(QString("已勾选 %1 / %2 张图片").arg(imageChecked).arg(ui->listImageCandidates->count()));
}

void LlmPromptWidget::loadSettings()
{
    QFile file(settingsPath());
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        QJsonObject root = doc.object();
        ui->editEndpoint->setText(root["llm_endpoint"].toString("http://127.0.0.1:11434"));
        ui->comboModel->setEditText(root["llm_model"].toString());
        ui->spinTemperature->setValue(root["llm_temperature"].toDouble(0.4));
        ui->spinCandidateLimit->setValue(root["llm_candidate_limit"].toInt(12));
        ui->spinPreferenceTopCount->setValue(root["llm_preference_top_count"].toInt(15));
        ui->editCustomOptions->setText(root["llm_custom_options"].toString());
        QString savedTaskKey = root["llm_task_type"].toString();
        int savedTaskIndex = 0;
        for (int i = 0; i < ui->comboTaskType->count(); ++i) {
            if (taskKeyForIndex(i) == savedTaskKey) {
                savedTaskIndex = i;
                break;
            }
        }
        ui->comboTaskType->setCurrentIndex(savedTaskIndex);
        ui->chkAutoContext->setChecked(root["llm_auto_context"].toBool(true));
        ui->chkUsePreference->setChecked(root["llm_use_preference"].toBool(true));
        ui->chkUseTriggerWords->setChecked(root["llm_use_trigger_words"].toBool(true));
        ui->chkUsePreviewPrompts->setChecked(root["llm_use_preview_prompts"].toBool(true));
        ui->chkSendSelectedImages->setChecked(root["llm_send_selected_images"].toBool(false));
        ui->chkStopModelAfterGenerate->setChecked(root["llm_stop_model_after_generate"].toBool(false));
        ui->chkManualTriggerWords->setChecked(root["llm_manual_trigger_words_enabled"].toBool(false));
        ui->chkManualLoraPrompts->setChecked(root["llm_manual_lora_prompts_enabled"].toBool(false));
        ui->textManualTriggerWords->setPlainText(root["llm_manual_trigger_words"].toString());
        ui->textManualLoraPrompts->setPlainText(root["llm_manual_lora_prompts"].toString());
        ui->textSystemPrompt->setPlainText(root["llm_system_prompt"].toString(
            "You are an expert Stable Diffusion prompt engineer. "
            "Rewrite prompts precisely, preserve useful quality tags, and prefer concise, production-ready outputs."
        ));
        QString legacyPromptTemplate = root["llm_prompt_template"].toString();
        const QStringList taskKeys = {kTaskCharacterReplace, kTaskOutfitReplace, kTaskPreferenceGenerate, kTaskImageAdjust};
        for (const QString &taskKey : taskKeys) {
            const QString keyName = "llm_prompt_template_" + taskKey;
            QString value = root[keyName].toString();
            if (value.isEmpty()) value = legacyPromptTemplate;
            if (value.isEmpty()) value = defaultPromptTemplate(taskKey);
            m_taskPromptTemplates.insert(taskKey, value);

            QString guidance = root["llm_task_guidance_" + taskKey].toString();
            if (guidance.isEmpty()) guidance = defaultTaskGuidance(taskKey);
            m_taskGuidances.insert(taskKey, guidance);

            QString attachmentNote = root["llm_image_attachment_note_" + taskKey].toString();
            if (attachmentNote.isEmpty()) attachmentNote = defaultImageAttachmentNote(taskKey);
            m_taskImageAttachmentNotes.insert(taskKey, attachmentNote);
        }
        loadPromptTemplateForTask(currentTaskKey());
        loadTaskPromptFieldsForTask(currentTaskKey());
        return;
    }

    ui->editEndpoint->setText("http://127.0.0.1:11434");
    ui->spinTemperature->setValue(0.4);
    ui->spinCandidateLimit->setValue(12);
    ui->spinPreferenceTopCount->setValue(15);
    ui->editCustomOptions->clear();
    ui->comboTaskType->setCurrentIndex(0);
    ui->chkAutoContext->setChecked(true);
    ui->chkUsePreference->setChecked(true);
    ui->chkUseTriggerWords->setChecked(true);
    ui->chkUsePreviewPrompts->setChecked(true);
    ui->chkSendSelectedImages->setChecked(false);
    ui->chkStopModelAfterGenerate->setChecked(false);
    ui->chkManualTriggerWords->setChecked(false);
    ui->chkManualLoraPrompts->setChecked(false);
    ui->textManualTriggerWords->clear();
    ui->textManualLoraPrompts->clear();
    ui->textSystemPrompt->setPlainText(
        "You are an expert Stable Diffusion prompt engineer. "
        "Rewrite prompts precisely, preserve useful quality tags, and prefer concise, production-ready outputs."
    );
    m_taskPromptTemplates.insert(kTaskCharacterReplace, defaultPromptTemplate(kTaskCharacterReplace));
    m_taskPromptTemplates.insert(kTaskOutfitReplace, defaultPromptTemplate(kTaskOutfitReplace));
    m_taskPromptTemplates.insert(kTaskPreferenceGenerate, defaultPromptTemplate(kTaskPreferenceGenerate));
    m_taskPromptTemplates.insert(kTaskImageAdjust, defaultPromptTemplate(kTaskImageAdjust));
    m_taskGuidances.insert(kTaskCharacterReplace, defaultTaskGuidance(kTaskCharacterReplace));
    m_taskGuidances.insert(kTaskOutfitReplace, defaultTaskGuidance(kTaskOutfitReplace));
    m_taskGuidances.insert(kTaskPreferenceGenerate, defaultTaskGuidance(kTaskPreferenceGenerate));
    m_taskGuidances.insert(kTaskImageAdjust, defaultTaskGuidance(kTaskImageAdjust));
    m_taskImageAttachmentNotes.insert(kTaskCharacterReplace, defaultImageAttachmentNote(kTaskCharacterReplace));
    m_taskImageAttachmentNotes.insert(kTaskOutfitReplace, defaultImageAttachmentNote(kTaskOutfitReplace));
    m_taskImageAttachmentNotes.insert(kTaskPreferenceGenerate, defaultImageAttachmentNote(kTaskPreferenceGenerate));
    m_taskImageAttachmentNotes.insert(kTaskImageAdjust, defaultImageAttachmentNote(kTaskImageAdjust));
    loadPromptTemplateForTask(currentTaskKey());
    loadTaskPromptFieldsForTask(currentTaskKey());
}

void LlmPromptWidget::saveSettings() const
{
    QFile file(settingsPath());
    QJsonObject root;
    if (file.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
    }

    root["llm_endpoint"] = ui->editEndpoint->text().trimmed();
    root["llm_model"] = ui->comboModel->currentText().trimmed();
    root["llm_temperature"] = ui->spinTemperature->value();
    root["llm_candidate_limit"] = ui->spinCandidateLimit->value();
    root["llm_preference_top_count"] = ui->spinPreferenceTopCount->value();
    root["llm_custom_options"] = ui->editCustomOptions->text().trimmed();
    root["llm_task_type"] = currentTaskKey();
    root["llm_auto_context"] = ui->chkAutoContext->isChecked();
    root["llm_use_preference"] = ui->chkUsePreference->isChecked();
    root["llm_use_trigger_words"] = ui->chkUseTriggerWords->isChecked();
    root["llm_use_preview_prompts"] = ui->chkUsePreviewPrompts->isChecked();
    root["llm_send_selected_images"] = ui->chkSendSelectedImages->isChecked();
    root["llm_stop_model_after_generate"] = ui->chkStopModelAfterGenerate->isChecked();
    root["llm_manual_trigger_words_enabled"] = ui->chkManualTriggerWords->isChecked();
    root["llm_manual_lora_prompts_enabled"] = ui->chkManualLoraPrompts->isChecked();
    root["llm_manual_trigger_words"] = ui->textManualTriggerWords->toPlainText();
    root["llm_manual_lora_prompts"] = ui->textManualLoraPrompts->toPlainText();
    root["llm_system_prompt"] = ui->textSystemPrompt->toPlainText();
    const QStringList taskKeys = {kTaskCharacterReplace, kTaskOutfitReplace, kTaskPreferenceGenerate, kTaskImageAdjust};
    for (const QString &taskKey : taskKeys) {
        root["llm_prompt_template_" + taskKey] = m_taskPromptTemplates.value(taskKey, defaultPromptTemplate(taskKey));
        root["llm_task_guidance_" + taskKey] = m_taskGuidances.value(taskKey, defaultTaskGuidance(taskKey));
        root["llm_image_attachment_note_" + taskKey] = m_taskImageAttachmentNotes.value(taskKey, defaultImageAttachmentNote(taskKey));
    }

    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson());
    }
}

QStringList LlmPromptWidget::extractKeywords() const
{
    QString raw = ui->editContextKeywords->text().trimmed();
    if (raw.isEmpty()) {
        raw = ui->textInstruction->toPlainText() + "\n" + ui->textSourcePrompt->toPlainText();
    }

    QStringList keywords;
    QSet<QString> seen;
    QRegularExpression re("[\\p{L}\\p{N}_-]{2,}");
    auto it = re.globalMatch(raw);
    while (it.hasNext()) {
        QString token = it.next().captured(0).trimmed();
        QString lower = token.toLower();
        if (lower.size() < 2 || seen.contains(lower)) continue;
        seen.insert(lower);
        keywords.append(token);
    }
    return keywords;
}

QStringList LlmPromptWidget::readInstalledModelsSync(bool *ok, QString *errorText) const
{
    if (ok) *ok = false;
    QStringList result;

    QNetworkRequest request(QUrl(endpointBaseUrl() + "/api/tags"));
    QNetworkReply *reply = m_netManager->get(request);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(5000);
    loop.exec();

    if (!reply->isFinished()) {
        reply->abort();
    }

    if (reply->error() != QNetworkReply::NoError) {
        if (errorText) *errorText = reply->errorString();
        reply->deleteLater();
        return result;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonArray models = doc.object()["models"].toArray();
    for (const QJsonValue &value : models) {
        QString name = value.toObject()["name"].toString();
        if (!name.isEmpty()) result.append(name);
    }

    reply->deleteLater();
    if (ok) *ok = !result.isEmpty();
    return result;
}

QList<LlmPromptWidget::GalleryCacheItem> LlmPromptWidget::loadGalleryCache() const
{
    QList<GalleryCacheItem> items;
    QFile file(qApp->applicationDirPath() + "/config/user_gallery_cache.json");
    if (!file.open(QIODevice::ReadOnly)) return items;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    QJsonObject root = doc.object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        GalleryCacheItem item;
        item.path = it.key();
        QJsonObject obj = it.value().toObject();
        item.prompt = obj["p"].toString();
        item.negativePrompt = obj["np"].toString();
        item.parameters = obj["param"].toString();
        items.append(item);
    }
    return items;
}

QStringList LlmPromptWidget::collectLocalLoraFiles() const
{
    QStringList files;
    QSet<QString> seen;
    for (const QString &root : m_loraPaths) {
        if (root.isEmpty() || !QDir(root).exists()) continue;
        QDirIterator it(root, QStringList() << "*.safetensors" << "*.pt", QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString path = it.next();
            if (seen.contains(path)) continue;
            seen.insert(path);
            files.append(path);
        }
    }
    return files;
}

LlmPromptWidget::LoraMetadataInfo LlmPromptWidget::readLoraMetadata(const QString &filePath) const
{
    LoraMetadataInfo info;
    info.filePath = filePath;
    info.displayName = QFileInfo(filePath).completeBaseName();
    info.insertionTag = QString("<lora:%1:1>").arg(info.displayName);

    QFileInfo fi(filePath);
    QStringList jsonCandidates = {
        fi.dir().filePath(fi.completeBaseName() + ".json"),
        fi.dir().filePath(fi.completeBaseName() + ".metadata.json")
    };

    for (const QString &jsonPath : jsonCandidates) {
        QFile file(jsonPath);
        if (!file.exists() || !file.open(QIODevice::ReadOnly)) continue;

        QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();

        QString modelName = root["model"].toObject()["name"].toString().trimmed();
        QString versionName = root["name"].toString().trimmed();
        if (!modelName.isEmpty()) {
            info.displayName = versionName.isEmpty() ? modelName : modelName + " [" + versionName + "]";
        }

        QString desc = root["description"].toString().trimmed();
        if (!desc.isEmpty()) info.description = desc;

        QJsonArray trainedWords = root["trainedWords"].toArray();
        for (const QJsonValue &value : trainedWords) {
            QString word = value.toString().trimmed();
            if (!word.isEmpty() && !info.triggerWords.contains(word)) {
                info.triggerWords.append(word);
            }
        }

        QJsonArray images = root["images"].toArray();
        for (const QJsonValue &value : images) {
            QString prompt = value.toObject()["meta"].toObject()["prompt"].toString().trimmed();
            if (!prompt.isEmpty() && !info.previewPrompts.contains(prompt)) {
                info.previewPrompts.append(prompt);
            }
            if (info.previewPrompts.size() >= 3) break;
        }
        break;
    }

    return info;
}

QString LlmPromptWidget::cleanTagText(QString text) const
{
    text = text.trimmed();
    static QRegularExpression weightRegex(":[0-9.]+$");
    static QRegularExpression bracketRegex("[\\{\\}\\[\\]\\(\\)]");
    text.remove(weightRegex);
    text.remove(bracketRegex);
    return text.trimmed();
}

QStringList LlmPromptWidget::parsePromptToTags(const QString &prompt) const
{
    QString normalized = prompt;
    normalized.replace("\r\n", ",");
    normalized.replace("\n", ",");
    QStringList parts = normalized.split(",", Qt::SkipEmptyParts);
    QStringList result;
    QSet<QString> seen;
    for (QString part : parts) {
        part = cleanTagText(part);
        if (part.isEmpty()) continue;
        QString lower = part.toLower();
        if (seen.contains(lower)) continue;
        seen.insert(lower);
        result.append(part);
    }
    return result;
}

QStringList LlmPromptWidget::extractLorasFromPrompt(const QString &prompt) const
{
    QStringList result;
    QRegularExpression re("<lora:([^:>]+)");
    auto it = re.globalMatch(prompt);
    while (it.hasNext()) {
        QString name = it.next().captured(1).trimmed();
        if (!name.isEmpty() && !result.contains(name)) {
            result.append(name);
        }
    }
    return result;
}

QStringList LlmPromptWidget::extractLoraTagsWithWeights(const QString &prompt) const
{
    QStringList result;
    QRegularExpression re("<lora:[^>]+>");
    auto it = re.globalMatch(prompt);
    while (it.hasNext()) {
        QString tag = it.next().captured(0).trimmed();
        if (!tag.isEmpty() && !result.contains(tag)) {
            result.append(tag);
        }
    }
    return result;
}

QStringList LlmPromptWidget::splitPromptTokens(const QString &prompt) const
{
    QString normalized = prompt;
    normalized.replace("\r\n", ",");
    normalized.replace("\n", ",");
    QStringList parts = normalized.split(",", Qt::SkipEmptyParts);
    for (QString &part : parts) {
        part = part.trimmed();
    }
    parts.erase(std::remove_if(parts.begin(), parts.end(), [](const QString &s){ return s.trimmed().isEmpty(); }), parts.end());
    return parts;
}

QString LlmPromptWidget::normalizeLooseText(const QString &text) const
{
    QString out = text.toLower().trimmed();
    out.replace(" ", "");
    out.replace("_", "");
    out.replace("-", "");
    out.replace(".", "");
    out.replace("<", "");
    out.replace(">", "");
    out.replace(":", "");
    out.replace(",", "");
    return out;
}

QStringList LlmPromptWidget::parseReplaceInstruction(QString *oldTarget, QString *newTarget) const
{
    QString instruction = ui->textInstruction->toPlainText().trimmed();
    QString oldValue;
    QString newValue;

    QList<QRegularExpression> patterns = {
        QRegularExpression("将\\s*([^，,\\s]+)\\s*换为\\s*([^，,\\s]+)"),
        QRegularExpression("把\\s*([^，,\\s]+)\\s*换成\\s*([^，,\\s]+)"),
        QRegularExpression("replace\\s+([^,\\s]+)\\s+with\\s+([^,\\s]+)", QRegularExpression::CaseInsensitiveOption)
    };

    for (const QRegularExpression &re : patterns) {
        QRegularExpressionMatch match = re.match(instruction);
        if (match.hasMatch()) {
            oldValue = match.captured(1).trimmed();
            newValue = match.captured(2).trimmed();
            break;
        }
    }

    if (oldTarget) *oldTarget = oldValue;
    if (newTarget) *newTarget = newValue;
    QStringList values;
    if (!oldValue.isEmpty()) values.append(oldValue);
    if (!newValue.isEmpty()) values.append(newValue);
    return values;
}

QString LlmPromptWidget::buildConservativeReplacementPrompt() const
{
    QString sourcePrompt = ui->textSourcePrompt->toPlainText().trimmed();
    if (sourcePrompt.isEmpty()) return QString();

    QString oldTarget;
    QString newTarget;
    parseReplaceInstruction(&oldTarget, &newTarget);
    QString oldNorm = normalizeLooseText(oldTarget);

    QStringList tokens = splitPromptTokens(sourcePrompt);
    QStringList selectedTags;
    for (int i = 0; i < ui->listLoraCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listLoraCandidates->item(i);
        if (item->checkState() != Qt::Checked) continue;
        selectedTags.append(QString("<lora:%1:1>").arg(QFileInfo(item->data(Qt::UserRole).toString()).completeBaseName()));
    }

    QStringList kept;
    QSet<QString> seen;
    for (QString token : tokens) {
        QString tokenNorm = normalizeLooseText(token);
        bool isLora = token.contains("<lora:", Qt::CaseInsensitive);
        bool shouldDrop = false;

        if (!oldNorm.isEmpty() && tokenNorm.contains(oldNorm)) {
            shouldDrop = true;
        }

        if (!shouldDrop && isLora) {
            QRegularExpression re("<lora:([^:>]+)");
            QRegularExpressionMatch match = re.match(token);
            if (match.hasMatch() && !oldNorm.isEmpty()) {
                QString loraNameNorm = normalizeLooseText(match.captured(1));
                if (loraNameNorm.contains(oldNorm)) {
                    shouldDrop = true;
                }
            }
        }

        if (shouldDrop) continue;
        QString dedup = token.trimmed().toLower();
        if (dedup.isEmpty() || seen.contains(dedup)) continue;
        seen.insert(dedup);
        kept.append(token.trimmed());
    }

    for (const QString &tag : selectedTags) {
        QString key = tag.toLower();
        if (!seen.contains(key)) {
            kept.prepend(tag);
            seen.insert(key);
        }
    }

    if (!newTarget.isEmpty() && ui->comboTaskType->currentIndex() == 0) {
        QString key = newTarget.toLower();
        if (!seen.contains(key)) {
            kept.append(newTarget);
        }
    }

    return kept.join(", ");
}

QString LlmPromptWidget::defaultPromptTemplate(const QString &taskKey) const
{
    if (taskKey == kTaskCharacterReplace) {
        return
            "Task type:\n{task_type}\n\n"
            "User instruction:\n{instruction}\n\n"
            "Source prompt:\n{source_prompt}\n\n"
            "Parsed replace target:\nOld target: {parsed_old_target}\nNew target: {parsed_new_target}\n\n"
            "Conservative baseline rewrite from source prompt:\n{conservative_prompt}\n\n"
            "Available or selected local LoRAs:\n{lora_context}\n\n"
            "Reference images or prompts:\n{image_context}\n\n"
            "Manual trigger words:\n{manual_trigger_words}\n\n"
            "Manual LoRA preview prompts:\n{manual_lora_prompts}\n\n"
            "{image_attachment_note}\n"
            "{task_guidance}\n"
            "Return in Chinese with this exact structure:\n"
            "推荐LoRA:\n"
            "正向提示词:\n"
            "负向提示词:\n"
            "说明:\n";
    }

    if (taskKey == kTaskOutfitReplace) {
        return
            "Task type:\n{task_type}\n\n"
            "User instruction:\n{instruction}\n\n"
            "Source prompt:\n{source_prompt}\n\n"
            "Conservative baseline rewrite from source prompt:\n{conservative_prompt}\n\n"
            "Reference images or prompts:\n{image_context}\n\n"
            "Available or selected local LoRAs:\n{lora_context}\n\n"
            "Manual LoRA preview prompts:\n{manual_lora_prompts}\n\n"
            "{image_attachment_note}\n"
            "{task_guidance}\n"
            "Return in Chinese with this exact structure:\n"
            "推荐LoRA:\n"
            "正向提示词:\n"
            "负向提示词:\n"
            "说明:\n";
    }

    if (taskKey == kTaskImageAdjust) {
        return
            "Task type:\n{task_type}\n\n"
            "User instruction:\n{instruction}\n\n"
            "Current prompt before adjustment:\n{source_prompt}\n\n"
            "Reference images or prompts:\n{image_context}\n\n"
            "Available or selected local LoRAs:\n{lora_context}\n\n"
            "Manual trigger words:\n{manual_trigger_words}\n\n"
            "Manual LoRA preview prompts:\n{manual_lora_prompts}\n\n"
            "User preference summary:\n{preference_summary}\n\n"
            "{image_attachment_note}\n"
            "{task_guidance}\n"
            "Return in Chinese with this exact structure:\n"
            "推荐LoRA:\n"
            "正向提示词:\n"
            "负向提示词:\n"
            "说明:\n";
    }

    return
        "Task type:\n{task_type}\n\n"
        "User instruction:\n{instruction}\n\n"
        "User preference summary:\n{preference_summary}\n\n"
        "Available or selected local LoRAs:\n{lora_context}\n\n"
        "Reference images or prompts:\n{image_context}\n\n"
        "Manual trigger words:\n{manual_trigger_words}\n\n"
        "Manual LoRA preview prompts:\n{manual_lora_prompts}\n\n"
        "{image_attachment_note}\n"
        "{task_guidance}\n"
        "Return in Chinese with this exact structure:\n"
        "推荐LoRA:\n"
        "正向提示词:\n"
        "负向提示词:\n"
        "说明:\n";
}

QString LlmPromptWidget::defaultTaskGuidance(const QString &taskKey) const
{
    if (taskKey == kTaskCharacterReplace) {
        return
            "Rewrite the Stable Diffusion prompt according to the instruction. "
            "This is character replacement. Preserve composition, camera, outfit, lighting, quality tags, and non-character style LoRAs. "
            "Replace only the old character identity and old character LoRA with the new character. "
            "If a selected replacement LoRA exists, add its exact insertion tag into the positive prompt. "
            "Never place any LoRA syntax or LoRA weight fragments into the negative prompt. "
            "Do not drop clothing tags unless explicitly requested.";
    }

    if (taskKey == kTaskOutfitReplace) {
        return
            "Rewrite the Stable Diffusion prompt according to the instruction. "
            "This is outfit replacement. Preserve character identity, pose, scene, lighting, and style tags unless explicitly changed. "
            "Only replace the clothing-related content. "
            "Keep useful LoRAs, and never place LoRA syntax into the negative prompt.";
    }

    if (taskKey == kTaskImageAdjust) {
        return
            "Rewrite the Stable Diffusion prompt according to the instruction. "
            "This is image-based prompt refinement. Compare the current prompt, the user's complaint, and any attached or referenced image prompts. "
            "Identify missing, inaccurate, or underdescribed visual details, then refine the prompt so the next generation better matches the intended image. "
            "Preserve the existing subject, composition, style, and useful quality tags unless the user explicitly asks to change them. "
            "If the user intended a checkered skirt but wrote striped skirt or omitted the skirt description, correct the clothing description explicitly. "
            "Do not invent large scene changes. Prefer precise tag additions or substitutions over full rewrites when possible.";
    }

    return
        "Generate a user-preferred Stable Diffusion prompt based on history and references. "
        "You may choose suitable local LoRAs from the provided list.";
}

QString LlmPromptWidget::defaultImageAttachmentNote(const QString &taskKey) const
{
    if (taskKey == kTaskImageAdjust) {
        return "Selected reference images may be attached in the request. Use them to identify missing clothing, props, textures, and other visual details that should be corrected in the next prompt.";
    }
    return "Selected reference images may be attached in the request. Use them to refine character features, outfit details, and prompt fidelity.";
}

QString LlmPromptWidget::renderPromptTemplate(const QHash<QString, QString> &values) const
{
    QString taskKey = currentTaskKey();
    QString text = m_taskPromptTemplates.value(taskKey).trimmed();
    if (text.isEmpty()) text = defaultPromptTemplate(taskKey);

    for (auto it = values.begin(); it != values.end(); ++it) {
        text.replace("{" + it.key() + "}", it.value());
    }
    return text;
}

QString LlmPromptWidget::preferenceSummary() const
{
    QList<GalleryCacheItem> items = loadGalleryCache();
    if (items.isEmpty()) {
        return "No gallery history cache available.";
    }

    QMap<QString, int> tagCounts;
    QMap<QString, int> loraCounts;
    QMap<QString, int> negativeCounts;

    for (const GalleryCacheItem &item : items) {
        for (const QString &tag : parsePromptToTags(item.prompt)) {
            tagCounts[tag]++;
        }
        for (const QString &lora : extractLorasFromPrompt(item.prompt)) {
            loraCounts[lora]++;
        }
        for (const QString &tag : parsePromptToTags(item.negativePrompt)) {
            negativeCounts[tag]++;
        }
    }

    const int topLimit = ui->spinPreferenceTopCount->value();
    auto topList = [](const QMap<QString, int> &map, int limit) {
        QList<QPair<QString, int>> pairs;
        for (auto it = map.begin(); it != map.end(); ++it) {
            pairs.append(qMakePair(it.key(), it.value()));
        }
        std::sort(pairs.begin(), pairs.end(), [](const auto &a, const auto &b){
            if (a.second == b.second) return a.first < b.first;
            return a.second > b.second;
        });
        QStringList lines;
        for (int i = 0; i < pairs.size() && i < limit; ++i) {
            lines.append(QString("%1 (%2)").arg(pairs[i].first).arg(pairs[i].second));
        }
        return lines;
    };

    QString summary;
    summary += QString("History images: %1\n").arg(items.size());
    summary += QString("Top count setting: %1\n").arg(topLimit);
    summary += "Top positive tags: " + topList(tagCounts, topLimit).join(", ") + "\n";
    summary += "Top LoRAs: " + topList(loraCounts, topLimit).join(", ") + "\n";
    summary += "Top negative tags: " + topList(negativeCounts, topLimit).join(", ");
    return summary.trimmed();
}

QString LlmPromptWidget::selectedLoraContext() const
{
    QStringList lines;
    for (int i = 0; i < ui->listLoraCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listLoraCandidates->item(i);
        if (item->checkState() != Qt::Checked) continue;
        LoraMetadataInfo meta = readLoraMetadata(item->data(Qt::UserRole).toString());
        QStringList one;
        one.append(QString("Name: %1").arg(meta.displayName));
        one.append(QString("Path: %1").arg(meta.filePath));
        one.append(QString("Insertion tag: %1").arg(meta.insertionTag));
        if (ui->chkUseTriggerWords->isChecked() && !meta.triggerWords.isEmpty()) {
            one.append("Trigger words: " + meta.triggerWords.join(", "));
        }
        if (ui->chkUsePreviewPrompts->isChecked() && !meta.previewPrompts.isEmpty()) {
            QStringList prompts;
            for (const QString &prompt : meta.previewPrompts) {
                prompts.append(prompt.left(280));
            }
            one.append("Preview prompts: " + prompts.join(" || "));
        }
        if (!meta.description.isEmpty()) {
            one.append("Description: " + meta.description.left(240));
        }
        lines.append(one.join("\n"));
    }
    return lines.join("\n");
}

QString LlmPromptWidget::selectedImageContext() const
{
    QStringList lines;
    for (int i = 0; i < ui->listImageCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listImageCandidates->item(i);
        if (item->checkState() != Qt::Checked) continue;
        QString prompt = item->data(Qt::UserRole + 1).toString();
        lines.append(QString("%1\nPrompt: %2").arg(item->data(Qt::UserRole).toString(), prompt));
    }
    return lines.join("\n\n");
}

QStringList LlmPromptWidget::selectedImagePayloads() const
{
    QStringList images;
    if (!ui->chkSendSelectedImages->isChecked()) return images;

    for (int i = 0; i < ui->listImageCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listImageCandidates->item(i);
        if (item->checkState() != Qt::Checked) continue;
        QString path = item->data(Qt::UserRole).toString();
        QFile file(path);
        if (!file.exists() || !file.open(QIODevice::ReadOnly)) continue;
        images.append(QString::fromLatin1(file.readAll().toBase64()));
        if (images.size() >= 4) break;
    }
    return images;
}

QString LlmPromptWidget::buildGenerationPrompt() const
{
    QString taskKey = currentTaskKey();
    QString taskType = ui->comboTaskType->currentText();
    QString instruction = ui->textInstruction->toPlainText().trimmed();
    QString sourcePrompt = ui->textSourcePrompt->toPlainText().trimmed();
    QString preference = ui->chkUsePreference->isChecked() ? ui->textPreferenceSummary->toPlainText().trimmed() : QString();
    QString loraContext = selectedLoraContext();
    QString imageContext = selectedImageContext();
    QString conservativePrompt = buildConservativeReplacementPrompt();
    QString oldTarget;
    QString newTarget;
    parseReplaceInstruction(&oldTarget, &newTarget);
    QString manualTriggerWords = ui->chkManualTriggerWords->isChecked()
        ? ui->textManualTriggerWords->toPlainText().trimmed()
        : QString();
    QString manualLoraPrompts = ui->chkManualLoraPrompts->isChecked()
        ? ui->textManualLoraPrompts->toPlainText().trimmed()
        : QString();

    QString taskGuidance = m_taskGuidances.value(taskKey, defaultTaskGuidance(taskKey)).trimmed();
    QString imageAttachmentNote = ui->chkSendSelectedImages->isChecked()
        ? m_taskImageAttachmentNotes.value(taskKey, defaultImageAttachmentNote(taskKey)).trimmed()
        : QString();

    QHash<QString, QString> values;
    values.insert("task_type", taskType);
    values.insert("instruction", instruction);
    values.insert("source_prompt", sourcePrompt);
    values.insert("parsed_old_target", oldTarget);
    values.insert("parsed_new_target", newTarget);
    values.insert("conservative_prompt", conservativePrompt);
    values.insert("preference_summary", preference);
    values.insert("lora_context", loraContext);
    values.insert("image_context", imageContext);
    values.insert("manual_trigger_words", manualTriggerWords);
    values.insert("manual_lora_prompts", manualLoraPrompts);
    values.insert("task_guidance", taskGuidance);
    values.insert("image_attachment_note", imageAttachmentNote);
    return renderPromptTemplate(values);
}

QJsonValue LlmPromptWidget::parseOptionValue(QString value) const
{
    value = value.trimmed();
    if (value.isEmpty()) return true;

    QString lower = value.toLower();
    if (lower == "true") return true;
    if (lower == "false") return false;
    if (lower == "null") return QJsonValue(QJsonValue::Null);

    bool intOk = false;
    qlonglong intValue = value.toLongLong(&intOk);
    if (intOk) {
        if (intValue >= std::numeric_limits<int>::min() && intValue <= std::numeric_limits<int>::max()) {
            return static_cast<int>(intValue);
        }
        return static_cast<double>(intValue);
    }

    bool doubleOk = false;
    double doubleValue = value.toDouble(&doubleOk);
    if (doubleOk) return doubleValue;

    return value;
}

QJsonObject LlmPromptWidget::buildGenerationOptions() const
{
    QJsonObject options;
    options["temperature"] = ui->spinTemperature->value();

    const QString customText = ui->editCustomOptions->text().trimmed();
    if (customText.isEmpty()) return options;

    const QStringList tokens = QProcess::splitCommand(customText);
    for (int i = 0; i < tokens.size(); ++i) {
        QString token = tokens[i].trimmed();
        if (!token.startsWith('-')) continue;

        QString key = token;
        while (key.startsWith('-')) key.remove(0, 1);
        key = key.trimmed();
        if (key.isEmpty()) continue;
        key.replace('-', '_');

        QString valueText = "true";
        if (i + 1 < tokens.size()) {
            QString nextToken = tokens[i + 1].trimmed();
            bool nextIsNegativeNumber = QRegularExpression("^-[0-9]+(\\.[0-9]+)?$").match(nextToken).hasMatch();
            if (!nextToken.startsWith('-') || nextIsNegativeNumber) {
                valueText = nextToken;
                ++i;
            }
        }
        options.insert(key, parseOptionValue(valueText));
    }

    return options;
}

QJsonObject LlmPromptWidget::buildGenerationPayload(const QString &modelName) const
{
    QJsonObject payload;
    payload["model"] = modelName.trimmed();
    payload["prompt"] = m_lastRenderedPrompt.isEmpty() ? buildGenerationPrompt() : m_lastRenderedPrompt;
    payload["system"] = ui->textSystemPrompt->toPlainText();
    payload["stream"] = true;

    const QStringList imagePayloads = selectedImagePayloads();
    if (!imagePayloads.isEmpty()) {
        QJsonArray images;
        for (const QString &img : imagePayloads) images.append(img);
        payload["images"] = images;
    }

    payload["options"] = buildGenerationOptions();
    return payload;
}

QString LlmPromptWidget::postProcessGenerationResult(const QString &text) const
{
    QString result = text;
    if (result.trimmed().isEmpty()) return result;

    auto normalizedSectionName = [](const QString &line) {
        QString trimmed = line.trimmed();
        trimmed.remove(' ');
        if (trimmed.startsWith("推荐LoRA:")) return QString("推荐LoRA");
        if (trimmed.startsWith("正向提示词:")) return QString("正向提示词");
        if (trimmed.startsWith("负向提示词:")) return QString("负向提示词");
        if (trimmed.startsWith("说明:")) return QString("说明");
        if (trimmed.startsWith("风格理由:")) return QString("风格理由");
        return QString();
    };

    QStringList originalLines = result.split('\n');
    QHash<QString, QStringList> sections;
    QStringList order;
    QString currentSection;
    for (const QString &line : originalLines) {
        QString section = normalizedSectionName(line);
        if (!section.isEmpty()) {
            currentSection = section;
            if (!order.contains(section)) order.append(section);
            int colon = line.indexOf(':');
            QString body = colon >= 0 ? line.mid(colon + 1).trimmed() : QString();
            if (!body.isEmpty()) sections[section].append(body);
        } else if (!currentSection.isEmpty() && !line.trimmed().isEmpty()) {
            sections[currentSection].append(line.trimmed());
        }
    }

    QStringList selectedTags;
    for (int i = 0; i < ui->listLoraCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listLoraCandidates->item(i);
        if (item->checkState() != Qt::Checked) continue;
        selectedTags.append(QString("<lora:%1:1>").arg(QFileInfo(item->data(Qt::UserRole).toString()).completeBaseName()));
    }

    QString recoBody = sections.value("推荐LoRA").join(", ").trimmed();
    QString posBody = sections.value("正向提示词").join(", ").trimmed();
    QString negBody = sections.value("负向提示词").join(", ").trimmed();
    QString explainBody = sections.value("说明").join("\n").trimmed();
    if (explainBody.isEmpty()) explainBody = sections.value("风格理由").join("\n").trimmed();

    if (recoBody.isEmpty() && !selectedTags.isEmpty()) {
        recoBody = selectedTags.join(", ");
    }

    if (!selectedTags.isEmpty()) {
        for (const QString &tag : selectedTags) {
            if (!posBody.contains(tag, Qt::CaseInsensitive)) {
                posBody = posBody.isEmpty() ? tag : (tag + ", " + posBody);
            }
        }
    }

    if (isReplacementTask(currentTaskKey())) {
        QString baseline = buildConservativeReplacementPrompt();
        int baselineCount = splitPromptTokens(baseline).size();
        int posCount = splitPromptTokens(posBody).size();
        if (!baseline.isEmpty() && (posCount < qMax(6, baselineCount * 2 / 3))) {
            posBody = baseline;
        }
    }

    {
        QStringList tokens = splitPromptTokens(negBody);
        QStringList cleaned;
        QSet<QString> seen;
        for (QString token : tokens) {
            QString trimmed = token.trimmed();
            if (trimmed.contains("<lora:", Qt::CaseInsensitive)) continue;
            if (QRegularExpression("^[^,<>]+:[0-9.]+$").match(trimmed).hasMatch()) continue;
            QString lower = trimmed.toLower();
            if (seen.contains(lower)) continue;
            seen.insert(lower);
            cleaned.append(trimmed);
        }
        negBody = cleaned.join(", ");
    }

    QStringList out;
    out << ("推荐LoRA: " + recoBody);
    out << ("正向提示词: " + posBody);
    out << ("负向提示词: " + negBody);
    out << ("说明: " + explainBody);
    return out.join("\n");
}

void LlmPromptWidget::updateStatus(const QString &text, bool isError)
{
    ui->lblGenerateStatus->setText(text);
    ui->lblGenerateStatus->setStyleSheet(isError ? "color:#ff7b7b;" : "color:#8c96a0;");
}

void LlmPromptWidget::populateModels(const QStringList &models)
{
    QString current = ui->comboModel->currentText().trimmed();
    ui->comboModel->blockSignals(true);
    ui->comboModel->clear();
    ui->comboModel->addItems(models);
    ui->comboModel->setEditText(current);
    ui->comboModel->blockSignals(false);
}

void LlmPromptWidget::appendUniqueImageCandidate(const QString &label, const QString &path, const QString &prompt, bool checked)
{
    for (int i = 0; i < ui->listImageCandidates->count(); ++i) {
        QListWidgetItem *item = ui->listImageCandidates->item(i);
        if (item->data(Qt::UserRole).toString() == path) {
            if (checked) item->setCheckState(Qt::Checked);
            return;
        }
    }

    QListWidgetItem *item = new QListWidgetItem(label);
    item->setData(Qt::UserRole, path);
    item->setData(Qt::UserRole + 1, prompt);
    item->setToolTip(path);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    ui->listImageCandidates->addItem(item);
}

void LlmPromptWidget::onFetchModelsClicked()
{
    updateStatus("正在读取 Ollama 本地模型...");
    QNetworkRequest request(QUrl(endpointBaseUrl() + "/api/tags"));
    QNetworkReply *reply = m_netManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            updateStatus("读取 Ollama 模型失败: " + reply->errorString(), true);
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonArray models = doc.object()["models"].toArray();
        QStringList names;
        for (const QJsonValue &value : models) {
            QString name = value.toObject()["name"].toString();
            if (!name.isEmpty()) names.append(name);
        }
        populateModels(names);
        updateStatus(names.isEmpty() ? "未发现可用模型" : QString("已加载 %1 个本地模型").arg(names.size()));
        saveSettings();
    });
}

void LlmPromptWidget::onRefreshCandidatesClicked()
{
    ui->listLoraCandidates->clear();
    ui->listImageCandidates->clear();

    QStringList keywords = extractKeywords();
    QString oldTarget;
    QString newTarget;
    parseReplaceInstruction(&oldTarget, &newTarget);
    QString newTargetNorm = normalizeLooseText(newTarget);
    const int limit = ui->spinCandidateLimit->value();

    QList<QPair<int, QString>> loraScores;
    for (const QString &path : collectLocalLoraFiles()) {
        QString baseName = QFileInfo(path).completeBaseName();
        LoraMetadataInfo meta = readLoraMetadata(path);
        int score = 0;
        QString baseNorm = normalizeLooseText(baseName);
        QString displayNorm = normalizeLooseText(meta.displayName);
        if (!newTargetNorm.isEmpty()) {
            if (baseNorm.contains(newTargetNorm) || displayNorm.contains(newTargetNorm)) score += 20;
            for (const QString &word : meta.triggerWords) {
                if (normalizeLooseText(word).contains(newTargetNorm)) score += 10;
            }
        }
        for (const QString &keyword : keywords) {
            if (baseName.contains(keyword, Qt::CaseInsensitive)) score += 2;
            if (path.contains(keyword, Qt::CaseInsensitive)) score += 1;
            for (const QString &word : meta.triggerWords) {
                if (word.contains(keyword, Qt::CaseInsensitive) || keyword.contains(word, Qt::CaseInsensitive)) score += 3;
            }
            for (const QString &prompt : meta.previewPrompts) {
                if (prompt.contains(keyword, Qt::CaseInsensitive)) score += 2;
            }
        }
        if (keywords.isEmpty()) score = 1;
        if (score > 0) loraScores.append(qMakePair(score, path));
    }
    std::sort(loraScores.begin(), loraScores.end(), [](const auto &a, const auto &b){
        if (a.first == b.first) return a.second < b.second;
        return a.first > b.first;
    });

    for (int i = 0; i < loraScores.size() && i < limit; ++i) {
        QString path = loraScores[i].second;
        LoraMetadataInfo meta = readLoraMetadata(path);
        QListWidgetItem *item = new QListWidgetItem(meta.displayName.isEmpty() ? QFileInfo(path).completeBaseName() : meta.displayName);
        item->setData(Qt::UserRole, path);
        QStringList tip;
        tip << path << meta.insertionTag;
        if (!meta.triggerWords.isEmpty()) tip << ("Trigger: " + meta.triggerWords.join(", "));
        if (!meta.previewPrompts.isEmpty()) tip << ("Preview: " + meta.previewPrompts.first().left(200));
        item->setToolTip(tip.join("\n"));
        bool autoCheck = ui->chkAutoContext->isChecked() && i < 3;
        item->setCheckState(autoCheck ? Qt::Checked : Qt::Unchecked);
        ui->listLoraCandidates->addItem(item);
    }

    QList<QPair<int, GalleryCacheItem>> imageScores;
    for (const GalleryCacheItem &item : loadGalleryCache()) {
        int score = 0;
        for (const QString &keyword : keywords) {
            if (item.prompt.contains(keyword, Qt::CaseInsensitive)) score += 2;
            if (item.path.contains(keyword, Qt::CaseInsensitive)) score += 1;
        }
        if (keywords.isEmpty()) score = 1;
        if (score > 0) imageScores.append(qMakePair(score, item));
    }
    std::sort(imageScores.begin(), imageScores.end(), [](const auto &a, const auto &b){
        if (a.first == b.first) return a.second.path < b.second.path;
        return a.first > b.first;
    });

    for (int i = 0; i < imageScores.size() && i < limit; ++i) {
        const GalleryCacheItem &item = imageScores[i].second;
        QString label = QFileInfo(item.path).fileName();
        if (!item.prompt.isEmpty()) {
            label += " | " + item.prompt.left(80);
        }
        appendUniqueImageCandidate(label, item.path, item.prompt, ui->chkAutoContext->isChecked() && i < 3);
    }

    updateContextSelectionSummary();
    updateStatus(QString("已刷新候选上下文: %1 个 LoRA, %2 张图片").arg(ui->listLoraCandidates->count()).arg(ui->listImageCandidates->count()));
}

void LlmPromptWidget::onAnalyzePreferenceClicked()
{
    QString summary = preferenceSummary();
    ui->textPreferenceSummary->setPlainText(summary);
    updateStatus("已根据历史图库缓存生成人物偏好摘要");
}

void LlmPromptWidget::onGenerateClicked()
{
    if (m_activeGenerateReply) {
        updateStatus("已有生成任务正在进行，请先停止当前任务", true);
        return;
    }

    saveSettings();

    if (ui->textInstruction->toPlainText().trimmed().isEmpty()) {
        QMessageBox::information(this, "提示", "请先输入生成指令。");
        return;
    }

    QString modelName = ui->comboModel->currentText().trimmed();
    if (modelName.isEmpty()) {
        bool ok = false;
        QString errorText;
        QStringList models = readInstalledModelsSync(&ok, &errorText);
        if (!ok || models.isEmpty()) {
            updateStatus("未能自动获取 Ollama 模型: " + errorText, true);
            QMessageBox::warning(this, "错误", "没有可用的 Ollama 本地模型，请先拉取模型或手动填写模型名。");
            return;
        }
        populateModels(models);
        modelName = models.first();
        ui->comboModel->setEditText(modelName);
    }

    m_lastRenderedPrompt = buildGenerationPrompt();
    m_streamBuffer.clear();
    m_streamResponseText.clear();
    m_streamThinkingText.clear();
    m_activeModelName = modelName;

    updateStatus("正在调用 Ollama 生成提示词...");
    ui->textResult->clear();
    ui->textThinking->clear();
    ui->btnToggleThinking->setChecked(false);
    ui->btnToggleThinking->setEnabled(false);
    ui->btnGenerate->setEnabled(false);
    ui->btnStopGenerate->setEnabled(true);

    const QJsonObject payload = buildGenerationPayload(modelName);
    ui->textLastRequest->setPlainText(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Indented)));

    QNetworkRequest request(QUrl(endpointBaseUrl() + "/api/generate"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_netManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    m_activeGenerateReply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        if (m_activeGenerateReply != reply) return;
        processStreamChunk(reply->readAll());
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, modelName]() {
        if (m_activeGenerateReply == reply) {
            QByteArray tail = reply->readAll();
            if (!tail.isEmpty()) processStreamChunk(tail);
        }

        bool canceled = (reply->error() == QNetworkReply::OperationCanceledError);
        bool hasError = (reply->error() != QNetworkReply::NoError);
        QString errorText = reply->errorString();

        if (m_activeGenerateReply == reply) {
            m_activeGenerateReply = nullptr;
        }
        reply->deleteLater();

        if (hasError && !canceled) {
            ui->btnGenerate->setEnabled(true);
            ui->btnStopGenerate->setEnabled(false);
            ParsedThinkingBlocks parsed = splitThinkingBlocks(m_streamResponseText);
            ui->textResult->setPlainText(parsed.visibleText.trimmed());
            ui->textThinking->setPlainText(combineThinkingText(m_streamThinkingText, parsed.thinkingText));
            ui->btnToggleThinking->setEnabled(!ui->textThinking->toPlainText().trimmed().isEmpty());
            m_activeModelName.clear();
            updateStatus("Ollama 调用失败: " + errorText, true);
            return;
        }

        finishStreaming(modelName, canceled);
    });
}

void LlmPromptWidget::onStopGenerateClicked()
{
    if (!m_activeGenerateReply) return;

    updateStatus("正在停止生成...");
    ui->btnStopGenerate->setEnabled(false);
    if (!m_activeModelName.isEmpty()) {
        QProcess::startDetached("ollama", QStringList() << "stop" << m_activeModelName);
    }
    m_activeGenerateReply->abort();
}

void LlmPromptWidget::onCopyPromptClicked()
{
    m_lastRenderedPrompt = buildGenerationPrompt();
    QApplication::clipboard()->setText(m_lastRenderedPrompt);

    const QJsonObject payload = buildGenerationPayload(ui->comboModel->currentText().trimmed());
    ui->textLastRequest->setPlainText(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Indented)));
    updateStatus("已复制交给 LLM 的提示词");
}

void LlmPromptWidget::onCopyResultClicked()
{
    QApplication::clipboard()->setText(ui->textResult->toPlainText());
    updateStatus("结果已复制到剪贴板");
}

void LlmPromptWidget::onAddManualImagesClicked()
{
    QStringList files = QFileDialog::getOpenFileNames(this, "选择参考图片", QString(), "Images (*.png *.jpg *.jpeg *.webp)");
    for (const QString &path : files) {
        QImageReader reader(path);
        QString prompt = reader.text("parameters");
        QString label = "[Manual] " + QFileInfo(path).fileName();
        appendUniqueImageCandidate(label, path, prompt, true);
    }
    updateContextSelectionSummary();
    updateStatus("已添加手动参考图片");
}

void LlmPromptWidget::onResetPromptTemplateClicked()
{
    QString taskKey = currentTaskKey();
    QString text = defaultPromptTemplate(taskKey);
    m_taskPromptTemplates.insert(taskKey, text);
    m_taskGuidances.insert(taskKey, defaultTaskGuidance(taskKey));
    m_taskImageAttachmentNotes.insert(taskKey, defaultImageAttachmentNote(taskKey));
    loadPromptTemplateForTask(taskKey);
    loadTaskPromptFieldsForTask(taskKey);
    updateStatus(taskLabelForKey(taskKey) + " 的提示词模板已重置");
}

void LlmPromptWidget::onTaskTypeChanged(int)
{
    if (!m_syncingTaskTypeSelectors) {
        m_syncingTaskTypeSelectors = true;
        ui->comboTemplateTaskType->setCurrentIndex(ui->comboTaskType->currentIndex());
        m_syncingTaskTypeSelectors = false;
    }

    loadPromptTemplateForTask(currentTaskKey());
    loadTaskPromptFieldsForTask(currentTaskKey());

    if (currentTaskKey() == kTaskImageAdjust) {
        ui->textInstruction->setPlaceholderText("例如：这张图想保留整体构图，但把裙子从 striped skirt 改成更明确的 checkered skirt，并补全袜子与袖口细节。");
        ui->textSourcePrompt->setPlaceholderText("输入生成这张图时使用的原始 prompt，便于 AI 找出哪里描述不足或写错。");
    } else if (currentTaskKey() == kTaskPreferenceGenerate) {
        ui->textInstruction->setPlaceholderText("例如：根据我历史常用 tag，生成一条偏向二次元、雨天窗边、细节丰富的人物 prompt。");
        ui->textSourcePrompt->setPlaceholderText("原始提示词可留空；如果有想延续的 prompt，也可以粘贴进来。");
    } else {
        ui->textInstruction->setPlaceholderText("例如：把提示词里的人物 Arisu 换成 Mika，保留教室构图与蓝白色调，并尽量选择合适的 LoRA。");
        ui->textSourcePrompt->setPlaceholderText("原始提示词，可为空；如果是人物/服装替换，建议粘贴原 prompt。");
    }
}

void LlmPromptWidget::onPromptTemplateEdited()
{
    persistCurrentPromptTemplate();
}

void LlmPromptWidget::onTaskGuidanceEdited()
{
    persistCurrentTaskPromptFields();
}

void LlmPromptWidget::onImageAttachmentNoteEdited()
{
    persistCurrentTaskPromptFields();
}

void LlmPromptWidget::onTemplateTaskTypeChanged(int index)
{
    if (m_syncingTaskTypeSelectors) return;
    m_syncingTaskTypeSelectors = true;
    ui->comboTaskType->setCurrentIndex(index);
    loadPromptTemplateForTask(taskKeyForIndex(index));
    loadTaskPromptFieldsForTask(taskKeyForIndex(index));
    m_syncingTaskTypeSelectors = false;
}

void LlmPromptWidget::onCandidateItemChanged(QListWidgetItem *)
{
    updateContextSelectionSummary();
}

void LlmPromptWidget::onThinkingToggled(bool checked)
{
    bool hasThinking = !ui->textThinking->toPlainText().trimmed().isEmpty();
    ui->textThinking->setVisible(checked && hasThinking);
    ui->btnToggleThinking->setText(checked ? "隐藏思考内容" : "显示思考内容");
}

void LlmPromptWidget::processStreamChunk(const QByteArray &chunk)
{
    if (chunk.isEmpty()) return;

    m_streamBuffer += QString::fromUtf8(chunk);
    int newlineIndex = m_streamBuffer.indexOf('\n');
    while (newlineIndex >= 0) {
        QString line = m_streamBuffer.left(newlineIndex).trimmed();
        m_streamBuffer.remove(0, newlineIndex + 1);
        if (!line.isEmpty()) processStreamLine(line.toUtf8());
        newlineIndex = m_streamBuffer.indexOf('\n');
    }
}

void LlmPromptWidget::processStreamLine(const QByteArray &line)
{
    QByteArray trimmed = line.trimmed();
    if (trimmed.isEmpty()) return;

    QJsonDocument doc = QJsonDocument::fromJson(trimmed);
    if (!doc.isObject()) {
        appendStreamPiece(m_streamResponseText, QString::fromUtf8(trimmed));
    } else {
        QJsonObject root = doc.object();
        QString responsePiece = root["response"].toString();
        if (responsePiece.isEmpty() && root["message"].isObject()) {
            responsePiece = root["message"].toObject()["content"].toString();
        }
        if (!responsePiece.isEmpty()) {
            appendStreamPiece(m_streamResponseText, responsePiece);
        }

        QString thinkingPiece = root["thinking"].toString();
        if (thinkingPiece.isEmpty()) thinkingPiece = root["reasoning"].toString();
        if (thinkingPiece.isEmpty() && root["message"].isObject()) {
            QJsonObject message = root["message"].toObject();
            if (thinkingPiece.isEmpty()) thinkingPiece = message["thinking"].toString();
            if (thinkingPiece.isEmpty()) thinkingPiece = message["reasoning"].toString();
        }
        if (!thinkingPiece.isEmpty()) {
            appendStreamPiece(m_streamThinkingText, thinkingPiece);
        }
    }

    ParsedThinkingBlocks parsed = splitThinkingBlocks(m_streamResponseText);
    QString combinedThinking = combineThinkingText(m_streamThinkingText, parsed.thinkingText);
    ui->textResult->setPlainText(parsed.visibleText.trimmed());
    ui->textThinking->setPlainText(combinedThinking);
    scrollPlainTextEditToBottom(ui->textResult);
    scrollPlainTextEditToBottom(ui->textThinking);
    bool hasThinking = !combinedThinking.isEmpty();
    ui->btnToggleThinking->setEnabled(hasThinking);
    if (hasThinking && ui->btnToggleThinking->isChecked()) {
        ui->textThinking->setVisible(true);
    } else if (!hasThinking) {
        ui->textThinking->setVisible(false);
    }
}

void LlmPromptWidget::finishStreaming(const QString &modelName, bool canceled)
{
    if (!m_streamBuffer.trimmed().isEmpty()) {
        processStreamLine(m_streamBuffer.toUtf8());
        m_streamBuffer.clear();
    }

    ParsedThinkingBlocks parsed = splitThinkingBlocks(m_streamResponseText);
    QString finalThinking = combineThinkingText(m_streamThinkingText, parsed.thinkingText);
    QString finalResponse = parsed.visibleText.trimmed();

    ui->btnGenerate->setEnabled(true);
    ui->btnStopGenerate->setEnabled(false);
    ui->textThinking->setPlainText(finalThinking);
    ui->btnToggleThinking->setEnabled(!finalThinking.isEmpty());
    if (finalThinking.isEmpty()) {
        ui->btnToggleThinking->setChecked(false);
        ui->textThinking->setVisible(false);
    }

    if (!finalResponse.isEmpty()) {
        ui->textResult->setPlainText(postProcessGenerationResult(finalResponse));
    }
    scrollPlainTextEditToBottom(ui->textResult);
    scrollPlainTextEditToBottom(ui->textThinking);

    if (canceled) {
        m_activeModelName.clear();
        updateStatus("已停止生成");
        return;
    }

    if (ui->chkStopModelAfterGenerate->isChecked()) {
        bool started = QProcess::startDetached("ollama", QStringList() << "stop" << modelName);
        m_activeModelName.clear();
        updateStatus(started ? "提示词生成完成，已请求停止模型" : "提示词生成完成，但停止模型失败", !started);
    } else {
        m_activeModelName.clear();
        updateStatus("提示词生成完成");
    }
}
