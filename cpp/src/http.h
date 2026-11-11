// 阻塞式 HTTP 请求（仅供工作线程使用；内部自带事件循环，不要在 GUI 线程调用）
#pragma once

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QUrl>
#include <QVector>

namespace iptv {

struct HttpRequestSpec {
    QUrl url;
    QVector<QPair<QString, QString>> headers;
    QByteArray method = "GET";
    QByteArray body;
    int timeoutMs = 10000;   // 空闲（无数据）超时，语义同 urllib 的 timeout
    qint64 maxBytes = -1;    // >0：读到这么多字节就主动断开（用于嗅探）
};

struct HttpResponse {
    bool ok = false;         // 网络成功且 HTTP 状态 < 400
    int status = 0;          // HTTP 状态码，0 表示网络层错误
    QByteArray body;
    QString error;           // 形如 "HTTP Error 404: Not Found" / 网络错误文本
    QUrl finalUrl;           // 重定向之后的最终地址
};

HttpResponse httpFetchBlocking(const HttpRequestSpec &spec);

} // namespace iptv
