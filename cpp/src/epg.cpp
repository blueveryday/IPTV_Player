#include "epg.h"

#include "http.h"
#include "util.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringConverter>
#include <QTextCodec>
#include <QThread>
#include <QUrl>
#include <QXmlStreamReader>
#include <algorithm>
#include <tuple>
#include <zlib.h>

#include <tuple>
inline size_t qHash(const std::tuple<QString, QString, QString> &k, size_t seed = 0) {
    return qHashMulti(seed, std::get<0>(k), std::get<1>(k), std::get<2>(k));
}

namespace iptv {
namespace epg {

// ===================================================================== 全局状态
namespace {

QMutex g_stateLock;                 // 保护下面的设置类变量
QMap<QString, QString> g_alias;
QStringList g_chanNames;
QSet<QString> g_wanted;
quint32 g_wantedSig = 0;
int g_tzHours = 8;
int g_keepDays = 7;

QMutex g_cacheLock;                 // 保护 g_cache
IndexPtr g_cache;
QString g_cacheSig;
QMutex g_buildLock;                 // 串行化索引重建

void invalidateCache() {
    QMutexLocker l(&g_cacheLock);
    g_cacheSig.clear();
}

void recomputeWantedLocked() {
    QSet<QString> keys;
    for (const QString &n : g_chanNames) {
        const QString k = normName(n);
        if (!k.isEmpty()) keys.insert(k);
    }
    for (const QString &v : g_alias) {
        if (v.isEmpty()) continue;
        keys.insert(normName(v));
        if (v.contains('_')) keys.insert(normName(v.left(v.lastIndexOf('_'))));
    }
    g_wanted = keys;
    QStringList sorted(keys.begin(), keys.end());
    std::sort(sorted.begin(), sorted.end());
    g_wantedSig = qHash(sorted.join('\n'), 0x9e3779b9);
}

} // namespace

QString normName(const QString &s) {
    static const QRegularExpression re(R"([\\/:*?"<>|_\s\-]+)");
    QString r = s;
    r.replace(re, "");
    return r.toLower();
}

void setChannels(const QStringList &names) {
    {
        QMutexLocker l(&g_stateLock);
        g_chanNames = names;
        recomputeWantedLocked();
    }
    invalidateCache();
}

void setAlias(const QMap<QString, QString> &alias) {
    {
        QMutexLocker l(&g_stateLock);
        g_alias = alias;
        recomputeWantedLocked();
    }
    invalidateCache();
}

QMap<QString, QString> aliasCopy() {
    QMutexLocker l(&g_stateLock);
    return g_alias;
}

QString aliasFor(const QString &channelName) {
    QMutexLocker l(&g_stateLock);
    return g_alias.value(channelName);
}

void setTz(int hours, int days) {
    {
        QMutexLocker l(&g_stateLock);
        g_tzHours = hours;
        if (days >= 0) g_keepDays = qMax(1, days);
    }
    invalidateCache();
}

int tzHours() {
    QMutexLocker l(&g_stateLock);
    return g_tzHours;
}

// ===================================================================== 小文件读写
static QJsonDocument readJsonFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll());
}

static bool writeJsonFile(const QString &path, const QJsonDocument &doc,
                          QJsonDocument::JsonFormat fmt = QJsonDocument::Indented) {
    QDir().mkpath(epgDir());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(doc.toJson(fmt));
    return f.commit();
}

QMap<QString, QString> loadAliasFile() {
    QMap<QString, QString> m;
    const QJsonDocument d = readJsonFile(epgAliasFile());
    if (!d.isObject()) return m;
    const QJsonObject o = d.object();
    for (auto it = o.begin(); it != o.end(); ++it)
        m.insert(it.key(), it.value().toString());
    return m;
}

bool saveAliasFile(const QMap<QString, QString> &d) {
    QJsonObject o;
    for (auto it = d.begin(); it != d.end(); ++it) o[it.key()] = it.value();
    return writeJsonFile(epgAliasFile(), QJsonDocument(o));
}

QStringList loadSourcesFile() {
    QStringList l;
    const QJsonDocument d = readJsonFile(epgSourcesFile());
    if (!d.isArray()) return l;
    for (const QJsonValue &v : d.array())
        if (v.isString()) l << v.toString();
    return l;
}

bool saveSourcesFile(const QStringList &lst) {
    QJsonArray a;
    for (const QString &s : lst) a.append(s);
    return writeJsonFile(epgSourcesFile(), QJsonDocument(a));
}

// ===================================================================== XMLTV 解析
static QDateTime parseXmltvTime(const QString &sIn, int localOff) {
    static const QRegularExpression re(
        R"(^(\d{8})(\d{0,6})\s*(?:([+-])(\d{2}):?(\d{2}))?)");
    const auto m = re.match(sIn.trimmed());
    if (!m.hasMatch()) return {};
    QString digits = m.captured(1) + m.captured(2).leftJustified(6, '0');
    const int y = digits.mid(0, 4).toInt(), mo = digits.mid(4, 2).toInt(),
              d = digits.mid(6, 2).toInt(), h = digits.mid(8, 2).toInt(),
              mi = digits.mid(10, 2).toInt(), s = digits.mid(12, 2).toInt();
    const QDate date(y, mo, d);
    const QTime time(h, mi, s);
    if (!date.isValid() || !time.isValid()) return {};
    QDateTime dt(date, time, Qt::UTC);
    if (!m.captured(3).isEmpty()) {
        int off = m.captured(4).toInt() * 60 + m.captured(5).toInt();
        if (m.captured(3) == "-") off = -off;
        dt = dt.addSecs(-qint64(off) * 60 + qint64(localOff) * 3600);
    }
    return dt;
}

