#include "mpvwidget.h"

#include <QMouseEvent>
#include <cstdio>
#include <cstdlib>
#include <QMetaObject>
#include <QCursor>
#include <mpv/client.h>

namespace iptv {

static QVariant nodeToVariant(const mpv_node *n) {
    switch (n->format) {
    case MPV_FORMAT_STRING: return QString::fromUtf8(n->u.string);
    case MPV_FORMAT_FLAG: return bool(n->u.flag);
    case MPV_FORMAT_INT64: return qlonglong(n->u.int64);
    case MPV_FORMAT_DOUBLE: return n->u.double_;
    case MPV_FORMAT_NODE_ARRAY: {
        QVariantList l;
        for (int i = 0; i < n->u.list->num; ++i) l << nodeToVariant(&n->u.list->values[i]);
        return l;
    }
    case MPV_FORMAT_NODE_MAP: {
        QVariantMap m;
        for (int i = 0; i < n->u.list->num; ++i)
            m.insert(QString::fromUtf8(n->u.list->keys[i]), nodeToVariant(&n->u.list->values[i]));
        return m;
    }
    default: return QVariant();
    }
}

MpvWidget::MpvWidget(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setStyleSheet("background:#000000;");
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
}

MpvWidget::~MpvWidget() { shutdown(); }

bool MpvWidget::initMpv(bool hwdec, QString *err) {
    if (m_mpv) return true;
    m_mpv = mpv_create();
    if (!m_mpv) {
        if (err) *err = "mpv_create 失败";
        return false;
    }
    const QByteArray wid = QByteArray::number(qlonglong(winId()));
    mpv_set_option_string(m_mpv, "wid", wid.constData());
    mpv_set_option_string(m_mpv, "gpu-sw", "yes"); // 虚拟机/软件渲染环境也能嵌入显示
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "input-vo-keyboard", "no");
    mpv_set_option_string(m_mpv, "osc", "yes");
    mpv_set_option_string(m_mpv, "hwdec", hwdec ? "auto" : "no");
    mpv_set_option_string(m_mpv, "keep-open", "yes");
    mpv_set_option_string(m_mpv, "network-timeout", "10");
    mpv_set_option_string(m_mpv, "idle", "yes");
    const int rc = mpv_initialize(m_mpv);
    if (rc < 0) {
        if (err) *err = QString::fromUtf8(mpv_error_string(rc));
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return false;
    }
    mpv_set_wakeup_callback(m_mpv, onMpvEvents, this);
    // 鼠标点击由 mpv 窗口接收，经 script-message 回传给 Qt
    command({"keybind", "MBTN_LEFT", "script-message iptv-click"});
    command({"keybind", "MBTN_LEFT_DBL", "script-message iptv-dblclick"});
    command({"keybind", "MBTN_RIGHT", "script-message iptv-rclick"});
    return true;
}

