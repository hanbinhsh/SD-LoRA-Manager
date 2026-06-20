#include "launcherwidget.h"
#include "ui_launcherwidget.h"
#include "styleconstants.h"

#include <QApplication>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QUrl>

namespace {
// 去掉所有 ANSI 转义序列，得到纯文本（用于 URL 识别 / 进度判断）。
QString stripAnsi(QString text)
{
    static const QRegularExpression re("\\x1B\\[[0-9;]*[A-Za-z]");
    text.remove(re);
    return text;
}

// 是否为 tqdm 风格进度行（包含 "<数字>%|"）。
bool isProgressLine(const QString &plain)
{
    static const QRegularExpression re("\\d{1,3}%\\|");
    return re.match(plain).hasMatch();
}

// 展开 %VAR% 与 ${VAR}（取自给定环境），便于用户写 PATH=...;%PATH% 之类。
QString expandEnvVars(QString value, const QProcessEnvironment &env)
{
    static const QRegularExpression rePercent("%([A-Za-z_][A-Za-z0-9_]*)%");
    static const QRegularExpression reBrace("\\$\\{([A-Za-z_][A-Za-z0-9_]*)\\}");
    for (const QRegularExpression *re : { &rePercent, &reBrace }) {
        QString out;
        int pos = 0;
        auto it = re->globalMatch(value);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            out += value.mid(pos, m.capturedStart() - pos);
            out += env.value(m.captured(1));
            pos = m.capturedEnd();
        }
        out += value.mid(pos);
        value = out;
    }
    return value;
}

QString htmlEscape(const QString &s)
{
    QString o;
    o.reserve(s.size());
    for (const QChar c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == ' ') o += "&nbsp;";
        else o += c;
    }
    return o;
}

// ANSI SGR 前景色 -> 暗色主题下的十六进制颜色。
QString ansiColor(int code)
{
    switch (code) {
    case 30: case 90: return "#7f8791";
    case 31: case 91: return "#ff6b6b";
    case 32: case 92: return "#5fd38d";
    case 33: case 93: return "#ffcc00";
    case 34: case 94: return "#66c0f4";
    case 35: case 95: return "#c678dd";
    case 36: case 96: return "#56b6c2";
    case 37: case 97: return "#dcdedf";
    default: return QString();
    }
}

// 把含 ANSI 颜色码的一行转换为带颜色的 HTML（保留空格对齐）。
QString ansiToHtml(QString input)
{
    // 先移除非颜色（非以 'm' 结尾）的 CSI 序列，如光标移动/清行。
    static const QRegularExpression nonSgr("\\x1B\\[[0-9;]*[A-Za-ln-z]");
    input.remove(nonSgr);

    static const QRegularExpression sgr("\\x1B\\[([0-9;]*)m");
    QString out;
    QString curColor;
    int pos = 0;
    auto appendText = [&out, &curColor](const QString &t) {
        if (t.isEmpty()) return;
        const QString esc = htmlEscape(t);
        if (curColor.isEmpty()) out += esc;
        else out += "<span style=\"color:" + curColor + ";\">" + esc + "</span>";
    };
    auto it = sgr.globalMatch(input);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        appendText(input.mid(pos, m.capturedStart() - pos));
        pos = m.capturedEnd();
        const QString params = m.captured(1);
        const QStringList codes = params.isEmpty() ? QStringList{ "0" } : params.split(';');
        for (const QString &cs : codes) {
            const int code = cs.toInt();
            if (code == 0) curColor.clear();
            else { const QString c = ansiColor(code); if (!c.isEmpty()) curColor = c; }
        }
    }
    appendText(input.mid(pos));
    return out;
}
}