static bool gunzip(const QByteArray &in, QByteArray *out) {
    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return false;
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    zs.avail_in = uInt(in.size());
    char buf[1 << 16];
    int ret;
    do {
        zs.next_out = reinterpret_cast<Bytef *>(buf);
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zs);
            return false;
        }
        out->append(buf, int(sizeof(buf) - zs.avail_out));
    } while (ret != Z_STREAM_END && (zs.avail_in > 0 || zs.avail_out == 0));
    inflateEnd(&zs);
    return ret == Z_STREAM_END;
}

static QByteArray readMaybeGz(const QString &path, bool *ok = nullptr) {
    QFile f(path);
    if (ok) *ok = false;
    if (!f.open(QIODevice::ReadOnly)) return {};
    QByteArray raw = f.readAll();
    f.close();
    if (raw.size() >= 2 && uchar(raw[0]) == 0x1f && uchar(raw[1]) == 0x8b) {
        QByteArray out;
        if (!gunzip(raw, &out)) return {};
        raw = out;
    }
    if (ok) *ok = true;
    return raw;
}

struct FileParse {
    QHash<QString, QVector<Prog>> progs;
    QHash<QString, QSet<QString>> names;
    QSet<QString> labels;
};

static QStringList extFiles() {
    QStringList res;
    QDir d(epgDir());
    QStringList names = d.entryList(QStringList() << "*.xml" << "*.XML",
                                    QDir::Files | QDir::NoDotAndDotDot);
    names.removeDuplicates();
    std::sort(names.begin(), names.end());
    for (const QString &n : names) {
        if (n.toLower() == "iptv_epg.xml") continue;
        res << d.filePath(n);
    }
    return res;
}

// 读取 <title> 等子元素文本；调用时 xr 位于 programme/channel 的 StartElement
static FileParse parseXmlFile(const QString &path, const QString &prefix,
                              const QSet<QString> &wanted, const QDateTime &lo,
                              const QDateTime &hi, int tz) {
    FileParse fp;
    QSet<QString> wantIds;
    bool ok = false;
    const QByteArray data = readMaybeGz(path, &ok);
    if (!ok) return fp;
    QBuffer buf;
    buf.setData(data);
    buf.open(QIODevice::ReadOnly);
    QXmlStreamReader xr(&buf);

    while (!xr.atEnd()) {
        const auto tok = xr.readNext();
        if (tok != QXmlStreamReader::StartElement) continue;
        const QStringView tag = xr.name();
        if (tag == QLatin1String("channel")) {
            const QString cid = xr.attributes().value("id").toString().trimmed();
            QStringList dns;
            // 读到 </channel>
            int depth = 1;
            while (!xr.atEnd() && depth > 0) {
                const auto t2 = xr.readNext();
                if (t2 == QXmlStreamReader::StartElement) {
                    if (xr.name() == QLatin1String("display-name")) {
                        const QString txt = xr.readElementText().trimmed();
                        if (!txt.isEmpty()) dns << txt;
                    } else {
                        ++depth;
                    }
                } else if (t2 == QXmlStreamReader::EndElement) {
                    --depth;
                }
            }
            if (cid.isEmpty()) continue;
            QSet<QString> keys{normName(cid)};
            for (const QString &n : dns) keys.insert(normName(n));
            for (const QString &k : keys)
                if (!k.isEmpty()) fp.names[k].insert(prefix + cid);
            if (dns.isEmpty()) fp.labels.insert(cid);
            for (const QString &n : dns) fp.labels.insert(n);
            bool want = wanted.isEmpty();
            if (!want)
                for (const QString &k : keys)
                    if (wanted.contains(k)) { want = true; break; }
            if (want) wantIds.insert(cid);
        } else if (tag == QLatin1String("programme")) {
            const QString cid = xr.attributes().value("channel").toString().trimmed();
            const QString startS = xr.attributes().value("start").toString();
            const QString stopS = xr.attributes().value("stop").toString();
            QString title;
            bool gotTitle = false;
            int depth = 1;
            while (!xr.atEnd() && depth > 0) {
                const auto t2 = xr.readNext();
                if (t2 == QXmlStreamReader::StartElement) {
                    if (!gotTitle && xr.name() == QLatin1String("title")) {
                        title = xr.readElementText().trimmed();
                        gotTitle = true;
                    } else {
                        ++depth;
                    }
                } else if (t2 == QXmlStreamReader::EndElement) {
                    --depth;
                }
            }
            if (cid.isEmpty() || !(wanted.isEmpty() || wantIds.contains(cid))) continue;
            const QDateTime st = parseXmltvTime(startS, tz);
            const QDateTime et = parseXmltvTime(stopS, tz);
            if (st.isValid() && et.isValid() && et > st && et > lo && st < hi)
                fp.progs[prefix + cid].push_back({st, et, title});
        }
    }
    return fp;
}

