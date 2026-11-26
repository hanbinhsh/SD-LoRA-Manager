#ifndef LOCALMETAEDITORPAGE_H
#define LOCALMETAEDITORPAGE_H

#include "core/modeltypes.h"

#include <QIcon>
#include <QImage>
#include <QList>
#include <QString>
#include <QWidget>

class QThreadPool;

namespace Ui {
class LocalMetaEditorPage;
}

struct LocalMetaEditorContext {
    QString filePath;
    QString baseName;
    QString modelDir;
    int usageCount = 0;
    qint64 lastUsedMs = 0;
    bool hasSyncFailure = false;
    QString syncFailureError;
};

class LocalMetaEditorPage : public QWidget
{
    Q_OBJECT

public:
    explicit LocalMetaEditorPage(QWidget *parent = nullptr);
    ~LocalMetaEditorPage() override;

    void setThreadPool(QThreadPool *pool);
    void setPlaceholderIcon(const QIcon &icon);

    void clear();
    void loadModel(const ModelMeta &meta, const LocalMetaEditorContext &context, bool isVisibleNow);
    void updateStatus(const ModelMeta &meta, const LocalMetaEditorContext &context);
    void ensureImagesRefreshed();

    static bool saveImageToPreviewPath(const QString &srcPath, const QString &destPath, int &outW, int &outH);
    static void applyParametersToImage(const QString &params, ImageInfo &img);

    Q_INVOKABLE void onIconLoaded(const QString &id, const QImage &image);

signals:
    void metadataSaved(const QString &filePath, const QString &jsonPath, const QString &previewPath);
    void resetRequested();
    void imagesChanged(const QList<ImageInfo> &images, const QString &previewPath, const QString &invalidatedIconPath);

private slots:
    void onSaveClicked();
    void onResetClicked();
    void onEditImageSelectionChanged(int row);
    void onAddImageClicked();
    void onReplaceImageClicked();
    void onRemoveImageClicked();
    void onSetCoverClicked();

private:
    void refreshEditImages();
    void commitEditImageFields();
    void loadEditImageFields(int index);
    void clearEditImageFields();
    QString previewPathForIndex(int index) const;
    static void applyImageMetadataFromFile(const QString &srcPath, ImageInfo &img);

    Ui::LocalMetaEditorPage *ui = nullptr;
    QThreadPool *m_threadPool = nullptr;
    QIcon m_placeholderIcon;
    LocalMetaEditorContext m_context;
    QList<ImageInfo> m_images;
    QString m_previewPath;
    int m_currentImageIndex = -1;
    int m_imageLoadToken = 0;
    bool m_imagesNeedRefresh = false;
    bool m_hasModel = false;
};

#endif // LOCALMETAEDITORPAGE_H
