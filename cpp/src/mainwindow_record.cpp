// 录制（ffmpeg 子进程 + 本地 HLS 重写代理）
#include "mainwindow.h"

#include "config.h"
#include "widgets.h"

#include <QDir>
#include <QRegularExpression>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QThread>
#include <QUrl>

namespace iptv {

QString MainWindow::recordDisplayName() const {
    QString raw;
    if (m_current) raw = !m_current->rawName.isEmpty() ? m_current->rawName : m_current->name;
    if (raw.isEmpty()) raw = m_currentTitle;
    raw = stripLeadingMark(raw);
    const QString n = safeFilename(raw);
    return n.isEmpty() ? QString("record") : n;
}

bool MainWindow::buildRecordCmd(const QString &mode, const QString &url, const QString &out,
                                QStringList *args, QString *ffmpegOut) {
    const QString ffmpeg = findFfmpeg();
    if (ffmpeg.isEmpty()) return false;
    QStringList cmd{"-hide_banner", "-loglevel", "info", "-y"};
    const QString low = url.toLower();

    if (low.startsWith("rtsp://")) {
        cmd << "-rw_timeout" << "15000000";
    } else if (low.startsWith("http://") || low.startsWith("https://")) {
        const bool isLocalProxy = low.contains("127.0.0.1") || low.contains("localhost");
        const bool isHls = isLocalProxy || low.contains(".m3u8");
        const bool isRtspGw = !isHls && (low.contains("/rtsp/") || low.contains("/rtsp?") ||
                                         low.contains("rtsp=") || low.contains("playseek="));
        cmd << "-protocol_whitelist" << "file,http,https,tcp,tls,crypto,httpproxy";
        if (isHls) {
            cmd << "-f" << "hls" << "-allowed_extensions" << "ALL";
            cmd << "-rw_timeout" << "20000000";
        } else if (isRtspGw) {
            cmd << "-user_agent" << "Mozilla/5.0 (IPTVPlayer)"
                << "-headers" << "Accept: */*\r\nConnection: keep-alive\r\n";
            cmd << "-rw_timeout" << "30000000";
        } else {
            cmd << "-rw_timeout" << "20000000";
        }
    }
    cmd << "-i" << url;

    const QString ffprobe = findFfprobe(ffmpeg);
    if (mode == "video") {
        QString vc, ac;
        probeStreamCodecs(ffprobe, url, &vc, &ac, 10);
        if (vc == "h264" || vc == "hevc" || vc == "h265" || vc == "mpeg4")
            cmd << "-c:v" << "copy";
        else
            cmd << "-c:v" << "libx264" << "-preset" << "veryfast" << "-crf" << "23" << "-pix_fmt"
                << "yuv420p";
        static const QSet<QString> copySafe{"mp3", "ac3", "eac3", "alac", "flac"};
        QStringList audio;
        if (copySafe.contains(ac)) audio << "-c:a" << "copy";
        else audio << "-c:a" << "aac" << "-b:a" << "128k" << "-ac" << "2" << "-strict" << "-2";
        // 映射必须在编码参数之前
        cmd << "-map" << "0:v:0?" << "-map" << "0:a:0?";
        cmd << audio;
        cmd << "-flush_packets" << "1" << "-f" << "mp4" << "-movflags"
            << "+frag_keyframe+empty_moov+default_base_moof" << out;
    } else {
        const int kb = nearestMp3Bitrate(probeAudioBitrate(ffprobe, url, 10));
        cmd << "-map" << "0:a:0?" << "-vn" << "-c:a" << "libmp3lame" << "-b:a"
            << QString("%1k").arg(kb) << "-f" << "mp3" << out;
    }
    *args = cmd;
    *ffmpegOut = ffmpeg;
    return true;
}

void MainWindow::stopRecordingForSwitch() {
    if (!m_recording) return;
    if (!cfgBool(m_cfg, "record_stop_on_switch", true)) return;
    stopRecordBlocking();
    if (m_recProc) m_recProc->waitForFinished(4000);
    finishRecording(true);
    setStatus("切换频道，已停止录制");
}

void MainWindow::onRecSwitchOptionChanged() {
    m_cfg["record_stop_on_switch"] = m_actRecSwitch->isChecked();
    saveCfg();
}

void MainWindow::startRecordAuto() {
    if (m_recording) {
        QMessageBox::information(this, "提示", "正在录制中，请先停止当前录制。");
        return;
    }
    if (isLocalMediaFile(m_currentUrl)) {
        QMessageBox::information(this, "提示", "本地媒体文件不支持录制。");
        return;
    }
    if (m_recProbing) {
        setStatus("正在准备录制，请稍候…");
        return;
    }
    if (m_currentUrl.isEmpty()) {
        QMessageBox::information(this, "提示", "请先播放频道或节目，再开始录制。");
        return;
    }
    const QString dir = recordDir();
    QDir().mkpath(dir);
    {
        QFile t(dir + "/.w");
        if (!t.open(QIODevice::WriteOnly)) {
            QMessageBox::critical(this, "错误",
                                  QString("录制目录不可写：%1\n%2").arg(dir, t.errorString()));
            return;
        }
        t.close();
        t.remove();
    }
    const QString kind = guessMediaKind(m_currentUrl);
    if (kind == "video" || kind == "audio") {
        startRecord(kind);
        return;
    }
    startRecord("video");
}

void MainWindow::startRecord(const QString &mode) {
    if (m_recording) {
        QMessageBox::information(this, "提示", "正在录制中，请先停止当前录制。");
        return;
    }
    if (m_recProbing) {
        setStatus("正在准备录制，请稍候…");
        return;
    }
    if (m_currentUrl.isEmpty()) {
        QMessageBox::information(this, "提示", "请先播放频道或节目，再开始录制。");
        return;
    }
    const QString url = m_currentUrl;
    if (!QDir().mkpath(recordDir())) {
        QMessageBox::critical(this, "错误", "无法创建录制目录：" + recordDir());
        return;
    }
    m_recProbing = true;
    setStatus("正在准备录制…");
    m_btnRec->setEnabled(false);

    // 阶段 1（工作线程）：嗅探是否为伪装扩展名的 HLS
    QThread *t1 = QThread::create([this, url, mode] {
        bool needProxy = false;
        const QString low = url.toLower();
        if (low.startsWith("http://") || low.startsWith("https://")) {
            bool isHls = false;
            try {
                isHls = sniffIsHls(url, 3);
            } catch (...) {
            }
            if (isHls && !(low.contains("playseek=") || low.contains("authinfo=") ||
                           low.contains("userid=")))
                needProxy = true;
        }
        QMetaObject::invokeMethod(
            this,
            [this, url, mode, needProxy] {
                if (m_closing) return;
                HlsRewriteProxy *proxy = nullptr;
                QString recUrl = url;
                if (needProxy) {
                    proxy = new HlsRewriteProxy(url, 10, 5);
                    if (proxy->startProxy()) {
                        recUrl = proxy->playlistUrl();
                    } else {
                        delete proxy;
                        proxy = nullptr;
                    }
                }
                // 输出路径
                const QString dir = recordDir();
                const QString name = recordDisplayName();
                const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
                const QString ext = mode == "video" ? "mp4" : "mp3";
                QString out = QString("%1/%2_%3.%4").arg(dir, name, ts, ext);
                int n = 1;
                while (QFileInfo::exists(out)) {
                    out = QString("%1/%2_%3_%4.%5").arg(dir, name, ts).arg(n).arg(ext);
                    ++n;
                }
                // 阶段 2（工作线程）：ffprobe 探测并生成命令
                QThread *t2 = QThread::create([this, url, mode, recUrl, proxy, out] {
                    QStringList args;
                    QString ffmpeg;
                    const bool ok = buildRecordCmd(mode, recUrl, out, &args, &ffmpeg);
                    QMetaObject::invokeMethod(
                        this,
                        [this, url, mode, recUrl, proxy, out, args, ffmpeg, ok] {
                            if (m_closing) {
                                if (proxy) {
                                    proxy->stopProxy();
                                    delete proxy;
                                }
                                return;
                            }
                            if (!ok) {
                                m_recProbing = false;
                                m_btnRec->setEnabled(true);
                                QMessageBox::critical(
                                    this, "缺少 ffmpeg",
                                    "未找到 ffmpeg，无法录制。\n\n"
                                    "请安装 ffmpeg 并加入 PATH，或把 ffmpeg 放到程序目录下，例如：\n" +
                                        appDir() + "/ffmpeg");
                                if (proxy) {
                                    proxy->stopProxy();
                                    delete proxy;
                                }
                                return;
                            }
                            doStartRecord(mode, url, recUrl, proxy, args, ffmpeg, out);
                        },
                        Qt::QueuedConnection);
                });
                connect(t2, &QThread::finished, t2, &QObject::deleteLater);
                t2->start();
            },
            Qt::QueuedConnection);
    });
    connect(t1, &QThread::finished, t1, &QObject::deleteLater);
    t1->start();
}

void MainWindow::doStartRecord(const QString &mode, const QString &origUrl, const QString &,
                               HlsRewriteProxy *proxy, const QStringList &args,
                               const QString &ffmpeg, const QString &outPath) {
    m_recProbing = false;
    m_btnRec->setEnabled(true);
    auto dropProxy = [&] {
        if (proxy) {
            proxy->stopProxy();
            delete proxy;
        }
    };
    if (m_recording) {
        QMessageBox::information(this, "提示", "正在录制中，请先停止当前录制。");
        dropProxy();
        return;
    }
    if (m_currentUrl != origUrl) {
        setStatus("流地址已改变，已取消录制");
        dropProxy();
        return;
    }
    if (!QDir().mkpath(recordDir())) {
        QMessageBox::critical(this, "错误", "无法创建录制目录：" + recordDir());
        dropProxy();
        return;
    }

    auto *proc = new QProcess(this);
    proc->setStandardOutputFile(QProcess::nullDevice());
    proc->setProcessChannelMode(QProcess::SeparateChannels);
    hideProcessWindow(proc);
    m_recStderr.clear();
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc] {
        const QByteArray data = proc->readAllStandardError();
        const QString text = QString::fromUtf8(data);
        for (QString line : text.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts)) {
            line = line.trimmed();
            if (line.isEmpty()) continue;
            m_recStderr << line;
            if (m_recStderr.size() > 300) m_recStderr = m_recStderr.mid(150);
        }
    });
    proc->start(ffmpeg, args);
    if (!proc->waitForStarted(3000)) {
        QMessageBox::critical(this, "录制失败", "无法启动 ffmpeg：" + proc->errorString());
        delete proc;
        dropProxy();
        return;
    }

    m_recProc = proc;
    m_recMode = mode;
    m_recPath = outPath;
    m_recStart = QDateTime::currentDateTime();
    m_recording = true;
    m_recProxy = proxy;
    m_recStopWait = 0;

    setRecButtonsState("active");
    const QString label = mode == "video" ? "录像" : "录音";
    setStatus(QString("正在%1：%2").arg(label, QFileInfo(outPath).fileName()));
    updateRecStatus();
    m_recCheckTimer.start(2000);
}

