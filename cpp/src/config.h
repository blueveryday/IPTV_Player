// 配置：iptv_config.json（与 Python 版字段、默认值、迁移逻辑一致）
#pragma once

#include <QJsonObject>
#include <QString>

namespace iptv {

QJsonObject defaultConfig();
// 读取配置并做迁移/校验；同时把 EPG 别名、EPG 源载入 epg 模块
QJsonObject loadConfig();
// 保存配置；失败返回 false 并填充 err
bool saveConfig(const QJsonObject &cfg, QString *err = nullptr);

// 取数值字段的便捷函数
int cfgInt(const QJsonObject &cfg, const char *key, int def);
double cfgDouble(const QJsonObject &cfg, const char *key, double def);
bool cfgBool(const QJsonObject &cfg, const char *key, bool def);
QString cfgStr(const QJsonObject &cfg, const char *key, const QString &def = QString());

} // namespace iptv