void MpvWidget::shutdown() {
    if (m_mpv) {
        mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
}

void MpvWidget::onMpvEvents(void *ctx) {
    QMetaObject::invokeMethod(static_cast<MpvWidget *>(ctx), "processEvents",
                              Qt::QueuedConnection);
}

void MpvWidget::processEvents() {
    while (m_mpv) {
        mpv_event *ev = mpv_wait_event(m_mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE) break;
        if (ev->event_id == MPV_EVENT_FILE_LOADED) {
            emit fileLoaded();
        } else if (ev->event_id == MPV_EVENT_END_FILE) {
            const auto *ef = static_cast<mpv_event_end_file *>(ev->data);
            if (ef) {
                const bool isErr = ef->reason == MPV_END_FILE_REASON_ERROR;
                emit endFile(isErr, isErr ? QString::fromUtf8(mpv_error_string(ef->error))
                                          : QString());
            }
        } else if (ev->event_id == MPV_EVENT_CLIENT_MESSAGE) {
            const auto *cm = static_cast<mpv_event_client_message *>(ev->data);
            if (cm && cm->num_args >= 1) {
                const QString m = QString::fromUtf8(cm->args[0]);
                if (m == "iptv-click") emit clicked();
                else if (m == "iptv-dblclick") emit doubleClicked();
                else if (m == "iptv-rclick") emit rightClicked(QCursor::pos());
            }
        } else if (ev->event_id == MPV_EVENT_SHUTDOWN) {
            break;
        }
    }
}

// ----------------------------------------------------------------- 命令与属性
bool MpvWidget::command(const QStringList &args) {
    if (!m_mpv || args.isEmpty()) return false;
    QList<QByteArray> bytes;
    for (const QString &a : args) bytes << a.toUtf8();
    QVector<const char *> argv;
    for (const QByteArray &b : bytes) argv << b.constData();
    argv << nullptr;
    return mpv_command(m_mpv, argv.data()) >= 0;
}

bool MpvWidget::setOption(const QString &name, const QString &value) {
    if (!m_mpv) return false;
    return mpv_set_property_string(m_mpv, name.toUtf8().constData(), value.toUtf8().constData()) >= 0;
}

bool MpvWidget::setPropertyBool(const QString &name, bool v) {
    if (!m_mpv) return false;
    int f = v ? 1 : 0;
    return mpv_set_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_FLAG, &f) >= 0;
}

bool MpvWidget::setPropertyInt(const QString &name, qint64 v) {
    if (!m_mpv) return false;
    int64_t x = v;
    return mpv_set_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_INT64, &x) >= 0;
}

bool MpvWidget::setPropertyDouble(const QString &name, double v) {
    if (!m_mpv) return false;
    return mpv_set_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_DOUBLE, &v) >= 0;
}

bool MpvWidget::getBool(const QString &name, bool def) const {
    if (!m_mpv) return def;
    int f = 0;
    if (mpv_get_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_FLAG, &f) < 0) return def;
    return f != 0;
}

double MpvWidget::getDouble(const QString &name, double def) const {
    if (!m_mpv) return def;
    double d = 0;
    if (mpv_get_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_DOUBLE, &d) < 0) return def;
    return d;
}

qint64 MpvWidget::getInt(const QString &name, qint64 def) const {
    if (!m_mpv) return def;
    int64_t v = 0;
    if (mpv_get_property(m_mpv, name.toUtf8().constData(), MPV_FORMAT_INT64, &v) < 0) return def;
    return v;
}

QString MpvWidget::getString(const QString &name) const {
    if (!m_mpv) return {};
    char *s = mpv_get_property_string(m_mpv, name.toUtf8().constData());
    if (!s) return {};
    const QString r = QString::fromUtf8(s);
    mpv_free(s);
    return r;
}

QVector<MpvTrack> MpvWidget::trackList() const {
    QVector<MpvTrack> out;
    if (!m_mpv) return out;
    mpv_node node;
    if (mpv_get_property(m_mpv, "track-list", MPV_FORMAT_NODE, &node) < 0) return out;
    const QVariant v = nodeToVariant(&node);
    mpv_free_node_contents(&node);
    for (const QVariant &e : v.toList()) {
        const QVariantMap m = e.toMap();
        MpvTrack t;
        t.id = m.value("id").toInt();
        t.type = m.value("type").toString();
        t.lang = m.value("lang").toString();
        t.title = m.value("title").toString();
        t.codec = m.value("codec").toString();
        t.selected = m.value("selected").toBool();
        t.external = m.value("external").toBool();
        out << t;
    }
    return out;
}

void MpvWidget::stop() { command({"stop"}); }

void MpvWidget::play(const QString &url) { command({"loadfile", url, "replace"}); }

void MpvWidget::screenshotToFile(const QString &path) {
    command({"screenshot-to-file", path});
}

// ----------------------------------------------------------------- 鼠标
void MpvWidget::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) emit clicked();
    else if (e->button() == Qt::RightButton) emit rightClicked(e->globalPosition().toPoint());
}

void MpvWidget::mouseDoubleClickEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) emit doubleClicked();
}

} // namespace iptv