void MainWindow::checkRecordStarted() {
    if (!m_recording) return;
    QProcess *p = m_recProc;
    if (!p || p->state() != QProcess::NotRunning) return;
    const int rc = p->exitCode();
    const QString err = m_recStderr.mid(qMax(0, m_recStderr.size() - 30)).join('\n').trimmed();
    const QString mode = m_recMode;
    finishRecording(true);
    const QString label = mode == "video" ? "录像" : "录音";
    setStatus(label + "启动失败");
    QMessageBox::critical(this, "录制失败",
                          QString("ffmpeg 启动后立即退出（代码 %1）。\n\n最近输出：\n%2")
                              .arg(rc)
                              .arg(err.isEmpty() ? QString("（无输出）") : err.right(1200)));
}

void MainWindow::updateRecStatus() {
    if (!m_recording) {
        m_recLabel->setText("");
        return;
    }
    QProcess *p = m_recProc;
    if (p && p->state() == QProcess::NotRunning) {
        const int rc = p->exitCode();
        const QString err = m_recStderr.mid(qMax(0, m_recStderr.size() - 30)).join('\n').trimmed();
        const QString mode = m_recMode, path = m_recPath;
        finishRecording(true);
        const QString label = mode == "video" ? "录像" : "录音";
        const QFileInfo fi(path);
        const QString tail = err.isEmpty() ? QString("（无输出）") : err.right(1200);
        if (!path.isEmpty() && fi.isFile() && fi.size() > 0) {
            const double mb = fi.size() / 1048576.0;
            setStatus(QString("%1异常终止，文件已保留：%2（%3 MB）")
                          .arg(label, path)
                          .arg(mb, 0, 'f', 2));
            QMessageBox::warning(this, "录制中断",
                                 QString("ffmpeg 意外退出（代码 %1）。\n已生成文件：\n%2（%3 MB）\n\n"
                                         "最近输出：\n%4")
                                     .arg(rc)
                                     .arg(path)
                                     .arg(mb, 0, 'f', 2)
                                     .arg(tail));
        } else {
            setStatus(label + "意外中断，未生成文件");
            QMessageBox::critical(this, "录制中断",
                                  QString("ffmpeg 意外退出（代码 %1），未生成有效文件。\n\n最近输出：\n%2")
                                      .arg(rc)
                                      .arg(tail));
        }
        return;
    }
    const qint64 elapsed = m_recStart.isValid() ? m_recStart.secsTo(QDateTime::currentDateTime()) : 0;
    const qint64 h = elapsed / 3600, m = (elapsed % 3600) / 60, s = elapsed % 60;
    const QString tag = m_recMode == "video" ? "正在录像" : "正在录音";
    m_recLabel->setText(QString("● %1 %2:%3:%4")
                            .arg(tag)
                            .arg(h, 2, 10, QChar('0'))
                            .arg(m, 2, 10, QChar('0'))
                            .arg(s, 2, 10, QChar('0')));
    m_recPollTimer.start(1000);
}

