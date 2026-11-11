// EPG 引擎：XMLTV 索引、别名映射、节目查询、网络更新（等价于 Python 版的 EPG 相关函数）
#pragma once

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QMutex>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <memory>

namespace iptv {
namespace epg {

constexpr int kRetry = 2;
constexpr int kThreads = 1;
constexpr int kTimeout = 10;
constexpr int kMissTtlSec = 6 * 3600;

struct Prog {
    QDateTime start; // naive（UTC 规格，表示“EPG 时区”下的墙上时间）
    QDateTime end;
    QString title;
};

struct IndexSnapshot {
    QHash<QString, QVector<Prog>> progs;       // 频道 id（外部文件带 "N:" 前缀）-> 节目
    QHash<QString, QSet<QString>> names;       // 规范化名称 -> 频道 id 集合
    QStringList labels;                        // 所有展示名（排序）
    QHash<QString, QSet<QString>> labelSources; // 展示名 -> 来源文件 stem
    QStringList files;                         // 参与索引的 XML 文件
    QString sig;
};
using IndexPtr = std::shared_ptr<const IndexSnapshot>;

QString normName(const QString &s);

// ---- 全局设置 ----
void setChannels(const QStringList &names);
void setAlias(const QMap<QString, QString> &alias);
QMap<QString, QString> aliasCopy();
QString aliasFor(const QString &channelName);
void setTz(int hours, int days = -1);
int tzHours();

QMap<QString, QString> loadAliasFile();
bool saveAliasFile(const QMap<QString, QString> &d);
QStringList loadSourcesFile();
bool saveSourcesFile(const QStringList &lst);

// ---- 索引与查询 ----
IndexPtr getIndex(bool block = false);
QVector<Prog> loadEpg(const QString &channelName, const QDate &day,
                      const QString &url = QString());
void clearAll();

// ---- 下载参数 ----
QString makeEpgUrl(const QJsonObject &cfg, const QString &code, const QString &dateYmd);
QString csvPath(const QJsonObject &cfg);
QVector<QPair<QString, QString>> loadChannelsCsv(const QString &path); // 失败抛 std::runtime_error

// ---- 更新任务 ----
struct UpdateState {
    // 输入
    QString csv;
    int threads = 1;
    int timeout = 10;
    int tz = 8;
    QStringList sources;
    QJsonObject urlCfg;      // 用于生成下载地址（epg_host/epg_path/epg_date_fmt）
    bool silent = false;
    // 进度
    std::atomic<int> total{0}, done{0}, ok{0}, fail{0}, nodata{0}, removed{0};
    std::atomic<int> srcOk{0}, srcFail{0}, xmlCount{0};
    std::atomic<bool> finished{false}, cancel{false}, built{false};

    QString error() const;
    void setError(const QString &e);

private:
    mutable QMutex m_errLock;
    QString m_error;
};

void updateWorker(const std::shared_ptr<UpdateState> &state, const QDate &today, int n,
                  const QSet<QString> &forceDates);

} // namespace epg
} // namespace iptv
