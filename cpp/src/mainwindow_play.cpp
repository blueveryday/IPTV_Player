// 播放控制、回看/倒退、进度条、字幕、截图
#include "mainwindow.h"

#include "config.h"
#include "mpvwidget.h"
#include "widgets.h"

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QStandardPaths>
#include <QUrl>

namespace iptv {

// ===================================================================== 播放器
void MainWindow::initPlayer() {
    QString err;
    const bool hw = cfgBool(m_cfg, "hw_decode", true);
    if (!m_video->initMpv(hw, &err)) {
        m_playerFailed = true;
        setStatus("MPV 初始化失败，改用外部播放器：" + err);
        QTimer::singleShot(500, this, [this, err] {
            const auto r = QMessageBox::question(
                this, "缺少 MPV 组件",
                "无法加载 libmpv：" + err +
                    "\n\n请安装 mpv 运行库（Linux：libmpv2；Windows：mpv-2.dll 放到程序目录）。"
                    "\n\n是否打开下载页面？");
            if (r == QMessageBox::Yes)
                QDesktopServices::openUrl(
                    QUrl("https://github.com/mpv-player/mpv/releases/tag/git-release"));
        });
        return;
    }
    connect(m_video, &MpvWidget::fileLoaded, this, [this] {
        // 本地文件的外挂字幕要在文件加载后再添加
        const QStringList subs = m_video->property("pendingSubs").toStringList();
        if (!subs.isEmpty()) {
            for (const QString &s : subs) m_video->command({"sub-add", s});
            m_video->setProperty("pendingSubs", QStringList());
        }
    });
    applyVolume();
    setStatus("MPV 播放器已就绪");
}

void MainWindow::onPlayerEndFile(bool error) {
    if (m_closing || !error) return;
    setStatus("播放失败：无法打开该地址（请检查网络、地址或回看参数）：" + m_currentUrl);
}

void MainWindow::applyVolume() {
    if (!m_video || !m_video->isReady()) return;
    const int vol = qBound(0, m_volSlider->value(), kMaxVolume);
    m_video->setPropertyDouble("volume", vol);
    m_video->setPropertyBool("mute", m_muted);
}

void MainWindow::submitPlay(const QString &url, bool, bool) {
    if (!m_video->isReady()) return;
    m_video->stop();
    m_video->setOption("hwdec", cfgBool(m_cfg, "hw_decode", true) ? "auto" : "no");
    m_video->play(url);
    applyVolume();
}

void MainWindow::submitPlayLocal(const QString &path) {
    if (!m_video->isReady()) return;
    m_video->stop();
    m_video->setOption("hwdec", cfgBool(m_cfg, "hw_decode", true) ? "auto" : "no");
    QStringList subs;
    if (cfgBool(m_cfg, "sub_auto_load", true)) {
        for (const QString &s : findSiblingSubs(path)) {
            const QString real = prepareSubFile(s);
            if (!real.isEmpty()) subs << real;
        }
    }
    m_video->setProperty("pendingSubs", subs);
    m_video->play(path);
    applyVolume();
}

void MainWindow::playLocalFile(const QString &path, const QString &title) {
    stopRecordingForSwitch();
    if (!m_recording) setRecButtonsState("disabled");
    showAudioPoster(guessMediaKind(path) == "audio");

    cancelStatusTimer();
    m_netPaused = false;
    m_currentUrl = path;
    m_currentTitle = title;
    m_currentLive = false;

    if (!m_video->isReady()) {
        playExternal(path);
        setStatus("正在使用外部播放器：" + path);
        return;
    }
    if (!QFileInfo(path).isFile()) {
        setStatus("文件不存在：" + path);
        QMessageBox::warning(this, "文件不存在", path);
        return;
    }
    submitPlayLocal(path);
    setStatus("正在播放：" + path);
}

void MainWindow::playUrl(const QString &url, const QString &title, bool live) {
    cancelStatusTimer();
    m_netPaused = false;
    if (isLocalMediaFile(url)) {
        playLocalFile(url, title);
        return;
    }
    m_currentUrl = url;
    m_currentTitle = title;
    m_currentLive = live;
    scheduleAudioPosterCheck();
    if (!m_recording) setRecButtonsState("normal");
    if (m_video->isReady()) submitPlay(url, false, false);
    else playExternal(url);
    if (!m_recording) setStatus("正在播放：" + title);
}

void MainWindow::playExternal(const QString &url) {
    killExternal();
    QString ext = cfgStr(m_cfg, "replay_player").trimmed();
    auto launch = [&](const QString &prog) {
        m_extProc = new QProcess(this);
        m_extProc->setProcessChannelMode(QProcess::ForwardedChannels);
        m_extProc->start(prog, {url});
        if (!m_extProc->waitForStarted(1500)) {
            delete m_extProc;
            m_extProc = nullptr;
            return false;
        }
        return true;
    };
    if (!ext.isEmpty() && QFileInfo(ext).isFile() && launch(ext)) return;
    for (const char *exe : {"vlc", "mpv", "ffplay"}) {
        const QString path = QStandardPaths::findExecutable(exe);
        if (!path.isEmpty() && launch(path)) return;
    }
    QMessageBox::critical(this, "无法播放",
                          "未找到可用播放器。\n请安装 libmpv 运行库，\n或将 vlc/mpv/ffplay 加入 PATH。");
}

void MainWindow::killExternal() {
    if (m_extProc) {
        if (m_extProc->state() != QProcess::NotRunning) {
            m_extProc->terminate();
            m_extProc->waitForFinished(300);
        }
        delete m_extProc;
        m_extProc = nullptr;
    }
}

// ===================================================================== 封面
void MainWindow::showAudioPoster(bool show) {
    if (!m_audioPoster) return;
    if (show) {
        if (m_posterPix.isNull()) {
            for (const char *n : {"iptv.png", "iptv.gif"}) {
                const QString cand = appDir() + "/src/asset/" + n;
                QPixmap pm(cand);
                if (pm.isNull()) continue;
                const int mx = qMax(pm.width(), pm.height());
                if (mx > 200) {
                    const int factor = qMax(1, (mx + 199) / 200);
                    pm = pm.scaled(pm.width() / factor, pm.height() / factor, Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation);
                }
                m_posterPix = pm;
                break;
            }
        }
        if (!m_posterPix.isNull()) {
            m_audioPoster->setPixmap(m_posterPix);
            m_audioPoster->setText(QString());
        } else {
            m_audioPoster->setPixmap(QPixmap());
            m_audioPoster->setText("🎵");
        }
        m_audioPoster->setGeometry(m_video->rect());
        m_audioPoster->show();
        m_audioPoster->raise();
    } else {
        m_audioPoster->hide();
    }
}

int MainWindow::videoTrackCount() const {
    if (!m_video || !m_video->isReady()) return -1;
    const auto tracks = m_video->trackList();
    if (tracks.isEmpty()) return -1; // 尚未载入
    int n = 0;
    for (const auto &t : tracks)
        if (t.type == "video") ++n;
    return n;
}

void MainWindow::scheduleAudioPosterCheck() {
    m_posterTimer.stop();
    showAudioPoster(false);
    m_posterTries = 0;
    m_posterTimer.start(600);
}

void MainWindow::checkAudioPoster() {
    if (!m_video->isReady() || m_closing) return;
    if (m_currentUrl.isEmpty()) return;
    const int v = videoTrackCount();
    if (v < 0) {
        if (++m_posterTries < 12) m_posterTimer.start(600);
        return;
    }
    showAudioPoster(v == 0);
}

// ===================================================================== 直播 / 回看
void MainWindow::playLive() {
    ChannelPtr ch = selectedChannel();
    if (!ch) {
        showInfo("请先选择频道");
        return;
    }
    stopRecordingForSwitch();

    if (ch->webdav) {
        if (ch->exitBrowse) {
            exitWebdav();
            return;
        }
        if (ch->isDir) {
            navigateWebdav(ch->path);
            return;
        }
        playWebdavFile(ch);
        return;
    }

    clearReplayRange();
    m_current = ch;
    if (isLocalMedia(ch)) {
        m_seekBar->setSeekEnabled(false);
        m_seekBar->setValue(0);
        m_timeLabel->setText("--:--:-- / --:--:--");
        playLocalFile(ch->url, "[媒体] " + ch->name);
    } else {
        playUrl(ch->url, "[直播] " + ch->name);
        refreshLiveBar();
    }
}

bool MainWindow::replayUrlForSelection(ChannelPtr *chOut, QDateTime *s, QDateTime *e,
                                       QString *url) {
    ChannelPtr ch = selectedChannel();
    if (!ch) ch = m_current;
    if (!ch) {
        QMessageBox::information(this, "提示", "请先在左侧选择频道");
        return false;
    }
    if (ch->webdav) {
        QMessageBox::information(this, "提示", "WebDAV 文件不支持回看");
        return false;
    }
    if (isLocalMedia(ch)) {
        QMessageBox::information(this, "提示", "本地媒体文件不支持回看");
        return false;
    }
    if (!replaySupported(ch->url, m_cfg)) {
        QMessageBox::information(this, "提示", "该频道不支持回看");
        return false;
    }
    QDateTime st, en;
    if (!selectedSlot(&st, &en)) {
        QMessageBox::information(this, "提示", "请选择正确的日期和回看时间段");
        return false;
    }
    *chOut = ch;
    *s = st;
    *e = en;
    *url = buildReplayUrl(ch->url, st, en, m_cfg);
    return true;
}

void MainWindow::playReplayAt(const ChannelPtr &ch, const QDateTime &start, const QDateTime &end) {
    const QDateTime now = localNow();
    if (start <= now && now < end) {
        clearReplayRange();
        m_current = ch;
        playUrl(ch->url, "[直播] " + ch->name, true);
        refreshLiveBar();
        setStatus("当前时段为直播：" + ch->name);
        return;
    }
    if (start >= now) {
        QMessageBox::warning(this, "提示", "所选时间段尚未开始，无法回看");
        return;
    }
    if (start.date() < earliestDate()) {
        QMessageBox::warning(
            this, "提示",
            QString("只能回看包含今天在内的 %1 天内的节目（最早 %2）")
                .arg(cfgInt(m_cfg, "replay_days", 7))
                .arg(earliestDate().toString("yyyy-MM-dd")));
        return;
    }
    const QString url = buildReplayUrl(ch->url, start, end, m_cfg);
    m_current = ch;
    m_currentUrl = url;
    const QString ext = cfgStr(m_cfg, "replay_player").trimmed();
    if (!ext.isEmpty()) {
        if (m_video->isReady()) m_video->stop();
        killExternal();
        m_extProc = new QProcess(this);
        m_extProc->start(ext, {url});
        if (m_extProc->waitForStarted(1500)) {
            setStatus("已用外部播放器打开回看：" + url);
        } else {
            QMessageBox::critical(this, "错误", "无法启动外部播放器：" + m_extProc->errorString());
            delete m_extProc;
            m_extProc = nullptr;
        }
        clearReplayRange();
        return;
    }
    setReplayRange(start, end);
    const QString title = QString("[回看] %1 %2 - %3")
                              .arg(ch->name, start.toString("yyyy-MM-dd HH:mm"),
                                   end.toString("HH:mm"));
    playUrl(url, title, false);
}

void MainWindow::playReplay() {
    ChannelPtr ch;
    QDateTime s, e;
    QString url;
    if (!replayUrlForSelection(&ch, &s, &e, &url)) return;
    playReplayAt(ch, s, e);
}

void MainWindow::copyReplayUrl() {
    ChannelPtr ch;
    QDateTime s, e;
    QString url;
    if (!replayUrlForSelection(&ch, &s, &e, &url)) return;
    QGuiApplication::clipboard()->setText(url);
    setStatus("回看地址已复制：" + url);
}

void MainWindow::onDecodeOptionChanged() {
    m_cfg["hw_decode"] = m_actHw->isChecked();
    saveCfg();
    if (!m_video->isReady() || m_currentUrl.isEmpty()) return;
    playUrl(m_currentUrl, m_currentTitle, m_currentLive);
    if (!m_recording)
        setStatus(QString("已切换（硬解=%1）并重新播放：%2")
                      .arg(cfgBool(m_cfg, "hw_decode", true) ? "开" : "关", m_currentTitle));
}

void MainWindow::togglePause() {
    if (!m_video->isReady()) return;
    if (isFilePlayback()) {
        m_video->command({"cycle", "pause"});
        return;
    }
    if (m_netPaused) {
        m_netPaused = false;
        const QString url = m_currentUrl, title = m_currentTitle;
        const bool live = m_currentLive;
        if (url.isEmpty()) return;
        playUrl(url, title, live);
        if (live) refreshLiveBar();
    } else {
        m_video->command({"cycle", "pause"});
        m_netPaused = true;
        flashStatus("已暂停（再按空格或点击画面继续）");
    }
}

void MainWindow::stopPlayback() {
    cancelStatusTimer();
    m_posterTimer.stop();
    m_netPaused = false;
    if (m_video->isReady()) m_video->stop();
    killExternal();
    clearReplayRange();
    m_current.reset();
    m_currentUrl.clear();
    m_timeLabel->setText("--:--:-- / --:--:--");
    showAudioPoster(false);
    if (!m_recording) setStatus("已停止");
}

// ===================================================================== 回看区间 / 倒退
void MainWindow::clearReplayRange() {
    m_rStart = m_rEnd = m_rAnchorTime = m_rAnchorWall = QDateTime();
    m_liveRewind = false;
    m_seekBar->setValue(0);
}

void MainWindow::setReplayRange(const QDateTime &s, const QDateTime &e) {
    m_rStart = s;
    m_rEnd = e;
    m_rAnchorTime = s;
    m_rAnchorWall = localNow();
    m_liveRewind = false;
    m_seekBar->setSeekEnabled(true);
    m_seekBar->setValue(0);
}

void MainWindow::refreshLiveBar() {
    m_seekBar->setSeekEnabled(true);
    m_seekBar->setValue(1000);
    const QString now = localNow().toString("HH:mm:ss");
    m_timeLabel->setText(now + " / " + now);
}

QDateTime MainWindow::currentReplayPos() const {
    if (!m_rStart.isValid() || !m_rEnd.isValid() || !m_rAnchorTime.isValid() ||
        !m_rAnchorWall.isValid())
        return QDateTime();
    const qint64 elapsed = m_rAnchorWall.secsTo(localNow());
    QDateTime cur = m_rAnchorTime.addSecs(elapsed);
    if (cur > m_rEnd) cur = m_rEnd;
    return cur;
}

void MainWindow::seekReplayBy(int deltaSec) {
    if (!m_rStart.isValid() || !m_rEnd.isValid()) return;
    QDateTime cur;
    if (m_liveRewind) {
        m_rEnd = localNow();
        cur = m_rAnchorTime;
    } else {
        cur = currentReplayPos();
        if (!cur.isValid()) cur = m_rStart;
    }
    const qint64 total = m_rStart.secsTo(m_rEnd);
    if (total <= 0) return;
    qint64 curOff = m_rStart.secsTo(cur);
    curOff = (curOff / kSeekGranularity) * kSeekGranularity;
    qint64 newOff = curOff + deltaSec;
    const qint64 maxOff = qMax<qint64>(0, total - kSeekGranularity);
    if (newOff < 0) newOff = 0;
    if (newOff > maxOff) newOff = maxOff;
    onSeekBar(newOff * 1000.0 / total, "commit");
}

void MainWindow::liveRewindTo(const QDateTime &t) {
    if (!m_current) return;
    const QDateTime now = localNow();
    if (t >= now) {
        resumeLive();
        return;
    }
    m_rAnchorTime = t;
    m_rAnchorWall = now;
    m_rEnd = now;
    const qint64 total = m_rStart.secsTo(m_rEnd);
    if (total > 0) {
        const qint64 off = m_rStart.secsTo(t);
        m_seekBar->setValue(qBound(0.0, off * 1000.0 / total, 1000.0));
    }
    const QString url = buildReplayUrl(m_current->url, t, now, m_cfg);
    const QString title = QString("[回看] %1 %2 - %3")
                              .arg(m_current->name, t.toString("yyyy-MM-dd HH:mm:ss"),
                                   now.toString("HH:mm:ss"));
    playUrl(url, title, false);
    flashStatus("已前进至 " + t.toString("HH:mm:ss") + " 回看");
}

bool MainWindow::findEpgBoundsForNow(const ChannelPtr &ch, const QDateTime &now, QDateTime *s,
                                     QDateTime *e) {
    const auto progs = epg::loadEpg(ch->name, now.date(), ch->url);
    for (const auto &p : progs) {
        if (p.start <= now && now < p.end) {
            *s = p.start;
            *e = p.end;
            return true;
        }
    }
    if (now.time().hour() < 6) {
        for (const auto &p : epg::loadEpg(ch->name, now.date().addDays(-1), ch->url)) {
            if (p.start <= now && now < p.end) {
                *s = p.start;
                *e = p.end;
                return true;
            }
        }
    }
    const epg::Prog *best = nullptr;
    for (const auto &p : progs)
        if (p.end <= now && (!best || p.end > best->end)) best = &p;
    if (best) {
        *s = best->start;
        *e = best->end;
        return true;
    }
    return false;
}

void MainWindow::liveRewindStart() {
    if (!m_current) return;
    if (!replaySupported(m_current->url, m_cfg)) {
        setStatus("该频道不支持回看，无法倒退");
        return;
    }
    const QDateTime now = localNow();
    if (!m_rStart.isValid()) {
        QDateTime bs, be;
        if (!findEpgBoundsForNow(m_current, now, &bs, &be)) {
            setStatus("没有可用的 EPG 数据，无法倒退");
            return;
        }
        m_rStart = bs;
        m_rAnchorTime = now;
        m_rAnchorWall = now;
        m_rEnd = now;
        m_seekBar->setSeekEnabled(true);
    }
    m_liveRewind = true;

    QDateTime nt = m_rAnchorTime.addSecs(-kSeekGranularity);
    if (nt < m_rStart) nt = m_rStart;
    if (nt >= m_rAnchorTime) {
        flashStatus(QString("已到达该时段最早时间（%1），无法继续倒退")
                        .arg(m_rStart.toString("HH:mm:ss")));
        return;
    }
    m_rAnchorTime = nt;
    m_rAnchorWall = now;
    m_rEnd = now;
    const qint64 total = m_rStart.secsTo(m_rEnd);
    if (total > 0) {
        const qint64 off = m_rStart.secsTo(nt);
        m_seekBar->setValue(qBound(0.0, off * 1000.0 / total, 1000.0));
    }
    const QString url = buildReplayUrl(m_current->url, nt, now, m_cfg);
    const QString title = QString("[回看] %1 %2 - %3")
                              .arg(m_current->name, nt.toString("yyyy-MM-dd HH:mm:ss"),
                                   now.toString("HH:mm:ss"));
    playUrl(url, title, false);
    flashStatus("已倒退至 " + nt.toString("HH:mm:ss") + " 回看");
}

void MainWindow::resumeLive() {
    if (!m_current) return;
    clearReplayRange();
    playUrl(m_current->url, "[直播] " + m_current->name, true);
    refreshLiveBar();
    flashStatus("已恢复直播：" + m_current->name);
}

// ===================================================================== 进度条
void MainWindow::onSeekBar(double value, const QString &phase) {
    if (isFilePlayback()) {
        seekLocalMedia(value, phase);
        return;
    }
    ChannelPtr ch = m_current;
    if (!m_rStart.isValid() || !m_rEnd.isValid()) {
        if (!ch) {
            m_seekBar->setValue(m_currentLive ? 1000 : 0);
            return;
        }
        if (!replaySupported(ch->url, m_cfg)) {
            setStatus("该频道不支持回看，无法拖动进度");
            m_seekBar->setValue(1000);
            return;
        }
        const QDateTime now = localNow();
        QDateTime bs, be;
        if (!findEpgBoundsForNow(ch, now, &bs, &be)) {
            setStatus("没有可用的 EPG 数据，无法拖动进度");
            m_seekBar->setValue(1000);
            return;
        }
        m_rStart = bs;
        m_rEnd = now;
        m_rAnchorTime = now;
        m_rAnchorWall = now;
        m_liveRewind = true;
    }

    const qint64 total = m_rStart.secsTo(m_rEnd);
    if (total <= 0) return;
    qint64 offset = qint64(total * value / 1000.0);
    offset = (offset / kSeekGranularity) * kSeekGranularity;
    if (offset >= total) offset = qMax<qint64>(0, total - kSeekGranularity);
    const QDateTime target = m_rStart.addSecs(offset);

    const bool atRight = value >= 999 || target >= m_rEnd.addSecs(-kSeekGranularity);
    if (m_liveRewind && atRight) {
        if (phase == "preview") {
            const QString es = m_rEnd.toString("HH:mm:ss");
            m_timeLabel->setText(es + " / " + es);
            return;
        }
        if (m_currentLive) {
            clearReplayRange();
            refreshLiveBar();
        } else {
            resumeLive();
        }
        return;
    }

    if (phase == "preview") {
        m_timeLabel->setText(target.toString("HH:mm:ss") + " / " + m_rEnd.toString("HH:mm:ss"));
        return;
    }
    if (!ch || !m_video->isReady()) return;
    const QString url = buildReplayUrl(ch->url, target, m_rEnd, m_cfg);
    m_rAnchorTime = target;
    m_rAnchorWall = localNow();
    const QString title = QString("[回看] %1 %2 - %3")
                              .arg(ch->name, target.toString("yyyy-MM-dd HH:mm:ss"),
                                   m_rEnd.toString("HH:mm:ss"));
    playUrl(url, title, false);
    flashStatus("已跳转到 " + target.toString("HH:mm:ss"));
}

void MainWindow::seekLocalMedia(double value, const QString &phase) {
    if (!m_video->isReady()) {
        if (phase == "commit") setStatus("当前使用外部播放器，无法在程序内拖动进度");
        return;
    }
    const double length = m_video->getDouble("duration", 0.0);
    if (length <= 0) {
        if (phase == "commit") setStatus("媒体长度尚未就绪，暂时无法拖动");
        return;
    }
    qint64 target = qint64(length * value / 1000.0 * 1000);
    target = qBound<qint64>(0, target, qint64(length * 1000));
    if (phase == "preview") {
        m_timeLabel->setText(fmtMs(target) + " / " + fmtMs(qint64(length * 1000)));
        return;
    }
    m_video->command({"seek", QString::number(target / 1000.0, 'f', 3), "absolute+exact"});
    m_timeLabel->setText(fmtMs(target) + " / " + fmtMs(qint64(length * 1000)));
    flashStatus("已跳转到 " + fmtMs(target));
}

void MainWindow::seekLocalMediaRelative(qint64 deltaMs) {
    if (!m_video->isReady()) {
        setStatus("当前使用外部播放器，无法在程序内快进/快退");
        return;
    }
    double t = m_video->getDouble("time-pos", 0.0);
    const double l = m_video->getDouble("duration", 0.0);
    if (l <= 0) {
        setStatus("媒体长度尚未就绪或不可 seek，请稍候再试");
        return;
    }
    if (t < 0) t = 0;
    const double nt = qBound(0.0, t + deltaMs / 1000.0, l);
    m_video->command({"seek", QString::number(nt, 'f', 3), "absolute+exact"});
    m_timeLabel->setText(fmtMs(qint64(nt * 1000)) + " / " + fmtMs(qint64(l * 1000)));
    flashStatus("已跳转到 " + fmtMs(qint64(nt * 1000)));
}

void MainWindow::updateLocalMediaProgress() {
    const double t = m_video->isReady() ? m_video->getDouble("time-pos", 0.0) : 0.0;
    const double l = m_video->isReady() ? m_video->getDouble("duration", 0.0) : 0.0;
    if (l > 0) {
        m_seekBar->setSeekEnabled(true);
        m_seekBar->setValue(qBound(0, int(t * 1000 / l), 1000));
        m_timeLabel->setText(fmtMs(qint64(t * 1000)) + " / " + fmtMs(qint64(l * 1000)));
    } else {
        m_seekBar->setSeekEnabled(false);
        m_seekBar->setValue(0);
        m_timeLabel->setText("--:--:-- / --:--:--");
    }
}

void MainWindow::updateProgress() {
    if (m_closing) return;
    if (m_seekBar->isDragging()) {
        // 拖动中不刷新
    } else if (isFilePlayback()) {
        updateLocalMediaProgress();
    } else if (m_currentLive && !m_rStart.isValid()) {
        if (m_current) {
            refreshLiveBar();
        } else {
            m_seekBar->setSeekEnabled(false);
            m_seekBar->setValue(0);
            m_timeLabel->setText("--:--:-- / --:--:--");
        }
    } else if (m_rStart.isValid() && m_rEnd.isValid() && m_rAnchorTime.isValid() &&
               m_rAnchorWall.isValid()) {
        const QDateTime now = localNow();
        if (m_liveRewind) m_rEnd = now;
        const QDateTime right = now;
        const qint64 offsetSec = m_rAnchorWall.secsTo(m_rAnchorTime);
        QDateTime cur = now.addSecs(offsetSec);
        if (cur < m_rStart) cur = m_rStart;
        if (cur > m_rEnd) cur = m_rEnd;
        const qint64 total = m_rStart.secsTo(m_rEnd);
        if (total > 0) {
            const qint64 off = m_rStart.secsTo(cur);
            m_seekBar->setValue(qBound(0.0, off * 1000.0 / total, 1000.0));
            m_timeLabel->setText(cur.toString("HH:mm:ss") + " / " + right.toString("HH:mm:ss"));
        }
    } else {
        m_seekBar->setSeekEnabled(false);
        m_seekBar->setValue(0);
        m_timeLabel->setText("--:--:-- / --:--:--");
    }
    m_progressTimer.start(500);
}

// ===================================================================== 字幕
void MainWindow::onSubAutoChanged() {
    m_cfg["sub_auto_load"] = m_actSubAuto->isChecked();
    saveCfg();
}

bool MainWindow::subPlayerReady() {
    if (!m_video->isReady()) {
        setStatus("当前使用外部播放器，无法加载外挂字幕");
        return false;
    }
    if (m_currentUrl.isEmpty()) {
        setStatus("请先播放视频，再加载字幕");
        return false;
    }
    return true;
}

void MainWindow::loadSubtitleDialog() {
    if (!subPlayerReady()) return;
    QStringList exts;
    QStringList sorted = subExts().values();
    sorted.sort();
    for (const QString &e : sorted) exts << "*" + e;
    const QString filter = QString("字幕文件 (%1);;所有文件 (*.*)").arg(exts.join(' '));
    QString dir;
    if (isLocalMediaFile(m_currentUrl)) dir = QFileInfo(m_currentUrl).absolutePath();
    const QString p = QFileDialog::getOpenFileName(this, "选择外挂字幕", dir, filter);
    if (!p.isEmpty()) applySubtitle(p);
}

void MainWindow::applySubtitle(const QString &path) {
    if (!subPlayerReady()) return;
    if (!QFileInfo(path).isFile()) {
        setStatus("字幕文件不存在：" + path);
        return;
    }
    const QString real = prepareSubFile(path);
    m_video->command({"sub-add", real.isEmpty() ? path : real});
    flashStatus("已加载字幕：" + QFileInfo(path).fileName());
}

void MainWindow::disableSubtitle() {
    if (!m_video->isReady()) return;
    m_video->setOption("sid", "no");
    flashStatus("已关闭字幕");
}

void MainWindow::selectSubTrack(int id) {
    if (!m_video->isReady()) return;
    if (id < 0) m_video->setOption("sid", "no");
    else m_video->setPropertyInt("sid", id);
}

void MainWindow::adjustSubDelay(int deltaMs, bool reset) {
    if (!m_video->isReady()) return;
    const double cur = m_video->getDouble("sub-delay", 0.0);
    const double nv = reset ? 0.0 : cur + deltaMs / 1000.0;
    m_video->setPropertyDouble("sub-delay", nv);
    flashStatus(QString::asprintf("字幕延迟：%+.1f 秒", nv));
}

void MainWindow::rebuildSubTrackMenu() {
    m_subTrackMenu->clear();
    QVector<MpvTrack> subs;
    if (m_video->isReady())
        for (const auto &t : m_video->trackList())
            if (t.type == "sub") subs << t;
    if (subs.isEmpty()) {
        m_subTrackMenu->addAction("（无可用字幕轨道）")->setEnabled(false);
        return;
    }
    int cur = -1;
    const QString sid = m_video->getString("sid");
    bool ok = false;
    const int v = sid.toInt(&ok);
    if (ok) cur = v;
    auto *grp = new QActionGroup(m_subTrackMenu);
    grp->setExclusive(true);
    for (const auto &t : subs) {
        QString name = !t.title.isEmpty() ? t.title : t.lang;
        if (name.isEmpty()) name = QString("轨道 %1").arg(t.id);
        QAction *a = m_subTrackMenu->addAction(name);
        a->setCheckable(true);
        a->setActionGroup(grp);
        a->setChecked(t.id == cur);
        const int id = t.id;
        connect(a, &QAction::triggered, this, [this, id] { selectSubTrack(id); });
    }
}

// ===================================================================== 截图
void MainWindow::takeSnapshot() {
    if (!m_video->isReady()) {
        setStatus("当前使用外部播放器，无法在程序内截图");
        return;
    }
    if (m_currentUrl.isEmpty()) {
        setStatus("没有正在播放的内容，无法截图");
        return;
    }
    if (videoTrackCount() == 0) {
        setStatus("当前是纯音频，无画面可截图");
        return;
    }
    const QString dir = screenshotDir();
    if (!QDir().mkpath(dir)) {
        QMessageBox::critical(this, "截图失败", QString("无法写入截图目录：\n%1").arg(dir));
        return;
    }
    {
        QFile t(dir + "/.w");
        if (!t.open(QIODevice::WriteOnly)) {
            QMessageBox::critical(this, "截图失败", QString("无法写入截图目录：\n%1\n%2")
                                                        .arg(dir, t.errorString()));
            return;
        }
        t.close();
        t.remove();
    }
    QString raw = m_currentTitle.isEmpty() ? QString("snapshot") : m_currentTitle;
    raw = stripLeadingMark(raw);
    QString base = safeFilename(raw);
    if (base.isEmpty()) base = "snapshot";
    const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString path = QString("%1/%2_%3.png").arg(dir, base, ts);
    int n = 1;
    while (QFileInfo::exists(path)) {
        path = QString("%1/%2_%3_%4.png").arg(dir, base, ts).arg(n);
        ++n;
    }
    m_video->screenshotToFile(path);
    setStatus("正在保存截图…");
    QTimer::singleShot(600, this, [this, path] { checkSnapshot(path, 0); });
}

void MainWindow::checkSnapshot(const QString &path, int tries) {
    if (m_closing) return;
    const QFileInfo fi(path);
    const bool exists = fi.isFile() && fi.size() > 0;
    if (!exists) {
        if (tries < 20) {
            QTimer::singleShot(150, this, [this, path, tries] { checkSnapshot(path, tries + 1); });
            return;
        }
        setStatus("截图未生成（画面可能尚未渲染完成，稍后重试）");
        return;
    }
    const double kb = fi.size() / 1024.0;
    bool copied = false;
    QString err;
    QImage img(path);
    if (!img.isNull()) {
        QGuiApplication::clipboard()->setImage(img);
        copied = true;
    } else {
        err = "无法读取截图";
    }
    if (copied)
        setStatus(QString("截图已保存并复制到剪贴板：%1（%2 KB）").arg(path).arg(kb, 0, 'f', 0));
    else
        setStatus(QString("截图已保存：%1（%2 KB）｜剪贴板失败：%3")
                      .arg(path)
                      .arg(kb, 0, 'f', 0)
                      .arg(err.isEmpty() ? "未知原因" : err));
}

} // namespace iptv