LauncherWidget::LauncherWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::LauncherWidget)
{
    ui->setupUi(this);
    // 不要把 toolpage.qss 设在 LauncherWidget 本体上——那会让子控件 targetTabs 走“局部”QStyleSheetStyle，
    // 杀掉它的 autoFillBackground+调色板，使 West 标签条里没有 tab 按钮的空白区域回退成 Dark 配色的 #1e1e1e，
    // 且局部 QSS 无法重绘那块本体区域（试过本体背景、QTabBar 背景都无效）。
    // 改为只给两个内容页设样式：这样 targetTabs（QTabWidget）不带局部样式表，和工具箱的 toolsTabWidget 一致——
    // 外层 tab 由全局 mainwindow.qss（#targetTabs 规则）+ 下面设置的调色板绘制，内容控件由 toolpage.qss 绘制。
    const QString toolQss = AppStyle::loadToolPageQss();
    ui->pageA1111->setStyleSheet(toolQss);
    ui->pageComfyUI->setStyleSheet(toolQss);

    // 与工具箱页保持一致：West 标签条使用深色侧栏背景。
    ui->targetTabs->setAutoFillBackground(true);
    QPalette tabPalette = ui->targetTabs->palette();
    tabPalette.setColor(QPalette::Window, QColor(AppStyle::SidebarDark));
    ui->targetTabs->setPalette(tabPalette);

    m_a1111.target = Target::A1111;
    m_a1111.keyPrefix = "launcher_a1111_";
    m_a1111.defaultUrl = "http://127.0.0.1:7860";
    m_a1111.editScript = ui->editScript_a1111;
    m_a1111.btnBrowse = ui->btnBrowse_a1111;
    m_a1111.editArgs = ui->editArgs_a1111;
    m_a1111.editWorkdir = ui->editWorkdir_a1111;
    m_a1111.btnBrowseWd = ui->btnBrowseWd_a1111;
    m_a1111.editUrl = ui->editUrl_a1111;
    m_a1111.editEnv = ui->editEnv_a1111;
    m_a1111.btnStartStop = ui->btnStartStop_a1111;
    m_a1111.btnOpen = ui->btnOpen_a1111;
    m_a1111.btnClear = ui->btnClear_a1111;
    m_a1111.progressBar = ui->progress_a1111;
    m_a1111.console = ui->console_a1111;

    m_comfy.target = Target::ComfyUI;
    m_comfy.keyPrefix = "launcher_comfyui_";
    m_comfy.defaultUrl = "http://127.0.0.1:8188";
    m_comfy.editScript = ui->editScript_comfyui;
    m_comfy.btnBrowse = ui->btnBrowse_comfyui;
    m_comfy.editArgs = ui->editArgs_comfyui;
    m_comfy.editWorkdir = ui->editWorkdir_comfyui;
    m_comfy.btnBrowseWd = ui->btnBrowseWd_comfyui;
    m_comfy.editUrl = ui->editUrl_comfyui;
    m_comfy.editEnv = ui->editEnv_comfyui;
    m_comfy.btnStartStop = ui->btnStartStop_comfyui;
    m_comfy.btnOpen = ui->btnOpen_comfyui;
    m_comfy.btnClear = ui->btnClear_comfyui;
    m_comfy.progressBar = ui->progress_comfyui;
    m_comfy.console = ui->console_comfyui;

    setupTarget(m_a1111);
    setupTarget(m_comfy);
    loadSettings();
}

LauncherWidget::~LauncherWidget()
{
    saveSettings();
    killProcessTree(m_a1111);
    killProcessTree(m_comfy);
    delete ui;
}

void LauncherWidget::setupTarget(TargetPanel &p)
{
    TargetPanel *pp = &p;

    p.console->setMaximumBlockCount(kMaxConsoleLines);
    QFont mono("Consolas");
    mono.setStyleHint(QFont::Monospace);
    p.console->setFont(mono);

    if (p.progressBar) {
        p.progressBar->setRange(0, 100);
        p.progressBar->setTextVisible(true);
        p.progressBar->setVisible(false);
        // 用较深的进度色 + 白色文字，避免亮蓝底上白字看不清。
        p.progressBar->setStyleSheet(
            "QProgressBar{border:1px solid #31363d;border-radius:3px;background:#16191e;"
            "color:#ffffff;text-align:center;min-height:18px;}"
            "QProgressBar::chunk{background-color:#2f5d8a;border-radius:2px;}");
    }

    p.process = new QProcess(this);
    p.process->setProcessChannelMode(QProcess::SeparateChannels);
    // 进程输出按 UTF-8 解码（A1111/ComfyUI 的 tqdm 方块字符等均为 UTF-8）。
    connect(p.process, &QProcess::readyReadStandardOutput, this, [this, pp]() {
        appendConsole(*pp, QString::fromUtf8(pp->process->readAllStandardOutput()));
    });
    connect(p.process, &QProcess::readyReadStandardError, this, [this, pp]() {
        appendConsole(*pp, QString::fromUtf8(pp->process->readAllStandardError()));
    });
    connect(p.process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, pp](int code, QProcess::ExitStatus) {
        setRunningUi(*pp, false);
        pp->console->appendPlainText(QString("[进程已退出，exit code %1]").arg(code));
    });
    connect(p.process, &QProcess::errorOccurred, this, [this, pp](QProcess::ProcessError) {
        setRunningUi(*pp, false);
        pp->console->appendPlainText("[进程错误] " + pp->process->errorString());
    });

    connect(p.btnStartStop, &QPushButton::clicked, this, [this, pp]() { startStop(*pp); });
    connect(p.btnBrowse, &QPushButton::clicked, this, [this, pp]() { browseScript(*pp); });
    connect(p.btnBrowseWd, &QPushButton::clicked, this, [this, pp]() { browseWorkdir(*pp); });
    connect(p.btnOpen, &QPushButton::clicked, this, [this, pp]() { openInBrowser(*pp); });
    connect(p.btnClear, &QPushButton::clicked, this, [pp]() { pp->console->clear(); });

    for (QLineEdit *edit : {p.editScript, p.editArgs, p.editWorkdir, p.editUrl}) {
        connect(edit, &QLineEdit::editingFinished, this, [this]() { saveSettings(); });
    }
    connect(p.editUrl, &QLineEdit::textChanged, this, [pp](const QString &text) {
        if (!pp->urlDetected) pp->btnOpen->setEnabled(!text.trimmed().isEmpty());
    });
}

