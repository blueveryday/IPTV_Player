// 菜单、界面布局、面板折叠/拖拽、全屏、键盘与右键菜单
#include "mainwindow.h"

#include "config.h"
#include "mpvwidget.h"
#include "widgets.h"

#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSlider>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace iptv {

using namespace Colors;

static void setBtnStyle(QPushButton *b, const QString &name, bool disabled = false) {
    b->setObjectName(name);
    b->style()->unpolish(b);
    b->style()->polish(b);
    b->setEnabled(!disabled);
}

static QWidget *wrapMargins(QWidget *w, int l, int t, int r, int b) {
    auto *host = new QWidget;
    auto *lay = new QHBoxLayout(host);
    lay->setContentsMargins(l, t, r, b);
    lay->setSpacing(0);
    lay->addWidget(w);
    return host;
}

static QPushButton *mkBtn(const QString &text, int width, QWidget *parent = nullptr) {
    auto *b = new QPushButton(text, parent);
    b->setFocusPolicy(Qt::NoFocus);
    if (width > 0) b->setFixedWidth(width);
    else b->setMinimumWidth(QFontMetrics(b->font()).horizontalAdvance(text) + 26);
    return b;
}

static QLabel *mkLabel(const QString &text, const QString &obj) {
    auto *l = new QLabel(text);
    l->setObjectName(obj);
    return l;
}

// ===================================================================== 菜单
void MainWindow::buildMenu() {
    m_menuBar = menuBar();
    m_menuBar->setNativeMenuBar(false);

    QMenu *fm = m_menuBar->addMenu("文件");
    fm->addAction("打开 M3U 文件...", this, [this] { openM3u(); });
    fm->addAction("打开本地文件...", this, [this] { openMediaFile(); });
    fm->addAction("浏览 WebDAV...", this, [this] { openWebdavDialog(); });
    fm->addAction("重新加载", this, [this] { loadDefault(); });
    fm->addSeparator();
    fm->addAction("回看参数设置...", this, [this] { openSettings(); });
    fm->addSeparator();
    fm->addAction("退出", this, [this] { close(); });

    QMenu *rm = m_menuBar->addMenu("录制");
    rm->addAction("开始录制", this, [this] { startRecordAuto(); });
    rm->addAction("停止录制", this, [this] { stopRecord(); });
    rm->addSeparator();
    m_actRecSwitch = rm->addAction("切换频道时停止录制");
    m_actRecSwitch->setCheckable(true);
    m_actRecSwitch->setChecked(cfgBool(m_cfg, "record_stop_on_switch", true));
    connect(m_actRecSwitch, &QAction::triggered, this, [this] { onRecSwitchOptionChanged(); });
    rm->addSeparator();
    rm->addAction("打开录制目录", this, [this] { openRecordDir(); });

    QMenu *sm = m_menuBar->addMenu("字幕");
    sm->addAction("加载外挂字幕... (F5)", this, [this] { loadSubtitleDialog(); });
    sm->addAction("关闭字幕", this, [this] { disableSubtitle(); });
    sm->addSeparator();
    m_actSubAuto = sm->addAction("播放本地文件时自动加载同名字幕");
    m_actSubAuto->setCheckable(true);
    m_actSubAuto->setChecked(cfgBool(m_cfg, "sub_auto_load", true));
    connect(m_actSubAuto, &QAction::triggered, this, [this] { onSubAutoChanged(); });
    sm->addSeparator();
    sm->addAction("字幕延迟 -0.5 秒", this, [this] { adjustSubDelay(-500); });
    sm->addAction("字幕延迟 +0.5 秒", this, [this] { adjustSubDelay(500); });
    sm->addAction("重置字幕延迟", this, [this] { adjustSubDelay(0, true); });
    sm->addSeparator();
    m_subTrackMenu = sm->addMenu("字幕轨道");
    connect(m_subTrackMenu, &QMenu::aboutToShow, this, [this] { rebuildSubTrackMenu(); });

    QMenu *em = m_menuBar->addMenu("选项");
    m_actHw = em->addAction("硬件解码");
    m_actHw->setCheckable(true);
    m_actHw->setChecked(cfgBool(m_cfg, "hw_decode", true));
    connect(m_actHw, &QAction::triggered, this, [this] { onDecodeOptionChanged(); });
    em->addSeparator();
    em->addAction("切换频道栏 (F2)", this, [this] { toggleLeft(); });
    em->addAction("切换EPG栏 (F3)", this, [this] { toggleRight(); });
    em->addAction("截图 (F4)", this, [this] { takeSnapshot(); });
    em->addSeparator();
    em->addAction("下载 EPG", this, [this] { startEpgUpdate(false); });
    em->addAction("自定义 EPG 下载参数...", this, [this] { openEpgSettings(); });
    em->addAction("设置 EPG 网络源...", this, [this] { openEpgSources(); });
    em->addSeparator();
    em->addAction("频道别名...", this, [this] { openEpgAlias(); });
    em->addSeparator();
    em->addAction("打开截图目录", this, [this] { openScreenshotDir(); });

    m_menuBar->addAction("关于", this, [this] { showAbout(); });
}

