#include "mainwindow.h"

#include "config.h"
#include "mpvwidget.h"
#include "widgets.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QSlider>
#include <QSpinBox>
#include <QTextEdit>
#include <QUrl>

namespace iptv {

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(kAppTitle);
    const QString ico = appDir() + "/src/asset/iptv.ico";
    const QString png = appDir() + "/src/asset/iptv.png";
    if (QFileInfo::exists(ico)) setWindowIcon(QIcon(ico));
    else if (QFileInfo::exists(png)) setWindowIcon(QIcon(png));

    resize(960, 590);
    setMinimumSize(960, 590);
    if (QScreen *sc = QGuiApplication::primaryScreen()) {
        const QRect g = sc->availableGeometry();
        move(g.center() - QPoint(960 / 2, 590 / 2));
    }

    m_cfg = loadConfig();
    epg::setTz(cfgInt(m_cfg, "tz_offset", 8), qMax(1, cfgInt(m_cfg, "replay_days", 7)));
    m_nam = new QNetworkAccessManager(this);

    m_rightVisible = cfgBool(m_cfg, "right_visible", false);

    for (QTimer *t : {&m_statusTimer, &m_videoClickTimer, &m_posterTimer, &m_numTimer,
                      &m_recCheckTimer, &m_recPollTimer, &m_recStopTimer, &m_epgPollTimer,
                      &m_epgAutoTimer})
        t->setSingleShot(true);
    connect(&m_statusTimer, &QTimer::timeout, this, [this] { restorePlayStatus(); });
    connect(&m_videoClickTimer, &QTimer::timeout, this, [this] {
        if (!m_closing) togglePause();
    });
    connect(&m_posterTimer, &QTimer::timeout, this, [this] { checkAudioPoster(); });
    connect(&m_numTimer, &QTimer::timeout, this, [this] { commitNumberInput(); });
    connect(&m_recCheckTimer, &QTimer::timeout, this, [this] { checkRecordStarted(); });
    connect(&m_recPollTimer, &QTimer::timeout, this, [this] { updateRecStatus(); });
    connect(&m_recStopTimer, &QTimer::timeout, this, [this] { pollRecStop(); });
    connect(&m_epgPollTimer, &QTimer::timeout, this, [this] { pollEpg(); });
    connect(&m_epgAutoTimer, &QTimer::timeout, this, [this] { autoEpgUpdate(); });
    m_progressTimer.setSingleShot(true);
    connect(&m_progressTimer, &QTimer::timeout, this, [this] { updateProgress(); });

    buildMenu();
    buildUi();
    m_currentM3uPath = resolveDefaultM3u();
    refreshWebdavCombo();

    qApp->installEventFilter(this);

    QTimer::singleShot(20, this, [this] { initPlayer(); });
    QTimer::singleShot(200, this, [this] { loadDefault(); });
    m_progressTimer.start(500);
    m_epgAutoTimer.start(qMax(1, cfgInt(m_cfg, "epg_start_delay", 30)) * 1000);
    QTimer::singleShot(400, this, [this] { startEpgColorLoad(); });
}

MainWindow::~MainWindow() {
    killExternal();
}

// ---------------------------------------------------------------- 状态栏
void MainWindow::setStatus(const QString &text) {
    m_lastStatus = text;
    if (m_statusLabel) m_statusLabel->setText(text);
    if (m_statusMute || m_closing) return;
    m_statusTimer.start(5000);
}

void MainWindow::setStatusSilent(const QString &text) {
    m_statusMute = true;
    setStatus(text);
    m_statusMute = false;
}

void MainWindow::cancelStatusTimer() { m_statusTimer.stop(); }

void MainWindow::restorePlayStatus() {
    if (m_closing) return;
    QString text;
    bool have = false;
    if (m_recording && !m_recPath.isEmpty()) {
        const QString label = m_recMode == "video" ? "录像" : "录音";
        text = QString("正在%1：%2").arg(label, QFileInfo(m_recPath).fileName());
        have = true;
    } else if (!m_currentUrl.isEmpty()) {
        if (isLocalMedia(m_current))
            text = QString("正在播放：%1").arg(m_currentUrl);
        else if (isFilePlayback())
            text = QString("正在播放：%1  %2").arg(m_currentTitle, m_currentUrl);
        else
            text = QString("正在播放：%1").arg(m_currentTitle);
        have = true;
    }
    if (!have) return;
    if (m_lastStatus == text) return;
    setStatusSilent(text);
}

