// EPG：日期/节目单、更新任务、着色、别名/网络源/下载参数对话框
#include "mainwindow.h"

#include "config.h"
#include "widgets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QThread>
#include <QVBoxLayout>

namespace iptv {

using namespace Colors;

// ===================================================================== 日期
QVector<QPair<QDate, QString>> MainWindow::dateList() const {
    const QDate today = localNow().date();
    const int n = qMax(1, cfgInt(m_cfg, "replay_days", 7));
    QVector<QPair<QDate, QString>> r;
    for (int i = 0; i < n; ++i) {
        const QDate d = today.addDays(-i);
        r.append({d, QString("%1 星期%2")
                         .arg(d.toString("yyyy-MM-dd"))
                         .arg(kWeekdayCn.at(d.dayOfWeek() - 1))});
    }
    return r;
}

QStringList MainWindow::dateChoices() const {
    QStringList l;
    for (const auto &p : dateList()) l << p.second;
    return l;
}

QDate MainWindow::earliestDate() const {
    return localNow().date().addDays(-(qMax(1, cfgInt(m_cfg, "replay_days", 7)) - 1));
}

QDate MainWindow::selectedDate() const {
    const QString txt = m_dateCb->currentText().trimmed();
    const auto m = QRegularExpression("^(\\d{4}-\\d{2}-\\d{2})").match(txt);
    if (!m.hasMatch()) return QDate();
    return QDate::fromString(m.captured(1), "yyyy-MM-dd");
}

QVector<MainWindow::DaySlot> MainWindow::buildDaySlots(const ChannelPtr &ch, const QDate &day,
                                                       bool *fromEpg) {
    QVector<DaySlot> out;
    QVector<epg::Prog> progs;
    if (ch) progs = epg::loadEpg(ch->name, day, ch->url);
    if (!progs.isEmpty()) {
        for (const auto &p : progs)
            out.append({QString("%1  %2").arg(fmtRange(p.start, p.end), p.title), p.start, p.end});
        *fromEpg = true;
        return out;
    }
    const QDateTime base = naiveFromDate(day);
    for (int h = 0; h < 24; ++h) {
        const QDateTime s = base.addSecs(h * 3600), e = s.addSecs(3600);
        out.append({fmtRange(s, e), s, e});
    }
    *fromEpg = false;
    return out;
}

int MainWindow::replayAllowed() const {
    const ChannelPtr ch = selectedChannel();
    if (!ch) return -1;
    if (ch->webdav) return 0;
    return replaySupported(ch->url, m_cfg) ? 1 : 0;
}

void MainWindow::onDateSelected() { refreshSlots(); }

void MainWindow::stepDate(int delta) {
    const int cnt = m_dateCb->count();
    if (cnt == 0) return;
    const int i = qBound(0, m_dateCb->currentIndex() + delta, cnt - 1);
    if (i != m_dateCb->currentIndex()) {
        m_dateCb->setCurrentIndex(i);
        onDateSelected();
    }
}

void MainWindow::refreshSlots() {
    if (!m_slotList) return;
    QSignalBlocker blk(m_slotList);
    m_slotList->clear();
    m_slotItems.clear();

    if (m_webdavMode) {
        m_slotsLocked = true;
        m_epgLabel->setText("WebDAV 浏览模式");
        m_slotList->viewport()->unsetCursor();
        return;
    }
    const QDate day = selectedDate();
    const QDateTime now = localNow();
    const ChannelPtr ch = selectedChannel();

    if (ch && isLocalMedia(ch)) {
        m_slotsLocked = true;
        m_epgLabel->setText("本地媒体文件，无回看");
        m_slotList->viewport()->unsetCursor();
        return;
    }
    m_slotsLocked = (replayAllowed() == 0);
    if (!day.isValid()) return;

    bool fromEpg = false;
    QVector<DaySlot> daySlots = buildDaySlots(ch, day, &fromEpg);
    if (m_slotsLocked && !fromEpg) daySlots.clear();
    int curIdx = -1;
    for (int i = 0; i < daySlots.size(); ++i) {
        const DaySlot &s = daySlots[i];
        auto *it = new QListWidgetItem("  " + s.label);
        const bool isCurrent = s.start <= now && now < s.end;
        if (isCurrent) it->setForeground(QColor(live_green));
        else if (s.start >= now) it->setForeground(QColor(epg_future));
        m_slotList->addItem(it);
        m_slotItems.append({s.start, s.end});
        if (curIdx < 0 && isCurrent) curIdx = i;
    }

    if (!ch) m_epgLabel->setText("");
    else if (m_slotsLocked)
        m_epgLabel->setText(fromEpg ? QString("节目单：（%1 个节目）｜该频道不支持回看").arg(daySlots.size())
                                    : QString("无 EPG 数据（可在 选项→频道别名 中映射）"));
    else if (fromEpg) m_epgLabel->setText(QString("节目单：（%1 个节目）").arg(daySlots.size()));
    else m_epgLabel->setText("无 EPG 数据，按整点时段显示");

    if (m_slotsLocked) {
        m_slotList->clearSelection();
        m_slotList->viewport()->unsetCursor();
    } else {
        m_slotList->viewport()->setCursor(Qt::PointingHandCursor);
    }
    if (curIdx >= 0) m_slotList->scrollToItem(m_slotList->item(curIdx));
}

void MainWindow::refreshSlotsKeepSelection() {
    bool have = false;
    SlotItem keep;
    const int row = m_slotList->currentRow();
    if (row >= 0 && row < m_slotItems.size() && !m_slotList->selectedItems().isEmpty()) {
        keep = m_slotItems[row];
        have = true;
    }
    refreshSlots();
    if (have && !m_slotsLocked) {
        for (int i = 0; i < m_slotItems.size(); ++i) {
            if (m_slotItems[i].start == keep.start && m_slotItems[i].end == keep.end) {
                m_slotList->setCurrentRow(i);
                m_slotList->scrollToItem(m_slotList->item(i));
                break;
            }
        }
    }
}

bool MainWindow::selectedSlot(QDateTime *s, QDateTime *e) const {
    if (m_slotsLocked || replayAllowed() == 0) return false;
    if (m_slotList->selectedItems().isEmpty()) return false;
    const int r = m_slotList->currentRow();
    if (r < 0 || r >= m_slotItems.size()) return false;
    *s = m_slotItems[r].start;
    *e = m_slotItems[r].end;
    return true;
}

// ===================================================================== 着色
bool MainWindow::channelHasEpg(const ChannelPtr &ch) const {
    if (!ch || !m_epgColorValid) return false;
    if (ch->name.isEmpty()) return false;
    return m_epgColorSet.contains(epg::normName(ch->name));
}

void MainWindow::startEpgColorLoad() {
    if (m_epgColorValid || m_epgColorLoading) return;
    m_epgColorLoading = true;
    QThread *t = QThread::create([this] {
        QSet<QString> result;
        try {
            const epg::IndexPtr idx = epg::getIndex(true);
            if (idx) {
                for (auto it = idx->names.begin(); it != idx->names.end(); ++it)
                    result.insert(it.key());
            }
            const auto alias = epg::aliasCopy();
            for (auto it = alias.begin(); it != alias.end(); ++it)
                if (!it.key().isEmpty()) result.insert(epg::normName(it.key()));
        } catch (...) {
            result.clear();
        }
        QMetaObject::invokeMethod(
            this, [this, result] { if (!m_closing) onEpgColorLoaded(result); },
            Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

void MainWindow::onEpgColorLoaded(const QSet<QString> &s) {
    m_epgColorSet = s;
    m_epgColorValid = true;
    m_epgColorLoading = false;
    applyEpgColorsToList();
    refreshSlots();
}

void MainWindow::applyEpgColorsToList() {
    if (m_epgColorSet.isEmpty() || m_webdavMode) return;
    for (int i = 0; i < m_filtered.size() && i < m_chList->count(); ++i) {
        const QString nm = m_filtered[i]->name;
        if (!nm.isEmpty() && m_epgColorSet.contains(epg::normName(nm)))
            m_chList->item(i)->setForeground(QColor(live_green));
    }
}

// ===================================================================== 更新任务
void MainWindow::autoEpgUpdate() {
    if (m_closing) return;
    double hours = 12.0;
    if (m_cfg.contains("epg_update_hours")) hours = cfgDouble(m_cfg, "epg_update_hours", 12.0);
    if (cfgBool(m_cfg, "epg_auto_update", true) && hours > 0) {
        const double last = cfgDouble(m_cfg, "epg_last_update", 0.0);
        const bool busy = m_epgState && !m_epgState->finished;
        const double nowS = QDateTime::currentSecsSinceEpoch();
        if (!busy && nowS - last >= hours * 3600) startEpgUpdate(true);
    }
    m_epgAutoTimer.start(600000);
}

void MainWindow::startEpgUpdate(bool silent) {
    if (m_epgState && !m_epgState->finished) {
        if (!silent) QMessageBox::information(this, "提示", "EPG 正在更新中，请稍候。");
        return;
    }
    const QDate today = localNow().date();
    const int n = qMax(1, cfgInt(m_cfg, "replay_days", 7));
    QSet<QString> force;
    if (!silent) force.insert(today.toString("yyyyMMdd"));
    int threads = qBound(1, cfgInt(m_cfg, "epg_threads", epg::kThreads), 10);
    int timeout = qBound(3, cfgInt(m_cfg, "epg_timeout", epg::kTimeout), 10);

    auto st = std::make_shared<epg::UpdateState>();
    st->csv = epg::csvPath(m_cfg);
    st->threads = threads;
    st->timeout = timeout;
    st->tz = cfgInt(m_cfg, "tz_offset", 8);
    for (const auto &v : m_cfg.value("epg_sources").toArray()) st->sources << v.toString();
    st->urlCfg = m_cfg;
    st->silent = silent;
    m_epgState = st;

    m_epgThread = QThread::create([st, today, n, force] { epg::updateWorker(st, today, n, force); });
    connect(m_epgThread, &QThread::finished, m_epgThread, &QObject::deleteLater);
    connect(m_epgThread, &QObject::destroyed, this, [this] { m_epgThread = nullptr; });
    m_epgThread->start();
    if (!silent) setStatus("正在更新 EPG…");
    m_epgPollTimer.start(500);
}

void MainWindow::pollEpg() {
    auto st = m_epgState;
    if (!st || m_closing) return;
    if (!st->finished) {
        if (!st->silent && st->total > 0)
            setStatus(QString("正在更新 EPG… %1 / %2").arg(int(st->done)).arg(int(st->total)));
        m_epgPollTimer.start(500);
        return;
    }
    const bool changed = st->ok > 0 || st->removed > 0 || st->built;
    const QString error = st->error();
    if (error.isEmpty()) {
        double hrs = cfgDouble(m_cfg, "epg_update_hours", 12.0);
        const double nowS = QDateTime::currentSecsSinceEpoch();
        if (st->fail == 0 && st->srcFail == 0) m_cfg["epg_last_update"] = nowS;
        else m_cfg["epg_last_update"] = nowS - qMax(0.0, hrs - 1.0) * 3600;
        saveCfg();
    }
    if (changed) {
        m_epgColorSet.clear();
        m_epgColorValid = false;
        m_epgColorLoading = false;
        startEpgColorLoad();
        refreshSlotsKeepSelection();
    }
    QString summary = QString("EPG 更新完成：新增 %1，无数据 %2，失败 %3，清理过期 %4")
                          .arg(int(st->ok)).arg(int(st->nodata)).arg(int(st->fail)).arg(int(st->removed));
    if (st->built) summary += QString("；iptv_epg.xml 共 %1 个节目").arg(int(st->xmlCount));
    if (!st->sources.isEmpty())
        summary += QString("；外部源 成功 %1 / 失败 %2").arg(int(st->srcOk)).arg(int(st->srcFail));

    if (st->silent) {
        if (error.isEmpty() && changed && m_currentUrl.isEmpty() && !m_recording) setStatus(summary);
    } else if (!error.isEmpty()) {
        setStatus("EPG 更新失败：" + error);
        QMessageBox::warning(this, "EPG 更新失败", error);
    } else {
        setStatus(summary);
        QMessageBox::information(this, "EPG 下载", summary);
    }
}

// ===================================================================== 对话框辅助
static QPushButton *dlgBtn(const QString &t) {
    auto *b = new QPushButton(t);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

static QString jsonToStr(const QJsonValue &v) {
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(v.toDouble(), 'g', 15);
    if (v.isBool()) return v.toBool() ? "true" : "false";
    return QString();
}

// ===================================================================== 频道别名
void MainWindow::openEpgAlias() {
    const epg::IndexPtr idx = epg::getIndex(false);
    QStringList labels;
    QHash<QString, QSet<QString>> labelSources;
    QHash<QString, QSet<QString>> names;
    if (idx) {
        labels = idx->labels;
        labelSources = idx->labelSources;
        names = idx->names;
    }
    if (labels.isEmpty()) {
        QMessageBox::information(this, "提示", "还没有 EPG 数据，请先通过 选项→下载 EPG 获取。");
        return;
    }
    QStringList chans;
    for (const auto &c : m_channels)
        if (!c->name.isEmpty() && !c->local && !chans.contains(c->name)) chans << c->name;
    if (chans.isEmpty()) {
        QMessageBox::information(this, "提示", "左侧频道列表为空，请先加载 m3u。");
        return;
    }
    QMap<QString, QString> alias = epg::aliasCopy();

    QStringList candidates;
    for (const QString &lb : labels) {
        QStringList srcs = labelSources.value(lb).values();
        srcs.sort();
        if (srcs.isEmpty()) candidates << lb;
        else
            for (const QString &s : srcs) candidates << lb + "_" + s;
    }
    QVector<QPair<QString, QString>> normed;
    for (const QString &c : candidates) normed.append({c, epg::normName(c)});

    QDialog dlg(this);
    dlg.setWindowTitle("频道别名（EPG 映射）");
    auto *g = new QGridLayout(&dlg);
    auto *tip = new QLabel("左侧选频道，右侧选对应的 EPG 频道名。"
                           "★ 已手动映射   ✓ 自动匹配   ✗ 无 EPG；"
                           "候选频道名后缀 _文件名 表示该 EPG 数据来自哪个 XML 源。");
    tip->setObjectName("dim");
    tip->setWordWrap(true);
    g->addWidget(tip, 0, 0, 1, 2);

    auto *lb = new QListWidget;
    lb->setStyleSheet(QString("QListWidget{background:%1;}").arg(bg_input));
    lb->setMinimumWidth(380);
    lb->setMinimumHeight(300);
    g->addWidget(lb, 1, 0);

    auto *rf = new QVBoxLayout;
    auto *l1 = new QLabel(QString("EPG 频道名（共 %1 个）").arg(candidates.size()));
    l1->setObjectName("dim");
    rf->addWidget(l1);
    auto *l2 = new QLabel("筛选（输入关键字缩小下拉范围）");
    l2->setObjectName("dim");
    rf->addSpacing(8);
    rf->addWidget(l2);
    auto *fedit = new QLineEdit;
    rf->addWidget(fedit);
    auto *cb = new QComboBox;
    cb->setMinimumWidth(300);
    cb->addItems(candidates.mid(0, 2000));
    cb->setCurrentIndex(-1);
    rf->addSpacing(6);
    rf->addWidget(cb);
    auto *bf = new QHBoxLayout;
    auto *bClear = dlgBtn("清除映射");
    auto *bSave = dlgBtn("保存");
    auto *bCancel = dlgBtn("取消");
    bf->addWidget(bClear);
    bf->addWidget(bSave);
    bf->addWidget(bCancel);
    rf->addSpacing(14);
    rf->addLayout(bf);
    rf->addStretch(1);
    g->addLayout(rf, 1, 1);

    auto stateOf = [&](const QString &nm) -> QString {
        if (!alias.value(nm).isEmpty()) return "★";
        return names.contains(epg::normName(nm)) ? "✓" : "✗";
    };
    auto rowText = [&](const QString &nm) {
        QString t = stateOf(nm) + " " + nm;
        return alias.value(nm).isEmpty() ? t : t + "  →  " + alias.value(nm);
    };
    auto paint = [&](int i) {
        lb->item(i)->setForeground(QColor(stateOf(chans[i]) == "✗" ? epg_future : fg_primary));
    };
    for (int i = 0; i < chans.size(); ++i) {
        lb->addItem(rowText(chans[i]));
        paint(i);
    }
    auto redraw = [&](int i) {
        QSignalBlocker b(lb);
        lb->item(i)->setText(rowText(chans[i]));
        paint(i);
        lb->setCurrentRow(i);
        lb->scrollToItem(lb->item(i));
    };

    QObject::connect(lb, &QListWidget::currentRowChanged, &dlg, [&](int i) {
        if (i < 0) return;
        const QString a = alias.value(chans[i]);
        const int ci = cb->findText(a);
        if (a.isEmpty()) cb->setCurrentIndex(-1);
        else if (ci >= 0) cb->setCurrentIndex(ci);
        else {
            cb->setEditable(false);
            cb->insertItem(0, a);
            cb->setCurrentIndex(0);
        }
    });
    QObject::connect(cb, QOverload<int>::of(&QComboBox::activated), &dlg, [&](int) {
        const int i = lb->currentRow();
        const QString v = cb->currentText();
        if (i < 0 || v.isEmpty()) return;
        alias[chans[i]] = v;
        redraw(i);
    });
    QObject::connect(fedit, &QLineEdit::textChanged, &dlg, [&](const QString &txt) {
        const QString k = epg::normName(txt);
        QStringList vals;
        if (k.isEmpty()) vals = candidates;
        else
            for (const auto &p : normed)
                if (p.second.contains(k)) vals << p.first;
        cb->clear();
        cb->addItems(vals.mid(0, 2000));
        cb->setCurrentIndex(-1);
    });
    QObject::connect(bClear, &QPushButton::clicked, &dlg, [&] {
        const int i = lb->currentRow();
        if (i < 0) return;
        alias.remove(chans[i]);
        cb->setCurrentIndex(-1);
        redraw(i);
    });
    QObject::connect(bCancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(bSave, &QPushButton::clicked, &dlg, [&] {
        if (!epg::saveAliasFile(alias)) {
            QMessageBox::critical(&dlg, "错误",
                                  QString("保存别名失败（无法写入 %1）").arg(epgAliasFile()));
            return;
        }
        epg::setAlias(alias);
        m_epgColorSet.clear();
        m_epgColorValid = false;
        m_epgColorLoading = false;
        startEpgColorLoad();
        refreshSlots();
        dlg.accept();
    });
    dlg.exec();
}

// ===================================================================== EPG 网络源
void MainWindow::openEpgSources(QWidget *parent) {
    QWidget *par = parent ? parent : this;
    QDialog dlg(par);
    dlg.setWindowTitle("EPG 网络源管理");
    auto *g = new QGridLayout(&dlg);
    auto *tip = new QLabel("每个地址对应一个 XMLTV 文件（.xml 或 .xml.gz，自动解压），"
                           "保存在 src/epg/，重名自动改名。");
    tip->setObjectName("dim");
    tip->setWordWrap(true);
    g->addWidget(tip, 0, 0, 1, 2);

    auto *lb = new QListWidget;
    lb->setSelectionMode(QAbstractItemView::ExtendedSelection);
    lb->setStyleSheet(QString("QListWidget{background:%1;}").arg(bg_input));
    lb->setMinimumWidth(520);
    lb->setMinimumHeight(150);
    for (const auto &v : m_cfg.value("epg_sources").toArray()) lb->addItem(v.toString());
    g->addWidget(lb, 1, 0, 1, 2);

    auto *ent = new QLineEdit;
    g->addWidget(ent, 2, 0);
    auto *bAdd = dlgBtn("添加");
    g->addWidget(bAdd, 2, 1);

    auto *sch = new QHBoxLayout;
    auto *autoCb = new QCheckBox("自动更新 EPG，间隔（小时）：");
    autoCb->setChecked(cfgBool(m_cfg, "epg_auto_update", true));
    sch->addWidget(autoCb);
    double h0 = cfgDouble(m_cfg, "epg_update_hours", 12);
    if (h0 <= 0) h0 = 12;
    auto *hrs = new QLineEdit(QString::number(h0, 'g', 6));
    hrs->setFixedWidth(60);
    sch->addWidget(hrs);
    sch->addStretch(1);
    g->addLayout(sch, 3, 0, 1, 2);

    const double last = cfgDouble(m_cfg, "epg_last_update", 0);
    auto *lastLb = new QLabel("上次更新：" + (last > 0 ? QDateTime::fromSecsSinceEpoch(qint64(last))
                                                             .toString("yyyy-MM-dd HH:mm")
                                                       : QString("从未")));
    lastLb->setObjectName("dim");
    g->addWidget(lastLb, 4, 0, 1, 2);

    auto *bf = new QHBoxLayout;
    auto *bBrowse = dlgBtn("本地文件…");
    auto *bDel = dlgBtn("删除选中");
    auto *bSave = dlgBtn("保存");
    auto *bSaveDl = dlgBtn("保存并下载");
    auto *bCancel = dlgBtn("取消");
    for (auto *b : {bBrowse, bDel, bSave, bSaveDl, bCancel}) bf->addWidget(b);
    g->addLayout(bf, 5, 0, 1, 2);

    auto add = [&] {
        QString s = ent->text().trimmed();
        if (s.startsWith('"')) s.remove(0, 1);
        if (s.endsWith('"')) s.chop(1);
        if (s.isEmpty()) return;
        const QString low = s.toLower();
        if (!(low.startsWith("http://") || low.startsWith("https://") || QFileInfo(s).isFile())) {
            QMessageBox::critical(&dlg, "错误", "地址无效（需 http(s):// 或本地文件）");
            return;
        }
        bool dup = false;
        for (int i = 0; i < lb->count(); ++i)
            if (lb->item(i)->text() == s) dup = true;
        if (!dup) lb->addItem(s);
        ent->clear();
    };
    QObject::connect(bAdd, &QPushButton::clicked, &dlg, add);
    QObject::connect(ent, &QLineEdit::returnPressed, &dlg, add);
    QObject::connect(bDel, &QPushButton::clicked, &dlg, [&] {
        qDeleteAll(lb->selectedItems());
    });
    QObject::connect(bBrowse, &QPushButton::clicked, &dlg, [&] {
        const QString p = QFileDialog::getOpenFileName(&dlg, "选择 EPG 文件", QString(),
                                                       "EPG 文件 (*.xml *.gz);;所有文件 (*.*)");
        if (!p.isEmpty()) ent->setText(p);
    });
    auto save = [&](bool download) {
        if (m_epgState && !m_epgState->finished) {
            QMessageBox::information(&dlg, "提示", "EPG 正在更新中，请稍后再修改。");
            return;
        }
        bool ok = false;
        const double h = hrs->text().trimmed().toDouble(&ok);
        if (!ok || h <= 0) {
            QMessageBox::critical(&dlg, "错误", "更新间隔必须是大于 0 的数字（小时）");
            return;
        }
        QStringList nw;
        for (int i = 0; i < lb->count(); ++i) nw << lb->item(i)->text();
        QStringList old;
        for (const auto &v : m_cfg.value("epg_sources").toArray()) old << v.toString();
        const bool changed = nw != old;
        m_cfg["epg_sources"] = QJsonArray::fromStringList(nw);
        m_cfg["epg_auto_update"] = autoCb->isChecked();
        m_cfg["epg_update_hours"] = h;
        if (!epg::saveSourcesFile(nw)) {
            QMessageBox::critical(&dlg, "错误", QString("无法写入 %1").arg(epgSourcesFile()));
            return;
        }
        saveCfg();
        dlg.accept();
        if (download || changed) startEpgUpdate(!download);
    };
    QObject::connect(bSave, &QPushButton::clicked, &dlg, [&] { save(false); });
    QObject::connect(bSaveDl, &QPushButton::clicked, &dlg, [&] { save(true); });
    QObject::connect(bCancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    dlg.exec();
}

// ===================================================================== EPG 下载参数
void MainWindow::openEpgSettings() {
    QDialog dlg(this);
    dlg.setWindowTitle("自定义 EPG 下载参数");
    auto *g = new QGridLayout(&dlg);

    const QString sample = "00000001000000050000000000000476";
    const QString todayStr = localNow().toString("yyyyMMdd");
    const QJsonObject defs = defaultConfig();
    struct Row {
        const char *label;
        const char *key;
    };
    const Row rows[] = {{"服务器地址", "epg_host"},
                        {"路径模板", "epg_path"},
                        {"日期格式（strftime）", "epg_date_fmt"},
                        {"频道表 CSV（空=src/channel_epg_chongqing.csv）", "epg_csv"},
                        {"下载线程数（1-10）", "epg_threads"},
                        {"超时秒数（3-10）", "epg_timeout"}};
    QMap<QString, QLineEdit *> edits;
    int r = 0;
    for (const Row &row : rows) {
        auto *lb = new QLabel(row.label);
        lb->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        g->addWidget(lb, r, 0);
        const QJsonValue cur = m_cfg.contains(row.key) ? m_cfg.value(row.key) : defs.value(row.key);
        auto *e = new QLineEdit(jsonToStr(cur));
        e->setMinimumWidth(420);
        g->addWidget(e, r, 1);
        edits[row.key] = e;
        ++r;
    }
    auto *bBrowse = dlgBtn("浏览…");
    g->addWidget(bBrowse, 3, 2);
    QObject::connect(bBrowse, &QPushButton::clicked, &dlg, [&] {
        const QString p = QFileDialog::getOpenFileName(&dlg, "选择频道表", QString(),
                                                       "CSV 文件 (*.csv);;所有文件 (*.*)");
        if (!p.isEmpty()) edits["epg_csv"]->setText(p);
    });
    auto *bSrc = dlgBtn("设置 EPG 网络源（xml / gz）…");
    g->addWidget(bSrc, r, 1, Qt::AlignLeft);
    QObject::connect(bSrc, &QPushButton::clicked, &dlg, [&] { openEpgSources(&dlg); });
    ++r;

    auto *help = new QLabel(
        "可用变量：{channelcode} 频道代码，{date} 按“日期格式”生成的日期。\n"
        "最终地址 = 服务器地址 + 路径模板（路径模板可含 ?参数）。\n"
        "下载内容合并保存为 src/epg/iptv_epg.xml（不保留 JSON）；外部 XML/GZ 源自动解压后\n"
        "保存在同目录（重名自动改名）。频道冲突时以 iptv_epg.xml 为准。\n"
        "频道别名保存在 src/epg/alias.json。");
    help->setObjectName("dim");
    g->addWidget(help, r++, 0, 1, 3);
    auto *pvLb = new QLabel("地址预览");
    pvLb->setObjectName("dim");
    pvLb->setAlignment(Qt::AlignRight | Qt::AlignTop);
    g->addWidget(pvLb, r, 0);
    auto *pv = new QLabel;
    pv->setWordWrap(true);
    pv->setStyleSheet(QString("color:%1;").arg(accent));
    pv->setMaximumWidth(520);
    g->addWidget(pv, r++, 1, 1, 2);

    auto collectRaw = [&] {
        QJsonObject c;
        for (auto it = edits.begin(); it != edits.end(); ++it) c[it.key()] = it.value()->text().trimmed();
        return c;
    };
    auto preview = [&] {
        try {
            pv->setText(epg::makeEpgUrl(collectRaw(), sample, todayStr));
        } catch (const std::exception &e) {
            pv->setText(QString("（格式有误：%1）").arg(QString::fromUtf8(e.what())));
        }
    };
    for (auto *e : edits) QObject::connect(e, &QLineEdit::textChanged, &dlg, preview);
    preview();

    auto collect = [&](QJsonObject *out, QString *err) -> bool {
        QJsonObject c = collectRaw();
        QString csv = c["epg_csv"].toString();
        if (csv.startsWith('"')) csv.remove(0, 1);
        if (csv.endsWith('"')) csv.chop(1);
        c["epg_csv"] = csv;
        const QString host = c["epg_host"].toString().toLower();
        if (!(host.startsWith("http://") || host.startsWith("https://"))) {
            *err = "服务器地址必须以 http:// 或 https:// 开头";
            return false;
        }
        for (const char *var : {"{channelcode}", "{date}"}) {
            if (!c["epg_path"].toString().contains(var)) {
                *err = QString("路径模板必须包含 %1").arg(var);
                return false;
            }
        }
        bool ok1 = false, ok2 = false;
        const int th = c["epg_threads"].toString().toInt(&ok1);
        const int to = c["epg_timeout"].toString().toInt(&ok2);
        if (!ok1 || !ok2) {
            *err = "线程数和超时秒数必须为整数";
            return false;
        }
        c["epg_threads"] = qBound(1, th, 10);
        c["epg_timeout"] = qBound(3, to, 10);
        if (c["epg_date_fmt"].toString().isEmpty()) c["epg_date_fmt"] = "%Y%m%d";
        try {
            epg::makeEpgUrl(c, sample, todayStr);
        } catch (const std::exception &e) {
            *err = QString("地址模板有误（含未知变量或花括号不匹配）：%1").arg(QString::fromUtf8(e.what()));
            return false;
        }
        *out = c;
        return true;
    };

    auto save = [&](bool download) {
        if (m_epgState && !m_epgState->finished) {
            QMessageBox::information(&dlg, "提示", "EPG 正在更新中，请稍后再修改。");
            return;
        }
        QJsonObject c;
        QString err;
        if (!collect(&c, &err)) {
            QMessageBox::critical(&dlg, "错误", err);
            return;
        }
        bool changed = false;
        for (const char *k : {"epg_host", "epg_path", "epg_date_fmt", "epg_csv"}) {
            const QString oldV = jsonToStr(m_cfg.contains(k) ? m_cfg.value(k) : defs.value(k));
            if (oldV != jsonToStr(c.value(k))) changed = true;
        }
        bool clear = false;
        if (changed && QDir(epgDir()).exists()) {
            clear = QMessageBox::question(&dlg, "下载参数已改变",
                                          "地址或频道表已改变，旧的 EPG 可能属于其他地区。\n"
                                          "是否清空已下载的 EPG 并重新下载？") == QMessageBox::Yes;
        }
        for (auto it = c.begin(); it != c.end(); ++it) m_cfg[it.key()] = it.value();
        saveCfg();
        if (clear) epg::clearAll();
        refreshSlots();
        dlg.accept();
        if (download || clear) startEpgUpdate(!download);
    };

    auto *bf = new QHBoxLayout;
    auto *bReset = dlgBtn("恢复默认");
    auto *bSave = dlgBtn("保存");
    auto *bSaveDl = dlgBtn("保存并下载");
    auto *bCancel = dlgBtn("取消");
    for (auto *b : {bReset, bSave, bSaveDl, bCancel}) bf->addWidget(b);
    g->addLayout(bf, r, 0, 1, 3);
    QObject::connect(bReset, &QPushButton::clicked, &dlg, [&] {
        for (auto it = edits.begin(); it != edits.end(); ++it)
            it.value()->setText(jsonToStr(defs.value(it.key())));
    });
    QObject::connect(bSave, &QPushButton::clicked, &dlg, [&] { save(false); });
    QObject::connect(bSaveDl, &QPushButton::clicked, &dlg, [&] { save(true); });
    QObject::connect(bCancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    dlg.exec();
}

} // namespace iptv
