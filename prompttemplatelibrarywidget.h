#ifndef PROMPTTEMPLATELIBRARYWIDGET_H
#define PROMPTTEMPLATELIBRARYWIDGET_H

#include <QHash>
#include <QPointer>
#include <QVector>
#include <QWidget>

namespace Ui {
class PromptTemplateLibraryWidget;
}

template <typename T>
class QFutureWatcher;
class QEvent;
class QLineEdit;
class QListWidgetItem;
class QObject;
class QTableWidgetItem;
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

    Ui::PromptTemplateLibraryWidget *ui;
    const QHash<QString, QString> *m_translationMap = nullptr;
    QVector<PromptTemplate> m_templates;
    QVector<PromptPlaceholder> m_placeholders;
    QVector<ImageExtractTemplate> m_imageTemplates;
    QHash<QString, QWidget*> m_placeholderEditors;
    QFutureWatcher<QVector<TagUsageRow>> *m_tagWatcher = nullptr;
    TagFlowWidget *m_generateImagePositiveTags = nullptr;
    TagFlowWidget *m_generateImageNegativeTags = nullptr;
    TagFlowWidget *m_templateImagePositiveTags = nullptr;
    TagFlowWidget *m_templateImageNegativeTags = nullptr;
    QVector<TagUsageRow> m_allTagRows;
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
    QStringList splitOptions(const QString &text) const;

    void loadLibrary();
    void saveLibrary();
    void createDefaultLibrary();
    void refreshAllLists();
    void refreshGenerateTemplateCombo();
    void refreshTemplateList();
    void refreshPlaceholderTable();
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
    void insertTagsIntoTarget(const QStringList &tags, bool positiveTarget);
    QStringList selectedTagTexts() const;
    void loadTagPickerRows(bool force = false);
    void refreshTagPickerTable();
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
    void onPlaceholderCellChanged(int row, int column);
    void onSavePlaceholderClicked();
    void onNewPlaceholderClicked();
    void onDeletePlaceholderClicked();
    void onTagPickerFiltersChanged();
};

#endif // PROMPTTEMPLATELIBRARYWIDGET_H