// ===================================================================== 索引
IndexPtr getIndex(bool block) {
    QStringList files;
    if (QFileInfo::exists(epgXmlFile())) files << epgXmlFile();
    files << extFiles();

    int tz, keep;
    quint32 wsig;
    QSet<QString> wanted;
    {
        QMutexLocker l(&g_stateLock);
        tz = g_tzHours;
        keep = g_keepDays;
        wsig = g_wantedSig;
        wanted = g_wanted;
    }
    QString sig = QString("%1|%2|%3").arg(tz).arg(keep).arg(wsig);
    for (const QString &p : files) {
        const QFileInfo fi(p);
        if (fi.exists())
            sig += QString("|%1:%2:%3").arg(p).arg(fi.lastModified().toMSecsSinceEpoch())
                       .arg(fi.size());
    }
    {
        QMutexLocker l(&g_cacheLock);
        if (g_cache && g_cacheSig == sig) return g_cache;
    }
    if (block) g_buildLock.lock();
    else if (!g_buildLock.tryLock()) {
        QMutexLocker l(&g_cacheLock);
        return g_cache ? g_cache : std::make_shared<IndexSnapshot>();
    }
    struct Unlock { ~Unlock() { g_buildLock.unlock(); } } unlock;
    {
        QMutexLocker l(&g_cacheLock);
        if (g_cache && g_cacheSig == sig) return g_cache;
    }

    const QDateTime now = nowLocal(tz);
    const QDateTime lo = now.addDays(-(keep + 1));
    const QDateTime hi = now.addDays(3);

    auto snap = std::make_shared<IndexSnapshot>();
    QSet<QString> labels;
    QSet<QString> primaryKeys;
    for (int i = 0; i < files.size(); ++i) {
        const QString &p = files[i];
        const bool isPrimary = (p == epgXmlFile());
        QString stem = QFileInfo(p).completeBaseName();
        if (stem.isEmpty()) stem = isPrimary ? "iptv_epg" : "epg";
        FileParse fp = parseXmlFile(p, isPrimary ? QString() : QString("%1:").arg(i),
                                    wanted, lo, hi, tz);
        for (const QString &lb : fp.labels) snap->labelSources[lb].insert(stem);
        labels |= fp.labels;
        if (isPrimary) {
            for (auto it = fp.names.begin(); it != fp.names.end(); ++it)
                primaryKeys.insert(it.key());
        } else if (!primaryKeys.isEmpty()) {
            QSet<QString> drop;
            for (auto it = fp.names.begin(); it != fp.names.end(); ++it)
                if (primaryKeys.contains(it.key())) drop |= it.value();
            if (!drop.isEmpty()) {
                for (const QString &cid : drop) fp.progs.remove(cid);
                for (auto it = fp.names.begin(); it != fp.names.end();) {
                    it.value() -= drop;
                    if (it.value().isEmpty()) it = fp.names.erase(it);
                    else ++it;
                }
            }
        }
        for (auto it = fp.progs.begin(); it != fp.progs.end(); ++it)
            snap->progs[it.key()] = it.value();
        for (auto it = fp.names.begin(); it != fp.names.end(); ++it)
            snap->names[it.key()] |= it.value();
    }
    QStringList lbl(labels.begin(), labels.end());
    std::sort(lbl.begin(), lbl.end(), [](const QString &a, const QString &b) {
        const QString na = normName(a), nb = normName(b);
        if (na != nb) return na < nb;
        return a < b;
    });
    snap->labels = lbl;
    snap->files = files;
    snap->sig = sig;
    {
        QMutexLocker l(&g_cacheLock);
        g_cache = snap;
        g_cacheSig = sig;
    }
    return snap;
}

static QString idFileStem(const QString &cid, const QStringList &files) {
    if (cid.isEmpty()) return {};
    static const QRegularExpression re(R"(^(\d+):)");
    const auto m = re.match(cid);
    if (m.hasMatch()) {
        const int idx = m.captured(1).toInt();
        if (idx >= 0 && idx < files.size()) {
            const QString stem = QFileInfo(files[idx]).completeBaseName();
            return stem;
        }
        return {};
    }
    return "iptv_epg";
}

static void splitAliasValue(const QString &aliasVal, const IndexSnapshot &ix,
                            QString *base, QString *stemHint) {
    *base = aliasVal;
    stemHint->clear();
    if (aliasVal.isEmpty()) return;
    QStringList stems;
    QSet<QString> seen;
    for (const QString &p : ix.files) {
        const QString s = QFileInfo(p).completeBaseName();
        if (!s.isEmpty() && !seen.contains(s)) { seen.insert(s); stems << s; }
    }
    std::sort(stems.begin(), stems.end(),
              [](const QString &a, const QString &b) { return a.size() > b.size(); });
    for (const QString &stem : stems) {
        const QString suffix = "_" + stem;
        if (aliasVal.endsWith(suffix) && aliasVal.size() > suffix.size()) {
            *base = aliasVal.left(aliasVal.size() - suffix.size());
            *stemHint = stem;
            return;
        }
    }
}

