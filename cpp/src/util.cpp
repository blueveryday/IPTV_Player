#include "util.h"

#include "http.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringConverter>
#include <QTextCodec>
#include <QUrl>
#include <algorithm>

namespace iptv {

// ------------------------------------------------------------------ 常量
static QSet<QString> makeSet(const QStringList &l) {
    return QSet<QString>(l.begin(), l.end());
}

const QSet<QString> &mediaExts() {
    static const QSet<QString> s = makeSet(
        {".mp4", ".mkv", ".avi", ".mov", ".wmv", ".flv", ".webm", ".ts",
         ".m4v", ".mpg", ".mpeg", ".m2ts", ".mts", ".3gp", ".rmvb", ".rm",
         ".vob", ".ogv", ".mp3", ".aac", ".flac", ".wav", ".ape", ".ogg",
         ".wma", ".m4a", ".opus", ".ac3", ".dts", ".aiff", ".aif", ".alac",
         ".mka", ".mp2", ".mpc", ".wv"});
    return s;
}

const QSet<QString> &audioExts() {
    static const QSet<QString> s = makeSet(
        {".mp3", ".aac", ".flac", ".wav", ".ape", ".ogg", ".wma", ".m4a",
         ".opus", ".ac3", ".dts", ".aiff", ".aif", ".alac", ".mka", ".mp2",
         ".mpc", ".wv"});
    return s;
}

const QSet<QString> &subExts() {
    static const QSet<QString> s = makeSet(
        {".srt", ".ass", ".ssa", ".vtt", ".sub", ".idx", ".smi", ".sami",
         ".rt", ".txt", ".aqt", ".jss", ".js", ".utf", ".utf8", ".utf-8",
         ".sup", ".mks", ".cdg"});
    return s;
}

const QSet<QString> &subExtsAuto() {
    static const QSet<QString> s = [] {
        QSet<QString> r = subExts();
        r.remove(".txt");
        r.remove(".js");
        return r;
    }();
    return s;
}

const QSet<QString> &subTextExts() {
    static const QSet<QString> s = makeSet(
        {".srt", ".ass", ".ssa", ".vtt", ".sub", ".smi", ".sami", ".rt",
         ".txt", ".aqt", ".jss", ".js"});
    return s;
}

const QString kWeekdayCn = QStringLiteral("一二三四五六日");

namespace Colors {
const QString bg_root = QStringLiteral("#1e1e1e");
const QString bg_panel = QStringLiteral("#252526");
const QString bg_input = QStringLiteral("#2d2d30");
const QString bg_video = QStringLiteral("#000000");
const QString fg_primary = QStringLiteral("#e0e0e0");
const QString fg_secondary = QStringLiteral("#9e9e9e");
const QString fg_dim = QStringLiteral("#6b6b6b");
const QString disabled_fg = QStringLiteral("#e0e0e0");
const QString accent = QStringLiteral("#4a90d9");
const QString accent_dim = QStringLiteral("#3a6fa8");
const QString border = QStringLiteral("#3f3f46");
const QString select_bg = QStringLiteral("#094771");
const QString select_fg = QStringLiteral("#ffffff");
const QString btn_bg = QStringLiteral("#333333");
const QString btn_active = QStringLiteral("#454545");
const QString status_bg = QStringLiteral("#2d2d30");
const QString live_green = QStringLiteral("#5ecb6b");
const QString rec_red = QStringLiteral("#ff3b30");
const QString epg_future = QStringLiteral("#ff5555");
} // namespace Colors

// ------------------------------------------------------------------ 路径
QString appDir() {
    const QByteArray env = qgetenv("IPTV_APP_DIR");
    if (!env.isEmpty())
        return QDir::cleanPath(QString::fromLocal8Bit(env));
    return QDir::cleanPath(QCoreApplication::applicationDirPath());
}
QString defaultM3u() { return appDir() + "/iptv.m3u"; }
QString configFile() { return appDir() + "/iptv_config.json"; }
QString recordDir() { return appDir() + "/record"; }
QString screenshotDir() { return appDir() + "/screenshot"; }
QString epgToolDir() { return appDir() + "/src"; }
QString epgDir() { return epgToolDir() + "/epg"; }
QString epgCsvDefault() { return epgToolDir() + "/channel_epg_chongqing.csv"; }
QString epgMissFile() { return epgDir() + "/.miss.json"; }
QString epgAliasFile() { return epgDir() + "/alias.json"; }
QString epgSourcesFile() { return epgDir() + "/epg_sources.json"; }
QString epgXmlFile() { return epgDir() + "/iptv_epg.xml"; }
QString epgExtMapFile() { return epgDir() + "/.ext_map.json"; }

// ------------------------------------------------------------------ 时间
QDateTime naiveDt(int y, int mo, int d, int h, int mi, int s) {
    return QDateTime(QDate(y, mo, d), QTime(h, mi, s), Qt::UTC);
}
QDateTime naiveFromDate(const QDate &d) {
    return QDateTime(d, QTime(0, 0, 0), Qt::UTC);
}
QDateTime nowLocal(int tzHours) {
    return QDateTime::currentDateTimeUtc().addSecs(qint64(tzHours) * 3600);
}

QString strftimeNaive(const QDateTime &dt, const QString &fmt) {
    static const char *wd[] = {"Monday",   "Tuesday", "Wednesday", "Thursday",
                               "Friday",   "Saturday", "Sunday"};
    static const char *mn[] = {"January",   "February", "March",    "April",
                               "May",       "June",     "July",     "August",
                               "September", "October",  "November", "December"};
    const QDate d = dt.date();
    const QTime t = dt.time();
    auto p2 = [](int v) { return QString("%1").arg(v, 2, 10, QChar('0')); };
    QString out;
    for (int i = 0; i < fmt.size(); ++i) {
        const QChar c = fmt[i];
        if (c != '%' || i + 1 >= fmt.size()) {
            out += c;
            continue;
        }
        const QChar f = fmt[++i];
        switch (f.unicode()) {
        case 'Y': out += QString("%1").arg(d.year(), 4, 10, QChar('0')); break;
        case 'y': out += p2(d.year() % 100); break;
        case 'm': out += p2(d.month()); break;
        case 'd': out += p2(d.day()); break;
        case 'e': out += QString("%1").arg(d.day(), 2, 10, QChar(' ')); break;
        case 'H': out += p2(t.hour()); break;
        case 'I': out += p2(t.hour() % 12 == 0 ? 12 : t.hour() % 12); break;
        case 'M': out += p2(t.minute()); break;
        case 'S': out += p2(t.second()); break;
        case 'p': out += t.hour() < 12 ? "AM" : "PM"; break;
        case 'j': out += QString("%1").arg(d.dayOfYear(), 3, 10, QChar('0')); break;
        case 'A': out += wd[d.dayOfWeek() - 1]; break;
        case 'a': out += QString(wd[d.dayOfWeek() - 1]).left(3); break;
        case 'B': out += mn[d.month() - 1]; break;
        case 'b': out += QString(mn[d.month() - 1]).left(3); break;
        case 'F': out += strftimeNaive(dt, "%Y-%m-%d"); break;
        case 'T': out += strftimeNaive(dt, "%H:%M:%S"); break;
        case '%': out += '%'; break;
        default: out += '%'; out += f; break;
        }
    }
    return out;
}

// ------------------------------------------------------------------ 文本
static QString stripBom(QString s) {
    if (!s.isEmpty() && s[0] == QChar(0xFEFF))
        s.remove(0, 1);
    return s;
}

QString readTextAuto(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error(f.errorString().toStdString());
    const QByteArray raw = f.readAll();
    f.close();

    {   // utf-8-sig（严格）
        QStringDecoder dec(QStringConverter::Utf8);
        const QString s = dec(raw);
        if (!dec.hasError())
            return stripBom(s);
    }
    {   // gbk（严格）
        QTextCodec *codec = QTextCodec::codecForName("GBK");
        if (!codec)
            codec = QTextCodec::codecForName("GB18030");
        if (codec) {
            QTextCodec::ConverterState st;
            const QString s = codec->toUnicode(raw.constData(), raw.size(), &st);
            if (st.invalidChars == 0)
                return stripBom(s);
        }
    }
    if (raw.size() % 2 == 0) {   // utf-16（有 BOM 按 BOM，无 BOM 按本机字节序）
        QStringDecoder dec(QStringConverter::Utf16);
        const QString s = dec(raw);
        if (!dec.hasError())
            return stripBom(s);
    }
    QString s = QString::fromUtf8(raw);
    s.remove(QChar(0xFFFD));
    return s;
}

QString cleanUrl(const QString &lineIn) {
    QString line = lineIn.trimmed();
    static const QRegularExpression re(R"(^\[(.*?)\]\((.*?)\)$)");
    const auto m = re.match(line);
    if (m.hasMatch())
        return m.captured(2).trimmed();
    int a = 0, b = line.size();
    while (a < b && (line[a] == '<' || line[a] == '>')) ++a;
    while (b > a && (line[b - 1] == '<' || line[b - 1] == '>')) --b;
    return line.mid(a, b - a);
}

QString safeFilename(const QString &nameIn, int maxlen) {
    static const QRegularExpression bad(R"([\\/:*?"<>|\x00-\x1f]+)");
    static const QRegularExpression ws(R"(\s+)");
    QString name = nameIn;
    name.replace(bad, "_");
    name.replace(ws, " ");
    name = name.trimmed();
    while (name.startsWith('.')) name.remove(0, 1);
    while (name.endsWith('.')) name.chop(1);
    if (name.size() > maxlen) {
        name = name.left(maxlen);
        while (!name.isEmpty() && name.back().isSpace()) name.chop(1);
    }
    return name.isEmpty() ? QStringLiteral("record") : name;
}

