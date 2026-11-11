// 频道列表、M3U 加载、本地文件、WebDAV 浏览
#include "mainwindow.h"

#include "config.h"
#include "widgets.h"
#include "mpvwidget.h"
#include <QComboBox>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkReply>
#include <QPushButton>
#include <QUrl>

namespace iptv {

// ===================================================================== 列表
ChannelPtr MainWindow::selectedChannel() const {
    if (!m_chList || m_chList->selectedItems().isEmpty()) return nullptr;
    const int r = m_chList->currentRow();
    if (r < 0 || r >= m_filtered.size()) return nullptr;
    return m_filtered[r];
}

void MainWindow::selectRow(int row) {
    {
        QSignalBlocker b(m_chList);
        m_chList->clearSelection();
        m_chList->setCurrentRow(row, QItemSelectionModel::ClearAndSelect);
        m_chList->scrollToItem(m_chList->item(row));
    }
    onChannelSelect();
}

void MainWindow::applyFilter() {
    const QString kw = m_searchEdit->text().trimmed().toLower();
    const QVector<ChannelPtr> &source = m_webdavMode ? m_webdavItems : m_channels;
    m_filtered.clear();
    for (const auto &c : source)
        if (kw.isEmpty() || c->isBack || c->name.toLower().contains(kw)) m_filtered << c;

    {
        QSignalBlocker b(m_chList);
        m_chList->clear();
        for (int i = 0; i < m_filtered.size(); ++i) {
            const auto &c = m_filtered[i];
            auto *it = new QListWidgetItem(m_webdavMode ? c->name
                                                        : QString("%1  %2").arg(i + 1).arg(c->name));
            if (!m_webdavMode && channelHasEpg(c)) it->setForeground(QColor(Colors::live_green));
            m_chList->addItem(it);
        }
    }
    if (m_webdavMode)
        m_countLabel->setText(QString("%1 / %2 项").arg(m_filtered.size()).arg(source.size()));
    else
        m_countLabel->setText(QString("%1 / %2 个频道").arg(m_filtered.size()).arg(source.size()));
    refreshSlots();
}

void MainWindow::onChannelSelect() {
    const ChannelPtr ch = selectedChannel();
    if (m_webdavMode) {
        if (ch) {
            if (ch->isBack) m_replayChLabel->setText("返回");
            else if (ch->isDir) m_replayChLabel->setText("目录：" + ch->rawName);
            else m_replayChLabel->setText("文件：" + ch->rawName);
        }
        refreshSlots();
        return;
    }
    if (ch) {
        QString tip;
        if (isLocalMedia(ch)) tip = "（本地媒体文件）";
        else if (replaySupported(ch->url, m_cfg)) tip = "";
        else tip = "（不支持回看）";
        m_replayChLabel->setText(QString("%1 %2").arg(ch->name, tip));
    }
    refreshSlots();
}

// ===================================================================== M3U
void MainWindow::loadM3u(const QString &path) {
    m_currentM3uPath = path;
    m_webdavMode = false;
    m_hasWebdavSource = false;
    m_webdavPath.clear();
    m_webdavItems.clear();
    try {
        m_channels = parseM3u(path);
    } catch (const std::exception &e) {
        QMessageBox::critical(this, "错误", QString("读取 m3u 失败：%1").arg(QString::fromUtf8(e.what())));
        return;
    }
    QStringList names;
    for (const auto &c : m_channels) names << c->name;
    epg::setChannels(names);
    m_epgColorSet.clear();
    m_epgColorValid = false;
    startEpgColorLoad();

    applyFilter();
    refreshWebdavCombo();
    setStatus(QString("已加载 %1 个频道：%2").arg(m_channels.size()).arg(path));
}

void MainWindow::loadDefault() {
    const QString p = resolveDefaultM3u();
    if (QFileInfo::exists(p)) {
        loadM3u(p);
    } else {
        setStatus(QString("未找到默认 m3u：%1，请通过“文件 → 打开 m3u 文件”加载，"
                          "或在 iptv_config.json 中设置 m3u_file")
                      .arg(p));
    }
}

void MainWindow::openM3u() {
    const QString p = QFileDialog::getOpenFileName(
        this, "选择 m3u 文件", QString(), "M3U 播放列表 (*.m3u *.m3u8);;所有文件 (*.*)");
    if (!p.isEmpty()) loadM3u(p);
}

void MainWindow::openMediaFile() {
    QStringList all = mediaExts().values();
    all.sort();
    QStringList allPat;
    for (const QString &e : all) allPat << "*" + e;
    const QString filter =
        QString("媒体文件（视频/音频） (%1);;视频文件 (*.mp4 *.mkv *.avi *.mov *.wmv *.flv *.webm *.ts "
                "*.m4v *.mpg *.mpeg);;音频文件 (*.mp3 *.aac *.flac *.wav *.ape *.ogg *.wma *.m4a "
                "*.opus);;所有文件 (*.*)")
            .arg(allPat.join(' '));
    const QStringList paths = QFileDialog::getOpenFileNames(this, "选择媒体文件", QString(), filter);
    if (paths.isEmpty()) return;

    if (m_webdavMode) exitWebdav();

    bool hasNonLocal = false;
    for (const auto &c : m_channels)
        if (!c->local) hasNonLocal = true;
    const bool isAppend = !hasNonLocal;
    if (hasNonLocal) m_channels.clear();

    QVector<ChannelPtr> added;
    for (const QString &p : paths) {
        if (!QFileInfo(p).isFile()) continue;
        bool dup = false;
        for (const auto &c : m_channels)
            if (c->url == p) dup = true;
        if (dup) continue;
        auto ch = std::make_shared<Channel>();
        ch->name = QFileInfo(p).completeBaseName();
        ch->url = p;
        ch->local = true;
        m_channels << ch;
        added << ch;
    }
    if (added.isEmpty()) {
        setStatus("未添加新的媒体文件");
        return;
    }
    const ChannelPtr prev = selectedChannel();
    if (!m_searchEdit->text().isEmpty()) m_searchEdit->clear();
    else applyFilter();

    if (isAppend) {
        if (prev) {
            const int idx = m_filtered.indexOf(prev);
            if (idx >= 0) selectRow(idx);
        }
        setStatus(QString("已追加 %1 个媒体文件到列表").arg(added.size()));
        return;
    }
    const ChannelPtr first = added.first();
    const int idx = m_filtered.indexOf(first);
    if (idx >= 0) selectRow(idx);

    m_current = first;
    clearReplayRange();
    m_seekBar->setSeekEnabled(false);
    m_seekBar->setValue(0);
    m_timeLabel->setText("--:--:-- / --:--:--");
    playLocalFile(first->url, "[媒体] " + first->name);
    setStatus(QString("已添加 %1 个媒体文件，正在播放：%2").arg(added.size()).arg(first->name));
}

// ===================================================================== 快捷菜单（M3U + WebDAV）
QVector<WebDavSource> MainWindow::webdavSources() const {
    QVector<WebDavSource> out;
    for (const auto &v : m_cfg.value("webdav_sources").toArray()) {
        const QJsonObject o = v.toObject();
        out.append({o["name"].toString(), o["url"].toString(), o["user"].toString(),
                    o["password"].toString()});
    }
    return out;
}

void MainWindow::setWebdavSources(const QVector<WebDavSource> &v) {
    QJsonArray a;
    for (const auto &s : v) {
        QJsonObject o;
        o["name"] = s.name;
        o["url"] = s.url;
        o["user"] = s.user;
        o["password"] = s.password;
        a.append(o);
    }
    m_cfg["webdav_sources"] = a;
}

void MainWindow::refreshWebdavCombo() {
    if (!m_webdavCb) return;
    m_comboSources = webdavSources();
    m_comboM3u = scanLocalM3uFiles();

    QStringList names;
    for (const auto &m : m_comboM3u) names << m.name;
    for (const auto &s : m_comboSources)
        names << "🌐 " + (!s.name.isEmpty() ? s.name : (!s.url.isEmpty() ? s.url : QString("?")));

    QSignalBlocker b(m_webdavCb);
    m_webdavCb->clear();
    m_webdavCb->addItems(names);

    if (m_webdavMode && m_hasWebdavSource) {
        const QString cur = !m_webdavSource.name.isEmpty() ? m_webdavSource.name : m_webdavSource.url;
        int idx = names.indexOf("🌐 " + cur);
        if (idx < 0) idx = 0;
        if (!names.isEmpty()) m_webdavCb->setCurrentIndex(idx);
        return;
    }
    const QString curPath = m_currentM3uPath.isEmpty() ? QString()
                                                       : QFileInfo(m_currentM3uPath).absoluteFilePath();
    for (int i = 0; i < m_comboM3u.size(); ++i) {
        if (!curPath.isEmpty() && QFileInfo(m_comboM3u[i].path).absoluteFilePath() == curPath) {
            m_webdavCb->setCurrentIndex(i);
            return;
        }
    }
    m_webdavCb->setCurrentIndex(names.isEmpty() ? -1 : 0);
}

void MainWindow::onWebdavCombo(int idx) {
    if (idx < 0) return;
    const int m3uCount = m_comboM3u.size();
    if (idx < m3uCount) {
        const QString path = m_comboM3u[idx].path;
        if (!QFileInfo(path).isFile()) {
            setStatus("文件不存在：" + path);
            refreshWebdavCombo();
            return;
        }
        loadM3u(path);
        setStatus("已加载：" + path);
        return;
    }
    const int si = idx - m3uCount;
    if (si >= m_comboSources.size()) return;
    const WebDavSource src = m_comboSources[si];
    if (m_webdavMode && m_hasWebdavSource && m_webdavSource.url == src.url &&
        m_webdavSource.user == src.user)
        return;
    enterWebdav(src);
}

void MainWindow::enterWebdav(const WebDavSource &src) {
    m_webdavMode = true;
    m_hasWebdavSource = true;
    m_webdavSource = src;
    m_webdavPath.clear();
    m_webdavItems.clear();
    if (!m_searchEdit->text().isEmpty()) m_searchEdit->clear();
    refreshWebdavCombo();
    setStatus("正在连接 WebDAV：" + src.url);
    navigateWebdav("");
}

void MainWindow::exitWebdav() {
    if (!m_webdavMode) return;
    m_webdavMode = false;
    m_hasWebdavSource = false;
    m_webdavPath.clear();
    m_webdavItems.clear();
    applyFilter();
    refreshWebdavCombo();
    setStatus("已退出 WebDAV 浏览");
}

void MainWindow::navigateWebdav(const QString &path) {
    if (!m_hasWebdavSource) return;
    const int reqId = ++m_webdavReqId;
    setStatus("正在加载 WebDAV 目录：" + (path.isEmpty() ? QString("/") : path));

    auto client = std::make_shared<WebDavClient>(m_webdavSource.url, m_webdavSource.user,
                                                 m_webdavSource.password, 15);
    QByteArray body;
    QNetworkRequest req = client->buildListRequest(path, &body);
    req.setTransferTimeout(client->timeoutMs());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_nam->sendCustomRequest(req, "PROPFIND", body);
    connect(reply, &QNetworkReply::finished, this, [this, reply, client, reqId, path] {
        reply->deleteLater();
        if (m_closing) return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status >= 400) {
            QString err;
            if (status >= 400)
                err = QString("HTTP Error %1: %2")
                          .arg(status)
                          .arg(reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString());
            else
                err = reply->errorString();
            onWebdavError(reqId, path, err);
            return;
        }
        try {
            const auto entries = client->parseListResponse(reply->readAll(), path);
            onWebdavLoaded(reqId, path, entries);
        } catch (const std::exception &e) {
            onWebdavError(reqId, path, QString::fromUtf8(e.what()));
        }
    });
}

