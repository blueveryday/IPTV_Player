import os
import sys
import re
import json
import shutil
import subprocess
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from datetime import datetime, timedelta, timezone

import importlib
import site
import struct
import threading
import hashlib
import zipfile
import urllib.request

CODE_VERSION="IPTV Player v2026.09.20"

PY_BITS = struct.calcsize("P") * 8
SEEK_GRANULARITY = 5

WEEKDAY_CN = "一二三四五六日"

COLORS = {
    "bg_root":      "#1e1e1e",
    "bg_panel":     "#252526",
    "bg_input":     "#2d2d30",
    "bg_video":     "#000000",
    "fg_primary":   "#e0e0e0",
    "fg_secondary": "#9e9e9e",
    "fg_dim":       "#6b6b6b",
    "disabled_fg":  "#ff5555",
    "accent":       "#4a90d9",
    "accent_dim":   "#3a6fa8",
    "border":       "#3f3f46",
    "select_bg":    "#094771",
    "select_fg":    "#ffffff",
    "btn_bg":       "#333333",
    "btn_active":   "#454545",
    "status_bg":    "#2d2d30",
}


def _pe_bits(path):
    try:
        with open(path, "rb") as f:
            f.seek(0x3C)
            off = struct.unpack("<I", f.read(4))[0]
            f.seek(off + 4)
            machine = struct.unpack("<H", f.read(2))[0]
        return {0x14C: 32, 0x8664: 64, 0xAA64: 64}.get(machine)
    except Exception:
        return None


def _script_dir():
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))


def portable_vlc_dirs():
    dirs = [os.path.join(_script_dir(), "vlc_module")]
    la = os.environ.get("LOCALAPPDATA")
    if la:
        dirs.append(os.path.join(la, "IPTVPlayer", "vlc_module"))
    return dirs


def portable_vlc_target():
    for d in portable_vlc_dirs():
        try:
            os.makedirs(d, exist_ok=True)
            t = os.path.join(d, ".w")
            open(t, "w").close()
            os.remove(t)
            return d
        except OSError:
            continue
    return portable_vlc_dirs()[-1]