QString fmtMs(qint64 ms) {
    if (ms < 0) ms = 0;
    qint64 s = ms / 1000;
    const qint64 h = s / 3600;
    const qint64 r = s % 3600;
    const qint64 m = r / 60;
    s = r % 60;
    return QString("%1:%2:%3")
        .arg(h, 2, 10, QChar('0'))
        .arg(m, 2, 10, QChar('0'))
        .arg(s, 2, 10, QChar('0'));
}

QString fmtRange(const QDateTime &start, const QDateTime &end) {
    const bool next = end.time().hour() == 0 && end.time().minute() == 0 &&
                      end.date() > start.date();
    return start.toString("HH:mm") + "-" +
           (next ? QStringLiteral("24:00") : end.toString("HH:mm"));
}

QString stripLeadingMark(const QString &raw) {
    static const QRegularExpression re(
        QString::fromUtf8(R"(^\s*(?:\[[^\]]*\]|📁|🎵|🎬|📄|🌐|⬅|▶|⏹|●)\s*)"));
    QString s = raw;
    s.replace(re, "");
    return s;
}

int nearestMp3Bitrate(int kbps) {
    static const int rates[] = {32, 40, 48, 56, 64, 80, 96,
                                112, 128, 160, 192, 224, 256, 320};
    if (kbps <= 0) return 192;
    int best = rates[0];
    for (int r : rates)
        if (qAbs(r - kbps) < qAbs(best - kbps)) best = r;
    return best;
}

