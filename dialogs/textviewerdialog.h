#ifndef TEXTVIEWERDIALOG_H
#define TEXTVIEWERDIALOG_H

#include <QDialog>
#include <QString>

namespace Ui {
class TextViewerDialog;
}

/**
 * Reusable modal viewer dialog backed by textviewerdialog.ui for displaying
 * rich HTML descriptions or editable plain/JSON text chunks.
 */
class TextViewerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TextViewerDialog(QWidget *parent = nullptr);
    ~TextViewerDialog() override;

    void configureForHtmlDescription(const QString &titleText, const QString &descriptionHtml);
    void configureForRawMetadata(const QString &titleText, const QString &rawText);

signals:
    void statusMessageRequested(const QString &message, int timeoutMs);

private:
    static QString normalizeCopiedText(QString text);
    void copySelectedText();
    void copyAllText();
    void formatJsonSections();

    Ui::TextViewerDialog *ui = nullptr;
    QString m_copySelectedFeedback;
    QString m_copyAllFeedback;
};

#endif // TEXTVIEWERDIALOG_H
