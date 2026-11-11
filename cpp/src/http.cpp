#include "http.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace iptv {

HttpResponse httpFetchBlocking(const HttpRequestSpec &spec) {
    HttpResponse r;
    QNetworkAccessManager nam;
    QNetworkRequest req(spec.url);
    for (const auto &h : spec.headers)
        req.setRawHeader(h.first.toUtf8(), h.second.toUtf8());
    req.setTransferTimeout(spec.timeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = (spec.method == "GET")
                               ? nam.get(req)
                               : nam.sendCustomRequest(req, spec.method, spec.body);
    bool truncated = false;
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&]() {
        r.body += reply->readAll();
        if (spec.maxBytes > 0 && r.body.size() >= spec.maxBytes && !truncated) {
            truncated = true;
            reply->abort();
        }
    });
    if (!reply->isFinished())
        loop.exec();
    r.body += reply->readAll();

    r.finalUrl = reply->url();
    r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (truncated) {
        r.body = r.body.left(int(spec.maxBytes));
        r.ok = true;
        if (r.status == 0) r.status = 200;
    } else if (reply->error() == QNetworkReply::NoError) {
        r.ok = r.status < 400;
    } else if (r.status >= 400) {
        r.error = QString("HTTP Error %1: %2")
                      .arg(r.status)
                      .arg(reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute)
                               .toString());
    } else {
        r.error = reply->errorString();
    }
    delete reply;
    return r;
}

} // namespace iptv