QString formatTemplate(const QString &tpl, const QMap<QString, QString> &vars) {
    QString out;
    for (int i = 0; i < tpl.size(); ++i) {
        const QChar c = tpl[i];
        if (c == '{') {
            if (i + 1 < tpl.size() && tpl[i + 1] == '{') {
                out += '{';
                ++i;
                continue;
            }
            const int e = tpl.indexOf('}', i + 1);
            if (e < 0)
                throw std::runtime_error("Single '{' encountered in format string");
            const QString key = tpl.mid(i + 1, e - i - 1);
            if (!vars.contains(key))
                throw std::runtime_error(("KeyError: " + key).toStdString());
            out += vars.value(key);
            i = e;
        } else if (c == '}') {
            if (i + 1 < tpl.size() && tpl[i + 1] == '}') {
                out += '}';
                ++i;
                continue;
            }
            throw std::runtime_error("Single '}' encountered in format string");
        } else {
            out += c;
        }
    }
    return out;
}

// ------------------------------------------------------------------ 媒体 / M3U
// 等价于 os.path.splitext(path)[1].lower()
static QString pyExt(const QString &path) {
    const int slash = qMax(path.lastIndexOf('/'), path.lastIndexOf('\\'));
    const QString base = path.mid(slash + 1);
    const int dot = base.lastIndexOf('.');
    if (dot <= 0) return QString();
    // 全是点开头的不算扩展名（如 ".bashrc"）
    bool allDots = true;
    for (int i = 0; i < dot; ++i)
        if (base[i] != '.') { allDots = false; break; }
    if (allDots) return QString();
    return base.mid(dot).toLower();
}

