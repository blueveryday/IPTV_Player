# IPTV Player TV（Android TV 版）

由 Tkinter 版 `IPTV Player v2026.09.20` 移植：Kotlin + libVLC（内置播放核心，无需另装 VLC）。

## 编译 APK
### 方式 A：GitHub 在线编译（无需装任何软件）
1. 把本文件夹全部内容推到你的 GitHub 仓库（含 `.github/workflows/build.yml`）。
2. 仓库 → Actions → Build APK → Run workflow。
3. 完成后在 Artifacts 下载 `IPTV-Player-TV-apk`，里面就是 `IPTV Player v26.09.21.apk`（版本号 = 编译日期，北京时间；软件内“菜单”标题和“关于”里也能看到）。

### 方式 B：Android Studio
用 Android Studio（Koala 及以上，JDK 17）打开本文件夹 → 等待 Gradle 同步 → Build > Build APK(s)。

## 安装到电视
- U 盘拷贝安装，或 `adb connect 电视IP` 后 `adb install "IPTV Player v26.09.21.apk"`。

## 准备数据
- 频道：菜单 → 打开 m3u 文件 / 从网址加载 m3u；或 `adb push iptv.m3u /sdcard/Android/data/com.iptv.tv/files/`；或放入 `app/src/main/assets/` 后编译。
- EPG：把 `channel_epg_chongqing.csv` 放进 `app/src/main/assets/`，或在“自定义 EPG 下载参数”里填 csv 的路径/网址。

## 遥控器操作
| 按键 | 无面板时 | 面板中 |
|---|---|---|
| 确定 | 打开频道列表（长按=菜单） | 播放所选频道 / 时段 |
| 上 / 下 | 换台（直播） | 移动选择 |
| 左 / 右 | 倒退 / 快进 5 秒（长按加速） | 频道列表按右→回看节目单；节目单按左→返回频道列表 |
| 菜单键 | 打开菜单 | 同左 |
| 返回 | 连按两次退出 | 关闭面板 |
| 播放/暂停、停止、频道+/- | 对应功能 | 同左 |

## 触屏（手机 / 平板）操作
| 手势 | 功能 |
|---|---|
| 左半屏上下滑动 | 调节亮度 |
| 右半屏上下滑动 | 调节音量 |
| 左侧屏幕边缘向右滑 | 弹出频道菜单（含搜索 / 菜单 / 回看按钮） |
| 右侧屏幕边缘向左滑 | 弹出回看菜单 |
| 单击屏幕 | 显示进度条；面板打开时点面板外区域=关闭面板 |
| 长按屏幕 | 弹出主菜单 |
