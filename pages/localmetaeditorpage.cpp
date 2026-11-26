#include "pages/localmetaeditorpage.h"
#include "ui_localmetaeditorpage.h"

#include "core/modelitemroles.h"
#include "utils/applanguage.h"
#include "utils/gallerymetadata.h"
#include "utils/imageloader.h"
#include "utils/modelmetadatacodec.h"
#include "utils/styleconstants.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidgetItem>
#include <QMap>
#include <QMessageBox>
#include <QPair>
#include <QPixmap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextBoundaryFinder>
#include <QTextDocument>
#include <QThreadPool>
#include <QUuid>

namespace {
QString forceWrapText(const QString &text)
{
    if (text.isEmpty()) return text;
    QString result;
    result.reserve(text.size() * 2);
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    int start = 0;
    int end;
    while ((end = finder.toNextBoundary()) != -1) {
        result += QStringView(text).mid(start, end - start);
        result += QChar(0x200B);
        start = end;
    }
    return result;
}

QStringList parseModelTagsText(const QString &text)
{
    QStringList tags;
    QSet<QString> seen;
    static const QRegularExpression separatorRegex("[,\\n\\r]+");
    const QStringList parts = text.split(separatorRegex, Qt::SkipEmptyParts);
    for (QString tag : parts) {
        tag = tag.trimmed();
        if (tag.isEmpty()) continue;
        const QString key = tag.toCaseFolded();
        if (seen.contains(key)) continue;
        seen.insert(key);
        tags.append(tag);
    }
    return tags;
}
} // namespace

LocalMetaEditorPage::LocalMetaEditorPage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::LocalMetaEditorPage)
{
    ui->setupUi(this);
    ui->listEditImages->setUniformItemSizes(true);
    ui->listEditImages->setGridSize(QSize(118, 186));
    ui->listEditImages->setWordWrap(false);

    connect(ui->btnLocalMetaSave, &QPushButton::clicked, this, &LocalMetaEditorPage::onSaveClicked);
    connect(ui->btnLocalMetaReset, &QPushButton::clicked, this, &LocalMetaEditorPage::onResetClicked);
    connect(ui->listEditImages, &QListWidget::currentRowChanged,
            this, &LocalMetaEditorPage::onEditImageSelectionChanged);
    connect(ui->btnEditAddImage, &QPushButton::clicked, this, &LocalMetaEditorPage::onAddImageClicked);
    connect(ui->btnEditReplaceImage, &QPushButton::clicked, this, &LocalMetaEditorPage::onReplaceImageClicked);
    connect(ui->btnEditRemoveImage, &QPushButton::clicked, this, &LocalMetaEditorPage::onRemoveImageClicked);
    connect(ui->btnEditSetCover, &QPushButton::clicked, this, &LocalMetaEditorPage::onSetCoverClicked);

    clear();
}

LocalMetaEditorPage::~LocalMetaEditorPage()
{
    delete ui;
}

void LocalMetaEditorPage::setThreadPool(QThreadPool *pool)
{
    m_threadPool = pool;
}

void LocalMetaEditorPage::setPlaceholderIcon(const QIcon &icon)
{
    m_placeholderIcon = icon;
}

void LocalMetaEditorPage::clear()
{
    ++m_imageLoadToken;
    m_hasModel = false;
    m_imagesNeedRefresh = false;
    m_currentImageIndex = -1;
    m_context = {};
    m_images.clear();
    m_previewPath.clear();

    ui->groupEditImages->setEnabled(false);
    ui->groupEditMeta->setEnabled(false);
    ui->editLocalModelName->clear();
    ui->editLocalVersion->clear();
    ui->editLocalBaseModel->clear();
    ui->editLocalType->clear();
    ui->editLocalModelUrl->clear();
    ui->editLocalCreatedAt->clear();
    ui->editLocalDownloads->clear();
    ui->editLocalLikes->clear();
    ui->editLocalCreator->clear();
    ui->textLocalModelTags->clear();
    ui->chkLocalNSFW->setChecked(false);
    ui->textLocalTriggers->clear();
    ui->textLocalDescription->clear();
    ui->lblLocalMetaStatus->setText(AppText::tr("状态: -"));
    ui->lblLocalMetaStatus->setToolTip(QString());
    ui->listEditImages->clear();
    clearEditImageFields();
}