static QString urlToLocalPath(const QString &url) {
    if (url.startsWith("file://", Qt::CaseInsensitive)) {
        const QString p = QUrl(url, QUrl::TolerantMode).toLocalFile();
        if (!p.isEmpty()) return p;
        return QUrl::fromPercentEncoding(url.mid(7).toUtf8());
    }
    return url;
}

bool isLocalMediaFile(const QString &url) {
    if (url.isEmpty()) return false;
    const QString path = urlToLocalPath(url);
    if (!QFileInfo(path).isFile()) return false;
    return mediaExts().contains(pyExt(path));
}

QString guessMediaKind(const QString &url) {
    if (url.isEmpty()) return QString();
    QString p = urlToLocalPath(url);
    p = p.section('?', 0, 0).section('#', 0, 0);
    const QString ext = pyExt(p);
    if (audioExts().contains(ext)) return "audio";
    if (mediaExts().contains(ext)) return "video";
    return QString();
}

QStringList findSiblingSubs(const QString &mediaPath) {
    const QFileInfo mi(mediaPath);
    const QDir dir(mi.absoluteDir());
    QString base = mi.fileName();
    {
        const int dot = base.lastIndexOf('.');
        if (dot > 0) base = base.left(dot);
    }
    base = base.toLower();

    QStringList names = dir.entryList(QDir::Files | QDir::NoDotAndDotDot);
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        return a.toLower() < b.toLower();
    });
    QSet<QString> lowerNames;
    for (const QString &n : names) lowerNames.insert(n.toLower());

    QStringList result;
    for (const QString &n : names) {
        const int dot = n.lastIndexOf('.');
        const QString stem = dot > 0 ? n.left(dot) : n;
        const QString ext = dot > 0 ? n.mid(dot).toLower() : QString();
        if (!subExtsAuto().contains(ext)) continue;
        const QString sl = stem.toLower();
        if (!(sl == base || sl.startsWith(base + "."))) continue;
        if (ext == ".sub" && lowerNames.contains((stem + ".idx").toLower())) continue;
        result << dir.absoluteFilePath(n);
    }
    return result;
}