void LauncherWidget::startStop(TargetPanel &p)
{
    if (p.process && p.process->state() != QProcess::NotRunning) stop(p);
    else start(p);
}

bool LauncherWidget::buildInvocation(const TargetPanel &p, QString &program, QStringList &args) const
{
    const QString script = p.editScript->text().trimmed();
    if (script.isEmpty() || !QFileInfo::exists(script)) return false;

    const QStringList userArgs = QProcess::splitCommand(p.editArgs->text().trimmed());
    const QString suffix = QFileInfo(script).suffix().toLower();
    const QString nativeScript = QDir::toNativeSeparators(script);

    if (suffix == "bat" || suffix == "cmd") {
        program = "cmd.exe";
        args = QStringList{ "/c", nativeScript } + userArgs;
    } else if (suffix == "py") {
        program = "python";
        args = QStringList{ nativeScript } + userArgs;
    } else {
        program = nativeScript;
        args = userArgs;
    }
    return true;
}

void LauncherWidget::start(TargetPanel &p)
{
    const QString script = p.editScript->text().trimmed();
    if (script.isEmpty() || !QFileInfo::exists(script)) {
        QMessageBox::warning(this, "启动器", "启动脚本/程序路径无效，请先选择有效的脚本或可执行文件。");
        return;
    }

    QString program;
    QStringList args;
    buildInvocation(p, program, args);

    QString workdir = p.editWorkdir->text().trimmed();
    if (workdir.isEmpty()) workdir = QFileInfo(script).absolutePath();
    p.process->setWorkingDirectory(workdir);

    // 自定义环境变量：在系统环境基础上追加/覆盖（每行 KEY=VALUE，支持 %VAR%/${VAR} 展开）。
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // 默认强制 python 不缓冲输出，否则管道下 stdout 会块缓冲，
    // 像 A1111 的 "Running on local URL" 这种最后一行可能一直不刷新导致识别不到。
    if (!env.contains("PYTHONUNBUFFERED")) env.insert("PYTHONUNBUFFERED", "1");
    int envCount = 0;
    const QStringList envLines = p.editEnv->toPlainText().split('\n', Qt::SkipEmptyParts);
    for (const QString &rawLine : envLines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;
        const QString key = line.left(eq).trimmed();
        const QString val = expandEnvVars(line.mid(eq + 1).trimmed(), env);
        env.insert(key, val);
        ++envCount;
    }
    p.process->setProcessEnvironment(env);

    // 重置 URL 识别状态与进度条。
    p.detectedUrl.clear();
    p.urlDetected = false;
    p.lineBuffer.clear();
    p.btnOpen->setStyleSheet(QString());
    p.btnOpen->setEnabled(!p.editUrl->text().trimmed().isEmpty());
    if (p.progressBar) {
        p.progressBar->setValue(0);
        p.progressBar->setVisible(false);
    }

    p.console->appendPlainText(QString("[启动] %1 %2").arg(program, args.join(' ')));
    p.console->appendPlainText(QString("[工作目录] %1").arg(workdir));
    if (envCount > 0) p.console->appendPlainText(QString("[自定义环境变量] %1 项").arg(envCount));

    saveSettings();
    setRunningUi(p, true);
    p.process->start(program, args);
}

void LauncherWidget::stop(TargetPanel &p)
{
    if (!p.process) return;
    p.console->appendPlainText("[停止] 正在结束进程…");
    killProcessTree(p);
}