// ---------------------------------------------------------------- 配置 / 杂项
void MainWindow::saveCfg() {
    QString err;
    saveConfig(m_cfg, &err);
}

QDateTime MainWindow::localNow() const { return nowLocal(cfgInt(m_cfg, "tz_offset", 8)); }

bool MainWindow::isLocalMedia(const ChannelPtr &ch) const {
    if (!ch) return false;
    if (ch->local) return true;
    return isLocalMediaFile(ch->url);
}

bool MainWindow::isFileMedia(const ChannelPtr &ch) const {
    if (!ch) return false;
    if (isLocalMedia(ch)) return true;
    if (ch->webdav && !ch->isDir) return true;
    return false;
}

bool MainWindow::focusIsTextInput() const {
    QWidget *w = QApplication::focusWidget();
    if (!w) return false;
    return qobject_cast<QLineEdit *>(w) || qobject_cast<QComboBox *>(w) ||
           qobject_cast<QAbstractSpinBox *>(w) || qobject_cast<QSlider *>(w) ||
           qobject_cast<QPlainTextEdit *>(w) || qobject_cast<QTextEdit *>(w);
}

void MainWindow::openDir(const QString &dir, const QString &what) {
    if (!QDir().mkpath(dir)) {
        QMessageBox::critical(this, "错误", QString("无法创建%1：%2").arg(what, dir));
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir)))
        QMessageBox::critical(this, "错误", QString("无法打开%1：%2").arg(what, dir));
}

QString MainWindow::resolveDefaultM3u() const {
    QString p = cfgStr(m_cfg, "m3u_file");
    if (p.isEmpty()) p = defaultM3u();
    p = p.trimmed();
    if (p.startsWith('"') && p.endsWith('"') && p.size() >= 2) p = p.mid(1, p.size() - 2);
    if (p.isEmpty()) return defaultM3u();
    if (p.startsWith("~")) p = QDir::homePath() + p.mid(1);
    if (QDir::isRelativePath(p)) p = QDir(appDir()).filePath(p);
    return QDir::cleanPath(p);
}

void MainWindow::showInfo(const QString &msg, const QString &title) {
    QMessageBox::information(this, title, msg);
}

// ---------------------------------------------------------------- 关闭
bool MainWindow::confirmClose() {
    if (!m_recording) return true;
    const auto r = QMessageBox::question(
        this, "正在录制",
        QString("当前正在录制：\n%1\n\n关闭程序会停止录制并保存已录内容。\n是否继续关闭？")
            .arg(QFileInfo(m_recPath).fileName()));
    return r == QMessageBox::Yes;
}

void MainWindow::closeEvent(QCloseEvent *e) {
    if (m_closing) {
        e->accept();
        return;
    }
    if (!confirmClose()) {
        e->ignore();
        return;
    }
    cancelStatusTimer();
    m_closing = true;

    if (m_recording) {
        stopRecordBlocking();
        if (m_recProc) m_recProc->waitForFinished(4000);
        finishRecording(true);
    }
    if (m_recProxy) {
        m_recProxy->stopProxy();
        delete m_recProxy;
        m_recProxy = nullptr;
    }
    if (m_epgState) m_epgState->cancel = true;

    for (QTimer *t : {&m_statusTimer, &m_progressTimer, &m_videoClickTimer, &m_posterTimer,
                      &m_numTimer, &m_recCheckTimer, &m_recPollTimer, &m_recStopTimer,
                      &m_epgPollTimer, &m_epgAutoTimer})
        t->stop();

    saveCfg();
    hide();
    if (m_video) {
        m_video->stop();
        m_video->shutdown();
    }
    killExternal();
    e->accept();
}

} // namespace iptv
