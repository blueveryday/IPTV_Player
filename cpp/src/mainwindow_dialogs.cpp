// 关于 / 流编码信息 / 回看参数设置
#include "mainwindow.h"

#include "config.h"
#include "widgets.h"
#include "mpvwidget.h"
#include <QComboBox>

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

namespace iptv {

using namespace Colors;

void MainWindow::showAbout() {
    QDialog dlg(this);
    dlg.setWindowTitle("关于");
    auto *v = new QVBoxLayout(&dlg);
    v->setContentsMargins(20, 16, 20, 14);
    auto *title = new QLabel(kCodeVersion);
    QFont f = title->font();
    f.setBold(true);
    f.setPointSize(11);
    title->setFont(f);
    v->addWidget(title);
    v->addSpacing(8);
    auto *gl = new QLabel("GitHub 仓库：");
    gl->setObjectName("dim");
    v->addWidget(gl);
    auto *link = new QLabel(QString("<a style=\"color:%1;text-decoration:none\" href=\"%2\">%2</a>")
                                .arg(accent, kGithubUrl));
    link->setTextFormat(Qt::RichText);
    link->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    link->setOpenExternalLinks(false);
    link->setCursor(Qt::PointingHandCursor);
    link->setMinimumWidth(460);
    link->setStyleSheet(QString("QLabel{background:%1;padding:6px 8px;}").arg(bg_input));
    QObject::connect(link, &QLabel::linkActivated, &dlg, [&](const QString &u) {
        if (!QDesktopServices::openUrl(QUrl(u)))
            QMessageBox::critical(&dlg, "错误", "无法打开浏览器");
    });
    v->addWidget(link);
    auto *bf = new QHBoxLayout;
    bf->addStretch(1);
    auto *b = new QPushButton("关闭");
    b->setFocusPolicy(Qt::NoFocus);
    QObject::connect(b, &QPushButton::clicked, &dlg, &QDialog::accept);
    bf->addWidget(b);
    bf->addStretch(1);
    v->addSpacing(6);
    v->addLayout(bf);
    dlg.setFixedSize(dlg.sizeHint());
    dlg.exec();
}

// ---------------------------------------------------------------- 流信息
void MainWindow::showStreamInfo() {
    const QString url = m_currentUrl;
    if (url.isEmpty()) {
        QMessageBox::information(this, "提示", "没有正在播放的内容");
        return;
    }
    const QString ffprobe = findFfprobe(findFfmpeg());
    if (ffprobe.isEmpty()) {
        QMessageBox::critical(this, "缺少 ffprobe",
                              "未找到 ffprobe，无法分析流编码信息。\n\n"
                              "请安装 ffmpeg 并加入 PATH，或把 ffprobe 放在程序目录下，例如：\n" +
                                  appDir() + "/ffprobe");
        return;
    }
    setStatus("正在分析流编码信息…");
    QThread *t = QThread::create([this, ffprobe, url] {
        const QJsonObject data = ffprobeFullInfo(ffprobe, url, 15);
        QMetaObject::invokeMethod(
            this, [this, url, data] { showStreamInfoDialog(url, data); }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

static QString sv(const QJsonValue &v) {
    if (v.isString()) return v.toString();
    if (v.isDouble()) {
        const double d = v.toDouble();
        return d == qint64(d) ? QString::number(qint64(d)) : QString::number(d);
    }
    if (v.isBool()) return v.toBool() ? "True" : "False";
    return QString();
}

static bool hasv(const QJsonValue &v) {
    if (v.isUndefined() || v.isNull()) return false;
    if (v.isString()) return !v.toString().isEmpty();
    if (v.isDouble()) return v.toDouble() != 0;
    return true;
}

static QString fmtBitrate(const QJsonValue &v) {
    bool ok = false;
    const qint64 bps = sv(v).toLongLong(&ok);
    if (!ok) return "?";
    if (bps >= 1000000) return QString("%1 Mb/s").arg(bps / 1000000.0, 0, 'f', 3);
    return QString("%1 kb/s").arg(bps / 1000.0, 0, 'f', 1);
}

static QString fmtDuration(const QJsonValue &v) {
    bool ok = false;
    const qint64 sec = qint64(sv(v).toDouble(&ok));
    if (!ok) return "?";
    return QString("%1:%2:%3")
        .arg(sec / 3600, 2, 10, QChar('0'))
        .arg((sec % 3600) / 60, 2, 10, QChar('0'))
        .arg(sec % 60, 2, 10, QChar('0'));
}

QString MainWindow::formatStreamInfo(const QJsonObject &data) const {
    QStringList L;
    const QJsonObject fmt = data.value("format").toObject();
    const QJsonArray streams = data.value("streams").toArray();

    L << "=== 容器 / 格式 ===";
    if (hasv(fmt["format_long_name"])) L << "格式名   : " + sv(fmt["format_long_name"]);
    if (hasv(fmt["format_name"])) L << "短名     : " + sv(fmt["format_name"]);
    if (hasv(fmt["duration"])) L << "时长     : " + fmtDuration(fmt["duration"]);
    if (hasv(fmt["bit_rate"])) L << "总码率   : " + fmtBitrate(fmt["bit_rate"]);
    if (hasv(fmt["size"])) {
        bool ok = false;
        const qint64 sz = sv(fmt["size"]).toLongLong(&ok);
        if (ok) L << QString("大小     : %1 MB").arg(sz / 1048576.0, 0, 'f', 2);
    }
    if (hasv(fmt["nb_streams"])) L << "流数量   : " + sv(fmt["nb_streams"]);
    L << "";

    int vi = 0, ai = 0;
    for (const QJsonValue &sval : streams) {
        const QJsonObject st = sval.toObject();
        const QString ct = st["codec_type"].toString();
        if (ct == "video") L << QString("=== 视频流 #%1（索引 %2） ===").arg(++vi).arg(sv(st["index"]));
        else if (ct == "audio") L << QString("=== 音频流 #%1（索引 %2） ===").arg(++ai).arg(sv(st["index"]));
        else L << QString("=== %1 流（索引 %2） ===").arg(ct.isEmpty() ? "?" : ct, sv(st["index"]));

        QString codec = st["codec_name"].toString();
        if (codec.isEmpty()) codec = "未知";
        if (!st["profile"].toString().isEmpty()) codec += " (" + st["profile"].toString() + ")";
        L << "编码     : " + codec;

        if (ct == "video") {
            if (hasv(st["width"]) && hasv(st["height"]))
                L << QString("分辨率   : %1x%2").arg(sv(st["width"]), sv(st["height"]));
            if (hasv(st["pix_fmt"])) L << "像素格式 : " + sv(st["pix_fmt"]);
            QString fr = st["avg_frame_rate"].toString();
            if (fr.isEmpty()) fr = st["r_frame_rate"].toString();
            if (!fr.isEmpty() && fr != "0/0") {
                const QStringList p = fr.split('/');
                bool o1 = false, o2 = false;
                double n = p.value(0).toDouble(&o1), d = p.value(1).toDouble(&o2);
                if (p.size() == 2 && o1 && o2 && d != 0) L << QString("帧率     : %1 fps").arg(n / d, 0, 'f', 3);
                else L << "帧率     : " + fr;
            }
            if (hasv(st["color_space"])) L << "色彩空间 : " + sv(st["color_space"]);
            if (hasv(st["color_range"])) L << "色彩范围 : " + sv(st["color_range"]);
            if (st.contains("level") && !st["level"].isNull()) L << "级别     : " + sv(st["level"]);
            if (hasv(st["field_order"]) && st["field_order"].toString() != "progressive")
                L << "场序     : " + sv(st["field_order"]);
        } else if (ct == "audio") {
            if (hasv(st["sample_rate"])) L << "采样率   : " + sv(st["sample_rate"]) + " Hz";
            if (st.contains("channels") && !st["channels"].isNull()) L << "声道数   : " + sv(st["channels"]);
            if (hasv(st["channel_layout"])) L << "声道布局 : " + sv(st["channel_layout"]);
            if (hasv(st["sample_fmt"])) L << "采样格式 : " + sv(st["sample_fmt"]);
        }
        if (hasv(st["bit_rate"])) L << "码率     : " + fmtBitrate(st["bit_rate"]);
        const QString lang = st["tags"].toObject()["language"].toString();
        if (!lang.isEmpty()) L << "语言     : " + lang;
        if (st["disposition"].toObject()["default"].toInt() != 0) L << "默认轨   : 是";
        L << "";
    }
    QString s = L.join('\n');
    while (s.endsWith('\n')) s.chop(1);
    return s + "\n";
}

void MainWindow::showStreamInfoDialog(const QString &url, const QJsonObject &data) {
    if (m_closing) return;
    if (data.isEmpty()) {
        setStatus("无法获取流编码信息");
        QMessageBox::critical(this, "获取失败",
                              "ffprobe 无法读取该流的编码信息。\n\n可能原因：\n"
                              "• 地址需要鉴权（回看地址请先播放再查看）\n"
                              "• 流被限制并发连接\n"
                              "• 网络超时或该源不响应分析");
        return;
    }
    setStatus("流编码信息已获取");
    QDialog dlg(this);
    dlg.setWindowTitle("流编码信息");
    dlg.resize(680, 425);
    auto *v = new QVBoxLayout(&dlg);
    v->setContentsMargins(10, 10, 10, 10);
    auto *top = new QHBoxLayout;
    auto *l1 = new QLabel("地址：");
    l1->setObjectName("dim");
    top->addWidget(l1);
    auto *l2 = new QLabel(url);
    l2->setWordWrap(true);
    l2->setStyleSheet(QString("color:%1;").arg(accent));
    l2->setTextInteractionFlags(Qt::TextSelectableByMouse);
    top->addWidget(l2, 1);
    v->addLayout(top);
    auto *txt = new QPlainTextEdit;
    txt->setReadOnly(true);
    QFont mf("Consolas");
    mf.setStyleHint(QFont::Monospace);
    mf.setPointSize(9);
    txt->setFont(mf);
    txt->setPlainText(formatStreamInfo(data));
    v->addWidget(txt, 1);
    auto *bf = new QHBoxLayout;
    bf->addStretch(1);
    auto *bc = new QPushButton("复制全部");
    auto *bx = new QPushButton("关闭");
    bc->setFocusPolicy(Qt::NoFocus);
    bx->setFocusPolicy(Qt::NoFocus);
    bf->addWidget(bc);
    bf->addWidget(bx);
    bf->addStretch(1);
    v->addLayout(bf);
    QObject::connect(bc, &QPushButton::clicked, &dlg, [&] {
        QGuiApplication::clipboard()->setText(txt->toPlainText().trimmed());
        setStatus("流编码信息已复制到剪贴板");
    });
    QObject::connect(bx, &QPushButton::clicked, &dlg, &QDialog::accept);
    dlg.exec();
}

// ---------------------------------------------------------------- 回看参数
void MainWindow::openSettings() {
    QDialog dlg(this);
    dlg.setWindowTitle("回看参数设置");
    auto *g = new QGridLayout(&dlg);
    struct Row {
        const char *label, *key;
    };
    const Row rows[] = {{"userid", "userid"},
                        {"AuthInfo", "authinfo"},
                        {"时区偏移(小时, 重庆=8)", "tz_offset"},
                        {"可回看天数(含今天)", "replay_days"},
                        {"回看地址模板", "template"},
                        {"HTTP 回看关键字(逗号分隔, 空=全部)", "http_replay_keys"},
                        {"外部播放器路径(可空，如 PotPlayer)", "replay_player"}};
    QMap<QString, QLineEdit *> ed;
    int r = 0;
    for (const Row &row : rows) {
        auto *l = new QLabel(row.label);
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        g->addWidget(l, r, 0);
        const QJsonValue v = m_cfg.value(row.key);
        QString s;
        if (v.isArray()) {
            QStringList parts;
            for (const auto &x : v.toArray()) parts << x.toString();
            s = parts.join(',');
        } else if (v.isDouble()) {
            s = QString::number(v.toDouble(), 'g', 15);
        } else {
            s = v.toString();
        }
        auto *e = new QLineEdit(s);
        e->setMinimumWidth(380);
        g->addWidget(e, r, 1);
        ed[row.key] = e;
        ++r;
    }
    auto *bf = new QHBoxLayout;
    bf->addStretch(1);
    auto *bo = new QPushButton("确定");
    auto *bc = new QPushButton("取消");
    bo->setFocusPolicy(Qt::NoFocus);
    bc->setFocusPolicy(Qt::NoFocus);
    bf->addWidget(bo);
    bf->addWidget(bc);
    bf->addStretch(1);
    g->addLayout(bf, r + 1, 0, 1, 2);

    QObject::connect(bo, &QPushButton::clicked, &dlg, [&] {
        bool ok1 = false, ok2 = false;
        const int tz = ed["tz_offset"]->text().trimmed().toInt(&ok1);
        const int days = ed["replay_days"]->text().trimmed().toInt(&ok2);
        if (!ok1 || !ok2) {
            QMessageBox::critical(&dlg, "错误", "时区偏移与回看天数必须为整数");
            return;
        }
        m_cfg["template"] = ed["template"]->text().trimmed();
        QString rp = ed["replay_player"]->text().trimmed();
        while (rp.startsWith('"')) rp.remove(0, 1);
        while (rp.endsWith('"')) rp.chop(1);
        m_cfg["replay_player"] = rp;
        QJsonArray keys;
        for (const QString &k : ed["http_replay_keys"]->text().trimmed().split(',')) {
            const QString t = k.trimmed();
            if (!t.isEmpty()) keys.append(t);
        }
        m_cfg["http_replay_keys"] = keys;
        m_cfg["userid"] = ed["userid"]->text().trimmed();
        m_cfg["authinfo"] = ed["authinfo"]->text().trimmed();
        m_cfg["tz_offset"] = tz;
        m_cfg["replay_days"] = qMax(1, days);
        epg::setTz(tz, qMax(1, days));
        saveCfg();
        m_dateCb->clear();
        m_dateCb->addItems(dateChoices());
        m_dateCb->setCurrentIndex(0);
        refreshSlots();
        startEpgUpdate(true);
        dlg.accept();
    });
    QObject::connect(bc, &QPushButton::clicked, &dlg, &QDialog::reject);
    dlg.exec();
}

} // namespace iptv