def download_portable_vlc(dest_dir, progress, base=None):
    arch = "win64" if PY_BITS == 64 else "win32"
    base = base or "https://download.videolan.org/pub/videolan/vlc/last/%s/" % arch
    hdr = {"User-Agent": "Mozilla/5.0 (IPTVPlayer)"}

    def get(url):
        return urllib.request.urlopen(urllib.request.Request(url, headers=hdr), timeout=30)

    progress("正在查询 VLC 最新版本…")
    name = None
    try:
        html = get(base).read().decode("utf-8", "replace")
        names = set(re.findall(r"vlc-\d+(?:\.\d+)+-%s\.zip(?![\w.])" % arch, html))
        if names:
            name = max(names, key=lambda n: [int(x) for x in n.split("-")[1].split(".")])
    except Exception:
        pass
    if not name:
        name = "vlc-3.0.23-%s.zip" % arch

    try:
        m = re.search(r"\b[0-9a-fA-F]{64}\b", get(base + name + ".sha256").read().decode("utf-8", "replace"))
        expected = m.group(0).lower()
    except Exception as e:
        raise RuntimeError("无法获取校验文件（%s.sha256），为安全起见已中止：%s" % (name, e))

    os.makedirs(dest_dir, exist_ok=True)
    tmp = os.path.join(dest_dir, "_vlc_download.zip")
    h, done = hashlib.sha256(), 0
    with get(base + name) as r, open(tmp, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        while True:
            chunk = r.read(256 * 1024)
            if not chunk:
                break
            f.write(chunk)
            h.update(chunk)
            done += len(chunk)
            if total:
                progress("正在下载 VLC 组件 %d%%（%.0f / %.0f MB）" % (
                    done * 100 // total, done / 1048576.0, total / 1048576.0))
            else:
                progress("正在下载 VLC 组件 %.0f MB" % (done / 1048576.0))
    if h.hexdigest().lower() != expected:
        os.remove(tmp)
        raise RuntimeError("下载文件的 SHA-256 校验失败，已丢弃，请重试")

    progress("正在解压…")
    root = os.path.abspath(dest_dir)
    with zipfile.ZipFile(tmp) as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            parts = info.filename.split("/")
            if len(parts) < 2:
                continue
            rel = "/".join(parts[1:])
            low = rel.lower()
            keep = (len(parts) == 2 and low.endswith(".dll")) or \
                   (low.startswith("plugins/") and not low.startswith("plugins/gui/"))
            if not keep:
                continue
            out = os.path.abspath(os.path.join(root, *rel.split("/")))
            if not out.startswith(root + os.sep):
                continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with z.open(info) as src, open(out, "wb") as dst:
                shutil.copyfileobj(src, dst)
    os.remove(tmp)
    if not os.path.isfile(os.path.join(root, "libvlc.dll")):
        raise RuntimeError("解压完成但未找到 libvlc.dll")


def find_vlc_installs():
    cands = list(portable_vlc_dirs())
    for env in ("ProgramW6432", "ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"):
        base = os.environ.get(env)
        if base:
            cands.append(os.path.join(base, "VideoLAN", "VLC"))
    try:
        import winreg
        for root in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
            for key in (r"SOFTWARE\VideoLAN\VLC", r"SOFTWARE\WOW6432Node\VideoLAN\VLC"):
                for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
                    try:
                        with winreg.OpenKey(root, key, 0, winreg.KEY_READ | view) as k:
                            cands.append(winreg.QueryValueEx(k, "InstallDir")[0])
                    except OSError:
                        pass
    except ImportError:
        pass
    found, seen = [], set()
    for d in cands:
        dll = os.path.join(d, "libvlc.dll")
        key = os.path.normcase(os.path.abspath(d))
        if key in seen or not os.path.isfile(dll):
            continue
        seen.add(key)
        found.append((d, _pe_bits(dll)))
    return found


def prepare_vlc_path():
    installs = find_vlc_installs()
    match = [d for d, b in installs if b == PY_BITS]
    if match:
        d = match[0]
        os.environ["PATH"] = d + os.pathsep + os.environ.get("PATH", "")
        try:
            os.add_dll_directory(d)
        except Exception:
            pass
    return (match[0] if match else None), installs


VLC_MATCH_DIR, VLC_INSTALLS = None, []
if sys.platform.startswith("win"):
    VLC_MATCH_DIR, VLC_INSTALLS = prepare_vlc_path()

vlc = None
VLC_IMPORT_ERROR = None
try:
    import vlc
except ModuleNotFoundError:
    vlc = None
    VLC_IMPORT_ERROR = "missing"
except (Exception, SystemExit) as _e:
    vlc = None
    VLC_IMPORT_ERROR = "libvlc:%s: %s" % (type(_e).__name__, _e)


def describe_vlc_problem(err):
    detail = "\n\n技术详情：%s" % err
    if not sys.platform.startswith("win"):
        return "已安装 python-vlc，但找不到 libvlc。请先安装 VLC 播放器（Linux 用包管理器，macOS 安装 VLC.app）。" + detail
    if not VLC_INSTALLS:
        return ("已安装 python-vlc，但没有找到 VLC 的核心组件（libvlc.dll）。\n"
                "python-vlc 只是调用接口，需要 %d 位的 VLC 组件（不必安装完整 VLC，免安装组件即可）。" % PY_BITS) + detail
    if not VLC_MATCH_DIR:
        bits = "、".join("%s 位" % b for _, b in VLC_INSTALLS)
        return ("找到的 VLC 是 %s，但当前 Python 是 %d 位，位数不一致无法加载。\n"
                "无需卸载：可以下载 %d 位的免安装组件放在程序目录里使用。" % (bits, PY_BITS, PY_BITS)) + detail
    return "找到了 VLC（%s），但加载失败，请尝试重新安装 VLC。" % VLC_MATCH_DIR + detail


def app_dir():
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))


APP_DIR = app_dir()
DEFAULT_M3U = os.path.join(APP_DIR, "iptv.m3u")
CONFIG_FILE = os.path.join(APP_DIR, "iptv_config.json")

DEFAULT_CONFIG = {
    "userid": "gf001",
    "authinfo": "xxx",
    "replay_player": "",
    "cfg_ver": 2,
    "auto_install_vlc": True,
    "hw_decode": True,
    "rtsp_tcp": True,
    "http_replay_keys": ["/rtsp/"],
    "template": "{base}?AuthInfo={authinfo}&userid={userid}&playseek={seek}",
    "tz_offset": 8,
    "replay_days": 7,
    "volume": 80,
    "left_width": 250,
}

class SeekBar(tk.Canvas):
    def __init__(self, master, on_seek=None, height=18, **kw):
        super().__init__(master, height=height, bg=COLORS["bg_panel"],
                         highlightthickness=0, bd=0, **kw)
        self._on_seek = on_seek
        self._value = 0.0
        self._dragging = False
        self._enabled = False
        self.bind("<Configure>", lambda e: self._redraw())
        self.bind("<Button-1>", self._on_press)
        self.bind("<B1-Motion>", self._on_motion)
        self.bind("<ButtonRelease-1>", self._on_release)

    def set_value(self, v):
        try:
            v = float(v)
        except Exception:
            return
        self._value = max(0.0, min(1000.0, v))
        self._redraw()

    def get_value(self):
        return self._value

    def is_dragging(self):
        return self._dragging

    def set_enabled(self, en):
        self._enabled = bool(en)
        try:
            self.configure(cursor="hand2" if en else "")
        except Exception:
            pass
        self._redraw()

    def _redraw(self):
        self.delete("all")
        w = self.winfo_width()
        h = self.winfo_height()
        if w <= 1 or h <= 1:
            return
        cy = h // 2
        self.create_rectangle(0, cy - 3, w, cy + 3,
                              fill=COLORS["border"], outline="")
        fw = int(w * self._value / 1000.0)
        color = COLORS["accent"] if self._enabled else COLORS["fg_dim"]
        if fw > 0:
            self.create_rectangle(0, cy - 3, fw, cy + 3,
                                  fill=color, outline="")
        tx = max(6, min(w - 6, fw))
        outline_c = COLORS["accent"] if self._enabled else COLORS["fg_dim"]
        self.create_oval(tx - 6, cy - 6, tx + 6, cy + 6,
                         fill=COLORS["bg_panel"], outline=outline_c, width=2)

    def _x_to_value(self, x):
        w = self.winfo_width()
        if w <= 1:
            return 0.0
        return max(0.0, min(1000.0, x * 1000.0 / w))

    def _on_press(self, event):
        if not self._enabled:
            return
        self._dragging = True
        v = self._x_to_value(event.x)
        self.set_value(v)
        if self._on_seek:
            self._on_seek(v, "preview")

    def _on_motion(self, event):
        if not self._dragging:
            return
        v = self._x_to_value(event.x)
        self.set_value(v)
        if self._on_seek:
            self._on_seek(v, "preview")

    def _on_release(self, event):
        if not self._dragging:
            return
        self._dragging = False
        v = self._x_to_value(event.x)
        self.set_value(v)
        if self._on_seek:
            self._on_seek(v, "commit")

def read_text_auto(path):
    raw = open(path, "rb").read()
    for enc in ("utf-8-sig", "gbk", "utf-16"):
        try:
            return raw.decode(enc)
        except Exception:
            continue
    return raw.decode("utf-8", errors="ignore")


def clean_url(line):
    line = line.strip()
    m = re.match(r"^\[(.*?)\]\((.*?)\)$", line)
    if m:
        return m.group(2).strip()
    return line.strip("<>")


def parse_m3u(path):
    channels = []
    name = None
    for line in read_text_auto(path).splitlines():
        line = line.strip()
        if not line:
            continue
        if line.upper().startswith("#EXTINF"):
            m = re.match(r'^#EXTINF:[^,"]*(?:"[^"]*"[^,"]*)*,(.*)$', line, re.I)
            name = m.group(1).strip() if m else "未知频道"
            if not name:
                t = re.search(r'tvg-name="([^"]*)"', line)
                name = t.group(1) if t else "未知频道"
        elif line.startswith("#"):
            continue
        else:
            url = clean_url(line)
            if url:
                channels.append({"name": name or url, "url": url})
            name = None
    return channels


def replay_supported(url, cfg=None):
    u = url.lower()
    if u.startswith("rtsp://"):
        return True
    if not u.startswith(("http://", "https://")):
        return False
    keys = None
    if cfg is not None:
        keys = cfg.get("http_replay_keys")
    if keys is None:
        keys = ["/rtsp/"]
    if isinstance(keys, str):
        keys = [k.strip() for k in keys.split(",") if k.strip()]
    keys = [str(k).lower() for k in keys if k]
    if not keys:
        return True
    return any(k in u for k in keys)


def build_replay_url(base_url, start_local, end_local, cfg):
    off = timedelta(hours=int(cfg["tz_offset"]))
    fmt = "%Y%m%d%H%M%S"
    seek = "%s-%s" % ((start_local - off).strftime(fmt),
                      (end_local - off).strftime(fmt))
    auth = (cfg.get("authinfo") or "").strip()
    userid = (cfg.get("userid") or "").strip()

    tpl = (cfg.get("template") or "").strip()
    if tpl:
        url = tpl.format(base=base_url, authinfo=auth, userid=userid, seek=seek)
        url = re.sub(r"(?<=[?&])(AuthInfo|userid)=(&|$)", "", url)
        return url.rstrip("&?")

    params = []
    if auth:
        params.append("AuthInfo=%s" % auth)
    if userid:
        params.append("userid=%s" % userid)
    params.append("playseek=%s" % seek)
    sep = "&" if "?" in base_url else "?"
    return base_url + sep + "&".join(params)


class IPTVApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(CODE_VERSION)
        self.geometry("1280x800")
        self.minsize(960, 590)
        self.configure(bg=COLORS["bg_root"])

        self.cfg = dict(DEFAULT_CONFIG)
        self.load_config()

        self.channels = []
        self.filtered = []
        self.current = None
        self.current_url = ""
        self.current_title = ""
        self.current_live = True
        self.ext_proc = None

        self.player = None
        self.vlc_instance = None
        self.fullscreen = False

        self._progress_after_id = None
        self.replay_range_start = None
        self.replay_range_end = None
        self.replay_anchor_time = None
        self.replay_anchor_wall = None

        self.left_btn_col = None
        self.right_btn_col = None
        self._slots_locked = False

        self._setup_ttk_style()
        self.build_menu()
        self.build_ui()
        self.init_player()

        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.after(200, self.load_default)
        self.after(500, self._update_progress)

    def _setup_ttk_style(self):
        style = ttk.Style(self)
        try:
            style.theme_use("clam")
        except Exception:
            pass

        style.configure(".", background=COLORS["bg_root"],
                        foreground=COLORS["fg_primary"],
                        fieldbackground=COLORS["bg_input"],
                        bordercolor=COLORS["border"],
                        darkcolor=COLORS["bg_panel"],
                        lightcolor=COLORS["bg_panel"])

        style.configure("TFrame", background=COLORS["bg_root"])
        style.configure("Panel.TFrame", background=COLORS["bg_panel"])
        style.configure("Toolbar.TFrame", background=COLORS["bg_panel"])

        style.configure("TLabel", background=COLORS["bg_root"],
                        foreground=COLORS["fg_primary"])
        style.configure("Panel.TLabel", background=COLORS["bg_panel"],
                        foreground=COLORS["fg_primary"])
        style.configure("Dim.TLabel", background=COLORS["bg_panel"],
                        foreground=COLORS["fg_secondary"])
        style.configure("Status.TLabel", background=COLORS["status_bg"],
                        foreground=COLORS["fg_secondary"], padding=(6, 3))

        style.configure("TEntry", fieldbackground=COLORS["bg_input"],
                        foreground=COLORS["fg_primary"],
                        insertcolor=COLORS["fg_primary"],
                        bordercolor=COLORS["border"])

        style.configure("TCombobox", fieldbackground=COLORS["bg_input"],
                        foreground=COLORS["fg_primary"],
                        background=COLORS["btn_bg"],
                        arrowcolor=COLORS["fg_primary"],
                        bordercolor=COLORS["border"])
        style.map("TCombobox",
                  fieldbackground=[("readonly", COLORS["bg_input"])],
                  foreground=[("readonly", COLORS["fg_primary"])])

        style.configure("TButton", background=COLORS["btn_bg"],
                        foreground=COLORS["fg_primary"],
                        bordercolor=COLORS["border"],
                        focuscolor=COLORS["accent"],
                        padding=(8, 4))
        style.map("TButton",
                  background=[("active", COLORS["btn_active"]),
                              ("pressed", COLORS["accent_dim"])],
                  foreground=[("active", COLORS["fg_primary"])])

        style.configure("Arrow.TButton", background=COLORS["bg_panel"],
                        foreground=COLORS["fg_secondary"],
                        bordercolor=COLORS["bg_panel"],
                        padding=(2, 2), relief="flat")
        style.map("Arrow.TButton",
                  background=[("active", COLORS["btn_active"])],
                  foreground=[("active", COLORS["accent"])])

        style.configure("TCheckbutton", background=COLORS["bg_root"],
                        foreground=COLORS["fg_primary"])
        style.map("TCheckbutton",
                  background=[("active", COLORS["bg_root"])])

        style.configure("Vertical.TScrollbar",
                        background=COLORS["btn_bg"],
                        troughcolor=COLORS["bg_root"],
                        bordercolor=COLORS["bg_root"],
                        arrowcolor=COLORS["fg_secondary"])
        style.map("Vertical.TScrollbar",
                  background=[("active", COLORS["btn_active"])])

        style.configure("TLabelframe", background=COLORS["bg_panel"],
                        foreground=COLORS["fg_primary"],
                        bordercolor=COLORS["border"])
        style.configure("TLabelframe.Label", background=COLORS["bg_panel"],
                        foreground=COLORS["fg_secondary"])

        style.configure("TSeparator", background=COLORS["border"])

        style.configure("TScale", background=COLORS["bg_panel"],
                        troughcolor=COLORS["border"],
                        bordercolor=COLORS["border"])
        style.map("TScale", background=[("active", COLORS["bg_panel"])])

    def load_config(self):
        try:
            with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                self.cfg.update(json.load(f))
        except Exception:
            pass
        if self.cfg.get("cfg_ver", 1) < 2:
            self.cfg["authinfo"] = DEFAULT_CONFIG["authinfo"]
            self.cfg["template"] = DEFAULT_CONFIG["template"]
            self.cfg["cfg_ver"] = 2
        keys = self.cfg.get("http_replay_keys", ["/rtsp/"])
        if isinstance(keys, str):
            keys = [k.strip() for k in keys.split(",") if k.strip()]
        self.cfg["http_replay_keys"] = keys
        try:
            self.cfg["left_width"] = int(self.cfg.get("left_width", 250))
        except Exception:
            self.cfg["left_width"] = 250

    def save_config(self):
        try:
            with open(CONFIG_FILE, "w", encoding="utf-8") as f:
                json.dump(self.cfg, f, ensure_ascii=False, indent=2)
        except Exception as e:
            messagebox.showerror("错误", "保存配置失败：%s" % e)

    def build_menu(self):
        menubar = tk.Menu(self, bg=COLORS["bg_panel"], fg=COLORS["fg_primary"],
                          disabledforeground=COLORS["disabled_fg"],
                          activebackground=COLORS["accent_dim"],
                          activeforeground=COLORS["fg_primary"],
                          bd=0, relief="flat")
        m = tk.Menu(menubar, tearoff=0, bg=COLORS["bg_panel"],
                    fg=COLORS["fg_primary"],
                    disabledforeground=COLORS["disabled_fg"],
                    activebackground=COLORS["accent_dim"],
                    activeforeground="#ffffff")
        m.add_command(label="打开 m3u 文件...", command=self.open_m3u)
        m.add_command(label="重新加载", command=self.load_default)
        m.add_separator()
        m.add_command(label="回看参数设置...", command=self.open_settings)
        m.add_command(label="下载免安装 VLC 组件...", command=self.manual_portable_download)
        m.add_separator()
        m.add_command(label="退出", command=self.on_close)
        menubar.add_cascade(label="文件", menu=m)

        pm = tk.Menu(menubar, tearoff=0, bg=COLORS["bg_panel"],
                     fg=COLORS["fg_primary"],
                     disabledforeground=COLORS["disabled_fg"],
                     activebackground=COLORS["accent_dim"],
                     activeforeground="#ffffff")
        self.hw_var = tk.BooleanVar(value=bool(self.cfg.get("hw_decode", True)))
        self.tcp_var = tk.BooleanVar(value=bool(self.cfg.get("rtsp_tcp", True)))
        pm.add_checkbutton(label="硬件解码", variable=self.hw_var,
                           command=self.on_decode_option_changed)
        pm.add_checkbutton(label="RTSP 使用 TCP（仅对 rtsp:// 生效）", variable=self.tcp_var,
                           command=self.on_decode_option_changed)
        menubar.add_cascade(label="播放选项", menu=pm)

        self.menubar = menubar
        self.config(menu=menubar)

    def build_ui(self):
        self.grid_rowconfigure(0, weight=1)
        self.grid_columnconfigure(1, weight=1)
        self.left_visible = True
        self.right_visible = bool(self.cfg.get("right_visible", False))

        left_w = max(150, min(600, int(self.cfg.get("left_width", 250))))
        left = ttk.Frame(self, width=left_w, style="Panel.TFrame")
        left.grid(row=0, column=0, sticky="ns")
        left.grid_propagate(False)
        left.pack_propagate(False)
        self.left_panel = left
        left.grid_rowconfigure(0, weight=1)
        left.grid_columnconfigure(0, weight=1)

        self.left_body = ttk.Frame(left, style="Panel.TFrame")
        self.left_body.grid(row=0, column=0, sticky="nsew")

        sf = ttk.Frame(self.left_body, style="Panel.TFrame")
        sf.pack(fill=tk.X, padx=8, pady=(8, 4))
        ttk.Label(sf, text="搜索", style="Dim.TLabel").pack(side=tk.LEFT, padx=(0, 6))
        self.search_var = tk.StringVar()
        self.search_var.trace_add("write", lambda *a: self.apply_filter())
        search_entry = ttk.Entry(sf, textvariable=self.search_var, style="TEntry")
        search_entry.pack(side=tk.LEFT, fill=tk.X, expand=True)

        lf = ttk.Frame(self.left_body, style="Panel.TFrame")
        lf.pack(fill=tk.BOTH, expand=True, padx=8, pady=4)
        sb = ttk.Scrollbar(lf, orient=tk.VERTICAL)
        self.ch_list = tk.Listbox(lf, activestyle="none", exportselection=False,
                                  yscrollcommand=sb.set,
                                  font=("Microsoft YaHei UI", 10),
                                  bg=COLORS["bg_input"], fg=COLORS["fg_primary"],
                                  selectbackground=COLORS["select_bg"],
                                  selectforeground=COLORS["select_fg"],
                                  highlightthickness=0, bd=0,
                                  relief="flat", width=1)
        sb.config(command=self.ch_list.yview)
        sb.pack(side=tk.RIGHT, fill=tk.Y)
        self.ch_list.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        self.ch_list.bind("<Double-Button-1>", lambda e: self.play_live())
        self.ch_list.bind("<Return>", lambda e: self.play_live())
        self.ch_list.bind("<<ListboxSelect>>", lambda e: self.on_channel_select())

        self.count_var = tk.StringVar(value="0 个频道")
        ttk.Label(self.left_body, textvariable=self.count_var,
                  style="Dim.TLabel").pack(anchor="w", padx=10, pady=(2, 8))

        left_btn_col = ttk.Frame(left, style="Panel.TFrame")
        left_btn_col.grid(row=0, column=1, sticky="ns")
        left_btn_col.grid_rowconfigure(0, weight=1)
        left_btn_col.grid_rowconfigure(2, weight=1)
        self.left_btn_col = left_btn_col
        self.left_btn = ttk.Button(left_btn_col, text="◀", width=1,
                                   style="Arrow.TButton",
                                   command=self.toggle_left, takefocus=0)
        self.left_btn.grid(row=1, column=0)

        mid = ttk.Frame(self)
        mid.grid(row=0, column=1, sticky="nsew")
        self.mid_panel = mid
        mid.grid_rowconfigure(0, weight=1)
        mid.grid_columnconfigure(0, weight=1)

        self.video = tk.Frame(mid, bg=COLORS["bg_video"])
        self.video.grid(row=0, column=0, sticky="nsew", padx=4, pady=(6, 3))
        self.video.bind("<Double-Button-1>", self._on_video_double_click)
        self.video.bind("<Button-3>", self._on_video_right_click)

        pg = ttk.Frame(mid)
        pg.grid(row=1, column=0, sticky="ew", padx=6, pady=(2, 2))
        self.progress_frame = pg

        self.seek_bar = SeekBar(pg, on_seek=self._on_seek_bar, height=18)
        self.seek_bar.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 8))

        self.time_var = tk.StringVar(value="--:--:-- / --:--:--")
        ttk.Label(pg, textvariable=self.time_var, width=21,
                  font=("Consolas", 10),
                  background=COLORS["bg_root"],
                  foreground=COLORS["fg_secondary"]).pack(side=tk.LEFT)

        ctl = ttk.Frame(mid, style="Toolbar.TFrame")
        ctl.grid(row=2, column=0, sticky="ew", padx=4, pady=3)
        self.ctl_bar = ctl
        ttk.Button(ctl, text="▶ 直播", command=self.play_live).pack(side=tk.LEFT, padx=2)
        ttk.Button(ctl, text="⏯ 暂停", command=self.toggle_pause).pack(side=tk.LEFT, padx=2)
        ttk.Button(ctl, text="⏹ 停止", command=self.stop).pack(side=tk.LEFT, padx=2)
        ttk.Button(ctl, text="⛶ 全屏", command=self.toggle_fullscreen).pack(side=tk.LEFT, padx=2)
        ttk.Separator(ctl, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=8)
        ttk.Label(ctl, text="音量", style="Dim.TLabel").pack(side=tk.LEFT, padx=(0, 4))
        self.vol_var = tk.IntVar(value=int(self.cfg.get("volume", 80)))
        ttk.Scale(ctl, from_=0, to=100, variable=self.vol_var, length=100,
                  command=self.on_volume).pack(side=tk.LEFT, padx=2)

        self.status_var = tk.StringVar(value="就绪")
        self.status_label = ttk.Label(mid, textvariable=self.status_var,
                                      style="Status.TLabel", anchor="w")
        self.status_label.grid(row=3, column=0, sticky="ew", padx=4, pady=(3, 4))

        right = ttk.Frame(self, style="Panel.TFrame")
        right.grid(row=0, column=2, sticky="ns")
        self.right_panel = right
        right.grid_rowconfigure(0, weight=1)
        right.grid_columnconfigure(1, weight=1)

        right_btn_col = ttk.Frame(right, style="Panel.TFrame")
        right_btn_col.grid(row=0, column=0, sticky="ns")
        right_btn_col.grid_rowconfigure(0, weight=1)
        right_btn_col.grid_rowconfigure(2, weight=1)
        self.right_btn_col = right_btn_col
        self.right_btn = ttk.Button(right_btn_col, text="▶", width=1,
                                    style="Arrow.TButton",
                                    command=self.toggle_right, takefocus=0)
        self.right_btn.grid(row=1, column=0)

        self.right_body = ttk.Frame(right, style="Panel.TFrame")
        self.right_body.grid(row=0, column=1, sticky="nsew")

        self.replay_ch_var = tk.StringVar(value="未选择频道")
        ttk.Label(self.right_body, textvariable=self.replay_ch_var,
                  style="Dim.TLabel", wraplength=180,
                  justify=tk.LEFT).pack(anchor="w", padx=8, pady=(8, 4))

        df = ttk.Frame(self.right_body, style="Panel.TFrame")
        df.pack(fill=tk.X, padx=8, pady=4)
        ttk.Label(df, text="日期", style="Dim.TLabel").pack(side=tk.LEFT, padx=(0, 4))
        self.date_var = tk.StringVar()
        self.date_cb = ttk.Combobox(df, textvariable=self.date_var, width=18,
                                    values=self.date_choices(), state="readonly")
        self.date_cb.pack(side=tk.LEFT, padx=2)
        self.date_cb.current(0)
        self.date_cb.bind("<<ComboboxSelected>>", lambda e: self.refresh_slots())
        self.date_cb.bind("<Return>", lambda e: self.refresh_slots())

        sf2 = ttk.Frame(self.right_body, style="Panel.TFrame")
        sf2.pack(fill=tk.BOTH, expand=True, padx=8, pady=4)
        sb2 = ttk.Scrollbar(sf2, orient=tk.VERTICAL)
        self.slot_list = tk.Listbox(sf2, exportselection=False, yscrollcommand=sb2.set,
                                    font=("Consolas", 10),
                                    bg=COLORS["bg_input"],
                                    fg=COLORS["fg_primary"],
                                    selectbackground=COLORS["select_bg"],
                                    selectforeground=COLORS["select_fg"],
                                    highlightthickness=0, bd=0,
                                    relief="flat", width=18)
        sb2.config(command=self.slot_list.yview)
        sb2.pack(side=tk.RIGHT, fill=tk.Y)
        self.slot_list.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        self.slot_list.bind("<Double-Button-1>", lambda e: self.play_replay())
        self.slot_list.bind("<Button-1>", self._on_slot_click)
        self.slot_list.bind("<Key>", self._on_slot_key)
        self.slot_list.bind("<<ListboxSelect>>", self._on_slot_select)

        ttk.Button(self.right_body, text="▶ 回看播放",
                   command=self.play_replay).pack(
            fill=tk.X, padx=8, pady=(4, 3))
        ttk.Button(self.right_body, text="复制回看地址",
                   command=self.copy_replay_url).pack(
            fill=tk.X, padx=8, pady=(3, 8))

        self._apply_left_visible()
        self._apply_right_visible()

        self.bind("<F2>", lambda e: self.toggle_left())
        self.bind("<F3>", lambda e: self.toggle_right())
        self.bind("<Escape>", self.on_escape)

        self.refresh_slots()

    def _on_video_right_click(self, event):
        """在视频区域右键：弹出日期/时段回看菜单
        未来时段用纯红色文字（不用 state='disabled'，避免浮雕白边）"""
        ch = self.selected_channel() or self.current
        m = tk.Menu(self, tearoff=0,
                    bg=COLORS["bg_panel"], fg=COLORS["fg_primary"],
                    disabledforeground=COLORS["disabled_fg"],
                    activebackground=COLORS["accent_dim"],
                    activeforeground="#ffffff")

        if not ch:
            m.add_command(label="（请先在左侧选择频道）", state="disabled")
            try:
                m.tk_popup(event.x_root, event.y_root)
            finally:
                m.grab_release()
            return

        m.add_command(label="▶ 直播  %s" % ch["name"],
                      command=self.play_live)
        m.add_separator()

        if not replay_supported(ch["url"], self.cfg):
            m.add_command(label="该频道不支持回看", state="disabled")
        else:
            replay_menu = tk.Menu(m, tearoff=0,
                                  bg=COLORS["bg_panel"], fg=COLORS["fg_primary"],
                                  disabledforeground=COLORS["disabled_fg"],
                                  activebackground=COLORS["accent_dim"],
                                  activeforeground="#ffffff")
            now = self.local_now()
            earliest = self.earliest_date()
            for d, display in self._date_list():
                day_menu = tk.Menu(replay_menu, tearoff=0,
                                   bg=COLORS["bg_panel"], fg=COLORS["fg_primary"],
                                   disabledforeground=COLORS["disabled_fg"],
                                   activebackground=COLORS["accent_dim"],
                                   activeforeground="#ffffff")
                for h in range(24):
                    label = "%02d:00 - %s" % (
                        h, "24:00" if h == 23 else "%02d:00" % (h + 1))
                    start = datetime(d.year, d.month, d.day) + timedelta(hours=h)
                    end = start + timedelta(hours=1)
                    if start >= now or d < earliest:
                        day_menu.add_command(
                            label=label,
                            foreground=COLORS["disabled_fg"],
                            activebackground=COLORS["bg_panel"],
                            activeforeground=COLORS["disabled_fg"],
                            command=lambda: None)
                    else:
                        day_menu.add_command(
                            label=label,
                            command=lambda c=ch, s=start, e=end: self._play_replay_at(c, s, e))
                replay_menu.add_cascade(label=display, menu=day_menu)
            m.add_cascade(label="回看", menu=replay_menu)

        m.add_separator()
        m.add_command(label="⏹ 停止", command=self.stop)

        try:
            m.tk_popup(event.x_root, event.y_root)
        finally:
            m.grab_release()

    def _clear_replay_range(self):
        self.replay_range_start = None
        self.replay_range_end = None
        self.replay_anchor_time = None
        self.replay_anchor_wall = None
        try:
            self.seek_bar.set_enabled(False)
            self.seek_bar.set_value(0)
        except Exception:
            pass

    def _set_replay_range(self, start, end):
        self.replay_range_start = start
        self.replay_range_end = end
        self.replay_anchor_time = start
        self.replay_anchor_wall = self.local_now()
        try:
            self.seek_bar.set_enabled(True)
            self.seek_bar.set_value(0)
        except Exception:
            pass

    def _update_progress(self):
        self._progress_after_id = None
        try:
            if self.seek_bar.is_dragging():
                pass
            elif (self.replay_range_start is not None
                  and self.replay_range_end is not None
                  and self.replay_anchor_time is not None
                  and self.replay_anchor_wall is not None):
                now = self.local_now()
                elapsed = (now - self.replay_anchor_wall).total_seconds()
                cur = self.replay_anchor_time + timedelta(seconds=elapsed)
                if cur > self.replay_range_end:
                    cur = self.replay_range_end
                total_sec = (self.replay_range_end - self.replay_range_start).total_seconds()
                if total_sec > 0:
                    offset = (cur - self.replay_range_start).total_seconds()
                    pos = max(0.0, min(1000.0, offset * 1000.0 / total_sec))
                    self.seek_bar.set_value(pos)
                    self.time_var.set("%s / %s" % (
                        cur.strftime("%H:%M:%S"),
                        self.replay_range_end.strftime("%H:%M:%S")))
            else:
                self.seek_bar.set_value(0)
                self.time_var.set("--:--:-- / --:--:--")
        except Exception:
            pass
        try:
            self._progress_after_id = self.after(500, self._update_progress)
        except Exception:
            pass

    def _on_seek_bar(self, value, phase):
        if (self.replay_range_start is None
                or self.replay_range_end is None):
            return
        total_sec = (self.replay_range_end - self.replay_range_start).total_seconds()
        if total_sec <= 0:
            return
        offset_sec = total_sec * float(value) / 1000.0
        offset_sec = (int(offset_sec) // SEEK_GRANULARITY) * SEEK_GRANULARITY
        if offset_sec >= total_sec:
            offset_sec = max(0, int(total_sec) - SEEK_GRANULARITY)
        target = self.replay_range_start + timedelta(seconds=offset_sec)

        if phase == "preview":
            self.time_var.set("%s / %s" % (
                target.strftime("%H:%M:%S"),
                self.replay_range_end.strftime("%H:%M:%S")))
            return

        ch = self.current
        if not ch or self.player is None:
            return
        url = build_replay_url(ch["url"], target, self.replay_range_end, self.cfg)
        self.replay_anchor_time = target
        self.replay_anchor_wall = self.local_now()

        title = "[回看] %s %s - %s" % (
            ch["name"], target.strftime("%Y-%m-%d %H:%M:%S"),
            self.replay_range_end.strftime("%H:%M:%S"))
        self.play_url(url, title, live=False)
        self.status_var.set("已跳转到 %s 继续回看" % target.strftime("%H:%M:%S"))

    def local_now(self):
        utc = datetime.now(timezone.utc).replace(tzinfo=None)
        return utc + timedelta(hours=int(self.cfg["tz_offset"]))

    def _date_list(self):
        today = self.local_now().date()
        n = max(1, int(self.cfg["replay_days"]))
        result = []
        for i in range(n):
            d = today - timedelta(days=i)
            display = "%s 星期%s" % (d.strftime("%Y-%m-%d"),
                                     WEEKDAY_CN[d.weekday()])
            result.append((d, display))
        return result

    def date_choices(self):
        return [display for _, display in self._date_list()]

    def earliest_date(self):
        n = max(1, int(self.cfg["replay_days"]))
        return self.local_now().date() - timedelta(days=n - 1)

    def selected_date(self):
        txt = self.date_var.get().strip()
        if not txt:
            return None
        m = re.match(r"(\d{4}-\d{2}-\d{2})", txt)
        if not m:
            return None
        try:
            return datetime.strptime(m.group(1), "%Y-%m-%d")
        except ValueError:
            return None

    def _replay_allowed(self):
        """None=未选择频道；True/False=所选频道是否支持回看。"""
        ch = self.selected_channel()
        if ch is None:
            return None
        return replay_supported(ch["url"], self.cfg)

    def _on_slot_click(self, event=None):
        """频道不支持回看时，禁止在时段列表里建立选择。"""
        if self._slots_locked:
            self.status_var.set("该频道不支持回看，无法选择时段")
            return "break"

    def _on_slot_key(self, event=None):
        """锁定状态下屏蔽键盘选择（方向键、空格等）。"""
        if self._slots_locked:
            if event is not None and event.keysym in ("Tab", "ISO_Left_Tab"):
                return None
            return "break"

    def _on_slot_select(self, event=None):
        """兜底：锁定状态下清除任何已产生的选择。"""
        if self._slots_locked and self.slot_list.curselection():
            self.slot_list.selection_clear(0, tk.END)
            self.status_var.set("该频道不支持回看，无法选择时段")

    def refresh_slots(self):
        self.slot_list.delete(0, tk.END)
        day = self.selected_date()
        now = self.local_now()
        self._slots_locked = (self._replay_allowed() is False)
        for h in range(24):
            raw = "%02d:00 - %s" % (
                h, "24:00" if h == 23 else "%02d:00" % (h + 1))
            label = "     " + raw
            self.slot_list.insert(tk.END, label)
            if self._slots_locked or (day and day + timedelta(hours=h) >= now):
                self.slot_list.itemconfig(h, fg=COLORS["disabled_fg"])
        if self._slots_locked:
            self.slot_list.selection_clear(0, tk.END)
            try:
                self.slot_list.configure(cursor="")
            except Exception:
                pass
        else:
            try:
                self.slot_list.configure(cursor="hand2")
            except Exception:
                pass

    def selected_slot(self):
        if self._slots_locked or self._replay_allowed() is False:
            return None
        sel = self.slot_list.curselection()
        day = self.selected_date()
        if not sel or day is None:
            return None
        h = sel[0]
        start = day + timedelta(hours=h)
        end = start + timedelta(hours=1)
        return start, end

    def load_default(self):
        if os.path.exists(DEFAULT_M3U):
            self.load_m3u(DEFAULT_M3U)
        else:
            self.status_var.set("未找到 %s，请通过“文件 → 打开 m3u 文件”加载" % DEFAULT_M3U)

    def open_m3u(self):
        p = filedialog.askopenfilename(
            title="选择 m3u 文件",
            filetypes=[("M3U 播放列表", "*.m3u *.m3u8"), ("所有文件", "*.*")])
        if p:
            self.load_m3u(p)

    def load_m3u(self, path):
        try:
            self.channels = parse_m3u(path)
        except Exception as e:
            messagebox.showerror("错误", "读取 m3u 失败：%s" % e)
            return
        self.apply_filter()
        self.status_var.set("已加载 %d 个频道：%s" % (len(self.channels), path))

    def apply_filter(self):
        kw = self.search_var.get().strip().lower()
        self.filtered = [c for c in self.channels if kw in c["name"].lower()]
        self.ch_list.delete(0, tk.END)
        for c in self.filtered:
            self.ch_list.insert(tk.END, c["name"])
        self.count_var.set("%d / %d 个频道" % (len(self.filtered), len(self.channels)))
        self.refresh_slots()

    def selected_channel(self):
        sel = self.ch_list.curselection()
        if not sel:
            return None
        return self.filtered[sel[0]]

    def on_channel_select(self):
        ch = self.selected_channel()
        if ch:
            tip = "" if replay_supported(ch["url"], self.cfg) else "（不支持回看）"
            self.replay_ch_var.set("%s %s" % (ch["name"], tip))
        self.refresh_slots()

    def init_player(self):
        if vlc is None:
            if (VLC_IMPORT_ERROR == "missing" and self.cfg.get("auto_install_vlc", True)
                    and not getattr(sys, "frozen", False)):
                self.start_auto_install()
            elif VLC_IMPORT_ERROR and VLC_IMPORT_ERROR.startswith("libvlc"):
                self.status_var.set("找不到可用的 VLC 播放器本体，暂用外部播放器")
                if not getattr(self, "_libvlc_prompted", False):
                    self._libvlc_prompted = True
                    self.after(500, self.on_libvlc_problem)
            else:
                self.status_var.set("未检测到 python-vlc/VLC，将使用外部播放器（vlc/mpv/ffplay）")
            return
        try:
            self.vlc_instance = vlc.Instance("--network-caching=1500", "--quiet")
            self.player = self.vlc_instance.media_player_new()
            self.update_idletasks()
            wid = self.video.winfo_id()
            if sys.platform.startswith("win"):
                self.player.set_hwnd(wid)
            elif sys.platform == "darwin":
                self.player.set_nsobject(wid)
            else:
                self.player.set_xwindow(wid)
            self.player.video_set_mouse_input(False)
            self.player.video_set_key_input(False)
            self.player.audio_set_volume(self.vol_var.get())
            em = self.player.event_manager()
            em.event_attach(vlc.EventType.MediaPlayerEncounteredError,
                            lambda e: self.after(0, lambda: self.status_var.set(
                                "播放失败：无法打开该地址（请检查网络、地址或回看参数）：" + self.current_url)))
        except Exception as e:
            self.player = None
            self.status_var.set("VLC 初始化失败，改用外部播放器：%s" % e)

    def start_auto_install(self):
        if getattr(self, "installing", False):
            return
        self.installing = True
        self.install_result = None
        self.status_var.set("未检测到 python-vlc，正在后台自动安装…（期间可临时使用外部播放器）")
        threading.Thread(target=self._install_worker, daemon=True).start()
        self.after(500, self._poll_install)

    def _install_worker(self):
        cmd = [sys.executable, "-m", "pip", "install", "--disable-pip-version-check", "python-vlc"]
        kw = {"capture_output": True, "text": True, "errors": "replace", "timeout": 300}
        if sys.platform.startswith("win"):
            kw["creationflags"] = 0x08000000
        try:
            r = subprocess.run(cmd, **kw)
            out = (r.stdout or "") + (r.stderr or "")
            if r.returncode != 0 and "externally-managed" in out:
                r = subprocess.run(cmd + ["--break-system-packages"], **kw)
                out = (r.stdout or "") + (r.stderr or "")
            self.install_result = (r.returncode == 0, out[-600:])
        except Exception as e:
            self.install_result = (False, str(e))

    def _poll_install(self):
        if self.install_result is None:
            self.after(500, self._poll_install)
            return
        global vlc, VLC_IMPORT_ERROR
        self.installing = False
        ok, msg = self.install_result
        if not ok:
            self.status_var.set("python-vlc 自动安装失败，请手动执行：pip install python-vlc")
            messagebox.showwarning("自动安装失败",
                                   "python-vlc 安装失败，请检查网络后手动执行：\n"
                                   "pip install python-vlc\n\n" + msg)
            return
        self._try_enable_vlc("python-vlc 安装成功，已启用内嵌播放，请重新选择频道播放")

    def _try_enable_vlc(self, ok_msg):
        global vlc, VLC_IMPORT_ERROR, VLC_MATCH_DIR, VLC_INSTALLS
        importlib.invalidate_caches()
        try:
            usp = site.getusersitepackages()
            if os.path.isdir(usp) and usp not in sys.path:
                sys.path.append(usp)
        except Exception:
            pass
        if sys.platform.startswith("win"):
            VLC_MATCH_DIR, VLC_INSTALLS = prepare_vlc_path()
        try:
            import vlc as _v
            vlc = _v
            VLC_IMPORT_ERROR = None
            self.init_player()
            if self.player is not None:
                self.status_var.set(ok_msg)
        except ModuleNotFoundError:
            self.status_var.set("python-vlc 已安装，请重启程序后生效")
        except (Exception, SystemExit) as e:
            VLC_IMPORT_ERROR = "libvlc:%s: %s" % (type(e).__name__, e)
            self.status_var.set("仍然无法加载 VLC：" + VLC_IMPORT_ERROR)

    def on_libvlc_problem(self):
        text = describe_vlc_problem(VLC_IMPORT_ERROR)
        if sys.platform.startswith("win") and not VLC_MATCH_DIR:
            if messagebox.askyesno(
                    "缺少 VLC 组件",
                    text + "\n\n不想安装 VLC？可以自动下载官方的免安装组件：\n"
                           "• 来源：VideoLAN 官方（下载后校验 SHA-256）\n"
                           "• 大小：压缩包约 80 MB\n"
                           "• 位置：%s\n"
                           "• 不安装、不写注册表，删除该文件夹即可清除\n\n"
                           "是否现在下载？选“否”则暂用外部播放器。" % portable_vlc_target()):
                self.start_portable_download()
        else:
            messagebox.showwarning("找不到 VLC 组件", text)

    def manual_portable_download(self):
        if not sys.platform.startswith("win"):
            messagebox.showinfo("提示", "免安装组件下载仅支持 Windows。请用系统包管理器安装 VLC。")
        elif self.player is not None:
            messagebox.showinfo("提示", "内嵌播放器已经可用，无需下载。")
        elif getattr(self, "downloading", False):
            messagebox.showinfo("提示", "正在下载中，请稍候。")
        else:
            self.start_portable_download()

    def start_portable_download(self):
        if getattr(self, "downloading", False):
            return
        self.downloading = True
        self.install_result = None
        self.dl_text = "准备下载 VLC 免安装组件…"
        threading.Thread(target=self._portable_worker, args=(portable_vlc_target(),),
                         daemon=True).start()
        self.after(300, self._poll_portable)

    def _portable_worker(self, target):
        try:
            download_portable_vlc(target, lambda t: setattr(self, "dl_text", t))
            self.install_result = (True, "")
        except Exception as e:
            self.install_result = (False, str(e))

    def _poll_portable(self):
        if self.install_result is None:
            self.status_var.set(self.dl_text)
            self.after(300, self._poll_portable)
            return
        self.downloading = False
        ok, msg = self.install_result
        if ok:
            self._try_enable_vlc("VLC 组件已就绪（免安装），已启用内嵌播放，请重新选择频道播放")
        else:
            self.status_var.set("VLC 组件下载失败：" + msg)
            messagebox.showwarning(
                "下载失败",
                "%s\n\n可稍后重试（文件 → 下载免安装 VLC 组件），或在“回看参数设置”里指定 PotPlayer 等外部播放器。" % msg)

    def play_url(self, url, title, live=True):
        self.current_url = url
        self.current_title = title
        self.current_live = live
        if self.player is not None:
            media = self.vlc_instance.media_new(url)
            low = url.lower()
            if low.startswith("rtsp://"):
                if self.cfg.get("rtsp_tcp", True):
                    media.add_option(":rtsp-tcp")
            else:
                media.add_option(":http-reconnect=true")
            media.add_option(":avcodec-hw=%s" %
                             ("any" if self.cfg.get("hw_decode", True) else "none"))
            self.player.set_media(media)
            self.player.play()
            self.player.audio_set_volume(self.vol_var.get())
        else:
            self.play_external(url)
        self.status_var.set("正在播放：%s" % title)

    def play_external(self, url):
        self.kill_external()
        ext = (self.cfg.get("replay_player") or "").strip()
        if not ext:
            for d in ("ProgramFiles", "ProgramFiles(x86)"):
                base = os.environ.get(d)
                if base:
                    for exe in ("PotPlayerMini64.exe", "PotPlayerMini.exe"):
                        cand = os.path.join(base, "DAUM", "PotPlayer", exe)
                        if os.path.isfile(cand):
                            ext = cand
                            break
                if ext:
                    break
        if ext and os.path.isfile(ext):
            try:
                self.ext_proc = subprocess.Popen([ext, url])
                return
            except Exception:
                pass
        is_rtsp = url.lower().startswith("rtsp://")
        for exe, args in (("vlc", []), ("mpv", []),
                          ("ffplay", ["-rtsp_transport", "tcp"] if is_rtsp else [])):
            path = shutil.which(exe)
            if path:
                try:
                    self.ext_proc = subprocess.Popen([path] + args + [url])
                    return
                except Exception:
                    continue
        messagebox.showerror(
            "无法播放",
            "未找到可用播放器。\n请执行 pip install python-vlc 并安装 VLC，\n"
            "或将 vlc/mpv/ffplay 加入 PATH。")

    def kill_external(self):
        if self.ext_proc and self.ext_proc.poll() is None:
            try:
                self.ext_proc.terminate()
            except Exception:
                pass
        self.ext_proc = None

    def play_live(self):
        ch = self.selected_channel()
        if not ch:
            messagebox.showinfo("提示", "请先选择频道")
            return
        self._clear_replay_range()
        self.current = ch
        self.play_url(ch["url"], "[直播] " + ch["name"])

    def replay_url_for_selection(self):
        ch = self.selected_channel() or self.current
        if not ch:
            messagebox.showinfo("提示", "请先在左侧选择频道")
            return None
        if not replay_supported(ch["url"], self.cfg):
            messagebox.showinfo("提示", "该频道不支持回看")
            return None
        slot = self.selected_slot()
        if not slot:
            messagebox.showinfo("提示", "请选择正确的日期和回看时间段")
            return None
        start, end = slot
        return ch, start, end, build_replay_url(ch["url"], start, end, self.cfg)

    def _play_replay_at(self, ch, start, end):
        if start >= self.local_now():
            messagebox.showwarning("提示", "所选时间段尚未开始，无法回看")
            return
        if start.date() < self.earliest_date():
            messagebox.showwarning(
                "提示", "只能回看包含今天在内的 %d 天内的节目（最早 %s）" % (
                    int(self.cfg["replay_days"]), self.earliest_date().strftime("%Y-%m-%d")))
            return
        url = build_replay_url(ch["url"], start, end, self.cfg)
        self.current = ch
        self.current_url = url
        ext = (self.cfg.get("replay_player") or "").strip()
        if ext:
            if self.player is not None:
                self.player.stop()
            self.kill_external()
            try:
                self.ext_proc = subprocess.Popen([ext, url])
                self.status_var.set("已用外部播放器打开回看：" + url)
            except Exception as e:
                messagebox.showerror("错误", "无法启动外部播放器：%s" % e)
            self._clear_replay_range()
            return
        self._set_replay_range(start, end)
        title = "[回看] %s %s - %s" % (ch["name"], start.strftime("%Y-%m-%d %H:%M"),
                                       end.strftime("%H:%M"))
        self.play_url(url, title, live=False)

    def play_replay(self):
        r = self.replay_url_for_selection()
        if not r:
            return
        ch, start, end, _ = r
        self._play_replay_at(ch, start, end)

    def copy_replay_url(self):
        r = self.replay_url_for_selection()
        if not r:
            return
        url = r[3]
        self.clipboard_clear()
        self.clipboard_append(url)
        self.status_var.set("回看地址已复制：" + url)

    def on_decode_option_changed(self):
        self.cfg["hw_decode"] = bool(self.hw_var.get())
        self.cfg["rtsp_tcp"] = bool(self.tcp_var.get())
        self.save_config()
        if self.player is None or not self.current_url:
            return
        try:
            active = self.player.get_state() in (
                vlc.State.Opening, vlc.State.Buffering,
                vlc.State.Playing, vlc.State.Paused)
        except Exception:
            active = False
        if active:
            self.player.stop()
            self.play_url(self.current_url, self.current_title, self.current_live)
            self.status_var.set("已切换（硬解=%s, RTSP-TCP=%s）并重新播放：%s" % (
                "开" if self.cfg["hw_decode"] else "关",
                "开" if self.cfg["rtsp_tcp"] else "关", self.current_title))

    def toggle_pause(self):
        if self.player is not None:
            self.player.pause()

    def stop(self):
        if self.player is not None:
            self.player.stop()
        self.kill_external()
        self._clear_replay_range()
        self.time_var.set("--:--:-- / --:--:--")
        self.status_var.set("已停止")

    def on_volume(self, _v=None):
        v = int(float(self.vol_var.get()))
        if self.player is not None:
            self.player.audio_set_volume(v)
        self.cfg["volume"] = v

    def _apply_left_visible(self):
        if self.left_visible:
            self.left_body.grid(row=0, column=0, sticky="nsew")
            self.left_btn.config(text="◀")
            self.left_panel.grid_propagate(False)
            self.left_panel.pack_propagate(False)
            w = max(150, min(600, int(self.cfg.get("left_width", 250))))
            self.left_panel.configure(width=w)
        else:
            self.left_body.grid_remove()
            self.left_btn.config(text="▶")
            self.left_panel.grid_propagate(False)
            self.left_panel.pack_propagate(False)
            self.update_idletasks()
            w = 20
            if self.right_btn_col is not None:
                rw = self.right_btn_col.winfo_reqwidth()
                if rw > 0:
                    w = rw
            elif self.left_btn_col is not None:
                lw = self.left_btn_col.winfo_reqwidth()
                if lw > 0:
                    w = lw
            self.left_panel.configure(width=w)

    def _apply_right_visible(self):
        if self.right_visible:
            self.right_body.grid(row=0, column=1, sticky="nsew")
            self.right_btn.config(text="▶")
        else:
            self.right_body.grid_remove()
            self.right_btn.config(text="◀")

    def toggle_left(self):
        if self.fullscreen:
            return
        self.left_visible = not self.left_visible
        self._apply_left_visible()
        self.cfg["left_visible"] = self.left_visible

    def toggle_right(self):
        if self.fullscreen:
            return
        self.right_visible = not self.right_visible
        self._apply_right_visible()
        self.cfg["right_visible"] = self.right_visible

    def _on_video_double_click(self, event=None):
        self.toggle_fullscreen()

    def toggle_fullscreen(self):
        if self.fullscreen:
            self.exit_fullscreen()
        else:
            self.enter_fullscreen()

    def enter_fullscreen(self):
        if self.fullscreen:
            return
        self.fullscreen = True

        self.left_panel.grid_remove()
        self.right_panel.grid_remove()
        self.progress_frame.grid_remove()
        self.ctl_bar.grid_remove()
        self.status_label.grid_remove()
        self.video.grid_configure(padx=0, pady=0)

        self._empty_menu = tk.Menu(self)
        self.config(menu=self._empty_menu)

        self.attributes("-fullscreen", True)
        self.update_idletasks()
        self.video.focus_set()

    def exit_fullscreen(self):
        if not self.fullscreen:
            return
        self.fullscreen = False

        self.config(menu=self.menubar)
        self.attributes("-fullscreen", False)

        self.video.grid(row=0, column=0, sticky="nsew", padx=4, pady=(6, 3))
        self.progress_frame.grid(row=1, column=0, sticky="ew", padx=6, pady=(2, 2))
        self.ctl_bar.grid(row=2, column=0, sticky="ew", padx=4, pady=3)
        self.status_label.grid(row=3, column=0, sticky="ew", padx=4, pady=(3, 4))

        self.left_panel.grid(row=0, column=0, sticky="ns")
        self.left_panel.grid_rowconfigure(0, weight=1)
        self.left_panel.grid_columnconfigure(0, weight=1)
        if self.left_visible:
            self.left_body.grid(row=0, column=0, sticky="nsew")
            self.left_panel.grid_propagate(False)
            self.left_panel.pack_propagate(False)
            w = max(150, min(600, int(self.cfg.get("left_width", 250))))
            self.left_panel.configure(width=w)
        else:
            self.left_body.grid_remove()
            self.left_panel.grid_propagate(False)
            self.left_panel.pack_propagate(False)
            self.update_idletasks()
            w = 20
            if self.right_btn_col is not None:
                rw = self.right_btn_col.winfo_reqwidth()
                if rw > 0:
                    w = rw
            self.left_panel.configure(width=w)

        self.right_panel.grid(row=0, column=2, sticky="ns")
        self.right_panel.grid_rowconfigure(0, weight=1)
        self.right_panel.grid_columnconfigure(1, weight=1)
        if self.right_visible:
            self.right_body.grid(row=0, column=1, sticky="nsew")
        else:
            self.right_body.grid_remove()

        self.update_idletasks()
        self.focus_set()

    def on_escape(self, event=None):
        if self.fullscreen:
            self.exit_fullscreen()

    def open_settings(self):
        win = tk.Toplevel(self)
        win.title("回看参数设置")
        win.configure(bg=COLORS["bg_panel"])
        win.transient(self)
        win.resizable(False, False)
        win.grab_set()

        rows = [("userid", "userid"), ("AuthInfo", "authinfo"),
                ("时区偏移(小时, 重庆=8)", "tz_offset"), ("可回看天数(含今天)", "replay_days"),
                ("回看地址模板", "template"),
                ("HTTP 回看关键字(逗号分隔, 空=全部)", "http_replay_keys"),
                ("外部播放器路径(可空，如 PotPlayer)", "replay_player")]
        vars_ = {}
        for i, (label, key) in enumerate(rows):
            ttk.Label(win, text=label,
                      background=COLORS["bg_panel"],
                      foreground=COLORS["fg_primary"]).grid(
                row=i, column=0, sticky="e", padx=10, pady=6)
            val = self.cfg.get(key, "")
            if isinstance(val, list):
                val = ",".join(val)
            v = tk.StringVar(value=str(val))
            ttk.Entry(win, textvariable=v, width=42).grid(
                row=i, column=1, padx=10, pady=6)
            vars_[key] = v

        tcp_var = tk.BooleanVar(value=bool(self.cfg.get("rtsp_tcp", True)))
        ttk.Checkbutton(win, text="RTSP 使用 TCP（仅对 rtsp:// 生效）",
                        variable=tcp_var).grid(row=len(rows), column=1,
                                               sticky="w", padx=10)

        def ok():
            try:
                self.cfg["template"] = vars_["template"].get().strip()
                self.cfg["replay_player"] = vars_["replay_player"].get().strip().strip('"')
                keys_raw = vars_["http_replay_keys"].get().strip()
                self.cfg["http_replay_keys"] = [k.strip() for k in keys_raw.split(",") if k.strip()]
                self.cfg["rtsp_tcp"] = bool(tcp_var.get())
                self.tcp_var.set(self.cfg["rtsp_tcp"])
                self.cfg["userid"] = vars_["userid"].get().strip()
                self.cfg["authinfo"] = vars_["authinfo"].get().strip()
                self.cfg["tz_offset"] = int(vars_["tz_offset"].get())
                self.cfg["replay_days"] = max(1, int(vars_["replay_days"].get()))
            except ValueError:
                messagebox.showerror("错误", "时区偏移与回看天数必须为整数", parent=win)
                return
            self.save_config()
            self.date_cb["values"] = self.date_choices()
            self.date_cb.current(0)
            self.refresh_slots()
            win.destroy()

        bf = ttk.Frame(win, style="Panel.TFrame")
        bf.grid(row=len(rows) + 1, column=0, columnspan=2, pady=10)
        ttk.Button(bf, text="确定", command=ok).pack(side=tk.LEFT, padx=8)
        ttk.Button(bf, text="取消", command=win.destroy).pack(side=tk.LEFT, padx=8)

    def on_close(self):
        if self._progress_after_id:
            try:
                self.after_cancel(self._progress_after_id)
            except Exception:
                pass
            self._progress_after_id = None
        self.save_config()
        try:
            if self.player is not None:
                self.player.stop()
                self.player.release()
        except Exception:
            pass
        self.kill_external()
        self.destroy()


if __name__ == "__main__":
    IPTVApp().mainloop()