void LocalMetaEditorPage::loadModel(const ModelMeta &meta, const LocalMetaEditorContext &context, bool isVisibleNow)
{
    m_context = context;
    m_images = meta.images;
    m_previewPath = meta.previewPath;
    m_hasModel = !m_context.filePath.isEmpty() || !m_context.baseName.isEmpty();

    ui->groupEditImages->setEnabled(true);
    ui->groupEditMeta->setEnabled(true);

    const QString modelName = meta.modelName.isEmpty() ? meta.name : meta.modelName;
    ui->editLocalModelName->setText(modelName);
    ui->editLocalVersion->setText(meta.versionName);
    ui->editLocalBaseModel->setText(meta.baseModel);
    ui->editLocalType->setText(meta.type);
    ui->editLocalModelUrl->setText(meta.modelUrl);
    ui->editLocalCreatedAt->setText(meta.createdAt);
    ui->editLocalDownloads->setText(QString::number(meta.downloadCount));
    ui->editLocalLikes->setText(QString::number(meta.thumbsUpCount));
    ui->editLocalCreator->setText(meta.creatorName);
    ui->textLocalModelTags->setPlainText(meta.modelTags.join("\n"));
    ui->chkLocalNSFW->setChecked(meta.nsfw);
    ui->textLocalTriggers->setPlainText(meta.trainedWordsGroups.join("\n"));

    QString descPlain = meta.description;
    if (!descPlain.isEmpty()) {
        QTextDocument doc;
        doc.setHtml(descPlain);
        descPlain = doc.toPlainText();
    }
    ui->textLocalDescription->setPlainText(descPlain);

    updateStatus(meta, context);
    m_imagesNeedRefresh = true;
    if (isVisibleNow) {
        refreshEditImages();
    }
}

void LocalMetaEditorPage::updateStatus(const ModelMeta &meta, const LocalMetaEditorContext &context)
{
    m_context = context;

    QString status;
    QString color = AppStyle::str("primaryText");
    if (meta.isLocalOnly && meta.isLocalEdited) {
        status = AppText::tr("状态: 本地模型 (已编辑)");
        color = AppStyle::WarningYellow();
    } else if (meta.isLocalEdited) {
        status = AppText::tr("状态: 已编辑 (本地元数据)");
        color = AppStyle::WarningYellow();
    } else if (meta.isLocalOnly) {
        status = AppText::tr("状态: 本地模型");
        color = AppStyle::str("accentBlue");
    } else {
        status = AppText::tr("状态: Civitai 元数据");
    }

    QStringList details;
    QString filePath;
    if (!meta.filePath.isEmpty()) filePath = QFileInfo(meta.filePath).absoluteFilePath();
    if (filePath.isEmpty() && !context.filePath.isEmpty()) {
        filePath = QFileInfo(context.filePath).absoluteFilePath();
    }

    QString jsonPath;
    if (!filePath.isEmpty()) {
        const QFileInfo modelInfo(filePath);
        const QString baseName = modelInfo.completeBaseName();
        jsonPath = modelInfo.absoluteDir().filePath(baseName + ".json");
        details << (QFileInfo::exists(jsonPath) ? AppText::tr("JSON: 已缓存") : AppText::tr("JSON: 未缓存"));
    }

    if (meta.modelId > 0) details << QString("Model ID: %1").arg(meta.modelId);
    if (meta.versionId > 0) details << QString("Version ID: %1").arg(meta.versionId);
    if (!meta.sha256.isEmpty()) details << AppText::tr("SHA256: 已记录");

    if (m_hasModel) {
        details << AppText::tr("本地返图: %1 张").arg(context.usageCount);
        if (context.lastUsedMs > 0) {
            details << AppText::tr("最近使用: %1")
                           .arg(QDateTime::fromMSecsSinceEpoch(context.lastUsedMs).toString("yyyy-MM-dd"));
        }
    }

    if (context.hasSyncFailure) {
        details << (context.syncFailureError.isEmpty()
                        ? AppText::tr("同步失败缓存: 已记录")
                        : AppText::tr("同步失败缓存: %1").arg(context.syncFailureError));
        color = AppStyle::str("errorRed");
    } else if (!jsonPath.isEmpty() && !QFileInfo::exists(jsonPath)) {
        details << AppText::tr("提示: 点击刷新模型详情可同步元数据");
    }

    if (!details.isEmpty()) {
        status += "\n" + details.join(" · ");
    }
    ui->lblLocalMetaStatus->setWordWrap(true);
    ui->lblLocalMetaStatus->setText(AppText::display(status));
    ui->lblLocalMetaStatus->setStyleSheet(QString("color: %1;").arg(color));
    ui->lblLocalMetaStatus->setToolTip(status);
}

