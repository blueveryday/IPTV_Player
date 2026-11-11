// libmpv 视频控件（wid 嵌入原生窗口，与原 Python 版一致）。所有调用都在 GUI 线程。
#pragma once

#include <QWidget>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

struct mpv_handle;

namespace iptv {

struct MpvTrack {
    int id = 0;
    QString type; // video / audio / sub
    QString lang;
    QString title;
    QString codec;
    bool selected = false;
    bool external = false;
};

class MpvWidget : public QWidget {
    Q_OBJECT
public:
    explicit MpvWidget(QWidget *parent = nullptr);
    ~MpvWidget() override;

    // 创建并初始化 mpv；失败返回 false 并填充 err
    bool initMpv(bool hwdec, QString *err);
    bool isReady() const { return m_mpv != nullptr; }
    void shutdown();

    bool command(const QStringList &args);
    bool setOption(const QString &name, const QString &value);   // 运行时属性
    bool setPropertyBool(const QString &name, bool v);
    bool setPropertyInt(const QString &name, qint64 v);
    bool setPropertyDouble(const QString &name, double v);
    bool getBool(const QString &name, bool def = false) const;
    double getDouble(const QString &name, double def = 0.0) const;
    qint64 getInt(const QString &name, qint64 def = 0) const;
    QString getString(const QString &name) const;
    QVector<MpvTrack> trackList() const;

    // 高层封装
    void stop();
    void play(const QString &url);
    void screenshotToFile(const QString &path);

signals:
    void fileLoaded();
    void endFile(bool error, const QString &reason);
    void clicked();
    void doubleClicked();
    void rightClicked(const QPoint &globalPos);

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;

private slots:
    void processEvents();

private:
    static void onMpvEvents(void *ctx);

    mpv_handle *m_mpv = nullptr;
};

} // namespace iptv