QString prepareSubFile(const QString &path) {
    const QString ext = pyExt(path);
    if (!subTextExts().contains(ext)) return path;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return path;
    const QByteArray raw = f.read(16 * 1024 * 1024);
    f.close();
    if (raw.startsWith("\xEF\xBB\xBF") || raw.startsWith("\xFF\xFE") ||
        raw.startsWith("\xFE\xFF"))
        return path;
    if (raw.left(4096).contains('\0')) return path;
    {
        QStringDecoder dec(QStringConverter::Utf8);
        (void)dec(raw);
        if (!dec.hasError()) return path;
    }
    QTextCodec *codec = QTextCodec::codecForName("GB18030");
    if (!codec) return path;
    const QString text = codec->toUnicode(raw);
    const QString cache = QDir::tempPath() + "/IPTVPlayer_sub";
    QDir().mkpath(cache);
    const qint64 mt = QFileInfo(path).lastModified().toSecsSinceEpoch();
    const QByteArray key = QCryptographicHash::hash(
        QString("%1|%2").arg(QFileInfo(path).absoluteFilePath()).arg(mt).toUtf8(),
        QCryptographicHash::Md5).toHex();
    const QString out = cache + "/" + QString::fromLatin1(key) + ext;
    QFile of(out);
    if (!of.open(QIODevice::WriteOnly | QIODevice::Truncate)) return path;
    of.write("\xEF\xBB\xBF");
    of.write(text.toUtf8());
    of.close();
    return out;
}

QVector<ChannelPtr> parseM3u(const QString &path) {
    const QString baseDir = QFileInfo(path).absolutePath();
    QVector<ChannelPtr> channels;
    QString name;
    bool haveName = false;
    static const QRegularExpression extinf(
        R"(^#EXTINF:[^,"]*(?:"[^"]*"[^,"]*)*,(.*)$)",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tvgName(R"RE(tvg-name="([^"]*)")RE");
    static const QRegularExpression scheme(R"(^[a-zA-Z][a-zA-Z0-9+.-]*://)");

    const QStringList lines = readTextAuto(path).split(QRegularExpression("\r\n|\n|\r"));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith("#EXTINF", Qt::CaseInsensitive)) {
            const auto m = extinf.match(line);
            haveName = true;
            name = m.hasMatch() ? m.captured(1).trimmed() : QStringLiteral("未知频道");
            if (name.isEmpty()) {
                const auto t = tvgName.match(line);
                name = t.hasMatch() ? t.captured(1) : QStringLiteral("未知频道");
            }
        } else if (line.startsWith('#')) {
            continue;
        } else {
            const QString url = cleanUrl(line);
            if (!url.isEmpty()) {
                QString resolved = url;
                if (!scheme.match(url).hasMatch() && !QDir::isAbsolutePath(url)) {
                    const QString cand = QDir(baseDir).filePath(url);
                    if (QFileInfo(cand).isFile()) resolved = cand;
                }
                QString entryName = haveName ? name : QString();
                if (entryName.isEmpty()) {
                    entryName = isLocalMediaFile(resolved)
                                    ? QFileInfo(resolved).fileName()
                                    : resolved;
                }
                auto ch = std::make_shared<Channel>();
                ch->name = entryName;
                ch->url = resolved;
                ch->local = isLocalMediaFile(resolved);
                channels.push_back(ch);
            }
            haveName = false;
            name.clear();
        }
    }
    return channels;
}

QVector<M3uFile> scanLocalM3uFiles() {
    QVector<M3uFile> files;
    QStringList names = QDir(appDir()).entryList(QDir::Files | QDir::NoDotAndDotDot);
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        return a.toLower() < b.toLower();
    });
    for (const QString &n : names) {
        const QString low = n.toLower();
        if (!(low.endsWith(".m3u") || low.endsWith(".m3u8"))) continue;
        files.push_back({n, appDir() + "/" + n});
    }
    return files;
}

