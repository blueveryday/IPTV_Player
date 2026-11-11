// 通用工具：常量、颜色、文本解码、M3U 解析、字幕、回看地址、ffmpeg/ffprobe 封装
#pragma once

#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>
#include <optional>
#include <stdexcept>

class QProcess;

namespace iptv {

constexpr const char *kCodeVersion = "IPTV Player v2026.10.12";
constexpr const char *kAppTitle = "IPTV Player";
constexpr const char *kGithubUrl = "https://github.com/blueveryday/IPTV_Player";
constexpr int kSeekGranularity = 5;
constexpr int kMaxVolume = 130;

const QSet<QString> &mediaExts();
const QSet<QString> &audioExts();
const QSet<QString> &subExts();
const QSet<QString> &subExtsAuto();
const QSet<QString> &subTextExts();
extern const QString kWeekdayCn;

// ---- 配色（与 Python 版 COLORS 一致）----
namespace Colors {
extern const QString bg_root, bg_panel, bg_input, bg_video, fg_primary,
    fg_secondary, fg_dim, disabled_fg, accent, accent_dim, border, select_bg,
    select_fg, btn_bg, btn_active, status_bg, live_green, rec_red, epg_future;
}

// ---- 路径约定（可执行文件同目录）----
QString appDir();
QString defaultM3u();
QString configFile();
QString recordDir();
QString screenshotDir();
QString epgToolDir();
QString epgDir();
QString epgCsvDefault();
QString epgMissFile();
QString epgAliasFile();
QString epgSourcesFile();
QString epgXmlFile();
QString epgExtMapFile();

// ---- 频道条目（M3U / 本地文件 / WebDAV 共用）----
struct Channel {
    QString name;
    QString url;
    QString rawName;
    QString path;
    bool local = false;
    bool webdav = false;
    bool isDir = false;
    bool isBack = false;
    bool exitBrowse = false;
};
using ChannelPtr = std::shared_ptr<Channel>;

// ---- 无时区（naive）时间：以 UTC 规格的 QDateTime 表示“墙上时间” ----
QDateTime naiveDt(int y, int mo, int d, int h = 0, int mi = 0, int s = 0);
QDateTime naiveFromDate(const QDate &d);
QDateTime nowLocal(int tzHours);
QString strftimeNaive(const QDateTime &dt, const QString &fmt);

// ---- 文本 / 字符串 ----
QString readTextAuto(const QString &path);
QString cleanUrl(const QString &line);
QString safeFilename(const QString &name, int maxlen = 80);
QString fmtMs(qint64 ms);
QString fmtRange(const QDateTime &start, const QDateTime &end);
QString stripLeadingMark(const QString &raw); // 去掉 [直播] / 📁 之类前缀
int nearestMp3Bitrate(int kbps);
// Python str.format 的最小实现：仅支持 {name} 与 {{ }}；未知变量抛出 std::runtime_error
QString formatTemplate(const QString &tpl, const QMap<QString, QString> &vars);

// ---- 媒体 / M3U ----
bool isLocalMediaFile(const QString &url);
QString guessMediaKind(const QString &url); // "audio" / "video" / ""
QStringList findSiblingSubs(const QString &mediaPath);
QString prepareSubFile(const QString &path);
QVector<ChannelPtr> parseM3u(const QString &path);
struct M3uFile {
    QString name;
    QString path;
};
QVector<M3uFile> scanLocalM3uFiles();

// ---- 回看 ----
bool replaySupported(const QString &url, const QJsonObject &cfg);
QString buildReplayUrl(const QString &baseUrl, const QDateTime &startLocal,
                       const QDateTime &endLocal, const QJsonObject &cfg);

// ---- ffmpeg / ffprobe ----
void hideProcessWindow(QProcess *p); // Windows 下不弹出控制台窗口
QString findFfmpeg();
QString findFfprobe(const QString &ffmpegPath = QString());
int probeAudioBitrate(const QString &ffprobe, const QString &url,
                      int timeoutSec = 10); // kbps，失败返回 -1
void probeStreamCodecs(const QString &ffprobe, const QString &url,
                       QString *vCodec, QString *aCodec, int timeoutSec = 10);
QJsonObject ffprobeFullInfo(const QString &ffprobe, const QString &url,
                            int timeoutSec = 15); // 失败返回空对象
bool sniffIsHls(const QString &url, int timeoutSec = 3);

} // namespace iptv
