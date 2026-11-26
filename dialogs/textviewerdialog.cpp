#include "textviewerdialog.h"
#include "ui_textviewerdialog.h"
#include "utils/applanguage.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMessageBox>
#include <QRegularExpression>
#include <QTextCursor>

TextViewerDialog::TextViewerDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::TextViewerDialog)
{
    ui->setupUi(this);

    connect(ui->btnCopySelected, &QPushButton::clicked, this, &TextViewerDialog::copySelectedText);
    connect(ui->btnCopyAll, &QPushButton::clicked, this, &TextViewerDialog::copyAllText);
    connect(ui->btnFormatJson, &QPushButton::clicked, this, &TextViewerDialog::formatJsonSections);
    connect(ui->btnClose, &QPushButton::clicked, this, &QDialog::accept);
}

TextViewerDialog::~TextViewerDialog()
{
    delete ui;
}

void TextViewerDialog::configureForHtmlDescription(const QString &titleText, const QString &descriptionHtml)
{
    setWindowTitle(AppText::tr("模型简介 / Description"));
    resize(820, 620);

    ui->lblTitle->setText(titleText);
    ui->textEdit->setReadOnly(true);
    ui->textEdit->setAcceptRichText(true);
    ui->textEdit->setLineWrapMode(QTextEdit::WidgetWidth);
    ui->textEdit->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
    ui->textEdit->setHtml(descriptionHtml);

    ui->btnCopySelected->setVisible(true);
    ui->btnCopyAll->setText(AppText::tr("复制全部 / Copy All"));
    ui->btnFormatJson->setVisible(false);

    m_copySelectedFeedback = AppText::tr("已复制选中的简介文本。");
    m_copyAllFeedback = AppText::tr("已复制完整简介。");
}

void TextViewerDialog::configureForRawMetadata(const QString &titleText, const QString &rawText)
{
    setWindowTitle(AppText::tr("原始 Metadata / Raw Metadata"));
    resize(900, 650);

    ui->lblTitle->setText(titleText);
    ui->textEdit->setAcceptRichText(false);
    ui->textEdit->setPlainText(rawText);
    ui->textEdit->setLineWrapMode(QTextEdit::NoWrap);
    ui->textEdit->setReadOnly(false);

    ui->btnCopySelected->setVisible(false);
    ui->btnCopyAll->setText(AppText::tr("复制 / Copy"));
    ui->btnFormatJson->setVisible(true);

    m_copySelectedFeedback.clear();
    m_copyAllFeedback.clear();
}

QString TextViewerDialog::normalizeCopiedText(QString text)
{
    text.replace(QChar(0x2029), QLatin1Char('\n'));
    text.replace(QChar(0x2028), QLatin1Char('\n'));
    return text.trimmed();
}

void TextViewerDialog::copySelectedText()
{
    const QString selected = normalizeCopiedText(ui->textEdit->textCursor().selectedText());
    if (selected.isEmpty()) {
        emit statusMessageRequested(AppText::tr("请先在简介中选中要复制的文本。"), 2000);
        return;
    }
    QGuiApplication::clipboard()->setText(selected);
    if (!m_copySelectedFeedback.isEmpty()) {
        emit statusMessageRequested(m_copySelectedFeedback, 2000);
    }
}

void TextViewerDialog::copyAllText()
{
    const QString text = ui->textEdit->isReadOnly()
        ? normalizeCopiedText(ui->textEdit->toPlainText())
        : ui->textEdit->toPlainText();
    QGuiApplication::clipboard()->setText(text);
    if (!m_copyAllFeedback.isEmpty()) {
        emit statusMessageRequested(m_copyAllFeedback, 2000);
    }
}

void TextViewerDialog::formatJsonSections()
{
    const QString text = ui->textEdit->toPlainText();
    static const QRegularExpression sectionRegex(
        QStringLiteral("===== ([^=]+) =====\\n(.*?)(?=\\n\\n===== |\\z)"),
        QRegularExpression::DotMatchesEverythingOption);
    QRegularExpressionMatchIterator it = sectionRegex.globalMatch(text);
    QStringList formattedSections;
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString key = match.captured(1).trimmed();
        const QString value = match.captured(2);
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(value.toUtf8(), &error);
        if (error.error == QJsonParseError::NoError && !doc.isNull()) {
            formattedSections << QStringLiteral("===== %1 =====\n%2")
                                     .arg(key, QString::fromUtf8(doc.toJson(QJsonDocument::Indented)));
        } else {
            formattedSections << QStringLiteral("===== %1 =====\n%2").arg(key, value);
        }
    }

    if (formattedSections.isEmpty()) {
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &error);
        if (error.error == QJsonParseError::NoError && !doc.isNull()) {
            ui->textEdit->setPlainText(QString::fromUtf8(doc.toJson(QJsonDocument::Indented)));
        } else {
            QMessageBox::information(this, AppText::tr("格式化 JSON"), AppText::tr("当前内容不是有效 JSON。"));
        }
        return;
    }

    ui->textEdit->setPlainText(formattedSections.join(QStringLiteral("\n\n")));
}