// ------------------------------------------------------------------ 回看
bool replaySupported(const QString &url, const QJsonObject &cfg) {
    if (isLocalMediaFile(url)) return false;
    const QString u = url.toLower();
    if (u.startsWith("rtsp://")) return true;
    if (!u.startsWith("http://") && !u.startsWith("https://")) return false;

    QStringList keys;
    const QJsonValue v = cfg.value("http_replay_keys");
    if (v.isUndefined() || v.isNull()) {
        keys << "/rtsp/";
    } else if (v.isString()) {
        for (const QString &k : v.toString().split(','))
            if (!k.trimmed().isEmpty()) keys << k.trimmed();
    } else if (v.isArray()) {
        for (const QJsonValue &k : v.toArray()) {
            const QString s = k.isString() ? k.toString() : QString::number(k.toDouble());
            if (!s.isEmpty()) keys << s;
        }
    }
    QStringList lk;
    for (const QString &k : keys)
        if (!k.isEmpty()) lk << k.toLower();
    if (lk.isEmpty()) return true;
    for (const QString &k : lk)
        if (u.contains(k)) return true;
    return false;
}

QString buildReplayUrl(const QString &baseUrl, const QDateTime &startLocal,
                       const QDateTime &endLocal, const QJsonObject &cfg) {
    const int tz = cfg.value("tz_offset").toInt(8);
    const qint64 off = qint64(tz) * 3600;
    const QString fmt = "%Y%m%d%H%M%S";
    const QString seek = strftimeNaive(startLocal.addSecs(-off), fmt) + "-" +
                         strftimeNaive(endLocal.addSecs(-off), fmt);
    const QString auth = cfg.value("authinfo").toString().trimmed();
    const QString userid = cfg.value("userid").toString().trimmed();
    const QString tpl = cfg.value("template").toString().trimmed();

    if (!tpl.isEmpty()) {
        QString url = formatTemplate(tpl, {{"base", baseUrl},
                                           {"authinfo", auth},
                                           {"userid", userid},
                                           {"seek", seek}});
        static const QRegularExpression emptyParam(
            R"((?<=[?&])(AuthInfo|userid)=(&|$))");
        url.replace(emptyParam, "");
        while (url.endsWith('&') || url.endsWith('?')) url.chop(1);
        return url;
    }
    QStringList params;
    if (!auth.isEmpty()) params << "AuthInfo=" + auth;
    if (!userid.isEmpty()) params << "userid=" + userid;
    params << "playseek=" + seek;
    const QString sep = baseUrl.contains('?') ? "&" : "?";
    return baseUrl + sep + params.join('&');
}

// ------------------------------------------------------------------ ffmpeg / ffprobe
void hideProcessWindow(QProcess *p) {
#ifdef Q_OS_WIN
    p->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) {
        a->flags |= 0x08000000; // CREATE_NO_WINDOW
    });
#else
    Q_UNUSED(p);
#endif
}

QString findFfmpeg() {
#ifdef Q_OS_WIN
    const QString exe = "ffmpeg.exe";
#else
    const QString exe = "ffmpeg";
#endif
    const QString a = appDir();
    for (const QString &c : {a + "/" + exe, a + "/ffmpeg/" + exe,
                             a + "/ffmpeg/bin/" + exe, a + "/bin/" + exe})
        if (QFileInfo(c).isFile()) return c;
    return QStandardPaths::findExecutable("ffmpeg");
}

QString findFfprobe(const QString &ffmpegPath) {
#ifdef Q_OS_WIN
    const QString exe = "ffprobe.exe";
#else
    const QString exe = "ffprobe";
#endif
    QStringList cands;
    if (!ffmpegPath.isEmpty()) {
        const QString d = QFileInfo(ffmpegPath).absolutePath();
        cands << d + "/" + exe << d + "/bin/" + exe;
    }
    const QString a = appDir();
    cands << a + "/" + exe << a + "/ffmpeg/" + exe << a + "/ffmpeg/bin/" + exe
          << a + "/bin/" + exe;
    for (const QString &c : cands)
        if (QFileInfo(c).isFile()) return c;
    return QStandardPaths::findExecutable("ffprobe");
}

