// 主窗口：对应 Python 版 IPTVApp。实现拆分在 mainwindow*.cpp 中。
#pragma once

#include "epg.h"
#include "hlsproxy.h"
#include "util.h"
#include "webdav.h"

#include <QJsonObject>
#include <QMainWindow>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QTimer>
#include <QVector>
#include <memory>

class QAction;
class QCheckBox;
class QComboBox;
class QFrame;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QMenuBar;
class QThread;
class QPushButton;
class QSlider;
class QVBoxLayout;
class QNetworkReply;

namespace iptv {

class MpvWidget;
class SeekBar;
class PanelToggle;
class VSplitter;

struct WebDavSource {
    QString name, url, user, password;
};

struct SlotItem {
    QDateTime start, end;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void showInfo(const QString &msg, const QString &title = QStringLiteral("提示"));

protected:
    void closeEvent(QCloseEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    // ---------------- mainwindow.cpp：状态/配置/杂项 ----------------
    void setStatus(const QString &text);            // 对应 StatusVar.set（5 秒后恢复）
    void setStatusSilent(const QString &text);      // 对应 _mute 写入
    void flashStatus(const QString &text) { setStatus(text); }
    void cancelStatusTimer();
    void restorePlayStatus();
    void saveCfg();
    QDateTime localNow() const;
    bool isLocalMedia(const ChannelPtr &ch) const;
    bool isFileMedia(const ChannelPtr &ch) const;   // _is_file_playback(ch)
    bool isFilePlayback() const { return isFileMedia(m_current); }
    bool focusIsTextInput() const;
    void openDir(const QString &dir, const QString &what);
    QString resolveDefaultM3u() const;

    // ---------------- mainwindow_ui.cpp：界面 ----------------
    void buildMenu();
    void buildUi();
    void applyLeftVisible();
    void applyRightVisible();
    void toggleLeft();
    void toggleRight();
    void onLeftSplitPress();
    void onLeftSplitDrag(int dx);
    void onRightSplitPress();
    void onRightSplitDrag(int dx);
    void onSplitRelease();
    void toggleFullscreen();
    void enterFullscreen();
    void exitFullscreen();
    void onEscape();
    void toggleMute();
    void onVolume(int v);
    void onVideoClick();
    void onVideoDoubleClick();
    void showVideoContextMenu(const QPoint &globalPos);
    bool handleKey(QKeyEvent *e);
    void onArrowLeft(bool *handled);
    void onArrowRight(bool *handled);
    void seekArrow(int delta);
    bool navigateChannel(int delta);
    void onListReturn();
    void onDigitKey(const QString &d);
    void onBackspaceKey(bool *handled);
    void updateNumStatus();
    void resetNumTimer();
    void commitNumberInput();
    void playChannelByNumber(int num);
    void setRecButtonsState(const QString &mode); // "normal" / "active" / "disabled"

    // ---------------- mainwindow_play.cpp：播放 ----------------
    void initPlayer();
    void onPlayerEndFile(bool error);
    void playUrl(const QString &url, const QString &title, bool live = true);
    void playLocalFile(const QString &path, const QString &title);
    void playExternal(const QString &url);
    void killExternal();
    void submitPlay(const QString &url, bool http, bool webdav);
    void submitPlayLocal(const QString &path);
    void applyVolume();
    void playLive();
    void playReplayAt(const ChannelPtr &ch, const QDateTime &start, const QDateTime &end);
    void playReplay();
    void copyReplayUrl();
    bool replayUrlForSelection(ChannelPtr *ch, QDateTime *s, QDateTime *e, QString *url);
    void onDecodeOptionChanged();
    void togglePause();
    void stopPlayback();
    void showAudioPoster(bool show);
    void scheduleAudioPosterCheck();
    void checkAudioPoster();
    int videoTrackCount() const;
    // 回看区间 / 进度条
    void clearReplayRange();
    void setReplayRange(const QDateTime &s, const QDateTime &e);
    void refreshLiveBar();
    QDateTime currentReplayPos() const;
    void seekReplayBy(int deltaSec);
    void liveRewindTo(const QDateTime &t);
    bool findEpgBoundsForNow(const ChannelPtr &ch, const QDateTime &now, QDateTime *s, QDateTime *e);
    void liveRewindStart();
    void resumeLive();
    void onSeekBar(double value, const QString &phase);
    void seekLocalMedia(double value, const QString &phase);
    void seekLocalMediaRelative(qint64 deltaMs);
    void updateLocalMediaProgress();
    void updateProgress();
    // 字幕 / 截图
    bool subPlayerReady();
    void loadSubtitleDialog();
    void applySubtitle(const QString &path);
    void disableSubtitle();
    void selectSubTrack(int id);
    void adjustSubDelay(int deltaMs, bool reset = false);
    void rebuildSubTrackMenu();
    void takeSnapshot();
    void checkSnapshot(const QString &path, int tries);
    void onSubAutoChanged();

    // ---------------- mainwindow_record.cpp：录制 ----------------
    QString recordDisplayName() const;
    bool buildRecordCmd(const QString &mode, const QString &url, const QString &out,
                        QStringList *args, QString *ffmpeg);
    void stopRecordingForSwitch();
    void startRecordAuto();
    void startRecord(const QString &mode);
    void doStartRecord(const QString &mode, const QString &origUrl, const QString &recUrl,
                       HlsRewriteProxy *proxy, const QStringList &args, const QString &ffmpeg,
                       const QString &outPath);
    void checkRecordStarted();
    void updateRecStatus();
    void stopRecord();
    void pollRecStop();
    void stopRecordBlocking();
    void finishRecording(bool silent = false);
    void onRecSwitchOptionChanged();
    void openRecordDir();
    void openScreenshotDir();

    // ---------------- mainwindow_epg.cpp：EPG ----------------
    QVector<QPair<QDate, QString>> dateList() const;
    QStringList dateChoices() const;
    QDate earliestDate() const;
    QDate selectedDate() const;
    struct DaySlot {
        QString label;
        QDateTime start, end;
    };
    QVector<DaySlot> buildDaySlots(const ChannelPtr &ch, const QDate &day, bool *fromEpg);
    void refreshSlots();
    void refreshSlotsKeepSelection();
    void onDateSelected();
    void stepDate(int delta);
    bool selectedSlot(QDateTime *s, QDateTime *e) const;
    int replayAllowed() const; // -1 未选 / 0 否 / 1 是
    void autoEpgUpdate();
    void startEpgUpdate(bool silent);
    void pollEpg();
    void startEpgColorLoad();
    void onEpgColorLoaded(const QSet<QString> &s);
    bool channelHasEpg(const ChannelPtr &ch) const;
    void applyEpgColorsToList();
    void openEpgAlias();
    void openEpgSources(QWidget *parent = nullptr);
    void openEpgSettings();

    // ---------------- mainwindow_webdav.cpp：列表 / WebDAV / M3U ----------------
    void loadM3u(const QString &path);
    void loadDefault();
    void openM3u();
    void openMediaFile();
    void applyFilter();
    ChannelPtr selectedChannel() const;
    void onChannelSelect();
    void selectRow(int row);
    void refreshWebdavCombo();
    void onWebdavCombo(int index);
    void enterWebdav(const WebDavSource &src);
    void exitWebdav();
    void navigateWebdav(const QString &path);
    void onWebdavLoaded(int reqId, const QString &path, const QVector<WebDavEntry> &entries);
    void onWebdavError(int reqId, const QString &path, const QString &err);
    void playWebdavFile(const ChannelPtr &item);
    void openWebdavDialog();
    QVector<WebDavSource> webdavSources() const;
    void setWebdavSources(const QVector<WebDavSource> &v);

    // ---------------- mainwindow_dialogs.cpp ----------------
    void showAbout();
    void showStreamInfo();
    void showStreamInfoDialog(const QString &url, const QJsonObject &data);
    QString formatStreamInfo(const QJsonObject &data) const;
    void openSettings();
    bool confirmClose();

    // ---------------- 数据成员 ----------------
    QJsonObject m_cfg;
    QVector<ChannelPtr> m_channels, m_filtered, m_webdavItems;
    ChannelPtr m_current;
    QString m_currentUrl, m_currentTitle;
    bool m_currentLive = true;
    QProcess *m_extProc = nullptr;
    bool m_closing = false;
    bool m_fullscreen = false;
    bool m_netPaused = false;
    bool m_muted = false;
    bool m_leftVisible = true, m_rightVisible = false;
    int m_leftDragW = 0, m_rightDragW = 0;
    bool m_playerFailed = false;

    // 回看 / 倒退
    QDateTime m_rStart, m_rEnd, m_rAnchorTime, m_rAnchorWall;
    bool m_liveRewind = false;

    // EPG 状态
    bool m_slotsLocked = false;
    QVector<SlotItem> m_slotItems;
    std::shared_ptr<epg::UpdateState> m_epgState;
    QSet<QString> m_epgColorSet;
    bool m_epgColorValid = false;
    bool m_epgColorLoading = false;

    // WebDAV
    bool m_webdavMode = false;
    bool m_hasWebdavSource = false;
    WebDavSource m_webdavSource;
    QString m_webdavPath;
    int m_webdavReqId = 0;
    QString m_currentM3uPath;
    QVector<M3uFile> m_comboM3u;
    QVector<WebDavSource> m_comboSources;
    QNetworkAccessManager *m_nam = nullptr;
    bool m_comboUpdating = false;

    // 录制
    bool m_recording = false;
    QProcess *m_recProc = nullptr;
    QString m_recMode, m_recPath;
    QDateTime m_recStart;
    HlsRewriteProxy *m_recProxy = nullptr;
    int m_recStopWait = 0;
    QStringList m_recStderr;
    bool m_recProbing = false;
    QTimer m_recPollTimer, m_recCheckTimer, m_recStopTimer;

    // 数字选台
    QString m_numBuffer;
    QTimer m_numTimer;
    static constexpr int kNumInputTimeoutMs = 1500;

    // 定时器
    QThread *m_epgThread = nullptr;
    QTimer m_statusTimer, m_progressTimer, m_videoClickTimer, m_posterTimer;
    QTimer m_epgPollTimer, m_epgAutoTimer;
    int m_posterTries = 0;

    // 控件
    MpvWidget *m_video = nullptr;
    QWidget *m_videoHost = nullptr;
    QLabel *m_audioPoster = nullptr;
    QPixmap m_posterPix;
    QWidget *m_leftHolder = nullptr, *m_rightHolder = nullptr;
    QWidget *m_leftPanel = nullptr, *m_rightPanel = nullptr;
    VSplitter *m_leftSplitter = nullptr, *m_rightSplitter = nullptr;
    PanelToggle *m_leftBtn = nullptr, *m_rightBtn = nullptr;
    QWidget *m_midPanel = nullptr, *m_progressFrame = nullptr, *m_ctlBar = nullptr,
            *m_statusBar = nullptr;
    QVBoxLayout *m_midLayout = nullptr;
    SeekBar *m_seekBar = nullptr;
    QLabel *m_timeLabel = nullptr, *m_statusLabel = nullptr, *m_recLabel = nullptr;
    QLabel *m_countLabel = nullptr, *m_replayChLabel = nullptr, *m_epgLabel = nullptr;
    QLabel *m_volPctLabel = nullptr;
    QPushButton *m_btnRec = nullptr, *m_btnRecStop = nullptr, *m_btnMute = nullptr;
    QSlider *m_volSlider = nullptr;
    QComboBox *m_webdavCb = nullptr, *m_dateCb = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QListWidget *m_chList = nullptr, *m_slotList = nullptr;
    QMenu *m_subTrackMenu = nullptr;
    QAction *m_actRecSwitch = nullptr, *m_actSubAuto = nullptr, *m_actHw = nullptr;
    QMenuBar *m_menuBar = nullptr;
    QString m_lastStatus;
    bool m_statusMute = false;
};

} // namespace iptv