void MainWindow::stopRecord() {
    if (!m_recording || !m_recProc) {
        setStatus("当前没有正在进行的录制");
        return;
    }
    m_recPollTimer.stop();
    setStatus("正在保存录制文件，请稍候…");
    m_recProc->write("q");
    m_recStopWait = 0;
    pollRecStop();
}

void MainWindow::pollRecStop() {
    if (!m_recording) return;
    QProcess *p = m_recProc;
    if (!p) {
        finishRecording();
        return;
    }
    if (p->state() != QProcess::NotRunning) {
        ++m_recStopWait;
        if (m_recStopWait == 25) p->terminate();
        else if (m_recStopWait >= 45) p->kill();
        m_recStopTimer.start(200);
        return;
    }
    finishRecording();
}

void MainWindow::stopRecordBlocking() {
    if (!m_recProc) return;
    m_recPollTimer.stop();
    m_recProc->write("q");
}

void MainWindow::finishRecording(bool silent) {
    if (!m_recording && !m_recProc) return;
    QProcess *proc = m_recProc;
    const QString path = m_recPath;
    const QString mode = m_recMode;
    HlsRewriteProxy *proxy = m_recProxy;

    m_recProc = nullptr;
    m_recording = false;
    m_recMode.clear();
    m_recPath.clear();
    m_recStart = QDateTime();
    m_recProxy = nullptr;
    m_recStopWait = 0;
    setRecButtonsState("normal");

    if (proxy) {
        proxy->stopProxy();
        delete proxy;
    }
    m_recPollTimer.stop();
    m_recCheckTimer.stop();
    m_recStopTimer.stop();
    m_recLabel->setText("");

    if (proc) {
        if (proc->state() != QProcess::NotRunning) {
            proc->kill();
            proc->waitForFinished(1000);
        }
        proc->deleteLater();
    }
    if (silent) return;

    const QString label = mode == "video" ? "录像" : "录音";
    const QFileInfo fi(path);
    if (!path.isEmpty() && fi.isFile() && fi.size() > 0) {
        setStatus(QString("%1完成：%2（%3 MB）").arg(label, path).arg(fi.size() / 1048576.0, 0, 'f', 2));
    } else {
        const QString err = m_recStderr.mid(qMax(0, m_recStderr.size() - 20)).join('\n').trimmed();
        if (!err.isEmpty()) {
            setStatus(label + "结束，未生成有效文件");
            QMessageBox::warning(this, label + "未生成文件",
                                 QString("未在录制目录生成有效文件：\n%1\n\nffmpeg 最近输出：\n%2")
                                     .arg(recordDir(), err.right(1200)));
        } else {
            setStatus(label + "结束，但未生成有效文件");
            QMessageBox::warning(this, label + "未生成文件",
                                 QString("未在录制目录生成有效文件：\n%1\n\n建议检查：\n"
                                         "• 流是否可被 ffmpeg 直接访问（有些源限制并发）\n"
                                         "• 是否使用了需要鉴权的回看地址\n")
                                     .arg(recordDir()));
        }
    }
}

void MainWindow::openRecordDir() { openDir(recordDir(), "录制目录"); }
void MainWindow::openScreenshotDir() { openDir(screenshotDir(), "截图目录"); }

} // namespace iptv