void LauncherWidget::killProcessTree(TargetPanel &p)
{
    if (!p.process || p.process->state() == QProcess::NotRunning) return;
#ifdef Q_OS_WIN
    // cmd.exe /c xxx.bat 启动的真正服务是其子进程，kill() 只结束 cmd.exe，需杀整棵进程树。
    const qint64 pid = p.process->processId();
    if (pid > 0) {
        QProcess::startDetached("taskkill", { "/PID", QString::number(pid), "/T", "/F" });
    }
#endif
    p.process->kill();
    p.process->waitForFinished(2000);
}

void LauncherWidget::appendConsole(TargetPanel &p, const QString &chunk)
{
    if (chunk.isEmpty()) return;
    QString data = p.lineBuffer + chunk;
    data.replace("\r\n", "\n");
    p.lineBuffer.clear();

    // 以 \n 为“永久行”、\r 为“瞬时行（进度/计数，会被覆盖）”进行切分。
    QString cur;
    for (const QChar c : data) {
        if (c == '\n') { commitConsoleSegment(p, cur, true); cur.clear(); }
        else if (c == '\r') { commitConsoleSegment(p, cur, false); cur.clear(); }
        else cur += c;
    }
    p.lineBuffer = cur; // 不完整片段留待下次（含进行中的进度，下个 \r 到来时提交）
}

void LauncherWidget::commitConsoleSegment(TargetPanel &p, const QString &segment, bool permanent)
{
    const QString plain = stripAnsi(segment);
    if (isProgressLine(plain)) {
        updateProgress(p, plain);   // 进度交给独立进度条，不进控制台
        return;
    }
    if (!permanent) return;         // 瞬时非进度内容（如 FETCH 计数）直接丢弃
    p.console->appendHtml(ansiToHtml(segment));
    scanForUrl(p, plain);
}

void LauncherWidget::updateProgress(TargetPanel &p, const QString &plain)
{
    if (!p.progressBar) return;
    int pct = -1;
    QString tail;
    static const QRegularExpression re("(\\d{1,3})%\\|[^|]*\\|\\s*([^\\r\\n]*)");
    const QRegularExpressionMatch m = re.match(plain);
    if (m.hasMatch()) {
        pct = m.captured(1).toInt();
        tail = m.captured(2).trimmed();
        const int rb = tail.indexOf(']');
        if (rb >= 0) tail = tail.left(rb + 1);   // 去掉进度条后面拼接的杂项（如 FETCH ...）
    } else {
        static const QRegularExpression re2("(\\d{1,3})%");
        const QRegularExpressionMatch m2 = re2.match(plain);
        if (!m2.hasMatch()) return;
        pct = m2.captured(1).toInt();
    }
    pct = qBound(0, pct, 100);
    p.progressBar->setVisible(true);
    p.progressBar->setValue(pct);
    tail.replace('%', "%%");
    p.progressBar->setFormat(tail.isEmpty() ? "%p%" : QString("%p%  %1").arg(tail));
}

void LauncherWidget::scanForUrl(TargetPanel &p, const QString &text)
{
    if (p.urlDetected || text.isEmpty()) return;
    // 严格匹配 scheme://host:port，避免把扩展打印的畸形地址（如 http://127.0.0.1,:::8188/mtb）误判。
    static const QRegularExpression re(
        "(https?)://(127\\.0\\.0\\.1|localhost|0\\.0\\.0\\.0):(\\d{2,5})",
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) return;

    const QString scheme = m.captured(1).toLower();
    QString host = m.captured(2);
    const QString port = m.captured(3);
    if (host.compare("0.0.0.0", Qt::CaseInsensitive) == 0) host = "127.0.0.1";  // 0.0.0.0 浏览器不可达
    const QString url = scheme + "://" + host + ":" + port + "/";

    p.detectedUrl = url;
    p.urlDetected = true;
    enableOpenButton(p, url);
    p.console->appendPlainText("[已检测到界面地址: " + url + "]");
}

void LauncherWidget::enableOpenButton(TargetPanel &p, const QString &url)
{
    p.btnOpen->setEnabled(true);
    // 内联样式需自带 :hover/:pressed，否则会覆盖全局 QSS 的悬停高亮。
    p.btnOpen->setStyleSheet(
        QString("QPushButton{background-color:%1;color:#10241a;font-weight:bold;}"
                "QPushButton:hover{background-color:#74e0a0;}"
                "QPushButton:pressed{background-color:#4cba79;}")
            .arg(AppStyle::CustomTriggerGreen));
    p.btnOpen->setToolTip(url);
}