void LocalMetaEditorPage::ensureImagesRefreshed()
{
    if (m_imagesNeedRefresh) {
        refreshEditImages();
    }
}

QString LocalMetaEditorPage::previewPathForIndex(int index) const
{
    if (m_context.modelDir.isEmpty() || m_context.baseName.isEmpty()) return QString();
    return ModelMetadata::previewPath(m_context.modelDir, m_context.baseName, index);
}

bool LocalMetaEditorPage::saveImageToPreviewPath(const QString &srcPath, const QString &destPath, int &outW, int &outH)
{
    outW = 0;
    outH = 0;
    if (srcPath.isEmpty() || destPath.isEmpty()) return false;

    QImage img(srcPath);
    if (img.isNull()) return false;

    outW = img.width();
    outH = img.height();

    const QFileInfo destInfo(destPath);
    QDir().mkpath(destInfo.absolutePath());
    QSaveFile output(destPath);
    if (!output.open(QIODevice::WriteOnly)) return false;
    if (!img.save(&output, "PNG")) return false;
    return output.commit();
}

void LocalMetaEditorPage::applyParametersToImage(const QString &params, ImageInfo &img)
{
    if (params.isEmpty()) return;

    static const QRegularExpression reSteps("Steps:\\s*(\\d+)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reSampler("Sampler:\\s*([^,]+)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reCfg("CFG\\s*scale:\\s*([0-9.]+)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reCfgAlt("CFG:\\s*([0-9.]+)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reSeed("Seed:\\s*([-0-9]+)", QRegularExpression::CaseInsensitiveOption);

    QRegularExpressionMatch m = reSteps.match(params);
    if (m.hasMatch()) img.steps = m.captured(1).trimmed();
    m = reSampler.match(params);
    if (m.hasMatch()) img.sampler = m.captured(1).trimmed();
    m = reCfg.match(params);
    if (m.hasMatch()) {
        img.cfgScale = m.captured(1).trimmed();
    } else {
        m = reCfgAlt.match(params);
        if (m.hasMatch()) img.cfgScale = m.captured(1).trimmed();
    }
    m = reSeed.match(params);
    if (m.hasMatch()) img.seed = m.captured(1).trimmed();
}

void LocalMetaEditorPage::applyImageMetadataFromFile(const QString &srcPath, ImageInfo &img)
{
    if (srcPath.isEmpty()) return;
    UserImageInfo info;
    GalleryMetadata::parseImage(srcPath, info, true, QStringList());
    if (!info.prompt.isEmpty()) img.prompt = info.prompt;
    if (!info.negativePrompt.isEmpty() && info.negativePrompt != "(empty)") {
        img.negativePrompt = info.negativePrompt;
    }
    if (!info.parameters.isEmpty()) {
        applyParametersToImage(info.parameters, img);
    }
}

void LocalMetaEditorPage::clearEditImageFields()
{
    ui->lblEditImagePath->setText(AppText::tr("文件: -"));
    ui->lblEditImageSize->setText(AppText::tr("尺寸: -"));
    ui->textEditImgPrompt->clear();
    ui->textEditImgNegPrompt->clear();
    ui->editImgSampler->clear();
    ui->editImgSteps->clear();
    ui->editImgCfg->clear();
    ui->editImgSeed->clear();
    ui->spinImgNsfw->setValue(1);
}

void LocalMetaEditorPage::refreshEditImages()
{
    m_imagesNeedRefresh = false;
    ++m_imageLoadToken;
    const int currentToken = m_imageLoadToken;

    m_currentImageIndex = -1;
    ui->listEditImages->clear();

    if (m_images.isEmpty()) {
        const QString coverPath = previewPathForIndex(0);
        if (!coverPath.isEmpty() && QFile::exists(coverPath)) {
            QImageReader reader(coverPath);
            ImageInfo img;
            img.width = reader.size().width();
            img.height = reader.size().height();
            img.nsfwLevel = 1;
            m_images.append(img);
            m_previewPath = coverPath;
            emit imagesChanged(m_images, m_previewPath, QString());
        }
    }

    for (int i = 0; i < m_images.size(); ++i) {
        const QString title = (i == 0) ? AppText::tr("封面 / Cover") : AppText::tr("预览 %1").arg(i);
        auto *item = new QListWidgetItem(title);
        item->setData(Qt::UserRole, i);
        item->setData(ROLE_EDIT_IMAGE_PATH, QString());
        item->setTextAlignment(Qt::AlignHCenter);
        item->setSizeHint(QSize(114, 182));

        const QString path = previewPathForIndex(i);
        if (!path.isEmpty() && QFile::exists(path)) {
            item->setData(ROLE_EDIT_IMAGE_PATH, path);
            item->setIcon(m_placeholderIcon);
            item->setText(title);
            item->setToolTip(title + AppText::tr(" (Loading...)"));

            if (m_threadPool) {
                const QString taskId = QString("EDITIMG|%1|%2|%3").arg(currentToken).arg(i).arg(path);
                auto *task = new IconLoaderTask(path, 100, 0, this, taskId, true);
                task->setAutoDelete(true);
                m_threadPool->start(task);
            }
        } else if (!m_images[i].url.isEmpty()) {
            item->setText(title);
            item->setToolTip(title + AppText::tr(" (Remote)"));
        } else {
            item->setText(title);
            item->setToolTip(title + AppText::tr(" (Missing)"));
        }

        ui->listEditImages->addItem(item);
    }

    if (ui->listEditImages->count() > 0) {
        ui->listEditImages->setCurrentRow(0);
    } else {
        clearEditImageFields();
    }
}

void LocalMetaEditorPage::onIconLoaded(const QString &id, const QImage &image)
{
    if (image.isNull() || !id.startsWith(QLatin1String("EDITIMG|"))) return;

    const QStringList parts = id.split('|');
    if (parts.size() < 4) return;

    bool okToken = false;
    bool okIndex = false;
    const int token = parts[1].toInt(&okToken);
    const int index = parts[2].toInt(&okIndex);
    const QString path = parts.mid(3).join('|');

    if (okToken && okIndex &&
        token == m_imageLoadToken &&
        index >= 0 && index < ui->listEditImages->count()) {
        QListWidgetItem *item = ui->listEditImages->item(index);
        if (item &&
            item->data(Qt::UserRole).toInt() == index &&
            item->data(ROLE_EDIT_IMAGE_PATH).toString() == path) {
            item->setIcon(QIcon(QPixmap::fromImage(image)));
            const QString title = (index == 0) ? AppText::tr("封面 / Cover") : AppText::tr("预览 %1").arg(index);
            item->setText(title);
            item->setToolTip(path);
        }
    }
}

void LocalMetaEditorPage::commitEditImageFields()
{
    if (m_currentImageIndex < 0 || m_currentImageIndex >= m_images.size()) return;
    ImageInfo &img = m_images[m_currentImageIndex];

    img.prompt = ui->textEditImgPrompt->toPlainText().trimmed();
    img.negativePrompt = ui->textEditImgNegPrompt->toPlainText().trimmed();
    img.sampler = ui->editImgSampler->text().trimmed();
    img.steps = ui->editImgSteps->text().trimmed();
    img.cfgScale = ui->editImgCfg->text().trimmed();
    img.seed = ui->editImgSeed->text().trimmed();
    img.nsfwLevel = ui->spinImgNsfw->value();
    img.nsfw = (img.nsfwLevel > 1);
}

void LocalMetaEditorPage::loadEditImageFields(int index)
{
    if (index < 0 || index >= m_images.size()) return;
    const ImageInfo &img = m_images[index];

    const QString path = previewPathForIndex(index);
    ui->lblEditImagePath->setText(
        path.isEmpty()
            ? AppText::tr("文件: -")
            : AppText::tr("文件: ") + forceWrapText(path)
    );
    if (img.width > 0 && img.height > 0) {
        ui->lblEditImageSize->setText(AppText::tr("尺寸: %1 x %2").arg(img.width).arg(img.height));
    } else {
        ui->lblEditImageSize->setText(AppText::tr("尺寸: -"));
    }

    ui->textEditImgPrompt->setPlainText(img.prompt);
    ui->textEditImgNegPrompt->setPlainText(img.negativePrompt);
    ui->editImgSampler->setText(img.sampler);
    ui->editImgSteps->setText(img.steps);
    ui->editImgCfg->setText(img.cfgScale);
    ui->editImgSeed->setText(img.seed);
    ui->spinImgNsfw->setValue(img.nsfwLevel > 0 ? img.nsfwLevel : 1);
}

void LocalMetaEditorPage::onEditImageSelectionChanged(int row)
{
    if (row == m_currentImageIndex) return;
    commitEditImageFields();
    m_currentImageIndex = row;
    loadEditImageFields(row);
}

void LocalMetaEditorPage::onSaveClicked()
{
    if (!m_hasModel) {
        QMessageBox::information(this, AppText::tr("提示"), AppText::tr("请先选择一个模型。"));
        return;
    }

    const QString baseName = m_context.baseName;
    const QString filePath = m_context.filePath;
    if (baseName.isEmpty() || filePath.isEmpty()) return;

    const QString modelDir = m_context.modelDir.isEmpty()
        ? QFileInfo(filePath).absolutePath()
        : m_context.modelDir;
    const QString jsonPath = QDir(modelDir).filePath(baseName + ".json");

    QJsonObject root;
    QFile jsonFile(jsonPath);
    if (jsonFile.exists() && jsonFile.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(jsonFile.readAll());
        root = doc.object();
        jsonFile.close();
    }

    QString modelName = ui->editLocalModelName->text().trimmed();
    if (modelName.isEmpty()) modelName = baseName;
    const QString versionName = ui->editLocalVersion->text().trimmed();
    const QString baseModel = ui->editLocalBaseModel->text().trimmed();
    const QString type = ui->editLocalType->text().trimmed();
    const QString modelUrl = ui->editLocalModelUrl->text().trimmed();
    const QString createdAt = ui->editLocalCreatedAt->text().trimmed();
    const bool nsfw = ui->chkLocalNSFW->isChecked();
    const QString description = ui->textLocalDescription->toPlainText().trimmed();
    bool okDownloads = false;
    int downloads = ui->editLocalDownloads->text().trimmed().toInt(&okDownloads);
    if (!okDownloads) downloads = 0;
    bool okLikes = false;
    int likes = ui->editLocalLikes->text().trimmed().toInt(&okLikes);
    if (!okLikes) likes = 0;
    const QString creatorName = ui->editLocalCreator->text().trimmed();
    const QStringList modelTags = parseModelTagsText(ui->textLocalModelTags->toPlainText());

    QJsonObject modelObj = root["model"].toObject();
    modelObj["name"] = modelName;
    if (!type.isEmpty()) modelObj["type"] = type;
    else modelObj.remove("type");
    modelObj["nsfw"] = nsfw;

    QJsonObject creatorObj = modelObj.value("creator").toObject();
    QJsonObject topCreatorObj = root.value("creator").toObject();
    if (!creatorName.isEmpty()) {
        creatorObj["username"] = creatorName;
        topCreatorObj["username"] = creatorName;
        modelObj["creator"] = creatorObj;
        root["creator"] = topCreatorObj;
    } else {
        creatorObj.remove("username");
        creatorObj.remove("name");
        topCreatorObj.remove("username");
        topCreatorObj.remove("name");
        if (creatorObj.isEmpty()) modelObj.remove("creator");
        else modelObj["creator"] = creatorObj;
        if (topCreatorObj.isEmpty()) root.remove("creator");
        else root["creator"] = topCreatorObj;
    }

    QJsonArray tagArray;
    for (const QString &tag : modelTags) tagArray.append(tag);
    if (modelTags.isEmpty()) {
        modelObj.remove("tags");
        root.remove("tags");
    } else {
        modelObj["tags"] = tagArray;
        root["tags"] = tagArray;
    }
    root["model"] = modelObj;

    root["name"] = versionName;
    if (!baseModel.isEmpty()) root["baseModel"] = baseModel;
    else root.remove("baseModel");
    root["description"] = description;

    QStringList triggers;
    const QStringList lines = ui->textLocalTriggers->toPlainText().split('\n', Qt::SkipEmptyParts);
    for (QString line : lines) {
        line = line.trimmed();
        if (line.endsWith(",")) line.chop(1);
        if (!line.isEmpty()) triggers.append(line);
    }
    QJsonArray twArray;
    for (const QString &w : triggers) twArray.append(w);
    root["trainedWords"] = twArray;

    root["localEdited"] = true;
    root["localEditedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    int modelId = root["modelId"].toInt();
    if (modelId <= 0) modelId = root["model"].toObject()["id"].toInt();
    if (!modelUrl.isEmpty()) {
        root["modelUrl"] = modelUrl;
        static const QRegularExpression re("/models/(\\d+)");
        const QRegularExpressionMatch match = re.match(modelUrl);
        if (match.hasMatch()) {
            modelId = match.captured(1).toInt();
        }
    } else {
        root.remove("modelUrl");
        modelId = 0;
    }
    root["modelId"] = modelId;

    bool localOnly = root["localOnly"].toBool(false);
    if (modelId <= 0) localOnly = true;
    root["localOnly"] = localOnly;

    const QFileInfo fi(filePath);
    const double sizeKB = fi.exists() ? fi.size() / 1024.0 : 0.0;
    QJsonArray files = root["files"].toArray();
    QJsonObject fileObj = files.isEmpty() ? QJsonObject() : files[0].toObject();
    if (sizeKB > 0) fileObj["sizeKB"] = sizeKB;
    fileObj["name"] = fi.fileName();
    if (files.isEmpty()) files.append(fileObj);
    else files[0] = fileObj;
    root["files"] = files;

    if (!createdAt.isEmpty()) {
        root["createdAt"] = createdAt;
    } else {
        root.remove("createdAt");
    }

    QJsonObject statsObj = root["stats"].toObject();
    statsObj["downloadCount"] = downloads;
    statsObj["thumbsUpCount"] = likes;
    root["stats"] = statsObj;

    commitEditImageFields();
    QJsonArray imagesArr;
    const QJsonArray oldImagesArr = root["images"].toArray();
    for (int imageIndex = 0; imageIndex < m_images.size(); ++imageIndex) {
        const ImageInfo &img = m_images.at(imageIndex);
        QJsonObject imgObj = imageIndex < oldImagesArr.size() ? oldImagesArr.at(imageIndex).toObject() : QJsonObject();
        if (!img.url.isEmpty()) imgObj["url"] = img.url;
        if (!img.hash.isEmpty()) imgObj["hash"] = img.hash;
        if (img.width > 0) imgObj["width"] = img.width;
        if (img.height > 0) imgObj["height"] = img.height;
        if (img.nsfwLevel > 0) imgObj["nsfwLevel"] = img.nsfwLevel;
        QJsonObject imgMeta = imgObj["meta"].toObject();
        if (!img.prompt.isEmpty()) imgMeta["prompt"] = img.prompt;
        else imgMeta.remove("prompt");
        if (!img.negativePrompt.isEmpty()) imgMeta["negativePrompt"] = img.negativePrompt;
        else imgMeta.remove("negativePrompt");
        if (!img.sampler.isEmpty()) imgMeta["sampler"] = img.sampler;
        else imgMeta.remove("sampler");
        bool okSteps = false;
        const int steps = img.steps.toInt(&okSteps);
        if (okSteps) imgMeta["steps"] = steps;
        else if (!img.steps.isEmpty()) imgMeta["steps"] = img.steps;
        else imgMeta.remove("steps");
        bool okCfg = false;
        const double cfg = img.cfgScale.toDouble(&okCfg);
        if (okCfg) imgMeta["cfgScale"] = cfg;
        else if (!img.cfgScale.isEmpty()) imgMeta["cfgScale"] = img.cfgScale;
        else imgMeta.remove("cfgScale");
        bool okSeed = false;
        const qlonglong seed = img.seed.toLongLong(&okSeed);
        if (okSeed) imgMeta["seed"] = seed;
        else if (!img.seed.isEmpty()) imgMeta["seed"] = img.seed;
        else imgMeta.remove("seed");
        if (!imgMeta.isEmpty()) imgObj["meta"] = imgMeta;
        else imgObj.remove("meta");
        imagesArr.append(imgObj);
    }
    root["images"] = imagesArr;

    QSaveFile saveFile(jsonPath);
    if (!saveFile.open(QIODevice::WriteOnly)
        || saveFile.write(QJsonDocument(root).toJson()) < 0
        || !saveFile.commit()) {
        QMessageBox::warning(this, AppText::tr("保存失败"), AppText::tr("无法写入本地元数据文件。"));
        return;
    }

    emit metadataSaved(filePath, jsonPath, m_previewPath);
}

void LocalMetaEditorPage::onResetClicked()
{
    if (!m_hasModel) return;
    emit resetRequested();
}

void LocalMetaEditorPage::onAddImageClicked()
{
    if (!m_hasModel) {
        QMessageBox::information(this, AppText::tr("提示"), AppText::tr("请先选择一个模型。"));
        return;
    }

    commitEditImageFields();

    const QString srcPath = QFileDialog::getOpenFileName(
        this,
        AppText::tr("选择预览图片"),
        m_context.modelDir,
        AppText::tr("Image Files (*.png *.jpg *.jpeg *.webp)")
    );
    if (srcPath.isEmpty()) return;

    const int index = m_images.size();
    const QString destPath = previewPathForIndex(index);

    int w = 0;
    int h = 0;
    if (!saveImageToPreviewPath(srcPath, destPath, w, h)) {
        QMessageBox::warning(this, AppText::tr("错误"), AppText::tr("图片保存失败，请检查文件格式。"));
        return;
    }

    ImageInfo img;
    img.width = w;
    img.height = h;
    img.nsfwLevel = 1;
    img.nsfw = false;
    applyImageMetadataFromFile(srcPath, img);
    m_images.append(img);
    if (index == 0) {
        m_previewPath = destPath;
    }

    emit imagesChanged(m_images, m_previewPath, index == 0 ? destPath : QString());
    refreshEditImages();
    ui->listEditImages->setCurrentRow(index);
}

void LocalMetaEditorPage::onReplaceImageClicked()
{
    const int row = ui->listEditImages->currentRow();
    if (row < 0 || row >= m_images.size()) return;

    commitEditImageFields();

    const QString srcPath = QFileDialog::getOpenFileName(
        this,
        AppText::tr("替换预览图片"),
        m_context.modelDir,
        AppText::tr("Image Files (*.png *.jpg *.jpeg *.webp)")
    );
    if (srcPath.isEmpty()) return;

    const QString destPath = previewPathForIndex(row);
    int w = 0;
    int h = 0;
    if (!saveImageToPreviewPath(srcPath, destPath, w, h)) {
        QMessageBox::warning(this, AppText::tr("错误"), AppText::tr("图片保存失败，请检查文件格式。"));
        return;
    }

    m_images[row].width = w;
    m_images[row].height = h;
    applyImageMetadataFromFile(srcPath, m_images[row]);
    if (row == 0) {
        m_previewPath = destPath;
    }

    emit imagesChanged(m_images, m_previewPath, destPath);
    refreshEditImages();
    ui->listEditImages->setCurrentRow(row);
}

void LocalMetaEditorPage::onRemoveImageClicked()
{
    const int row = ui->listEditImages->currentRow();
    if (row < 0 || row >= m_images.size()) return;

    const QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        AppText::tr("删除预览图"),
        AppText::tr("确定删除选中的预览图吗？"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );
    if (reply != QMessageBox::Yes) return;

    commitEditImageFields();

    const QString transactionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QMap<int, QPair<QString, QString>> stagedFiles;
    for (int i = row; i < m_images.size(); ++i) {
        const QString originalPath = previewPathForIndex(i);
        if (!QFile::exists(originalPath)) continue;
        const QString stagedPath = originalPath + ".edit-backup-" + transactionId;
        if (!QFile::rename(originalPath, stagedPath)) {
            auto it = stagedFiles.constEnd();
            while (it != stagedFiles.constBegin()) {
                --it;
                QFile::rename(it.value().second, it.value().first);
            }
            QMessageBox::warning(this, AppText::tr("删除失败"), AppText::tr("无法安全暂存预览图，原文件未删除。"));
            return;
        }
        stagedFiles.insert(i, qMakePair(originalPath, stagedPath));
    }

    QList<int> movedIndexes;
    bool moveFailed = false;
    for (int i = row + 1; i < m_images.size(); ++i) {
        if (!stagedFiles.contains(i)) continue;
        const QString destination = previewPathForIndex(i - 1);
        if (!QFile::rename(stagedFiles.value(i).second, destination)) {
            moveFailed = true;
            break;
        }
        movedIndexes.append(i);
    }
    if (moveFailed) {
        for (auto it = movedIndexes.crbegin(); it != movedIndexes.crend(); ++it) {
            const int index = *it;
            QFile::rename(previewPathForIndex(index - 1), stagedFiles.value(index).second);
        }
        bool restored = true;
        auto it = stagedFiles.constEnd();
        while (it != stagedFiles.constBegin()) {
            --it;
            if (QFile::exists(it.value().second)
                && !QFile::rename(it.value().second, it.value().first)) {
                restored = false;
            }
        }
        QMessageBox::critical(this, AppText::tr("删除失败"),
                              restored ? AppText::tr("预览图重新编号失败，原文件已恢复。")
                                       : AppText::tr("预览图重新编号失败，部分备份文件无法恢复，请检查模型目录。"));
        return;
    }

    if (stagedFiles.contains(row) && !QFile::remove(stagedFiles.value(row).second)) {
        qWarning() << "Unable to remove staged preview image:" << stagedFiles.value(row).second;
    }

    const QString invalidatedPath = previewPathForIndex(0);
    m_images.removeAt(row);
    if (!m_images.isEmpty()) {
        m_previewPath = previewPathForIndex(0);
    } else {
        m_previewPath.clear();
    }

    emit imagesChanged(m_images, m_previewPath, invalidatedPath);
    refreshEditImages();
    const int nextRow = qMin(row, ui->listEditImages->count() - 1);
    if (nextRow >= 0) ui->listEditImages->setCurrentRow(nextRow);
}

void LocalMetaEditorPage::onSetCoverClicked()
{
    const int row = ui->listEditImages->currentRow();
    if (row <= 0 || row >= m_images.size()) return;

    commitEditImageFields();

    const QString path0 = previewPathForIndex(0);
    const QString pathN = previewPathForIndex(row);
    if (!QFile::exists(path0) || !QFile::exists(pathN)) {
        QMessageBox::warning(this, AppText::tr("设置封面失败"), AppText::tr("封面或目标预览图文件不存在，未修改图片顺序。"));
        return;
    }

    const QString tempPath = path0 + ".swap-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QFile::rename(path0, tempPath)) {
        QMessageBox::warning(this, AppText::tr("设置封面失败"), AppText::tr("无法暂存当前封面，原文件未修改。"));
        return;
    }
    if (!QFile::rename(pathN, path0)) {
        QFile::rename(tempPath, path0);
        QMessageBox::warning(this, AppText::tr("设置封面失败"), AppText::tr("无法移动目标预览图，原封面已恢复。"));
        return;
    }
    if (!QFile::rename(tempPath, pathN)) {
        const bool restoredTarget = QFile::rename(path0, pathN);
        const bool restoredCover = QFile::rename(tempPath, path0);
        QMessageBox::critical(this, AppText::tr("设置封面失败"),
                              restoredTarget && restoredCover
                                  ? AppText::tr("无法完成封面交换，原文件已恢复。")
                                  : AppText::tr("无法完成封面交换，自动恢复失败，请检查模型目录。"));
        return;
    }

    qSwap(m_images[0], m_images[row]);

    m_previewPath = previewPathForIndex(0);
    emit imagesChanged(m_images, m_previewPath, m_previewPath);
    refreshEditImages();
    ui->listEditImages->setCurrentRow(0);
}
