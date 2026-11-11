// WebDAV PROPFIND 客户端（请求构造 + 响应解析；网络收发由主窗口的 QNetworkAccessManager 异步完成）
#pragma once

#include <QByteArray>
#include <QNetworkRequest>
#include <QString>
#include <QVector>

namespace iptv {

struct WebDavEntry {
    QString name;
    bool isDir = false;
    QString url;
    qint64 size = 0;
};

class WebDavClient {
public:
    WebDavClient(const QString &url, const QString &user = QString(),
                 const QString &password = QString(), int timeoutSec = 15);

    // 构造 PROPFIND 请求（Depth: 1）；body 返回请求体
    QNetworkRequest buildListRequest(const QString &relPath, QByteArray *body) const;
    // 解析 207 Multi-Status 响应；失败抛 std::runtime_error
    QVector<WebDavEntry> parseListResponse(const QByteArray &xml,
                                           const QString &relPath) const;
    int timeoutMs() const { return m_timeout * 1000; }

private:
    static QString encodeSegments(const QString &rel);

    QString m_baseUrl;
    QString m_user;
    QString m_password;
    int m_timeout;
};

} // namespace iptv