// ===================================================================== 主界面
void MainWindow::buildUi() {
    auto *central = new QWidget;
    central->setFocusPolicy(Qt::ClickFocus);
    setCentralWidget(central);
    auto *root = new QHBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    const int leftW = qBound(180, cfgInt(m_cfg, "left_width", 200), 900);
    const int rightW = qBound(180, cfgInt(m_cfg, "right_width", 300), 900);

    // ---------- 左侧：折叠按钮列 + 频道面板 ----------
    m_leftHolder = new QWidget;
    m_leftHolder->setObjectName("sidebar");
    auto *lh = new QHBoxLayout(m_leftHolder);
    lh->setContentsMargins(0, 0, 0, 0);
    lh->setSpacing(0);

    auto *lbtnCol = new QWidget;
    lbtnCol->setObjectName("sidebar");
    auto *lbc = new QVBoxLayout(lbtnCol);
    lbc->setContentsMargins(0, 0, 0, 0);
    lbc->setSpacing(0);
    lbc->addStretch(1);
    m_leftBtn = new PanelToggle;
    connect(m_leftBtn, &PanelToggle::clicked, this, [this] { toggleLeft(); });
    lbc->addWidget(m_leftBtn);
    lbc->addStretch(1);
    lh->addWidget(lbtnCol);

    m_leftPanel = new QWidget;
    m_leftPanel->setObjectName("sidebar");
    m_leftPanel->setFixedWidth(leftW);
    auto *lp = new QVBoxLayout(m_leftPanel);
    lp->setContentsMargins(0, 0, 0, 0);
    lp->setSpacing(0);

    {
        auto *wf = new QWidget;
        wf->setObjectName("sidebar");
        auto *h = new QHBoxLayout(wf);
        h->setContentsMargins(8, 8, 8, 2);
        h->setSpacing(6);
        h->addWidget(mkLabel("快捷菜单", "sidebarDim"));
        m_webdavCb = new QComboBox;
        m_webdavCb->setFocusPolicy(Qt::NoFocus);
        m_webdavCb->setMinimumWidth(1);
        m_webdavCb->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_webdavCb->setMinimumContentsLength(1);
        h->addWidget(m_webdavCb, 1);
        lp->addWidget(wf);
        connect(m_webdavCb, QOverload<int>::of(&QComboBox::activated), this,
                [this](int i) { onWebdavCombo(i); });
        m_webdavCb->installEventFilter(this);
    }
    {
        auto *sf = new QWidget;
        sf->setObjectName("sidebar");
        auto *h = new QHBoxLayout(sf);
        h->setContentsMargins(8, 4, 8, 4);
        h->setSpacing(6);
        h->addWidget(mkLabel("搜索", "sidebarDim"));
        m_searchEdit = new QLineEdit;
        h->addWidget(m_searchEdit, 1);
        lp->addWidget(sf);
        connect(m_searchEdit, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    }
    {
        auto *lf = new QWidget;
        lf->setObjectName("sidebar");
        auto *h = new QHBoxLayout(lf);
        h->setContentsMargins(8, 4, 8, 4);
        h->setSpacing(0);
        m_chList = new QListWidget;
        QFont f = m_chList->font();
        f.setFamilies({"Microsoft YaHei UI", "Noto Sans CJK SC", f.family()});
        f.setPointSize(10);
        m_chList->setFont(f);
        m_chList->setSelectionMode(QAbstractItemView::SingleSelection);
        m_chList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_chList->setMinimumWidth(1);
        h->addWidget(m_chList);
        lp->addWidget(lf, 1);
        connect(m_chList, &QListWidget::itemSelectionChanged, this, [this] { onChannelSelect(); });
        connect(m_chList, &QListWidget::itemClicked, this, [this] {
            if (m_chList->currentRow() >= 0) playLive();
        });
    }
    m_countLabel = mkLabel("0 个频道", "sidebarDim");
    {
        auto *w = wrapMargins(m_countLabel, 10, 2, 8, 8);
        w->setObjectName("sidebar");
        lp->addWidget(w);
    }
    lh->addWidget(m_leftPanel);

    m_leftSplitter = new VSplitter;
    connect(m_leftSplitter, &VSplitter::pressed, this, [this] { onLeftSplitPress(); });
    connect(m_leftSplitter, &VSplitter::dragged, this, [this](int dx) { onLeftSplitDrag(dx); });
    connect(m_leftSplitter, &VSplitter::released, this, [this] { onSplitRelease(); });

    // ---------- 中部 ----------
    m_midPanel = new QWidget;
    m_midPanel->setObjectName("sidebar");
    m_midLayout = new QVBoxLayout(m_midPanel);
    m_midLayout->setContentsMargins(0, 0, 0, 0);
    m_midLayout->setSpacing(0);

    m_videoHost = new QWidget;
    {
        auto *g = new QHBoxLayout(m_videoHost);
        g->setContentsMargins(4, 6, 4, 3);
        g->setSpacing(0);
        auto *stack = new QWidget;
        stack->setObjectName("videoStack");
        stack->setStyleSheet(QString("#videoStack{background:%1;}").arg(bg_video));
        auto *sl = new QVBoxLayout(stack);
        sl->setContentsMargins(0, 0, 0, 0);
        m_video = new MpvWidget(stack);
        sl->addWidget(m_video);
        m_audioPoster = new QLabel(m_video);
        m_audioPoster->setAlignment(Qt::AlignCenter);
        m_audioPoster->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_audioPoster->setStyleSheet(QString("background:%1; color:%2; font-size:64px;")
                                         .arg(bg_video, fg_secondary));
        m_audioPoster->hide();
        m_video->installEventFilter(this);
        g->addWidget(stack);
        connect(m_video, &MpvWidget::clicked, this, [this] { onVideoClick(); });
        connect(m_video, &MpvWidget::doubleClicked, this, [this] { onVideoDoubleClick(); });
        connect(m_video, &MpvWidget::rightClicked, this,
                [this](const QPoint &p) { showVideoContextMenu(p); });
        connect(m_video, &MpvWidget::endFile, this,
                [this](bool err, const QString &) { onPlayerEndFile(err); });
    }
    m_midLayout->addWidget(m_videoHost, 1);

    // 进度条
    {
        auto *pg = new QWidget;
        auto *h = new QHBoxLayout(pg);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        m_seekBar = new SeekBar;
        h->addWidget(m_seekBar, 1);
        m_timeLabel = new QLabel("--:--:-- / --:--:--");
        m_timeLabel->setObjectName("dim");
        QFont mf("Consolas");
        mf.setStyleHint(QFont::Monospace);
        mf.setPointSize(10);
        m_timeLabel->setFont(mf);
        m_timeLabel->setFixedWidth(QFontMetrics(mf).horizontalAdvance("00:00:00 / 00:00:00") + 12);
        h->addWidget(m_timeLabel);
        m_progressFrame = wrapMargins(pg, 6, 2, 6, 2);
        m_midLayout->addWidget(m_progressFrame);
        connect(m_seekBar, &SeekBar::seek, this,
                [this](double v, const QString &ph) { onSeekBar(v, ph); });
    }

    // 控制栏
    {
        auto *ctl = new QFrame;
        ctl->setObjectName("toolbar");
        auto *h = new QHBoxLayout(ctl);
        h->setContentsMargins(2, 2, 2, 2);
        h->setSpacing(4);
        auto add = [&](QPushButton *b, auto fn) {
            h->addWidget(b);
            connect(b, &QPushButton::clicked, this, fn);
        };
        add(mkBtn("▶ 直播", 0), [this] { playLive(); });
        add(mkBtn("⏯ 暂停", 0), [this] { togglePause(); });
        add(mkBtn("⏹ 停止", 0), [this] { stopPlayback(); });
        add(mkBtn("⛶ 全屏", 0), [this] { toggleFullscreen(); });
        m_btnRec = mkBtn("● 录制", 0);
        add(m_btnRec, [this] { startRecordAuto(); });
        m_btnRecStop = mkBtn("■ 停止录制", 0);
        add(m_btnRecStop, [this] { stopRecord(); });
        m_btnMute = mkBtn("🔊", 0);
        add(m_btnMute, [this] { toggleMute(); });
        m_volSlider = new QSlider(Qt::Horizontal);
        m_volSlider->setRange(0, kMaxVolume);
        m_volSlider->setMinimumWidth(40);
        m_volSlider->setMaximumWidth(100);
        m_volSlider->setFocusPolicy(Qt::NoFocus);
        const int vol = qBound(0, cfgInt(m_cfg, "volume", 100), kMaxVolume);
        m_volSlider->setValue(vol);
        h->addWidget(m_volSlider, 1);
        connect(m_volSlider, &QSlider::valueChanged, this, [this](int v) { onVolume(v); });
        m_volPctLabel = mkLabel(QString("%1%").arg(vol), "dim");
        m_volPctLabel->setFixedWidth(40);
        m_volPctLabel->installEventFilter(this);
        h->addSpacing(2);
        h->addWidget(m_volPctLabel);
        h->addStretch(1);
        m_ctlBar = wrapMargins(ctl, 4, 3, 4, 3);
        m_midLayout->addWidget(m_ctlBar);
    }

    // 状态栏
    {
        auto *sb = new QFrame;
        sb->setObjectName("statusBar");
        auto *h = new QHBoxLayout(sb);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(0);
        m_statusLabel = mkLabel("就绪", "status");
        m_statusLabel->setMinimumWidth(1);
        m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        h->addWidget(m_statusLabel, 1);
        m_recLabel = mkLabel("", "rec");
        m_recLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        h->addWidget(m_recLabel);
        m_statusBar = wrapMargins(sb, 4, 3, 4, 4);
        m_midLayout->addWidget(m_statusBar);
    }

    m_rightSplitter = new VSplitter;
    connect(m_rightSplitter, &VSplitter::pressed, this, [this] { onRightSplitPress(); });
    connect(m_rightSplitter, &VSplitter::dragged, this, [this](int dx) { onRightSplitDrag(dx); });
    connect(m_rightSplitter, &VSplitter::released, this, [this] { onSplitRelease(); });

    // ---------- 右侧：EPG 面板 + 折叠按钮列 ----------
    m_rightHolder = new QWidget;
    m_rightHolder->setObjectName("sidebar");
    auto *rh = new QHBoxLayout(m_rightHolder);
    rh->setContentsMargins(0, 0, 0, 0);
    rh->setSpacing(0);

    m_rightPanel = new QWidget;
    m_rightPanel->setObjectName("sidebar");
    m_rightPanel->setFixedWidth(rightW);
    auto *rp = new QVBoxLayout(m_rightPanel);
    rp->setContentsMargins(0, 0, 0, 0);
    rp->setSpacing(0);

    m_replayChLabel = mkLabel("未选择频道", "sidebarDim");
    m_replayChLabel->setWordWrap(true);
    m_replayChLabel->setMinimumWidth(1);
    m_replayChLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    {
        auto *w = wrapMargins(m_replayChLabel, 8, 8, 8, 2);
        w->setObjectName("sidebar");
        rp->addWidget(w);
    }
    m_epgLabel = mkLabel("", "sidebarDim");
    m_epgLabel->setMinimumWidth(1);
    m_epgLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    {
        auto *w = wrapMargins(m_epgLabel, 8, 0, 8, 4);
        w->setObjectName("sidebar");
        rp->addWidget(w);
    }
    {
        auto *df = new QWidget;
        df->setObjectName("sidebar");
        auto *h = new QHBoxLayout(df);
        h->setContentsMargins(8, 4, 8, 4);
        h->setSpacing(4);
        h->addWidget(mkLabel("EPG", "sidebarDim"));
        m_dateCb = new QComboBox;
        m_dateCb->setMinimumContentsLength(16);
        m_dateCb->addItems(dateChoices());
        m_dateCb->setCurrentIndex(0);
        h->addWidget(m_dateCb);
        h->addStretch(1);
        rp->addWidget(df);
        connect(m_dateCb, QOverload<int>::of(&QComboBox::activated), this,
                [this] { onDateSelected(); });
        m_dateCb->installEventFilter(this);
    }
    {
        auto *sf = new QWidget;
        sf->setObjectName("sidebar");
        auto *h = new QHBoxLayout(sf);
        h->setContentsMargins(8, 4, 8, 4);
        h->setSpacing(0);
        m_slotList = new QListWidget;
        QFont f = m_slotList->font();
        f.setFamilies({"Microsoft YaHei UI", "Noto Sans CJK SC", f.family()});
        f.setPointSize(10);
        m_slotList->setFont(f);
        m_slotList->setWordWrap(false);
        m_slotList->setTextElideMode(Qt::ElideNone);
        m_slotList->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_slotList->setMinimumWidth(1);
        h->addWidget(m_slotList);
        rp->addWidget(sf, 1);
        m_slotList->installEventFilter(this);
        m_slotList->viewport()->installEventFilter(this);
        connect(m_slotList, &QListWidget::itemSelectionChanged, this, [this] {
            if (m_slotsLocked && !m_slotList->selectedItems().isEmpty()) {
                m_slotList->clearSelection();
                setStatus("该频道不支持回看，无法选择时段");
            }
        });
        connect(m_slotList, &QListWidget::itemDoubleClicked, this, [this] { playReplay(); });
    }
    {
        auto *b1 = mkBtn("▶ 回看播放", 0);
        auto *b2 = mkBtn("复制回看地址", 0);
        auto *b3 = mkBtn("打开录制目录", 0);
        connect(b1, &QPushButton::clicked, this, [this] { playReplay(); });
        connect(b2, &QPushButton::clicked, this, [this] { copyReplayUrl(); });
        connect(b3, &QPushButton::clicked, this, [this] { openRecordDir(); });
        auto *w1 = wrapMargins(b1, 8, 4, 8, 3);
        auto *w2 = wrapMargins(b2, 8, 3, 8, 3);
        auto *w3 = wrapMargins(b3, 8, 3, 8, 8);
        for (QWidget *w : {w1, w2, w3}) {
            w->setObjectName("sidebar");
            rp->addWidget(w);
        }
    }
    rh->addWidget(m_rightPanel);

    auto *rbtnCol = new QWidget;
    rbtnCol->setObjectName("sidebar");
    auto *rbc = new QVBoxLayout(rbtnCol);
    rbc->setContentsMargins(0, 0, 0, 0);
    rbc->setSpacing(0);
    rbc->addStretch(1);
    m_rightBtn = new PanelToggle;
    connect(m_rightBtn, &PanelToggle::clicked, this, [this] { toggleRight(); });
    rbc->addWidget(m_rightBtn);
    rbc->addStretch(1);
    rh->addWidget(rbtnCol);

    root->addWidget(m_leftHolder);
    root->addWidget(m_leftSplitter);
    root->addWidget(m_midPanel, 1);
    root->addWidget(m_rightSplitter);
    root->addWidget(m_rightHolder);

    applyLeftVisible();
    applyRightVisible();
    refreshSlots();
    QTimer::singleShot(0, central, [central] { central->setFocus(); });
}

// ===================================================================== 面板显示 / 拖拽
void MainWindow::applyLeftVisible() {
    if (m_leftVisible) {
        m_leftPanel->setFixedWidth(qBound(180, cfgInt(m_cfg, "left_width", 200), 900));
        m_leftPanel->show();
    } else {
        m_leftPanel->hide();
    }
    m_leftSplitter->setVisible(!m_fullscreen && m_leftVisible);
}

void MainWindow::applyRightVisible() {
    if (m_rightVisible) {
        m_rightPanel->setFixedWidth(qBound(180, cfgInt(m_cfg, "right_width", 300), 900));
        m_rightPanel->show();
    } else {
        m_rightPanel->hide();
    }
    m_rightSplitter->setVisible(!m_fullscreen && m_rightVisible);
}

void MainWindow::toggleLeft() {
    if (m_fullscreen) return;
    m_leftVisible = !m_leftVisible;
    applyLeftVisible();
    m_cfg["left_visible"] = m_leftVisible;
}

void MainWindow::toggleRight() {
    if (m_fullscreen) return;
    m_rightVisible = !m_rightVisible;
    applyRightVisible();
    m_cfg["right_visible"] = m_rightVisible;
}

static constexpr int kCollapseThreshold = 100;
static constexpr int kPanelMin = 180;
static constexpr int kPanelMax = 360;

void MainWindow::onLeftSplitPress() {
    m_leftDragW = m_leftVisible ? cfgInt(m_cfg, "left_width", 200) : 0;
}

void MainWindow::onLeftSplitDrag(int dx) {
    m_leftDragW += dx;
    if (m_leftDragW < kCollapseThreshold) {
        m_leftDragW = qMax(0, m_leftDragW);
        if (m_leftVisible) {
            m_leftVisible = false;
            applyLeftVisible();
        }
    } else {
        const int w = qBound(kPanelMin, m_leftDragW, kPanelMax);
        m_cfg["left_width"] = w;
        if (!m_leftVisible) m_leftVisible = true;
        m_leftPanel->setFixedWidth(w);
        applyLeftVisible();
    }
}

void MainWindow::onRightSplitPress() {
    m_rightDragW = m_rightVisible ? cfgInt(m_cfg, "right_width", 300) : 0;
}

void MainWindow::onRightSplitDrag(int dx) {
    m_rightDragW -= dx;
    if (m_rightDragW < kCollapseThreshold) {
        m_rightDragW = qMax(0, m_rightDragW);
        if (m_rightVisible) {
            m_rightVisible = false;
            applyRightVisible();
        }
    } else {
        const int w = qBound(kPanelMin, m_rightDragW, kPanelMax);
        m_cfg["right_width"] = w;
        if (!m_rightVisible) m_rightVisible = true;
        m_rightPanel->setFixedWidth(w);
        applyRightVisible();
    }
}

void MainWindow::onSplitRelease() { saveCfg(); }

// ===================================================================== 全屏
void MainWindow::toggleFullscreen() {
    if (m_fullscreen) exitFullscreen();
    else enterFullscreen();
}

void MainWindow::enterFullscreen() {
    if (m_fullscreen) return;
    m_fullscreen = true;
    m_leftHolder->hide();
    m_leftSplitter->hide();
    m_rightHolder->hide();
    m_rightSplitter->hide();
    m_progressFrame->hide();
    m_ctlBar->hide();
    m_statusBar->hide();
    m_videoHost->layout()->setContentsMargins(0, 0, 0, 0);
    m_menuBar->hide();
    showFullScreen();
}

void MainWindow::exitFullscreen() {
    if (!m_fullscreen) return;
    m_fullscreen = false;
    m_menuBar->show();
    showNormal();
    m_videoHost->layout()->setContentsMargins(4, 6, 4, 3);
    m_progressFrame->show();
    m_ctlBar->show();
    m_statusBar->show();
    m_leftHolder->show();
    m_rightHolder->show();
    applyLeftVisible();
    applyRightVisible();
}

void MainWindow::onEscape() {
    if (m_fullscreen) exitFullscreen();
}

// ===================================================================== 音量 / 静音
void MainWindow::toggleMute() {
    m_muted = !m_muted;
    if (m_muted) {
        m_btnMute->setText("🔇");
        setBtnStyle(m_btnMute, "muted");
    } else {
        m_btnMute->setText("🔊");
        setBtnStyle(m_btnMute, "");
    }
    applyVolume();
    flashStatus(m_muted ? "已静音" : "已取消静音");
}

void MainWindow::onVolume(int v) {
    v = qBound(0, v, kMaxVolume);
    if (m_muted && v > 0) {
        m_muted = false;
        m_btnMute->setText("🔊");
        setBtnStyle(m_btnMute, "");
    }
    m_cfg["volume"] = v;
    m_volPctLabel->setText(QString("%1%").arg(v));
    applyVolume();
}

// ===================================================================== 视频点击
void MainWindow::onVideoClick() { m_videoClickTimer.start(220); }

void MainWindow::onVideoDoubleClick() {
    m_videoClickTimer.stop();
    toggleFullscreen();
}

// ===================================================================== 数字选台 / 方向键
void MainWindow::setRecButtonsState(const QString &mode) {
    if (mode == "active") {
        setBtnStyle(m_btnRec, "recActive");
        setBtnStyle(m_btnRecStop, "stopActive");
    } else if (mode == "disabled") {
        setBtnStyle(m_btnRec, "disabledBtn", true);
        setBtnStyle(m_btnRecStop, "disabledBtn", true);
    } else {
        setBtnStyle(m_btnRec, "");
        setBtnStyle(m_btnRecStop, "");
    }
}

void MainWindow::onArrowLeft(bool *handled) {
    *handled = false;
    if (focusIsTextInput()) return;
    if (m_seekBar->isDragging()) return;
    if (isFilePlayback()) {
        seekLocalMediaRelative(-kSeekGranularity * 1000);
        *handled = true;
        return;
    }
    if (m_currentLive) {
        liveRewindStart();
        *handled = true;
        return;
    }
    if (!m_rStart.isValid() || !m_rEnd.isValid()) return;
    seekReplayBy(-kSeekGranularity);
    *handled = true;
}

void MainWindow::onArrowRight(bool *handled) {
    *handled = false;
    if (focusIsTextInput()) return;
    if (m_seekBar->isDragging()) return;
    if (isFilePlayback()) {
        seekLocalMediaRelative(kSeekGranularity * 1000);
        *handled = true;
        return;
    }
    if (m_currentLive) {
        *handled = true;
        return;
    }
    if (!m_rStart.isValid() || !m_rEnd.isValid()) return;
    if (m_liveRewind) {
        const QDateTime now = localNow();
        const QDateTime nt = m_rAnchorTime.addSecs(kSeekGranularity);
        if (nt >= now) resumeLive();
        else liveRewindTo(nt);
        *handled = true;
        return;
    }
    seekReplayBy(kSeekGranularity);
    *handled = true;
}

void MainWindow::seekArrow(int delta) {
    if (m_seekBar->isDragging()) return;
    bool h = false;
    if (delta < 0) onArrowLeft(&h);
    else onArrowRight(&h);
}

bool MainWindow::navigateChannel(int delta) {
    if (focusIsTextInput()) return false;
    if (QApplication::focusWidget() == m_slotList) return false;
    if (m_filtered.isEmpty()) return true;

    int idx;
    const int cur = m_chList->currentRow();
    if (cur >= 0 && !m_chList->selectedItems().isEmpty()) {
        idx = cur + delta;
        if (idx < 0) idx = 0;
        else if (idx >= m_filtered.size()) idx = m_filtered.size() - 1;
        if (idx == cur) return true;
    } else {
        idx = 0;
    }
    selectRow(idx);
    if (!m_webdavMode) playLive();
    return true;
}

void MainWindow::onListReturn() {
    if (!m_numBuffer.isEmpty()) {
        m_numTimer.stop();
        commitNumberInput();
        return;
    }
    playLive();
}

void MainWindow::onDigitKey(const QString &d) {
    m_numBuffer += d;
    if (m_numBuffer.size() > 6) m_numBuffer = m_numBuffer.right(6);
    updateNumStatus();
    resetNumTimer();
}

void MainWindow::onBackspaceKey(bool *handled) {
    *handled = false;
    if (m_closing || focusIsTextInput()) return;
    if (m_numBuffer.isEmpty()) return;
    m_numBuffer.chop(1);
    updateNumStatus();
    if (!m_numBuffer.isEmpty()) {
        resetNumTimer();
    } else {
        m_numTimer.stop();
        restorePlayStatus();
    }
    *handled = true;
}

void MainWindow::updateNumStatus() {
    const double sec = kNumInputTimeoutMs / 1000.0;
    setStatus(QString("频道号：%1  （共 %2 个，%3 秒内无输入自动播放，Enter 立即播放）")
                  .arg(m_numBuffer)
                  .arg(m_filtered.size())
                  .arg(QString::number(sec, 'g')));
}

void MainWindow::resetNumTimer() { m_numTimer.start(kNumInputTimeoutMs); }

void MainWindow::commitNumberInput() {
    const QString buf = m_numBuffer;
    m_numBuffer.clear();
    if (buf.isEmpty()) return;
    bool ok = false;
    const int num = buf.toInt(&ok);
    if (!ok) return;
    playChannelByNumber(num);
}

void MainWindow::playChannelByNumber(int num) {
    if (m_webdavMode) return;
    const int total = m_filtered.size();
    if (total == 0) {
        setStatus("频道列表为空");
        return;
    }
    if (num < 1 || num > total) {
        setStatus(QString("没有编号 %1 的频道（共 %2 个）").arg(num).arg(total));
        return;
    }
    selectRow(num - 1);
    playLive();
}

// ===================================================================== 键盘
bool MainWindow::handleKey(QKeyEvent *e) {
    if (m_closing) return false;
    const int k = e->key();
    const Qt::KeyboardModifiers mods = e->modifiers();
    QWidget *fw = QApplication::focusWidget();

    switch (k) {
    case Qt::Key_F2: toggleLeft(); return true;
    case Qt::Key_F3: toggleRight(); return true;
    case Qt::Key_F4:
        if (focusIsTextInput()) return false;
        takeSnapshot();
        return true;
    case Qt::Key_F5: loadSubtitleDialog(); return true;
    case Qt::Key_Escape:
        if (m_fullscreen) {
            exitFullscreen();
            return true;
        }
        return false;
    case Qt::Key_Space:
        if (focusIsTextInput()) return false;
        if (!m_video || !m_video->isReady()) return false;
        togglePause();
        return true;
    case Qt::Key_Backspace: {
        bool h = false;
        onBackspaceKey(&h);
        return h;
    }
    case Qt::Key_Left: {
        bool h = false;
        onArrowLeft(&h);
        return h;
    }
    case Qt::Key_Right: {
        bool h = false;
        onArrowRight(&h);
        return h;
    }
    case Qt::Key_Up:
    case Qt::Key_Down: {
        const int d = (k == Qt::Key_Up) ? -1 : 1;
        if (fw == m_dateCb) {
            stepDate(d);
            return true;
        }
        return navigateChannel(d);
    }
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (fw == m_chList) {
            onListReturn();
            return true;
        }
        if (fw == m_slotList) {
            playReplay();
            return true;
        }
        return false;
    default: break;
    }

    if (k >= Qt::Key_0 && k <= Qt::Key_9 && !(mods & Qt::ControlModifier)) {
        if (focusIsTextInput() || m_webdavMode) return false;
        onDigitKey(QString(QChar('0' + (k - Qt::Key_0))));
        return true;
    }
    return false;
}

bool MainWindow::eventFilter(QObject *obj, QEvent *ev) {
    if (m_closing) return false;
    switch (ev->type()) {
    case QEvent::KeyPress: {
        if (QApplication::activePopupWidget()) return false;
        if (QApplication::activeWindow() != this) return false;
        auto *ke = static_cast<QKeyEvent *>(ev);
        // 回看列表被锁定时屏蔽按键（Tab 除外）
        if (obj == m_slotList && m_slotsLocked && ke->key() != Qt::Key_Tab &&
            ke->key() != Qt::Key_Backtab) {
            if (ke->key() == Qt::Key_Left || ke->key() == Qt::Key_Right) {
                seekArrow(ke->key() == Qt::Key_Left ? -kSeekGranularity : kSeekGranularity);
            }
            return true;
        }
        return handleKey(ke);
    }
    case QEvent::MouseButtonPress:
        if (m_slotList && obj == m_slotList->viewport() && m_slotsLocked) {
            setStatus("该频道不支持回看，无法选择时段");
            return true;
        }
        if (obj == m_webdavCb) {
            // 点击展开前刷新（对应 <Button-1> 绑定）
            refreshWebdavCombo();
        }
        break;
    case QEvent::MouseButtonDblClick:
        if (obj == m_volPctLabel) {
            m_volSlider->setValue(100);
            onVolume(100);
            return true;
        }
        break;
    case QEvent::Resize:
        if (obj == m_video && m_audioPoster && m_audioPoster->isVisible()) {
            m_audioPoster->setGeometry(m_video->rect());
        }
        break;
    default: break;
    }
    return QMainWindow::eventFilter(obj, ev);
}

// ===================================================================== 右键菜单
static void addColoredNoop(QMenu *m, const QString &text, const QString &color) {
    auto *wa = new QWidgetAction(m);
    auto *lb = new QLabel(text);
    lb->setStyleSheet(QString("QLabel{background:transparent;color:%1;padding:4px 24px 4px 22px;}")
                          .arg(color));
    wa->setDefaultWidget(lb);
    m->addAction(wa);
}

void MainWindow::showVideoContextMenu(const QPoint &globalPos) {
    ChannelPtr ch = selectedChannel();
    if (!ch) ch = m_current;
    QMenu m(this);

    if (m_webdavMode) {
        if (!ch) {
            m.addAction("（请先在左侧选择项目）")->setEnabled(false);
        } else if (ch->isDir) {
            m.addAction("📁 打开目录", this, [this] { playLive(); });
        } else {
            m.addAction(QString("▶ 播放  %1").arg(ch->name), this, [this] { playLive(); });
        }
        m.addSeparator();
        m.addAction("⏹ 停止", this, [this] { stopPlayback(); });
        m.exec(globalPos);
        return;
    }
    if (!ch) {
        m.addAction("（请先在左侧选择频道）")->setEnabled(false);
        m.exec(globalPos);
        return;
    }

    const bool isLocal = isLocalMedia(ch);
    if (isLocal) m.addAction(QString("▶ 播放  %1").arg(ch->name), this, [this] { playLive(); });
    else m.addAction(QString("▶ 直播  %1").arg(ch->name), this, [this] { playLive(); });
    m.addSeparator();
    m.addAction("开始录制", this, [this] { startRecordAuto(); });
    m.addAction("停止录制", this, [this] { stopRecord(); });
    m.addSeparator();

    if (isLocal) {
        m.addAction("本地媒体文件，无回看")->setEnabled(false);
    } else if (!replaySupported(ch->url, m_cfg)) {
        m.addAction("该频道不支持回看")->setEnabled(false);
    } else {
        QMenu *rmenu = m.addMenu("回看");
        const QDateTime now = localNow();
        const QDate earliest = earliestDate();
        for (const auto &pr : dateList()) {
            QMenu *dm = rmenu->addMenu(pr.second);
            bool fromEpg = false;
            const auto daySlots = buildDaySlots(ch, pr.first, &fromEpg);
            for (const DaySlot &s : daySlots) {
                if (s.start >= now || pr.first < earliest) {
                    addColoredNoop(dm, s.label, epg_future);
                } else {
                    const QDateTime st = s.start, en = s.end;
                    dm->addAction(s.label, this, [this, ch, st, en] { playReplayAt(ch, st, en); });
                }
            }
        }
    }
    m.addSeparator();
    m.addAction("⏹ 停止", this, [this] { stopPlayback(); });
    m.addSeparator();
    if (!m_currentUrl.isEmpty()) {
        m.addAction("📷 截图 (F4)", this, [this] { takeSnapshot(); });
        m.addAction("🔍 查看流编码信息", this, [this] { showStreamInfo(); });
    }
    m.exec(globalPos);
}

} // namespace iptv