static QSet<QString> resolveAliasIds(const QString &aliasVal, const IndexSnapshot &ix) {
    if (aliasVal.isEmpty()) return {};
    QString base, hint;
    splitAliasValue(aliasVal, ix, &base, &hint);
    QSet<QString> ids = ix.names.value(normName(base));
    if (ids.isEmpty()) ids = ix.names.value(normName(aliasVal));
    if (ids.isEmpty()) return {};
    if (!hint.isEmpty()) {
        QSet<QString> filtered;
        for (const QString &cid : ids)
            if (idFileStem(cid, ix.files) == hint) filtered.insert(cid);
        if (!filtered.isEmpty()) return filtered;
    }
    return ids;
}

QVector<Prog> loadEpg(const QString &channelName, const QDate &day, const QString &url) {
    const IndexPtr ix = getIndex(false);
    QSet<QString> ids;
    const bool preferPrimary = !url.isEmpty() && url.contains("5140/rtsp/");
    const QString aliasVal = aliasFor(channelName);

    if (preferPrimary) {
        const QSet<QString> all = ix->names.value(normName(channelName));
        if (!all.isEmpty()) {
            QSet<QString> primary;
            for (const QString &cid : all)
                if (idFileStem(cid, ix->files) == "iptv_epg") primary.insert(cid);
            if (!primary.isEmpty()) ids = primary;
        }
        if (ids.isEmpty()) ids = resolveAliasIds(aliasVal, *ix);
    } else {
        ids = resolveAliasIds(aliasVal, *ix);
        if (ids.isEmpty()) ids = ix->names.value(normName(channelName));
    }
    if (ids.isEmpty()) return {};

    QVector<Prog> rows;
    for (const QString &cid : ids) rows += ix->progs.value(cid);
    std::sort(rows.begin(), rows.end(), [](const Prog &a, const Prog &b) {
        return std::tie(a.start, a.end, a.title) < std::tie(b.start, b.end, b.title);
    });
    const QDateTime dayStart = naiveFromDate(day);
    const QDateTime dayEnd = dayStart.addDays(1);
    QVector<Prog> result;
    QSet<QString> seen;
    for (const Prog &p : rows) {
        if (!(p.start < dayEnd && p.end > dayStart)) continue;
        const QString key = QString("%1|%2|%3").arg(p.start.toSecsSinceEpoch())
                                .arg(p.end.toSecsSinceEpoch()).arg(p.title);
        if (seen.contains(key)) continue;
        seen.insert(key);
        result.push_back(p);
    }
    return result;
}

void clearAll() {
    const QStringList keep = loadSourcesFile();
    QDir d(epgDir());
    if (d.exists()) d.removeRecursively();
    if (!keep.isEmpty()) saveSourcesFile(keep);
    invalidateCache();
}

// ===================================================================== 下载参数
QString makeEpgUrl(const QJsonObject &cfg, const QString &code, const QString &dateYmd) {
    QString host = cfg.value("epg_host").toString().trimmed();
    while (host.endsWith('/')) host.chop(1);
    QString path = cfg.value("epg_path").toString().trimmed();
    if (!path.isEmpty() && !path.startsWith('/') && !path.startsWith('?'))
        path = "/" + path;
    const QString tpl = host + path;
    QString fmt = cfg.value("epg_date_fmt").toString().trimmed();
    if (fmt.isEmpty()) fmt = "%Y%m%d";
    const QDate d = QDate::fromString(dateYmd, "yyyyMMdd");
    if (!d.isValid()) throw std::runtime_error("时间格式不匹配: " + dateYmd.toStdString());
    const QString ds = strftimeNaive(naiveFromDate(d), fmt);
    return formatTemplate(tpl, {{"channelcode", code}, {"date", ds}});
}

QString csvPath(const QJsonObject &cfg) {
    QString p = cfg.value("epg_csv").toString().trimmed();
    if (p.startsWith('"')) p.remove(0, 1);
    if (p.endsWith('"')) p.chop(1);
    if (p.isEmpty()) return epgCsvDefault();
    return QDir::isAbsolutePath(p) ? p : QDir(appDir()).filePath(p);
}

static QVector<QStringList> parseCsv(const QString &text) {
    QVector<QStringList> rows;
    QStringList row;
    QString field;
    bool inQ = false, any = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (inQ) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') { field += '"'; ++i; }
                else inQ = false;
            } else field += c;
        } else if (c == '"') {
            inQ = true; any = true;
        } else if (c == ',') {
            row << field; field.clear(); any = true;
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            if (any || !field.isEmpty()) { row << field; rows.push_back(row); }
            row.clear(); field.clear(); any = false;
        } else {
            field += c; any = true;
        }
    }
    if (any || !field.isEmpty()) { row << field; rows.push_back(row); }
    return rows;
}

QVector<QPair<QString, QString>> loadChannelsCsv(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error(f.errorString().toStdString());
    QString text = QString::fromUtf8(f.readAll());
    if (!text.isEmpty() && text[0] == QChar(0xFEFF)) text.remove(0, 1);
    const QVector<QStringList> rows = parseCsv(text);
    QVector<QPair<QString, QString>> items;
    if (rows.isEmpty()) throw std::runtime_error("channel_epg_chongqing.csv 缺少 channelcode 或 title 字段");
    int ci = -1, ti = -1;
    const QStringList &hdr = rows[0];
    for (int i = 0; i < hdr.size(); ++i) {
        const QString k = hdr[i].trimmed().toLower();
        if (k == "channelcode" && ci < 0) ci = i;
        if (k == "title" && ti < 0) ti = i;
    }
    if (ci < 0 || ti < 0)
        throw std::runtime_error("channel_epg_chongqing.csv 缺少 channelcode 或 title 字段");
    QSet<QString> seen;
    for (int r = 1; r < rows.size(); ++r) {
        const QString code = rows[r].value(ci).trimmed();
        const QString title = rows[r].value(ti).trimmed();
        if (code.isEmpty() || seen.contains(code)) continue;
        seen.insert(code);
        items.push_back({code, title.isEmpty() ? code : title});
    }
    return items;
}

