// 本地 HLS 重写代理：把分片/子清单地址改写成 /seg/<md5>.<ext>，供 ffmpeg 录制
// 伪装扩展名（非 .ts/.m3u8）的 HLS 源使用。等价于 Python 版 HLSRewriteProxy。
#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QPair>
#include <QStringList>
#include <QTcpServer>
#include <QThread>
#include <atomic>

namespace iptv {

class HlsRewriteProxy : public QTcpServer {
    Q_OBJECT
public:
    explicit HlsRewriteProxy(const QString &originUrl, int timeoutSec = 10,
                             int fetchRetries = 5, QObject *parent = nullptr);
    ~HlsRewriteProxy() override;

    bool startProxy();            // 监听 127.0.0.1 随机端口
    void stopProxy();
    QString playlistUrl() const;

protected:
    void incomingConnection(qintptr socketDescriptor) override;

private:
    struct Fetched {
        QByteArray body;
        QString finalUrl;
        bool ok = false;
    };
    Fetched fetch(const QString &url);
    QString registerUrl(const QString &realUrl);
    QString resolveToken(const QString &token);
    QString rewriteUriAttr(const QString &line, const QString &baseUrl,
                           const QString &host, const QString &ext);
    QString rewritePlaylist(const QString &text, const QString &baseUrl,
                            const QString &host);
    void handleConnection(qintptr sd);

    QString m_origin;
    int m_timeoutMs;
    int m_retries;
    QHash<QString, QString> m_tokenMap;
    QHash<QString, QPair<QByteArray, QString>> m_playlistCache;
    QHash<QString, QByteArray> m_segCache;
    QStringList m_segOrder;
    QMutex m_lock;
    QMutex m_threadsLock;
    QList<QThread *> m_threads;
    std::atomic<bool> m_stopping{false};
};

} // namespace iptv
