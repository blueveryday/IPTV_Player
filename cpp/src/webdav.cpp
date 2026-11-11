#include "webdav.h"

#include <QUrl>
#include <QXmlStreamReader>
#include <stdexcept>

namespace iptv {

WebDavClient::WebDavClient(const QString &urlIn, const QString &user,
                           const QString &password, int timeoutSec)
    : m_user(user), m_password(password), m_timeout(timeoutSec) {
    QString url = urlIn.trimmed();
    if (!url.startsWith("http://", Qt::CaseInsensitive) &&
        !url.startsWith("https://", Qt::CaseInsensitive))
        url = "http://" + url;
    while (url.endsWith('/')) url.chop(1);
    m_baseUrl = url;
}

QString WebDavClient::encodeSegments(const QString &rel) {
    QStringList segs;
    for (const QString &s : rel.split('/'))
        segs << QString::fromLatin1(QUrl::toPercentEncoding(s));
    return segs.join('/');
}

QNetworkRequest WebDavClient::buildListRequest(const QString &relPath,
                                               QByteArray *body) const {
    QString rel = relPath.trimmed();
    while (rel.startsWith('/')) rel.remove(0, 1);
    while (rel.endsWith('/')) rel.chop(1);
    const QString enc = rel.isEmpty() ? QString() : encodeSegments(rel);
    const QString path = enc.isEmpty() ? QStringLiteral("/") : "/" + enc;

    if (body) {
        *body = QByteArray(
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
            "<d:propfind xmlns:d=\"DAV:\">"
            "<d:prop>"
            "<d:resourcetype/>"
            "<d:getcontentlength/>"
            "</d:prop>"
            "</d:propfind>");
    }
    QNetworkRequest req(QUrl(m_baseUrl + path, QUrl::TolerantMode));
    req.setRawHeader("User-Agent", "IPTVPlayer/1.0 (WebDAV)");
    req.setRawHeader("Accept", "*/*");
    if (!m_user.isEmpty() || !m_password.isEmpty()) {
        const QByteArray token = (m_user + ":" + m_password).toUtf8().toBase64();
        req.setRawHeader("Authorization", "Basic " + token);
    }
    req.setRawHeader("Depth", "1");
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/xml; charset=utf-8");
    req.setTransferTimeout(m_timeout * 1000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    return req;
}

static QString hrefPath(const QString &href) {
    if (href.contains("://"))
        return QString::fromLatin1(
            QUrl(href, QUrl::TolerantMode).path(QUrl::FullyEncoded).toLatin1());
    QString p = href;
    p = p.section('#', 0, 0).section('?', 0, 0);
    return p;
}

static QString stripSlashes(QString s) {
    while (s.startsWith('/')) s.remove(0, 1);
    while (s.endsWith('/')) s.chop(1);
    return s;
}

QVector<WebDavEntry> WebDavClient::parseListResponse(const QByteArray &xml,
                                                     const QString &relPath) const {
    const QString rel = stripSlashes(relPath.trimmed());
    QString basePath = QUrl(m_baseUrl, QUrl::TolerantMode).path(QUrl::FullyEncoded);
    while (basePath.endsWith('/')) basePath.chop(1);

    struct Resp {
        QString href;
        bool isDir = false;
        QString size;
    };
    QVector<Resp> resps;

    static const QString kDav = QStringLiteral("DAV:");
    QXmlStreamReader xr(xml);
    xr.setNamespaceProcessing(true);
    Resp cur;
    bool inResp = false;
    while (!xr.atEnd()) {
        xr.readNext();
        if (xr.isStartElement()) {
            const QString ns = xr.namespaceUri().toString();
            const QString nm = xr.name().toString();
            if (ns == kDav && nm == "response") {
                inResp = true;
                cur = Resp();
            } else if (inResp && ns == kDav && nm == "href") {
                cur.href = xr.readElementText().trimmed();
            } else if (inResp && ns == kDav && nm == "collection") {
                cur.isDir = true;
            } else if (inResp && ns == kDav && nm == "getcontentlength") {
                cur.size = xr.readElementText().trimmed();
            }
        } else if (xr.isEndElement()) {
            if (xr.namespaceUri() == kDav && xr.name() == QLatin1String("response")) {
                if (inResp) resps.push_back(cur);
                inResp = false;
            }
        }
    }
    if (xr.hasError() && resps.isEmpty())
        throw std::runtime_error(xr.errorString().toStdString());

    QVector<WebDavEntry> items;
    for (const Resp &r : resps) {
        if (r.href.isEmpty()) continue;
        const QString hp = QUrl::fromPercentEncoding(hrefPath(r.href).toUtf8());
        QString sub;
        if (!basePath.isEmpty()) {
            const QString baseDecoded = QUrl::fromPercentEncoding(basePath.toUtf8());
            if (hp.startsWith(baseDecoded)) sub = hp.mid(baseDecoded.size());
            else if (hp.startsWith(basePath)) sub = hp.mid(basePath.size());
            else sub = hp;
        } else {
            sub = hp;
        }
        sub = stripSlashes(sub);

        const QString cur2 = rel;
        if (sub == cur2) continue;
        if (!cur2.isEmpty() && !sub.startsWith(cur2 + "/")) continue;
        QString rest = cur2.isEmpty() ? sub : sub.mid(cur2.size());
        while (rest.startsWith('/')) rest.remove(0, 1);
        if (rest.isEmpty() || rest.contains('/')) continue;

        WebDavEntry e;
        e.name = rest;
        e.isDir = r.isDir;
        bool ok = false;
        const qint64 sz = r.size.toLongLong(&ok);
        e.size = ok ? sz : 0;
        e.url = m_baseUrl + "/" + encodeSegments(sub);
        items.push_back(e);
    }
    return items;
}

} // namespace iptv
