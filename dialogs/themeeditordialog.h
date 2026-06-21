#ifndef THEMEEDITORDIALOG_H
#define THEMEEDITORDIALOG_H

#include <QDialog>
#include <QHash>
#include <QString>

class QComboBox;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

// 调色板主题编辑器：常用色优先 + 高级全量 token，逐项取色，实时预览。
// 编辑结果存为用户主题 JSON（config/themes/<id>.json），通过信号驱动 MainWindow
// 完成实时预览 / 保存后应用 / 取消还原。
class ThemeEditorDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ThemeEditorDialog(const QString &baseThemeId, QWidget *parent = nullptr);

signals:
    // 任一改动 -> 整窗预览。lightScheme 决定原生 colorScheme（标题栏/复选框/未样式控件明暗）。
    void previewRequested(const QHash<QString, QString> &palette, bool lightScheme);
    void canceled();                   // 取消/关闭 -> 请求还原
    void saved(const QString &themeId); // 保存成功 -> 请求应用并持久化

private:
    struct TokenRow {
        QLineEdit *hex = nullptr;
        QPushButton *swatch = nullptr;
    };

    void buildUi(const QString &baseThemeId);
    void addSectionHeader(QVBoxLayout *layout, const QString &title);
    void addTokenRow(QVBoxLayout *layout, const QString &token, const QString &label);
    void loadPalette(const QHash<QString, QString> &palette);
    void setTokenColor(const QString &token, const QString &hex, bool doEmit);
    void updateSwatch(const QString &token, const QString &hex);
    void onBaseChanged();
    void onSaveClicked();
    void emitPreview();
    bool isLightSelected() const;

    static QString sanitizeId(const QString &name);

    QComboBox *m_comboBase = nullptr;
    QComboBox *m_comboScheme = nullptr;
    QLineEdit *m_editName = nullptr;
    QHash<QString, TokenRow> m_rows;
    QHash<QString, QString> m_palette; // 当前工作调色板（全量）
    bool m_loading = false;
};

#endif // THEMEEDITORDIALOG_H
