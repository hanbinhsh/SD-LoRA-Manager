#ifndef LAUNCHERWIDGET_H
#define LAUNCHERWIDGET_H

#include <QWidget>
#include <QString>

namespace Ui {
class LauncherWidget;
}

class QLineEdit;
class QPushButton;
class QPlainTextEdit;
class QProgressBar;
class QProcess;

class LauncherWidget : public QWidget
{
    Q_OBJECT

public:
    explicit LauncherWidget(QWidget *parent = nullptr);
    ~LauncherWidget() override;

private:
    enum class Target { A1111 = 0, ComfyUI = 1 };

    // 每个目标（A1111 / ComfyUI）一份：控件指针 + 运行态。
    struct TargetPanel {
        Target target = Target::A1111;
        QString keyPrefix;   // "launcher_a1111_" / "launcher_comfyui_"
        QString defaultUrl;  // http://127.0.0.1:7860 / :8188

        QLineEdit *editScript = nullptr;
        QPushButton *btnBrowse = nullptr;
        QLineEdit *editArgs = nullptr;
        QLineEdit *editWorkdir = nullptr;
        QPushButton *btnBrowseWd = nullptr;
        QLineEdit *editUrl = nullptr;
        QPlainTextEdit *editEnv = nullptr;
        QPushButton *btnStartStop = nullptr;
        QPushButton *btnOpen = nullptr;
        QPushButton *btnClear = nullptr;
        QProgressBar *progressBar = nullptr;
        QPlainTextEdit *console = nullptr;

        QProcess *process = nullptr;
        QString detectedUrl;
        bool urlDetected = false;
        QString lineBuffer;   // 跨 chunk 的不完整行缓冲
    };

    void setupTarget(TargetPanel &p);
    void startStop(TargetPanel &p);
    void start(TargetPanel &p);
    void stop(TargetPanel &p);
    void appendConsole(TargetPanel &p, const QString &chunk);
    void commitConsoleSegment(TargetPanel &p, const QString &segment, bool permanent);
    void updateProgress(TargetPanel &p, const QString &plain);
    void scanForUrl(TargetPanel &p, const QString &text);
    void openInBrowser(TargetPanel &p);
    void browseScript(TargetPanel &p);
    void browseWorkdir(TargetPanel &p);
    void setRunningUi(TargetPanel &p, bool running);
    void enableOpenButton(TargetPanel &p, const QString &url);
    QString effectiveUrl(const TargetPanel &p) const;
    bool buildInvocation(const TargetPanel &p, QString &program, QStringList &args) const;
    void killProcessTree(TargetPanel &p);

    QString settingsPath() const;
    void loadSettings();
    void saveSettings() const;

    Ui::LauncherWidget *ui = nullptr;
    TargetPanel m_a1111;
    TargetPanel m_comfy;

    static constexpr int kMaxConsoleLines = 5000;
};

#endif // LAUNCHERWIDGET_H