QString LauncherWidget::effectiveUrl(const TargetPanel &p) const
{
    if (!p.detectedUrl.isEmpty()) return p.detectedUrl;
    const QString manual = p.editUrl->text().trimmed();
    if (!manual.isEmpty()) return manual;
    return p.defaultUrl;
}

void LauncherWidget::openInBrowser(TargetPanel &p)
{
    const QString url = effectiveUrl(p);
    if (url.isEmpty()) {
        p.console->appendPlainText("[打开界面] 暂无可用地址。");
        return;
    }
    p.console->appendPlainText("[打开界面] " + url);
    const bool ok = QDesktopServices::openUrl(QUrl::fromUserInput(url));
    if (!ok) p.console->appendPlainText("[打开界面] 调用系统浏览器失败，请手动访问该地址。");
}

void LauncherWidget::browseScript(TargetPanel &p)
{
    const QString current = p.editScript->text().trimmed();
    const QString dir = current.isEmpty() ? QString() : QFileInfo(current).absolutePath();
    const QString file = QFileDialog::getOpenFileName(
        this, "选择启动脚本/程序", dir,
        "启动脚本/程序 (*.bat *.cmd *.py *.exe);;所有文件 (*)");
    if (file.isEmpty()) return;
    p.editScript->setText(QDir::toNativeSeparators(file));
    saveSettings();
}

void LauncherWidget::browseWorkdir(TargetPanel &p)
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, "选择工作目录", p.editWorkdir->text().trimmed());
    if (dir.isEmpty()) return;
    p.editWorkdir->setText(QDir::toNativeSeparators(dir));
    saveSettings();
}

void LauncherWidget::setRunningUi(TargetPanel &p, bool running)
{
    p.editScript->setEnabled(!running);
    p.btnBrowse->setEnabled(!running);
    p.editArgs->setEnabled(!running);
    p.editWorkdir->setEnabled(!running);
    p.btnBrowseWd->setEnabled(!running);

    if (running) {
        p.btnStartStop->setText("停止 / Stop");
        p.btnStartStop->setStyleSheet(
            QString("QPushButton{background-color:%1;color:%2;font-weight:bold;}"
                    "QPushButton:hover{background-color:#c24444;}"
                    "QPushButton:pressed{background-color:#8e2a2a;}")
                .arg(AppStyle::SyncStopRed, AppStyle::WhiteText));
    } else {
        p.btnStartStop->setText("启动 / Start");
        p.btnStartStop->setStyleSheet(QString());
        if (p.progressBar) p.progressBar->setVisible(false);
        // 进程已停止：复位“打开界面”按钮（清除绿色高亮并禁用），下次启动重新识别地址。
        p.btnOpen->setStyleSheet(QString());
        p.btnOpen->setEnabled(false);
        p.btnOpen->setToolTip(QString());
        p.urlDetected = false;
        p.detectedUrl.clear();
    }
}

QString LauncherWidget::settingsPath() const
{
    const QString configDir = qApp->applicationDirPath() + "/config";
    QDir().mkpath(configDir);
    return configDir + "/settings.json";
}

void LauncherWidget::loadSettings()
{
    QFile file(settingsPath());
    QJsonObject root;
    if (file.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
    }
    auto apply = [&root](TargetPanel &p) {
        p.editScript->setText(root.value(p.keyPrefix + "script").toString());
        p.editArgs->setText(root.value(p.keyPrefix + "args").toString());
        p.editWorkdir->setText(root.value(p.keyPrefix + "workdir").toString());
        p.editUrl->setText(root.value(p.keyPrefix + "url").toString());
        p.editEnv->setPlainText(root.value(p.keyPrefix + "env").toString());
    };
    apply(m_a1111);
    apply(m_comfy);
}

void LauncherWidget::saveSettings() const
{
    QFile file(settingsPath());
    QJsonObject root;
    if (file.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
    }
    auto store = [&root](const TargetPanel &p) {
        root[p.keyPrefix + "script"] = p.editScript->text();
        root[p.keyPrefix + "args"] = p.editArgs->text();
        root[p.keyPrefix + "workdir"] = p.editWorkdir->text();
        root[p.keyPrefix + "url"] = p.editUrl->text();
        root[p.keyPrefix + "env"] = p.editEnv->toPlainText();
    };
    store(m_a1111);
    store(m_comfy);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.close();
    }
}