// ===================================================================== 更新任务
QString UpdateState::error() const {
    QMutexLocker l(&m_errLock);
    return m_error;
}
void UpdateState::setError(const QString &e) {
    QMutexLocker l(&m_errLock);
    m_error = e;
}

namespace {

using ProgKey = std::tuple<QString, QString, QString>; // start_s, stop_s, title

struct Item {
    QDateTime st, et;
    QString title;
};

QString fmtTz(int h) {
    return QString("%1%200").arg(h >= 0 ? "+" : "-").arg(qAbs(h), 2, 10, QChar('0'));
}

QVector<Item> parseEpgData(const QJsonDocument &doc) {
    QVector<Item> items;
    if (!doc.isObject()) return items;
    for (const QJsonValue &v : doc.object().value("schedules").toArray()) {
        const QJsonObject s = v.toObject();
        const QDateTime st = QDateTime::fromString(s.value("starttime").toString(), "yyyy-MM-dd HH:mm:ss");
        const QDateTime et = QDateTime::fromString(s.value("endtime").toString(), "yyyy-MM-dd HH:mm:ss");
        if (!st.isValid() || !et.isValid()) continue;
        QDateTime a = st, b = et;
        a.setTimeSpec(Qt::UTC);
        b.setTimeSpec(Qt::UTC);
        if (b <= a) continue;
        const QJsonValue t = s.value("title");
        items.push_back({a, b, (t.isString() ? t.toString() : QString()).trimmed()});
    }
    return items;
}

ProgKey progKey(const Item &it, const QString &tzS) {
    return {strftimeNaive(it.st, "%Y%m%d%H%M%S") + " " + tzS,
            strftimeNaive(it.et, "%Y%m%d%H%M%S") + " " + tzS, it.title};
}

QString progDate(const QString &stS, int tz) {
    const QDateTime st = parseXmltvTime(stS, tz);
    return st.isValid() ? strftimeNaive(st, "%Y%m%d") : QString();
}

QString xmlEscape(const QString &s) {
    QString r = s;
    r.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;");
    return r;
}

QString quoteAttr(const QString &s) {
    QString r = xmlEscape(s);
    r.replace("\n", "&#10;").replace("\r", "&#13;").replace("\t", "&#9;");
    if (r.contains('"')) {
        if (r.contains('\'')) {
            r.replace('"', "&quot;");
            return "\"" + r + "\"";
        }
        return "'" + r + "'";
    }
    return "\"" + r + "\"";
}

struct PrimaryData {
    QMap<QString, QSet<ProgKey>> progs;
    QMap<QString, QStringList> names;
};

PrimaryData readPrimaryXml() {
    PrimaryData d;
    QFile f(epgXmlFile());
    if (!f.open(QIODevice::ReadOnly)) return d;
    QXmlStreamReader xr(&f);
    while (!xr.atEnd()) {
        if (xr.readNext() != QXmlStreamReader::StartElement) continue;
        if (xr.name() == QLatin1String("channel")) {
            const QString cid = xr.attributes().value("id").toString().trimmed();
            QStringList dns;
            int depth = 1;
            while (!xr.atEnd() && depth > 0) {
                const auto t = xr.readNext();
                if (t == QXmlStreamReader::StartElement) {
                    if (xr.name() == QLatin1String("display-name")) {
                        const QString s = xr.readElementText().trimmed();
                        if (!s.isEmpty()) dns << s;
                    } else ++depth;
                } else if (t == QXmlStreamReader::EndElement) --depth;
            }
            if (!cid.isEmpty()) d.names[cid] = dns;
        } else if (xr.name() == QLatin1String("programme")) {
            const QString cid = xr.attributes().value("channel").toString().trimmed();
            const QString st = xr.attributes().value("start").toString();
            const QString sp = xr.attributes().value("stop").toString();
            QString title;
            bool got = false;
            int depth = 1;
            while (!xr.atEnd() && depth > 0) {
                const auto t = xr.readNext();
                if (t == QXmlStreamReader::StartElement) {
                    if (!got && xr.name() == QLatin1String("title")) {
                        title = xr.readElementText().trimmed();
                        got = true;
                    } else ++depth;
                } else if (t == QXmlStreamReader::EndElement) --depth;
            }
            if (!cid.isEmpty() && !st.isEmpty() && !sp.isEmpty())
                d.progs[cid].insert({st, sp, title});
        }
    }
    return d;
}

bool importLegacyJson(PrimaryData &d, const QMap<QString, QString> &code2title,
                      const QString &tzS) {
    bool found = false;
    QDir root(epgDir());
    if (!root.exists()) return found;
    static const QRegularExpression re(R"(^(.+)_(\d{8})\.json$)",
                                       QRegularExpression::CaseInsensitiveOption);
    for (const QString &sub : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir sp(root.filePath(sub));
        for (const QString &fn : sp.entryList(QDir::Files)) {
            const auto m = re.match(fn);
            if (!m.hasMatch()) continue;
            const QString code = m.captured(1);
            QVector<Item> items;
            try {
                items = parseEpgData(QJsonDocument::fromJson(readTextAuto(sp.filePath(fn)).toUtf8()));
            } catch (...) { continue; }
            if (items.isEmpty()) continue;
            found = true;
            if (!d.names.contains(code))
                d.names[code] = QStringList{code2title.value(code, sub)};
            for (const Item &it : items) d.progs[code].insert(progKey(it, tzS));
        }
        sp.removeRecursively();
    }
    return found;
}

int writePrimaryXml(const PrimaryData &d) {
    QDir().mkpath(epgDir());
    const QString tmp = epgXmlFile() + ".tmp";
    QFile out(tmp);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        throw std::runtime_error(out.errorString().toStdString());
    int count = 0;
    out.write(("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tv generator-info-name=" +
               quoteAttr(kCodeVersion) + ">\n").toUtf8());
    QStringList ids;
    for (auto it = d.progs.begin(); it != d.progs.end(); ++it)
        if (!it.value().isEmpty()) ids << it.key();
    std::sort(ids.begin(), ids.end());
    for (const QString &cid : ids) {
        QString s = "<channel id=" + quoteAttr(cid) + ">";
        QStringList nms = d.names.value(cid);
        if (nms.isEmpty()) nms << cid;
        for (const QString &nm : nms) s += "<display-name>" + xmlEscape(nm) + "</display-name>";
        s += "</channel>\n";
        out.write(s.toUtf8());
    }
    for (const QString &cid : ids) {
        QList<ProgKey> lst(d.progs[cid].begin(), d.progs[cid].end());
        std::sort(lst.begin(), lst.end());
        for (const ProgKey &k : lst) {
            out.write(("<programme start=" + quoteAttr(std::get<0>(k)) +
                       " stop=" + quoteAttr(std::get<1>(k)) +
                       " channel=" + quoteAttr(cid) + "><title lang=\"zh\">" +
                       xmlEscape(std::get<2>(k)) + "</title></programme>\n").toUtf8());
            ++count;
        }
    }
    out.write("</tv>\n");
    out.close();
    QFile::remove(epgXmlFile());
    if (!QFile::rename(tmp, epgXmlFile()))
        throw std::runtime_error("无法写入 " + epgXmlFile().toStdString());
    return count;
}

QString extStemFromUrl(const QString &src) {
    QString p;
    if (src.startsWith("http://", Qt::CaseInsensitive) || src.startsWith("https://", Qt::CaseInsensitive))
        p = QUrl(src, QUrl::TolerantMode).path();
    else
        p = src;
    p.replace('\\', '/');
    QString base = p.section('/', -1);
    if (base.isEmpty()) base = "epg";
    for (const QString &suf : {QStringLiteral(".gz"), QStringLiteral(".xml")})
        if (base.toLower().endsWith(suf)) base.chop(suf.size());
    const QString r = safeFilename(base, 60);
    return r.isEmpty() ? QStringLiteral("epg") : r;
}

QMap<QString, QString> loadExtMap() {
    QMap<QString, QString> m;
    const QJsonDocument d = readJsonFile(epgExtMapFile());
    if (!d.isObject()) return m;
    const QJsonObject o = d.object();
    for (auto it = o.begin(); it != o.end(); ++it) m.insert(it.key(), it.value().toString());
    return m;
}

QMap<QString, QString> assignExtNames(const QStringList &sources,
                                      const QMap<QString, QString> &oldMap) {
    QSet<QString> used{"iptv_epg", "alias"};
    QMap<QString, QString> nm;
    for (const QString &src : sources) {
        const QString stem = oldMap.value(src);
        if (!stem.isEmpty() && !used.contains(stem.toLower())) {
            used.insert(stem.toLower());
            nm.insert(src, stem);
        }
    }
    for (const QString &src : sources) {
        if (nm.contains(src)) continue;
        const QString base = extStemFromUrl(src);
        QString stem = base;
        int i = 1;
        while (used.contains(stem.toLower()) || QFileInfo::exists(epgDir() + "/" + stem + ".xml")) {
            ++i;
            stem = QString("%1_%2").arg(base).arg(i);
        }
        used.insert(stem.toLower());
        nm.insert(src, stem);
    }
    return nm;
}

// 把外部 XMLTV 规范成 UTF-8；失败抛异常
QByteArray normalizeXml(QByteArray raw) {
    if (!raw.left(16384).toLower().contains("<tv"))
        throw std::runtime_error("不是 XMLTV 内容");
    QStringDecoder dec(QStringConverter::Utf8);
    const QString s = dec(raw);
    if (!dec.hasError()) {
        QString text = s;
        if (!text.isEmpty() && text[0] == QChar(0xFEFF)) text.remove(0, 1);
        static const QRegularExpression re(R"RE((<\?xml[^>]*?encoding=)["'][^"']*["'])RE");
        text.replace(re, "\\1\"UTF-8\"");   // 仅替换第一处
        return text.toUtf8();
    }
    // 不是 UTF-8：按声明的编码转换（比如 GBK）；失败则原样保留
    static const QRegularExpression enc(R"RE(<\?xml[^>]*?encoding=["']([^"']*)["'])RE",
                                        QRegularExpression::CaseInsensitiveOption);
    const QString head = QString::fromLatin1(raw.left(200));
    const auto m = enc.match(head);
    if (m.hasMatch()) {
        if (QTextCodec *c = QTextCodec::codecForName(m.captured(1).toLatin1())) {
            QString text = c->toUnicode(raw);
            QString fixed = text;
            static const QRegularExpression re(R"RE((<\?xml[^>]*?encoding=)["'][^"']*["'])RE");
            fixed.replace(re, "\\1\"UTF-8\"");
            return fixed.toUtf8();
        }
    }
    return raw;
}