static QByteArray runProbe(const QStringList &cmd, int timeoutMs) {
    if (cmd.isEmpty()) return {};
    QProcess p;
    hideProcessWindow(&p);
    p.setProgram(cmd.first());
    p.setArguments(cmd.mid(1));
    p.setStandardInputFile(QProcess::nullDevice());
    p.setStandardErrorFile(QProcess::nullDevice());
    p.start();
    if (!p.waitForStarted(5000)) return {};
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(3000);
        return {};
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) return {};
    return p.readAllStandardOutput();
}

static QStringList probeBase(const QString &ffprobe, bool format, bool selectA0) {
    QStringList cmd{ffprobe, "-v", "quiet", "-print_format", "json", "-show_streams"};
    if (format) cmd << "-show_format";
    if (selectA0) cmd << "-select_streams" << "a:0";
    cmd << "-analyzeduration" << "3000000" << "-probesize" << "3000000";
    return cmd;
}

int probeAudioBitrate(const QString &ffprobe, const QString &url, int timeoutSec) {
    if (ffprobe.isEmpty()) return -1;
    QStringList cmd = probeBase(ffprobe, false, true);
    cmd << "-i" << url;
    const QByteArray out = runProbe(cmd, timeoutSec * 1000);
    if (out.isEmpty()) return -1;
    const QJsonDocument doc = QJsonDocument::fromJson(out);
    for (const QJsonValue &s : doc.object().value("streams").toArray()) {
        const QJsonValue br = s.toObject().value("bit_rate");
        bool ok = false;
        const qint64 v = (br.isString() ? br.toString().toLongLong(&ok)
                                        : qint64(br.toDouble(0)));
        if (br.isString() ? ok : v > 0) return int(v / 1000);
    }
    return -1;
}

void probeStreamCodecs(const QString &ffprobe, const QString &url, QString *vCodec,
                       QString *aCodec, int timeoutSec) {
    if (vCodec) vCodec->clear();
    if (aCodec) aCodec->clear();
    if (ffprobe.isEmpty()) return;
    QStringList cmd = probeBase(ffprobe, false, false);
    cmd << "-i" << url;
    const QByteArray out = runProbe(cmd, timeoutSec * 1000);
    if (out.isEmpty()) return;
    const QJsonDocument doc = QJsonDocument::fromJson(out);
    bool gotV = false, gotA = false;
    for (const QJsonValue &sv : doc.object().value("streams").toArray()) {
        const QJsonObject s = sv.toObject();
        const QString ct = s.value("codec_type").toString();
        const QString name = s.value("codec_name").toString().toLower();
        if (ct == "video" && !gotV) {
            gotV = true;
            if (vCodec) *vCodec = name;
        } else if (ct == "audio" && !gotA) {
            gotA = true;
            if (aCodec) *aCodec = name;
        }
    }
}

QJsonObject ffprobeFullInfo(const QString &ffprobe, const QString &url, int timeoutSec) {
    if (ffprobe.isEmpty()) return {};
    QStringList cmd = probeBase(ffprobe, true, false);
    cmd << "-i" << url;
    const QByteArray out = runProbe(cmd, timeoutSec * 1000);
    if (out.isEmpty()) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(out);
    return doc.isObject() ? doc.object() : QJsonObject();
}

bool sniffIsHls(const QString &url, int timeoutSec) {
    const QString low = url.toLower();
    if (!low.startsWith("http://") && !low.startsWith("https://")) return false;
    HttpRequestSpec spec;
    spec.url = QUrl(url);
    spec.headers = {{"User-Agent", "Mozilla/5.0 (IPTVPlayer)"},
                    {"Range", "bytes=0-4095"}};
    spec.timeoutMs = timeoutSec * 1000;
    spec.maxBytes = 4096;
    const HttpResponse r = httpFetchBlocking(spec);
    if (!r.ok) return false;
    return r.body.left(7) == "#EXTM3U";
}

} // namespace iptv
