#ifndef PROMPTTEMPLATELIBRARYWIDGET_H
#define PROMPTTEMPLATELIBRARYWIDGET_H

#include <QHash>
#include <QIcon>
#include <QPointer>
#include <QSet>
#include <QVector>
#include <QWidget>

namespace Ui {
class PromptTemplateLibraryWidget;
}

template <typename T>
class QFutureWatcher;
class QComboBox;
class QEvent;
class QLabel;
class QLineEdit;
class QListWidgetItem;
class QObject;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;
class QTreeWidget;
class TagFlowWidget;

class PromptTemplateLibraryWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PromptTemplateLibraryWidget(QWidget *parent = nullptr);
    ~PromptTemplateLibraryWidget();

    void setTranslationMap(const QHash<QString, QString> *map);
    void reloadTemplateLibrary();

    struct TagUsageRow {
        QString tag;
        QString kind;
        int count = 0;
    };

    struct ModelTriggerRow {
        QString modelKey;
        QString modelName;
        QString previewPath;
        QIcon previewIcon;
        QString trigger;
        QString source;
        QString modelType;
    };

    void setModelTriggerRows(const QVector<ModelTriggerRow> &rows);

signals:
    void modelTriggerRowsRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class PlaceholderType {
        Text,
        SingleChoice,
        MultiChoice
    };

    struct PromptTemplate {
        QString id;
        QString name;
        QString category;
        QString positiveTemplate;
        QString negativeTemplate;
        QString notes;
        QHash<QString, QString> placeholderDefaults;
    };

    struct PromptPlaceholder {
        QString id;
        QString name;
        QString label;
        PlaceholderType type = PlaceholderType::Text;
        QString defaultValue;
        QStringList options;
    };

    struct ImageExtractTemplate {
        QString id;
        QString name;
        QString positiveTemplate;
        QString negativeTemplate;
        QString notes;
    };

    struct PromptFavorite {
        QString id;
        QString name;
        QString positive;
        QString negative;
        QString createdAt;
        QString updatedAt;
    };

    struct TagPickerUi {
        QWidget *page = nullptr;
        QLineEdit *search = nullptr;
        QComboBox *scope = nullptr;
        QPushButton *refresh = nullptr;
        QPushButton *insertPositive = nullptr;
        QPushButton *insertNegative = nullptr;
        QTableWidget *table = nullptr;
        QLabel *status = nullptr;
        bool insertIntoTemplate = false;
    };

    struct ModelTriggerPickerUi {
        QWidget *page = nullptr;
        QLineEdit *search = nullptr;
        QPushButton *insertPositive = nullptr;
        QTreeWidget *tree = nullptr;
        QLabel *status = nullptr;
        bool insertIntoTemplate = false;
        QSet<QString> expandedModelKeys;
    };

    Ui::PromptTemplateLibraryWidget *ui;
    const QHash<QString, QString> *m_translationMap = nullptr;
    QVector<PromptTemplate> m_templates;
    QVector<PromptPlaceholder> m_placeholders;
    QVector<ImageExtractTemplate> m_imageTemplates;
    QVector<PromptFavorite> m_favorites;
    QHash<QString, QWidget*> m_placeholderEditors;
    QFutureWatcher<QVector<TagUsageRow>> *m_tagWatcher = nullptr;
    TagFlowWidget *m_generateImagePositiveTags = nullptr;
    TagFlowWidget *m_generateImageNegativeTags = nullptr;
    TagFlowWidget *m_templateImagePositiveTags = nullptr;
    TagFlowWidget *m_templateImageNegativeTags = nullptr;
    QVector<TagUsageRow> m_allTagRows;
    int m_loadedTagScope = -1;
    TagPickerUi m_generateTagPicker;
    TagPickerUi m_templateTagPicker;
    ModelTriggerPickerUi m_generateModelTriggerPicker;
    ModelTriggerPickerUi m_templateModelTriggerPicker;
    QVector<ModelTriggerRow> m_modelTriggerRows;
    bool m_modelTriggerRowsDirty = true;
    bool m_modelTriggerRowsLoaded = false;
    QString m_currentImagePath;
    QString m_lastRenderedPositivePrompt;
    QString m_lastRenderedNegativePrompt;
    bool m_loadingUi = false;
    bool m_dirty = false;

    QString libraryPath() const;
    QString ensureId(const QString &prefix) const;
    QString typeToString(PlaceholderType type) const;
    PlaceholderType typeFromString(const QString &text) const;
    QString translatedTextForTag(const QString &tag) const;

    QStringList currentPlaceholderOptionValues() const;
    void setPlaceholderOptionValues(const QStringList &options);
    void updatePlaceholderOptionEditorFromSelection();
    void updatePlaceholderOptionControls();
    void addPlaceholderOptionFromEditor();
    void updateSelectedPlaceholderOptionFromEditor();
    void deleteSelectedPlaceholderOption();
    void moveSelectedPlaceholderOptionUp();
    void moveSelectedPlaceholderOptionDown();

    void refreshPlaceholderTableKeepingName(const QString &name);

    void loadLibrary();
    void saveLibrary();
    void createDefaultLibrary();
    void refreshAllLists();
    void refreshGenerateTemplateCombo();
    void refreshTemplateList();
    void refreshPlaceholderTable();
    void refreshFavoritesTable();
    void rebuildPlaceholderInputs();
    void updateGeneratedPrompt();
    void updateTemplateEditorFromSelection();
    void updatePlaceholderEditorFromSelection();
    void saveCurrentTemplateEditor();
    void saveCurrentPlaceholderEditor();
    QString renderTemplateText(const QString &text, QStringList *missing = nullptr) const;
    QHash<QString, QString> currentPlaceholderValues(QStringList *missing = nullptr) const;
    int templateIndexById(const QString &id) const;
    int placeholderIndexByName(const QString &name) const;
    QString selectedTemplateId() const;
    void setStatus(const QString &text);
    void copyText(const QString &text) const;
    void setupTagPickerUi(TagPickerUi &picker);
    void setupModelTriggerPickerUi(ModelTriggerPickerUi &picker);
    QStringList selectedTagTexts(const TagPickerUi &picker) const;
    QStringList selectedModelTriggerTexts(const ModelTriggerPickerUi &picker) const;
    void loadTagPickerRows(TagPickerUi &picker, bool force = false);
    void refreshTagPickerTable(TagPickerUi &picker);
    void addPickerTags(TagPickerUi &picker, bool positiveTarget);
    void refreshModelTriggerPickerTable(ModelTriggerPickerUi &picker);
    void addModelTriggerTags(ModelTriggerPickerUi &picker);
    void addCurrentPromptToFavorites();
    void copyFavoriteById(const QString &id) const;
    void replacePromptFromFavoriteById(const QString &id);
    void deleteFavoriteById(const QString &id);
    void parseGenerateImageTags();
    void addGenerateImageTagsToPrompt(bool positiveTarget);
    void parseTemplateImageTags();
    void addTemplateImageTagsToTemplate(bool positiveTarget);
    QMap<QString, int> tagCountsFromPrompt(const QString &prompt) const;
    QStringList selectedGenerateImageTags(bool positiveTarget) const;
    QStringList selectedTemplateImageTags(bool positiveTarget) const;

private slots:
    void onTabChanged(int index);
    void onGenerateTemplateChanged(int index);
    void onTemplateListCurrentRowChanged(int row);
    void onSaveTemplateClicked();
    void onNewTemplateClicked();
    void onDuplicateTemplateClicked();
    void onDeleteTemplateClicked();
    void onSavePlaceholderClicked();
    void onNewPlaceholderClicked();
    void onDeletePlaceholderClicked();
    void onTagPickerFiltersChanged(TagPickerUi &picker);
};

#endif // PROMPTTEMPLATELIBRARYWIDGET_H
