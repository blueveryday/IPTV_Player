#include "hlsproxy.h"

#include "http.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QHostAddress>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QUrl>

namespace iptv {

static const char *kUa = "Mozilla/5.0 (IPTVPlayer)";

static QString urlJoin(const QString &base, const QString &rel) {
    const QUrl b(base, QUrl::TolerantMode);
    const QUrl r(rel, QUrl::TolerantMode);
    return QString::fromUtf8(b.resolved(r).toEncoded());
}

HlsRewriteProxy::HlsRewriteProxy(const QString &originUrl, int timeoutSec,
                                 int fetchRetries, QObject *parent)
    : QTcpServer(parent), m_origin(originUrl), m_timeoutMs(timeoutSec * 1000),
      m_retries(qMax(1, fetchRetries)) {}

HlsRewriteProxy::~HlsRewriteProxy() { stopProxy(); }

bool HlsRewriteProxy::startProxy() {
    return listen(QHostAddress::LocalHost, 0);
}

void HlsRewriteProxy::stopProxy() {
    m_stopping = true;
    if (isListening()) close();
    QList<QThread *> ths;
    {
        QMutexLocker l(&m_threadsLock);
        ths = m_threads;
    }
    for (QThread *t : ths) {
        if (t && !t->wait(3000)) {
            t->requestInterruption();
        }
    }
}

QString HlsRewriteProxy::playlistUrl() const {
    return QString("http://127.0.0.1:%1/playlist.m3u8").arg(serverPort());
}

void HlsRewriteProxy::incomingConnection(qintptr sd) {
    if (m_stopping) {
        QTcpSocket s;
        s.setSocketDescriptor(sd);
        return;
    }
    QThread *t = QThread::create([this, sd]() { handleConnection(sd); });
    QObject::connect(t, &QThread::finished, t, [this, t]() {
        QMutexLocker l(&m_threadsLock);
        m_threads.removeAll(t);
        t->deleteLater();
    });
    {
        QMutexLocker l(&m_threadsLock);
        m_threads.push_back(t);
    }
    t->start();
}

QString HlsRewriteProxy::registerUrl(const QString &realUrl) {
    const QString token = QString::fromLatin1(
        QCryptographicHash::hash(realUrl.toUtf8(), QCryptographicHash::Md5).toHex());
    QMutexLocker l(&m_lock);
    m_tokenMap.insert(token, realUrl);
    return token;
}

QString HlsRewriteProxy::resolveToken(const QString &token) {
    QMutexLocker l(&m_lock);
    return m_tokenMap.value(token);
}

HlsRewriteProxy::Fetched HlsRewriteProxy::fetch(const QString &url) {
    Fetched f;
    for (int attempt = 0; attempt < m_retries && !m_stopping; ++attempt) {
        HttpRequestSpec spec;
        spec.url = QUrl(url, QUrl::TolerantMode);
        spec.headers = {{"User-Agent", kUa}};
        spec.timeoutMs = m_timeoutMs;
        const HttpResponse r = httpFetchBlocking(spec);
        if (r.ok) {
            f.ok = true;
            f.body = r.body;
            f.finalUrl = r.finalUrl.isEmpty() ? url
                                              : QString::fromUtf8(r.finalUrl.toEncoded());
            return f;
        }
        if (attempt < m_retries - 1)
            QThread::msleep(400 * (attempt + 1));
    }
    return f;
}

QString HlsRewriteProxy::rewriteUriAttr(const QString &line, const QString &baseUrl,
                                        const QString &host, const QString &ext) {
    static const QRegularExpression re(R"RE(URI="([^"]+)")RE");
    const auto m = re.match(line);
    if (!m.hasMatch()) return line;
    const QString real = urlJoin(baseUrl, m.captured(1));
    const QString token = registerUrl(real);
    const QString newUri = QString("http://%1/seg/%2%3").arg(host, token, ext);
    return line.left(m.capturedStart(1)) + newUri + line.mid(m.capturedEnd(1));
}

QString HlsRewriteProxy::rewritePlaylist(const QString &text, const QString &baseUrl,
                                         const QString &host) {
    const bool isMedia = text.contains("#EXTINF") || text.contains("#EXT-X-TARGETDURATION");
    const bool isMaster = !isMedia && (text.contains("#EXT-X-STREAM-INF") ||
                                       text.contains("#EXT-X-MEDIA") ||
                                       text.contains("#EXT-X-I-FRAME-STREAM-INF"));
    QStringList out;
    const QStringList lines = text.split(QRegularExpression("\r\n|\n|\r"));
    // Python 的 splitlines() 不会在末尾产生空串
    int count = lines.size();
    if (count > 0 && lines.last().isEmpty()) --count;
    for (int i = 0; i < count; ++i) {
        const QString &raw = lines[i];
        QString s = raw.trimmed();
        if (s.isEmpty()) {
            out << raw;
            continue;
        }
        if (s.startsWith("#EXT-X-MAP")) {
            out << rewriteUriAttr(s, baseUrl, host, ".m4s");
        } else if (s.startsWith("#EXT-X-MEDIA")) {
            out << rewriteUriAttr(s, baseUrl, host, ".m3u8");
        } else if (s.startsWith("#EXT-X-I-FRAME-STREAM-INF")) {
            out << rewriteUriAttr(s, baseUrl, host, ".m3u8");
        } else if (s.startsWith("#EXT-X-KEY") || s.startsWith("#EXT-X-SESSION-KEY")) {
            out << rewriteUriAttr(s, baseUrl, host, ".bin");
        } else if (s.startsWith("#EXT-X-PART") || s.startsWith("#EXT-X-PRELOAD-HINT")) {
            out << rewriteUriAttr(s, baseUrl, host, ".m4s");
        } else if (s.startsWith('#')) {
            out << s;
        } else {
            const QString real = urlJoin(baseUrl, s);
            const QString token = registerUrl(real);
            out << QString("http://%1/seg/%2%3")
                       .arg(host, token, isMaster ? ".m3u8" : ".m4s");
        }
    }
    return out.join('\n') + "\n";
}

static void sendResponse(QTcpSocket &sock, int code, const QByteArray &reason,
                         const QByteArray &contentType, const QByteArray &body,
                         bool closeAfter) {
    QByteArray head = "HTTP/1.1 " + QByteArray::number(code) + " " + reason + "\r\n";
    head += "Content-Type: " + contentType + "\r\n";
    head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    head += "Cache-Control: no-cache\r\n";
    if (closeAfter) head += "Connection: close\r\n";
    head += "\r\n";
    sock.write(head);
    if (!body.isEmpty()) sock.write(body);
    sock.flush();
    sock.waitForBytesWritten(15000);
}

void HlsRewriteProxy::handleConnection(qintptr sd) {
    QTcpSocket sock;
    if (!sock.setSocketDescriptor(sd)) return;
    QByteArray buf;

    while (!m_stopping) {
        int idleMs = 0;
        while (!buf.contains("\r\n\r\n")) {
            if (m_stopping) return;
            if (!sock.waitForReadyRead(500)) {
                if (sock.state() != QAbstractSocket::ConnectedState) return;
                idleMs += 500;
                if (idleMs >= 20000) return;
                continue;
            }
            buf += sock.readAll();
            idleMs = 0;
            if (buf.size() > 65536) return;
        }
        const int end = buf.indexOf("\r\n\r\n");
        const QByteArray head = buf.left(end);
        buf.remove(0, end + 4);

        const QList<QByteArray> lines = head.split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        const QByteArray method = first.value(0);
        const QByteArray target = first.value(1);
        const QByteArray version = first.value(2);
        QString host;
        bool wantClose = (version != "HTTP/1.1");
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray l = lines[i].trimmed();
            const int c = l.indexOf(':');
            if (c <= 0) continue;
            const QByteArray k = l.left(c).trimmed().toLower();
            const QByteArray v = l.mid(c + 1).trimmed();
            if (k == "host") host = QString::fromLatin1(v);
            else if (k == "connection" && v.toLower() == "close") wantClose = true;
        }
        if (host.isEmpty()) host = QString("127.0.0.1:%1").arg(serverPort());

        if (method != "GET") {
            sendResponse(sock, 501, "Not Implemented", "text/plain", "501", true);
            return;
        }
        const QString path = QString::fromLatin1(target).section('?', 0, 0);

        if (path.startsWith("/playlist")) {
            // ---- 清单 ----
            Fetched f = fetch(m_origin);
            if (f.ok) {
                QMutexLocker l(&m_lock);
                m_playlistCache.insert(m_origin, {f.body, f.finalUrl});
            } else {
                QMutexLocker l(&m_lock);
                if (m_playlistCache.contains(m_origin)) {
                    const auto c = m_playlistCache.value(m_origin);
                    f.body = c.first;
                    f.finalUrl = c.second;
                    f.ok = true;
                    qWarning().noquote() << "[HLS-proxy] playlist fetch failed, using cache:" << m_origin;
                }
            }
            if (!f.ok) {
                qWarning().noquote() << "[HLS-proxy] playlist fetch failed:" << m_origin;
                sendResponse(sock, 503, "Service Unavailable", "text/plain",
                             "upstream fetch failed", true);
                return;
            }
            const QString text = QString::fromUtf8(f.body);
            const QByteArray out = rewritePlaylist(text, f.finalUrl, host).toUtf8();
            sendResponse(sock, 200, "OK", "application/vnd.apple.mpegurl", out, wantClose);
        } else if (path.startsWith("/seg/")) {
            // ---- 分片 / 子清单 ----
            static const QRegularExpression re(R"(^/seg/([0-9a-f]{32})(\.[a-zA-Z0-9]+)?$)");
            const auto m = re.match(path);
            const QString real = m.hasMatch() ? resolveToken(m.captured(1)) : QString();
            if (real.isEmpty()) {
                sendResponse(sock, 404, "Not Found", "text/plain", "404", true);
                return;
            }
            QByteArray data;
            Fetched f = fetch(real);
            if (f.ok) {
                data = f.body;
                QMutexLocker l(&m_lock);
                if (data.startsWith("#EXTM3U")) {
                    m_playlistCache.insert(real, {data, real});
                } else {
                    // 只保留最近几个分片作为失败回退，避免内存无限增长
                    m_segCache.insert(real, data);
                    m_segOrder.removeAll(real);
                    m_segOrder.push_back(real);
                    while (m_segOrder.size() > 4)
                        m_segCache.remove(m_segOrder.takeFirst());
                }
            } else {
                QMutexLocker l(&m_lock);
                if (m_segCache.contains(real)) data = m_segCache.value(real);
                else if (m_playlistCache.contains(real)) data = m_playlistCache.value(real).first;
                else {
                    l.unlock();
                    qWarning().noquote() << "[HLS-proxy] segment fetch failed:" << real;
                    sendResponse(sock, 503, "Service Unavailable", "text/plain",
                                 "upstream fetch failed", true);
                    return;
                }
            }
            QByteArray body, ctype;
            if (data.startsWith("#EXTM3U")) {
                body = rewritePlaylist(QString::fromUtf8(data), real, host).toUtf8();
                ctype = "application/vnd.apple.mpegurl";
            } else {
                body = data;
                ctype = "video/mp4";
            }
            sendResponse(sock, 200, "OK", ctype, body, wantClose);
        } else {
            sendResponse(sock, 404, "Not Found", "text/plain", "404", true);
            return;
        }
        if (wantClose) break;
    }
    sock.disconnectFromHost();
    if (sock.state() != QAbstractSocket::UnconnectedState)
        sock.waitForDisconnected(1000);
}

} // namespace iptv
