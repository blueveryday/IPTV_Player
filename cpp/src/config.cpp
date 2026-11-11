#include "config.h"

#include "epg.h"
#include "util.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

namespace iptv {

QJsonObject defaultConfig() {
    QJsonObject c;
    c["userid"] = "gf001";
    c["authinfo"] = "xxx";
    c["replay_player"] = "";
    c["cfg_ver"] = 2;
    c["auto_install_vlc"] = false;
    c["hw_decode"] = true;
    c["http_replay_keys"] = QJsonArray{"/rtsp/"};
    c["template"] = "{base}?AuthInfo={authinfo}&userid={userid}&playseek={seek}";
    c["tz_offset"] = 8;
    c["replay_days"] = 7;
    c["volume"] = 100;
    c["left_width"] = 200;
    c["right_width"] = 250;
    c["epg_auto_update"] = true;
    c["epg_start_delay"] = 30;
    c["epg_host"] = "http://123.147.117.163:8081";
    c["epg_path"] = "/resource/schedules_v2/{channelcode}_{date}.json";
    c["epg_date_fmt"] = "%Y%m%d";
    c["epg_csv"] = "";
    c["epg_threads"] = 1;
    c["epg_timeout"] = 10;
    c["epg_sources"] = QJsonArray();
    c["epg_update_hours"] = 6;
    c["epg_last_update"] = 0;
    c["webdav_sources"] = QJsonArray();
    c["record_stop_on_switch"] = true;
    c["sub_auto_load"] = true;
    c["m3u_file"] = "";
    return c;
}

int cfgInt(const QJsonObject &cfg, const char *key, int def) {
    const QJsonValue v = cfg.value(QLatin1String(key));
    if (v.isDouble()) return int(v.toDouble());
    if (v.isString()) {
        bool ok = false;
        const int n = v.toString().trimmed().toInt(&ok);
        return ok ? n : def;
    }
    if (v.isBool()) return v.toBool() ? 1 : 0;
    return def;
}

double cfgDouble(const QJsonObject &cfg, const char *key, double def) {
    const QJsonValue v = cfg.value(QLatin1String(key));
    if (v.isDouble()) return v.toDouble();
    if (v.isString()) {
        bool ok = false;
        const double n = v.toString().trimmed().toDouble(&ok);
        return ok ? n : def;
    }
    return def;
}

bool cfgBool(const QJsonObject &cfg, const char *key, bool def) {
    const QJsonValue v = cfg.value(QLatin1String(key));
    if (v.isBool()) return v.toBool();
    if (v.isDouble()) return v.toDouble() != 0;
    return def;
}

QString cfgStr(const QJsonObject &cfg, const char *key, const QString &def) {
    const QJsonValue v = cfg.value(QLatin1String(key));
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(v.toDouble());
    return def;
}

QJsonObject loadConfig() {
    QJsonObject cfg = defaultConfig();
    {
        QFile f(configFile());
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject()) {
                const QJsonObject o = doc.object();
                for (auto it = o.begin(); it != o.end(); ++it)
                    cfg[it.key()] = it.value();
            }
        }
    }
    const QJsonObject def = defaultConfig();
    if (cfgInt(cfg, "cfg_ver", 1) < 2) {
        cfg["authinfo"] = def["authinfo"];
        cfg["template"] = def["template"];
        cfg["cfg_ver"] = 2;
    }

    // http_replay_keys 允许写成逗号分隔字符串
    {
        QJsonArray keys;
        const QJsonValue v = cfg.value("http_replay_keys");
        if (v.isString()) {
            for (const QString &k : v.toString().split(','))
                if (!k.trimmed().isEmpty()) keys.append(k.trimmed());
        } else if (v.isArray()) {
            keys = v.toArray();
        } else if (v.isUndefined() || v.isNull()) {
            keys = QJsonArray{"/rtsp/"};
        }
        cfg["http_replay_keys"] = keys;
    }

    cfg["left_width"] = cfgInt(cfg, "left_width", 200);
    cfg["right_width"] = cfgInt(cfg, "right_width", 300);
    cfg["volume"] = qMax(0, qMin(kMaxVolume, cfgInt(cfg, "volume", 100)));
    cfg["tz_offset"] = cfgInt(cfg, "tz_offset", 8);
    cfg["replay_days"] = qMax(1, cfgInt(cfg, "replay_days", 7));
    if (!cfg.value("webdav_sources").isArray()) cfg["webdav_sources"] = QJsonArray();
    if (!cfg.contains("record_stop_on_switch")) cfg["record_stop_on_switch"] = true;
    if (!cfg.contains("hw_decode")) cfg["hw_decode"] = true;
    cfg.remove("rtsp_tcp");

    // EPG 源：以 src/epg/epg_sources.json 为准，旧配置里的 epg_sources 迁移过去
    QStringList sources = epg::loadSourcesFile();
    const QJsonValue oldSources = cfg.value("epg_sources");
    if (sources.isEmpty() && oldSources.isArray() && !oldSources.toArray().isEmpty()) {
        for (const QJsonValue &s : oldSources.toArray())
            if (s.isString()) sources << s.toString();
        epg::saveSourcesFile(sources);
    }
    QJsonArray srcArr;
    for (const QString &s : sources) srcArr.append(s);
    cfg["epg_sources"] = srcArr;
    epg::setTz(cfgInt(cfg, "tz_offset", 8), cfgInt(cfg, "replay_days", 7));

    // 频道别名：以 src/epg/alias.json 为准，旧配置里的 epg_alias 迁移过去
    QMap<QString, QString> alias = epg::loadAliasFile();
    const QJsonValue oldAlias = cfg.value("epg_alias");
    if (oldAlias.isObject() && !oldAlias.toObject().isEmpty()) {
        const QJsonObject oa = oldAlias.toObject();
        for (auto it = oa.begin(); it != oa.end(); ++it)
            if (!alias.contains(it.key())) alias.insert(it.key(), it.value().toString());
        epg::saveAliasFile(alias);
    }
    cfg.remove("epg_alias");
    epg::setAlias(alias);
    return cfg;
}

bool saveConfig(const QJsonObject &cfgIn, QString *err) {
    QJsonObject cfg = cfgIn;
    cfg.remove("epg_sources");
    cfg.remove("rtsp_tcp");
    QSaveFile f(configFile());
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    f.write(QJsonDocument(cfg).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (err) *err = f.errorString();
        return false;
    }
    return true;
}

} // namespace iptv
