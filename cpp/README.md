# IPTV Player（C++ / Qt 6 重构版）

对原 `IPTV Player.pyw`（Python + tkinter）的 C++ 重写：界面布局、菜单、快捷键、EPG、回看、录制、WebDAV 等功能一一对应。

## 依赖

- C++17 编译器、CMake ≥ 3.16
- Qt 6（Core / Gui / Widgets / Network / Core5Compat）
- zlib
- libmpv（开发包）
- 运行时：`ffmpeg`、`ffprobe`（录制、流信息）在 PATH 中即可

## 构建

Linux / macOS：

```bash
sudo apt install qt6-base-dev qt6-5compat-dev libmpv-dev zlib1g-dev ffmpeg   # Debian/Ubuntu
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/IPTVPlayer
```

Windows（MSVC + Qt 6）：下载 libmpv 开发包（含 `include/mpv/client.h` 与 `mpv.lib`/`libmpv-2.dll`），

```bat
cmake -S . -B build -DMPV_ROOT=C:\path\to\libmpv -DCMAKE_PREFIX_PATH=C:\Qt\6.x\msvc2019_64
cmake --build build --config Release
```

并把 `libmpv-2.dll`、`ffmpeg.exe`、`ffprobe.exe` 放到可执行文件旁。

## 说明

- 视频使用 mpv 的 `wid` 嵌入方式（与原版相同），并启用 mpv 自带控制条；单击/双击/右键由 mpv 回传给 Qt（单击暂停、双击全屏、右键菜单）。
- 配置、播放列表、录制目录与原版一致；可用环境变量 `IPTV_APP_DIR` 指定应用目录。
- 在无 GPU 的虚拟机/软件渲染环境中可能出现视频黑屏，属于 mpv 软件 GL 的问题；真实显卡环境不受影响。
- 与原版的已知差异：无 tkinter 原生控件的细微像素差异。