void downloadExt(UpdateState &st) {
    QDir().mkpath(epgDir());
    QStringList sources = st.sources;
    sources.removeDuplicates();
    const QMap<QString, QString> oldMap = loadExtMap();
    const QMap<QString, QString> newMap = assignExtNames(sources, oldMap);

    for (auto it = oldMap.begin(); it != oldMap.end(); ++it)
        if (!newMap.contains(it.key())) QFile::remove(epgDir() + "/" + it.value() + ".xml");
    {
        QJsonObject o;
        for (auto it = newMap.begin(); it != newMap.end(); ++it) o[it.key()] = it.value();
        writeJsonFile(epgExtMapFile(), QJsonDocument(o));
    }
    for (const QString &src : sources) {
        if (st.cancel) break;
        const QString stem = newMap.value(src);
        const QString outp = epgDir() + "/" + stem + ".xml.tmp";
        const QString fin = epgDir() + "/" + stem + ".xml";
        try {
            QByteArray raw;
            if (src.startsWith("http://", Qt::CaseInsensitive) ||
                src.startsWith("https://", Qt::CaseInsensitive)) {
                HttpRequestSpec spec;
                spec.url = QUrl(src, QUrl::TolerantMode);
                spec.headers = {{"User-Agent", "Mozilla/5.0 (IPTVPlayer)"}};
                spec.timeoutMs = qMax(st.timeout, 30) * 1000;
                const HttpResponse r = httpFetchBlocking(spec);
                if (!r.ok) throw std::runtime_error(r.error.toStdString());
                raw = r.body;
            } else {
                QFile f(src);
                if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error(f.errorString().toStdString());
                raw = f.readAll();
            }
            if (raw.size() >= 2 && uchar(raw[0]) == 0x1f && uchar(raw[1]) == 0x8b) {
                QByteArray un;
                if (!gunzip(raw, &un)) throw std::runtime_error("gzip 解压失败");
                raw = un;
            }
            const QByteArray out = normalizeXml(raw);
            QFile of(outp);
            if (!of.open(QIODevice::WriteOnly | QIODevice::Truncate))
                throw std::runtime_error(of.errorString().toStdString());
            of.write(out);
            of.close();
            QFile::remove(fin);
            if (!QFile::rename(outp, fin)) throw std::runtime_error("rename failed");
            ++st.srcOk;
        } catch (...) {
            ++st.srcFail;
            QFile::remove(outp);
        }
    }
}