void MainWindow::onWebdavLoaded(int reqId, const QString &path, const QVector<WebDavEntry> &entries) {
    if (reqId != m_webdavReqId) return;
    m_webdavPath = path;
    QVector<ChannelPtr> items;

    auto back = std::make_shared<Channel>();
    back->webdav = true;
    back->isDir = true;
    back->isBack = true;
    if (!path.isEmpty()) {
        back->name = "⬅ 返回上级";
        back->path = path.contains('/') ? path.left(path.lastIndexOf('/')) : QString();
    } else {
        back->name = "⬅ 退出 WebDAV 浏览";
        back->exitBrowse = true;
    }
    items << back;

    QVector<WebDavEntry> dirs, files;
    for (const auto &e : entries) {
        if (e.name.isEmpty() || e.name.startsWith('.')) continue;
        if (e.isDir) {
            dirs << e;
        } else {
            const QString ext = "." + QFileInfo(e.name).suffix().toLower();
            if (mediaExts().contains(ext)) files << e;
        }
    }
    auto byName = [](const WebDavEntry &a, const WebDavEntry &b) {
        return a.name.toLower() < b.name.toLower();
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    auto joinPath = [&](const QString &n) {
        QString p = path.isEmpty() ? n : path + "/" + n;
        while (p.startsWith('/')) p.remove(0, 1);
        while (p.endsWith('/')) p.chop(1);
        return p;
    };
    for (const auto &d : dirs) {
        auto c = std::make_shared<Channel>();
        c->name = "📁 " + d.name;
        c->webdav = true;
        c->isDir = true;
        c->path = joinPath(d.name);
        c->url = d.url;
        c->rawName = d.name;
        items << c;
    }
    for (const auto &f : files) {
        auto c = std::make_shared<Channel>();
        const QString ext = "." + QFileInfo(f.name).suffix().toLower();
        c->name = (audioExts().contains(ext) ? "🎵 " : "🎬 ") + f.name;
        c->webdav = true;
        c->isDir = false;
        c->path = joinPath(f.name);
        c->url = f.url;
        c->rawName = f.name;
        items << c;
    }
    m_webdavItems = items;
    applyFilter();
    setStatus(QString("WebDAV %1：%2 个子目录，%3 个媒体文件")
                  .arg(path.isEmpty() ? QString("/") : path)
                  .arg(dirs.size())
                  .arg(files.size()));
}

void MainWindow::onWebdavError(int reqId, const QString &path, const QString &err) {
    if (reqId != m_webdavReqId) return;
    setStatus("WebDAV 加载失败：" + err);
    QMessageBox::critical(this, "WebDAV 错误",
                          QString("无法读取目录：%1\n\n%2").arg(path.isEmpty() ? QString("/") : path, err));
}

void MainWindow::playWebdavFile(const ChannelPtr &item) {
    stopRecordingForSwitch();
    if (!m_recording) {
        setRecButtonsState("normal");
    }
    QString url = item->url;
    if (url.isEmpty()) {
        setStatus("WebDAV 条目缺少 URL");
        return;
    }
    const QString user = m_webdavSource.user.trimmed();
    if (m_hasWebdavSource && !user.isEmpty()) {
        QUrl u(url);
        u.setUserName(user, QUrl::DecodedMode);
        u.setPassword(m_webdavSource.password, QUrl::DecodedMode);
        url = u.toString(QUrl::FullyEncoded);
    }
    const QString rawName = item->rawName.isEmpty() ? item->name : item->rawName;
    m_netPaused = false;
    m_current = item;
    m_currentUrl = url;
    m_currentTitle = "[WebDAV] " + rawName;
    m_currentLive = false;
    clearReplayRange();

    if (!m_video->isReady()) {
        playExternal(url);
        setStatus("正在使用外部播放器：" + item->name);
        return;
    }
    const QString kind = guessMediaKind(rawName);
    if (kind == "audio") showAudioPoster(true);
    else if (kind == "video") showAudioPoster(false);
    else scheduleAudioPosterCheck();

    submitPlay(url, true, true);
    setStatus("正在播放：" + item->name);
}

// ===================================================================== WebDAV 连接对话框
void MainWindow::openWebdavDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle("浏览 WebDAV");
    auto *g = new QGridLayout(&dlg);
    g->setContentsMargins(14, 12, 14, 12);

    const QVector<WebDavSource> sources = webdavSources();
    QStringList names;
    for (const auto &s : sources) names << (!s.name.isEmpty() ? s.name : (!s.url.isEmpty() ? s.url : "?"));

    auto lab = [&](const QString &t, bool dim, int row) {
        auto *l = new QLabel(t);
        if (dim) l->setObjectName("dim");
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        g->addWidget(l, row, 0);
    };
    lab("已保存的连接", true, 0);
    auto *saved = new QComboBox;
    saved->setMinimumWidth(360);
    saved->addItems(names);
    saved->setCurrentIndex(-1);
    g->addWidget(saved, 0, 1);
    lab("名称", false, 1);
    auto *nameE = new QLineEdit;
    g->addWidget(nameE, 1, 1);
    lab("地址", false, 2);
    auto *urlE = new QLineEdit;
    g->addWidget(urlE, 2, 1);
    lab("用户名", false, 3);
    auto *userE = new QLineEdit;
    g->addWidget(userE, 3, 1);
    lab("密码", false, 4);
    auto *passE = new QLineEdit;
    passE->setEchoMode(QLineEdit::Password);
    g->addWidget(passE, 4, 1);
    auto *saveCb = new QCheckBox("保存到配置");
    saveCb->setChecked(true);
    g->addWidget(saveCb, 5, 1);
    auto *ex = new QLabel("示例：http://192.168.1.10:5005/dav  或  https://nas.local/webdav");
    ex->setObjectName("dim");
    g->addWidget(ex, 6, 0, 1, 2);

    auto loadSaved = [&](int i) {
        if (i < 0 || i >= sources.size()) return;
        const auto &s = sources[i];
        nameE->setText(s.name);
        urlE->setText(s.url);
        userE->setText(s.user);
        passE->setText(s.password);
    };
    QObject::connect(saved, QOverload<int>::of(&QComboBox::activated), &dlg, loadSaved);
    if (!names.isEmpty()) {
        saved->setCurrentIndex(0);
        loadSaved(0);
    }

    auto *bf = new QHBoxLayout;
    auto *bOk = new QPushButton("连接");
    auto *bDel = new QPushButton("删除");
    auto *bCancel = new QPushButton("取消");
    for (auto *b : {bOk, bDel, bCancel}) {
        b->setFocusPolicy(Qt::NoFocus);
        bf->addWidget(b);
    }
    g->addLayout(bf, 7, 0, 1, 2);

    bool reopen = false;
    bool connectNow = false;
    WebDavSource target;
    QObject::connect(bOk, &QPushButton::clicked, &dlg, [&] {
        QString name = nameE->text().trimmed();
        QString url = urlE->text().trimmed();
        const QString user = userE->text().trimmed();
        const QString pwd = passE->text();
        if (url.isEmpty()) {
            QMessageBox::critical(&dlg, "错误", "请填写 WebDAV 地址");
            return;
        }
        if (!url.toLower().startsWith("http://") && !url.toLower().startsWith("https://"))
            url = "http://" + url;
        if (name.isEmpty()) name = url;
        if (saveCb->isChecked()) {
            QVector<WebDavSource> lst = webdavSources();
            const WebDavSource entry{name, url, user, pwd};
            bool replaced = false;
            for (auto &s : lst)
                if (s.name == name) {
                    s = entry;
                    replaced = true;
                    break;
                }
            if (!replaced) lst << entry;
            setWebdavSources(lst);
            saveCfg();
        }
        target = {name, url, user, pwd};
        connectNow = true;
        dlg.accept();
    });
    QObject::connect(bDel, &QPushButton::clicked, &dlg, [&] {
        const int i = saved->currentIndex();
        if (i < 0 || i >= sources.size()) return;
        const auto &s = sources[i];
        if (QMessageBox::question(&dlg, "确认",
                                  QString("删除已保存的连接“%1”？").arg(!s.name.isEmpty() ? s.name : s.url)) !=
            QMessageBox::Yes)
            return;
        QVector<WebDavSource> lst = webdavSources();
        if (i < lst.size()) lst.removeAt(i);
        setWebdavSources(lst);
        saveCfg();
        refreshWebdavCombo();
        reopen = true;
        dlg.accept();
    });
    QObject::connect(bCancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    dlg.exec();

    if (reopen) {
        QTimer::singleShot(0, this, [this] { openWebdavDialog(); });
    } else if (connectNow) {
        refreshWebdavCombo();
        enterWebdav(target);
    }
}

} // namespace iptv