struct MissMap {
    QMap<QString, double> m;
};

MissMap loadMiss() {
    MissMap r;
    const QJsonDocument d = readJsonFile(epgMissFile());
    if (d.isObject()) {
        const QJsonObject o = d.object();
        for (auto it = o.begin(); it != o.end(); ++it) r.m.insert(it.key(), it.value().toDouble());
    }
    return r;
}

void saveMiss(const QMap<QString, double> &m) {
    QJsonObject o;
    for (auto it = m.begin(); it != m.end(); ++it) o[it.key()] = it.value();
    writeJsonFile(epgMissFile(), QJsonDocument(o), QJsonDocument::Compact);
}

} // namespace

void updateWorker(const std::shared_ptr<UpdateState> &stp, const QDate &today, int n,
                  const QSet<QString> &forceDates) {
    UpdateState &state = *stp;
    try {
        const int tz = state.tz;
        const QString tzS = fmtTz(tz);
        QStringList dates;
        for (int i = n - 1; i >= 0; --i) dates << today.addDays(-i).toString("yyyyMMdd");
        const QDateTime winStart = naiveFromDate(today).addDays(-(n - 1));
        QDir().mkpath(epgDir());

        // 迁移旧位置的 iptv_epg.xml
        const QString oldXml = appDir() + "/iptv_epg.xml";
        if (QFileInfo::exists(oldXml)) {
            if (QFileInfo::exists(epgXmlFile())) QFile::remove(oldXml);
            else QFile::rename(oldXml, epgXmlFile());
        }

        QVector<QPair<QString, QString>> channels;
        if (QFileInfo::exists(state.csv)) {
            channels = loadChannelsCsv(state.csv);
        } else if (state.sources.isEmpty()) {
            state.setError("未找到频道表：" + state.csv);
            state.finished = true;
            return;
        }
        QMap<QString, QString> code2title;
        for (const auto &c : channels) code2title.insert(c.first, c.second);

        PrimaryData pd = readPrimaryXml();
        bool changed = importLegacyJson(pd, code2title, tzS);

        int removed = 0;
        for (auto it = pd.progs.begin(); it != pd.progs.end(); ++it) {
            QSet<ProgKey> keep;
            for (const ProgKey &k : it.value()) {
                const QDateTime et = parseXmltvTime(std::get<1>(k), tz);
                if (et.isValid() && et > winStart) keep.insert(k);
                else ++removed;
            }
            it.value() = keep;
        }
        state.removed = removed;

        QMap<QString, QSet<QString>> covered;
        for (auto it = pd.progs.begin(); it != pd.progs.end(); ++it)
            for (const ProgKey &k : it.value()) {
                const QString d = progDate(std::get<0>(k), tz);
                if (!d.isEmpty()) covered[it.key()].insert(d);
            }

        MissMap miss = loadMiss();
        const double nowTs = QDateTime::currentSecsSinceEpoch();
        QVector<QPair<QString, QString>> jobs;
        for (const auto &c : channels)
            for (const QString &d : dates) {
                if (!forceDates.contains(d)) {
                    if (covered.value(c.first).contains(d)) continue;
                    if (nowTs - miss.m.value(c.first + "_" + d, 0.0) < kMissTtlSec) continue;
                }
                jobs.push_back({c.first, d});
            }
        state.total = jobs.size();

        QMutex lock;
        QMap<QPair<QString, QString>, QVector<Item>> results;
        std::atomic<int> next{0};

        auto loop = [&]() {
            while (!state.cancel) {
                const int idx = next.fetch_add(1);
                if (idx >= jobs.size()) return;
                const auto job = jobs[idx];
                const QString key = job.first + "_" + job.second;
                QString status = "error";
                QVector<Item> items;
                try {
                    // _epg_fetch：404 视为 nodata，其余失败重试
                    status = "error";
                    for (int attempt = 0; attempt < kRetry; ++attempt) {
                        HttpRequestSpec spec;
                        spec.url = QUrl(makeEpgUrl(state.urlCfg, job.first, job.second), QUrl::TolerantMode);
                        spec.headers = {{"User-Agent", "Mozilla/5.0 (IPTVPlayer)"}};
                        spec.timeoutMs = state.timeout * 1000;
                        const HttpResponse r = httpFetchBlocking(spec);
                        if (r.ok) {
                            QByteArray body = r.body;
                            if (body.startsWith("\xEF\xBB\xBF")) body.remove(0, 3);
                            QJsonParseError pe;
                            const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
                            if (pe.error != QJsonParseError::NoError) { status = "error"; break; }
                            items = parseEpgData(doc);
                            status = items.isEmpty() ? "nodata" : "ok";
                            break;
                        }
                        if (r.status == 404) { status = "nodata"; break; }
                        if (attempt < kRetry - 1) QThread::sleep(1);
                    }
                } catch (...) {
                    status = "error";
                }
                QMutexLocker l(&lock);
                if (status == "ok") {
                    ++state.ok;
                    miss.m.remove(key);
                    results.insert(job, items);
                } else if (status == "nodata") {
                    ++state.nodata;
                    miss.m[key] = double(QDateTime::currentSecsSinceEpoch());
                } else {
                    ++state.fail;
                }
                ++state.done;
            }
        };

        QList<QThread *> ths;
        for (int i = 0; i < qMax(1, state.threads); ++i) {
            QThread *t = QThread::create(loop);
            ths << t;
            t->start();
        }
        for (QThread *t : ths) { t->wait(); delete t; }

        const QSet<QString> keepD(dates.begin(), dates.end());
        QMap<QString, double> keptMiss;
        for (auto it = miss.m.begin(); it != miss.m.end(); ++it)
            if (keepD.contains(it.key().section('_', -1))) keptMiss.insert(it.key(), it.value());
        saveMiss(keptMiss);

        for (auto it = results.begin(); it != results.end(); ++it) {
            const QString &code = it.key().first;
            const QString &d = it.key().second;
            QSet<ProgKey> &ks = pd.progs[code];
            for (auto k = ks.begin(); k != ks.end();) {
                if (progDate(std::get<0>(*k), tz) == d) k = ks.erase(k);
                else ++k;
            }
            for (const Item &item : it.value()) ks.insert(progKey(item, tzS));
            changed = true;
        }

        for (auto it = pd.progs.begin(); it != pd.progs.end(); ++it) {
            const QString &code = it.key();
            if (code2title.contains(code)) pd.names[code] = QStringList{code2title[code]};
            else if (pd.names.value(code).isEmpty()) pd.names[code] = QStringList{code};
        }

        int xmlCount = 0;
        for (auto it = pd.progs.begin(); it != pd.progs.end(); ++it) xmlCount += it.value().size();
        state.xmlCount = xmlCount;
        if ((changed || removed || !QFileInfo::exists(epgXmlFile())) &&
            (xmlCount || QFileInfo::exists(epgXmlFile())) && !state.cancel) {
            state.xmlCount = writePrimaryXml(pd);
            state.built = true;
        }

        if (!state.cancel) {
            downloadExt(state);
            if (state.srcOk > 0) state.built = true;
        }
    } catch (const std::exception &e) {
        state.setError(QString::fromUtf8(e.what()));
    } catch (...) {
        state.setError("未知错误");
    }
    state.finished = true;
}

} // namespace epg
} // namespace iptv
