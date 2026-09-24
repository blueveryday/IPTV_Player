import os
import sys
import re
import json
import shutil
import subprocess
import csv
import time
import queue
import importlib
import site
import struct
import threading
import hashlib
import zipfile
import urllib.request
import urllib.parse
import urllib.error
import base64
import xml.etree.ElementTree as ET
import webbrowser
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from datetime import datetime, timedelta, timezone

CODE_VERSION = "IPTV Player v2026.09.24"

PY_BITS = struct.calcsize("P") * 8
SEEK_GRANULARITY = 5

MEDIA_EXTS = {
    ".mp4", ".mkv", ".avi", ".mov", ".wmv", ".flv", ".webm", ".ts", ".m4v",
    ".mpg", ".mpeg", ".m2ts", ".mts", ".3gp", ".rmvb", ".rm", ".vob", ".ogv",
    ".mp3", ".aac", ".flac", ".wav", ".ape", ".ogg", ".wma", ".m4a", ".opus",
    ".ac3", ".dts", ".aiff", ".aif", ".alac", ".mka", ".mp2", ".mpc", ".wv",
}

AUDIO_EXTS = {
    ".mp3", ".aac", ".flac", ".wav", ".ape", ".ogg", ".wma", ".m4a", ".opus",
    ".ac3", ".dts", ".aiff", ".aif", ".alac", ".mka", ".mp2", ".mpc", ".wv",
}

WEEKDAY_CN = "一二三四五六日"

GITHUB_URL = "https://github.com/blueveryday/IPTV_Player"

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
    "live_green":   "#5ecb6b",
}


def center_window(win, parent=None):
    win.update_idletasks()
    w, h = win.winfo_width(), win.winfo_height()
    sw, sh = win.winfo_screenwidth(), win.winfo_screenheight()
    px = py = None
    if parent is not None:
        try:
            px = parent.winfo_rootx() + (parent.winfo_width() - w) // 2
            py = parent.winfo_rooty() + (parent.winfo_height() - h) // 2
        except Exception:
            px = py = None
    if px is None:
        px = (sw - w) // 2
        py = (sh - h) // 2
    px = max(0, min(px, sw - w))
    py = max(0, min(py, sh - h))
    win.geometry("+%d+%d" % (px, py))


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
    "epg_auto_update": True,
    "epg_host": "http://123.147.117.163:8081",
    "epg_path": "/resource/schedules_v2/{channelcode}_{date}.json",
    "epg_date_fmt": "%Y%m%d",
    "epg_csv": "",
    "epg_threads": 1,
    "epg_timeout": 10,
    "webdav_sources": [],
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

    def is_enabled(self):
        return self._enabled

    def set_enabled(self, en):
        en = bool(en)
        if en == self._enabled:
            return
        self._enabled = en
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
        if fw > 0:
            self.create_rectangle(0, cy - 3, fw, cy + 3,
                                  fill=COLORS["accent"], outline="")
        tx = max(6, min(w - 6, fw))
        self.create_oval(tx - 6, cy - 6, tx + 6, cy + 6,
                         fill=COLORS["bg_panel"], outline=COLORS["accent"], width=2)

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


def is_local_media_file(url):
    if not url or not isinstance(url, str):
        return False
    path = url
    if path.lower().startswith("file://"):
        try:
            path = urllib.request.url2pathname(path[7:])
        except Exception:
            return False
    try:
        if not os.path.isfile(path):
            return False
    except Exception:
        return False
    return os.path.splitext(path)[1].lower() in MEDIA_EXTS


def fmt_ms(ms):
    try:
        ms = int(ms)
    except Exception:
        ms = 0
    if ms < 0:
        ms = 0
    s = ms // 1000
    h, r = divmod(s, 3600)
    m, s = divmod(r, 60)
    return "%02d:%02d:%02d" % (h, m, s)


def parse_m3u(path):
    base_dir = os.path.dirname(os.path.abspath(path))
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
                resolved = url
                if not re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*://", url) \
                        and not os.path.isabs(url):
                    cand = os.path.join(base_dir, url)
                    if os.path.isfile(cand):
                        resolved = cand
                entry_name = name
                if not entry_name:
                    entry_name = (os.path.basename(resolved)
                                  if is_local_media_file(resolved) else resolved)
                ch = {"name": entry_name, "url": resolved}
                if is_local_media_file(resolved):
                    ch["local"] = True
                channels.append(ch)
            name = None
    return channels


class WebDAVClient:
    def __init__(self, url, user="", password="", timeout=15):
        url = (url or "").strip()
        if not url.lower().startswith(("http://", "https://")):
            url = "http://" + url
        self.base_url = url.rstrip("/")
        self.user = user or ""
        self.password = password or ""
        self.timeout = timeout

    def _headers(self, extra=None):
        h = {
            "User-Agent": "IPTVPlayer/1.0 (WebDAV)",
            "Accept": "*/*",
        }
        if self.user or self.password:
            token = base64.b64encode(
                ("%s:%s" % (self.user, self.password)).encode("utf-8")
            ).decode("ascii")
            h["Authorization"] = "Basic " + token
        if extra:
            h.update(extra)
        return h

    def list(self, rel_path=""):
        rel = (rel_path or "").strip("/")

        if rel:
            encoded_rel = "/".join(
                urllib.parse.quote(seg, safe="") for seg in rel.split("/")
            )
        else:
            encoded_rel = ""

        path = "/" + encoded_rel if encoded_rel else "/"
        url = self.base_url + path

        body = (
            '<?xml version="1.0" encoding="utf-8"?>'
            '<d:propfind xmlns:d="DAV:">'
            '<d:prop>'
            '<d:resourcetype/>'
            '<d:getcontentlength/>'
            '</d:prop>'
            '</d:propfind>'
        ).encode("utf-8")

        req = urllib.request.Request(
            url, data=body, method="PROPFIND",
            headers=self._headers({
                "Depth": "1",
                "Content-Type": "application/xml; charset=utf-8",
            }),
        )
        with urllib.request.urlopen(req, timeout=self.timeout) as r:
            data = r.read()

        ns = {"d": "DAV:"}
        root = ET.fromstring(data)

        base_path = urllib.parse.urlparse(self.base_url).path.rstrip("/")
        items = []

        for resp in root.findall("d:response", ns):
            href_el = resp.find("d:href", ns)
            if href_el is None or not href_el.text:
                continue
            href = href_el.text.strip()
            p = urllib.parse.urlparse(href)
            href_path = urllib.parse.unquote(p.path)

            if base_path:
                base_decoded = urllib.parse.unquote(base_path)
                if href_path.startswith(base_decoded):
                    sub = href_path[len(base_decoded):]
                elif href_path.startswith(base_path):
                    sub = href_path[len(base_path):]
                else:
                    sub = href_path
            else:
                sub = href_path
            sub = sub.strip("/")

            cur = rel
            if sub == cur:
                continue
            if cur and not sub.startswith(cur + "/"):
                continue
            rest = sub[len(cur):].lstrip("/") if cur else sub
            if not rest or "/" in rest:
                continue

            rtype = resp.find("d:propstat/d:prop/d:resourcetype", ns)
            is_dir = rtype is not None and rtype.find("d:collection", ns) is not None

            size = 0
            sz_txt = resp.findtext("d:propstat/d:prop/d:getcontentlength", "", ns)
            try:
                size = int(sz_txt)
            except Exception:
                pass

            encoded_sub = "/".join(
                urllib.parse.quote(seg, safe="") for seg in sub.split("/")
            )
            item_url = self.base_url + "/" + encoded_sub

            items.append({
                "name": rest,
                "is_dir": is_dir,
                "url": item_url,
                "size": size,
            })
        return items


def replay_supported(url, cfg=None):
    if is_local_media_file(url):
        return False
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


EPG_TOOL_DIR = os.path.join(APP_DIR, "src")
EPG_DIR = os.path.join(EPG_TOOL_DIR, "epg")
EPG_CSV = os.path.join(EPG_TOOL_DIR, "channel_epg_chongqing.csv")
EPG_MISS_FILE = os.path.join(EPG_DIR, ".miss.json")
EPG_URL = "http://123.147.117.163:8081/resource/schedules_v2/{channelcode}_{date}.json"
EPG_TIMEOUT = 10
EPG_RETRY = 2
EPG_THREADS = 1
EPG_MISS_TTL = 6 * 3600
EPG_FILE_RE = re.compile(r"_(\d{8})\.json$", re.I)
_epg_cache = {}


def _norm_name(s):
    return re.sub(r'[\\/:*?"<>|_\s]+', "", s or "").lower()


def find_epg_dir(name):
    if not os.path.isdir(EPG_DIR):
        return None
    p = os.path.join(EPG_DIR, name)
    if os.path.isdir(p):
        return p
    key = _norm_name(name)
    try:
        for d in os.listdir(EPG_DIR):
            fp = os.path.join(EPG_DIR, d)
            if os.path.isdir(fp) and _norm_name(d) == key:
                return fp
    except OSError:
        pass
    return None


def _parse_epg_file(path):
    items = []
    try:
        data = json.loads(read_text_auto(path))
    except Exception:
        return items
    fmt = "%Y-%m-%d %H:%M:%S"
    for s in (data.get("schedules") or []):
        try:
            st = datetime.strptime(s["starttime"], fmt)
            et = datetime.strptime(s["endtime"], fmt)
        except Exception:
            continue
        if et <= st:
            continue
        items.append({"title": str(s.get("title") or "").strip(),
                      "start": st, "end": et})
    return items


def load_epg(channel_name, day):
    d = find_epg_dir(channel_name)
    if not d:
        return []
    suffix = "_%s.json" % day.strftime("%Y%m%d")
    try:
        files = [f for f in os.listdir(d) if f.lower().endswith(suffix)]
    except OSError:
        return []
    progs = []
    for f in files:
        path = os.path.join(d, f)
        try:
            mtime = os.path.getmtime(path)
        except OSError:
            continue
        hit = _epg_cache.get(path)
        if hit and hit[0] == mtime:
            items = hit[1]
        else:
            items = _parse_epg_file(path)
            _epg_cache[path] = (mtime, items)
        progs.extend(items)
    day_start = datetime(day.year, day.month, day.day)
    day_end = day_start + timedelta(days=1)
    seen, result = set(), []
    for p in sorted(progs, key=lambda x: x["start"]):
        if not (p["start"] < day_end and p["end"] > day_start):
            continue
        k = (p["start"], p["end"], p["title"])
        if k in seen:
            continue
        seen.add(k)
        result.append(p)
    return result


def fmt_range(start, end):
    e = "24:00" if (end.hour == 0 and end.minute == 0 and end.date() > start.date()) \
        else end.strftime("%H:%M")
    return "%s-%s" % (start.strftime("%H:%M"), e)


def _epg_url_fn(cfg):
    host = (cfg.get("epg_host") or "").strip().rstrip("/")
    path = (cfg.get("epg_path") or "").strip()
    if path and not path.startswith(("/", "?")):
        path = "/" + path
    tpl = host + path
    fmt = (cfg.get("epg_date_fmt") or "").strip() or "%Y%m%d"

    def make(code, d):
        ds = datetime.strptime(d, "%Y%m%d").strftime(fmt)
        return tpl.format(channelcode=code, date=ds)
    return make


def epg_csv_path(cfg):
    p = (cfg.get("epg_csv") or "").strip().strip('"')
    if not p:
        return EPG_CSV
    return p if os.path.isabs(p) else os.path.join(APP_DIR, p)


def epg_clear_all():
    if os.path.isdir(EPG_DIR):
        shutil.rmtree(EPG_DIR, ignore_errors=True)
    _epg_cache.clear()


def epg_folder_name(name):
    name = re.sub(r'[\\/:*?"<>|]', "_", name or "").strip().rstrip(".")
    return name or "_"


def load_epg_channels(csv_path):
    items, seen = [], set()
    with open(csv_path, "r", encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        fm = {(k or "").strip().lower(): k for k in (reader.fieldnames or [])}
        cf, tf = fm.get("channelcode"), fm.get("title")
        if not cf or not tf:
            raise RuntimeError("channel_epg_chongqing.csv 缺少 channelcode 或 title 字段")
        for row in reader:
            code = (row.get(cf) or "").strip()
            title = (row.get(tf) or "").strip()
            if not code or code in seen:
                continue
            seen.add(code)
            items.append((code, title or code))
    return items


def epg_valid_dates(today, n):
    return [(today - timedelta(days=i)).strftime("%Y%m%d")
            for i in range(n - 1, -1, -1)]


def _load_miss():
    try:
        with open(EPG_MISS_FILE, "r", encoding="utf-8") as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except Exception:
        return {}


def _save_miss(d):
    try:
        os.makedirs(EPG_DIR, exist_ok=True)
        with open(EPG_MISS_FILE, "w", encoding="utf-8") as f:
            json.dump(d, f)
    except Exception:
        pass


def _epg_fetch(url, timeout=EPG_TIMEOUT):
    hdr = {"User-Agent": "Mozilla/5.0 (IPTVPlayer)"}
    for attempt in range(EPG_RETRY):
        try:
            req = urllib.request.Request(url, headers=hdr)
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return "ok", r.read()
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return "nodata", None
        except Exception:
            pass
        if attempt < EPG_RETRY - 1:
            time.sleep(1)
    return "error", None


def epg_cleanup(valid_dates):
    removed = 0
    if not os.path.isdir(EPG_DIR):
        return 0
    keep = set(valid_dates)
    for sub in os.listdir(EPG_DIR):
        sp = os.path.join(EPG_DIR, sub)
        if not os.path.isdir(sp):
            continue
        for f in os.listdir(sp):
            fp = os.path.join(sp, f)
            m = EPG_FILE_RE.search(f)
            stale = (m and m.group(1) not in keep) or f.lower().endswith(".json.tmp")
            if stale:
                try:
                    os.remove(fp)
                    removed += 1
                    _epg_cache.pop(fp, None)
                except OSError:
                    pass
        try:
            os.rmdir(sp)
        except OSError:
            pass
    return removed


def epg_update_worker(state, today, n, force_dates):
    try:
        dates = epg_valid_dates(today, n)
        state["removed"] = epg_cleanup(dates)

        csv_path = state["csv"]
        if not os.path.isfile(csv_path):
            state["error"] = "未找到频道表：%s" % csv_path
            return
        channels = load_epg_channels(csv_path)

        miss = _load_miss()
        now_ts = time.time()
        jobs = []
        for code, title in channels:
            folder = os.path.join(EPG_DIR, epg_folder_name(title))
            for d in dates:
                fp = os.path.join(folder, "%s_%s.json" % (code, d))
                if d not in force_dates:
                    if os.path.isfile(fp):
                        continue
                    if now_ts - miss.get("%s_%s" % (code, d), 0) < EPG_MISS_TTL:
                        continue
                jobs.append((code, d, folder, fp))
        state["total"] = len(jobs)

        lock = threading.Lock()
        q = queue.Queue()
        for j in jobs:
            q.put(j)

        def run(job):
            code, d, folder, fp = job
            key = "%s_%s" % (code, d)
            status = "error"
            try:
                status, body = _epg_fetch(state["url_fn"](code, d), state["timeout"])
                if status == "ok":
                    try:
                        data = json.loads(body.decode("utf-8-sig"))
                        if not (isinstance(data, dict) and data.get("schedules")):
                            status = "nodata"
                    except Exception:
                        status = "error"
                if status == "ok":
                    os.makedirs(folder, exist_ok=True)
                    tmp = fp + ".tmp"
                    with open(tmp, "wb") as f:
                        f.write(body)
                    os.replace(tmp, fp)
            except Exception:
                status = "error"
            with lock:
                if status == "ok":
                    state["ok"] += 1
                    miss.pop(key, None)
                elif status == "nodata":
                    state["nodata"] += 1
                    miss[key] = time.time()
                else:
                    state["fail"] += 1
                state["done"] += 1

        def loop():
            while not state["cancel"]:
                try:
                    job = q.get_nowait()
                except queue.Empty:
                    return
                run(job)

        ths = [threading.Thread(target=loop, daemon=True)
               for _ in range(state["threads"])]
        for t in ths:
            t.start()
        for t in ths:
            t.join()

        keep = set(dates)
        _save_miss({k: v for k, v in miss.items() if k.rsplit("_", 1)[-1] in keep})
    except Exception as e:
        state["error"] = str(e)
    finally:
        state["finished"] = True


def apply_app_icon(win):
    if not sys.platform.startswith("win"):
        return
    try:
        ico = os.path.join(APP_DIR, "src", "asset", "iptv.ico")
        if os.path.isfile(ico):
            win.iconbitmap(default=ico)
    except Exception:
        pass


class IPTVApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(CODE_VERSION)
        apply_app_icon(self)
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

        self._closing = False
        self._player_error_cb = None
        self._closing_after_id = None
        self._seek_status_after_id = None
        self._video_click_after_id = None
        self._net_paused = False

        self._progress_after_id = None
        self.replay_range_start = None
        self.replay_range_end = None
        self.replay_anchor_time = None
        self.replay_anchor_wall = None

        self.left_btn_col = None
        self.right_btn_col = None
        self._slots_locked = False
        self.slot_items = []
        self.epg_state = None
        self._live_rewind_mode = False

        self.webdav_mode = False
        self.webdav_source = None
        self.webdav_path = ""
        self.webdav_items = []
        self._webdav_req_id = 0

        self._setup_ttk_style()
        self.build_menu()
        self.build_ui()
        self.init_player()

        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.after(200, self.load_default)
        self.after(500, self._update_progress)
        self.after(1500, self.auto_epg_update)

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
                  foreground=[("readonly", COLORS["fg_primary"])],
                  selectbackground=[("readonly", COLORS["bg_input"])],
                  selectforeground=[("readonly", COLORS["fg_primary"])])
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
        if not isinstance(self.cfg.get("webdav_sources"), list):
            self.cfg["webdav_sources"] = []

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
        m.add_command(label="打开媒体文件...", command=self.open_media_file)
        m.add_command(label="浏览 WebDAV...", command=self.open_webdav_dialog)
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

        em = tk.Menu(menubar, tearoff=0, bg=COLORS["bg_panel"],
                     fg=COLORS["fg_primary"],
                     disabledforeground=COLORS["disabled_fg"],
                     activebackground=COLORS["accent_dim"],
                     activeforeground="#ffffff")
        em.add_command(label="下载 EPG", command=self.manual_epg_update)
        em.add_command(label="自定义 EPG 下载参数...", command=self.open_epg_settings)
        menubar.add_cascade(label="EPG选项", menu=em)

        menubar.add_command(label="关于", command=self.show_about)

        self.menubar = menubar
        self.config(menu=menubar)

    def show_about(self):
        win = tk.Toplevel(self)
        win.title("关于")
        win.configure(bg=COLORS["bg_panel"])
        win.transient(self)
        win.resizable(False, False)

        ttk.Label(win, text=CODE_VERSION,
                  background=COLORS["bg_panel"],
                  foreground=COLORS["fg_primary"],
                  font=("Microsoft YaHei UI", 11, "bold")).pack(
            anchor="w", padx=20, pady=(16, 4))

        ttk.Label(win, text="GitHub 仓库：",
                  background=COLORS["bg_panel"],
                  foreground=COLORS["fg_secondary"]).pack(
            anchor="w", padx=20, pady=(8, 2))

        def open_link(_e=None):
            try:
                webbrowser.open(GITHUB_URL)
            except Exception as e:
                messagebox.showerror("错误", "无法打开浏览器：%s" % e, parent=win)

        link = tk.Label(win, text=GITHUB_URL, anchor="w", justify=tk.LEFT,
                        wraplength=460, padx=8, pady=6,
                        bg=COLORS["bg_input"], fg=COLORS["accent"],
                        cursor="hand2")
        link.pack(fill=tk.X, padx=20, pady=(0, 8))

        base_font = ("Microsoft YaHei UI", 9, "normal")
        hover_font = ("Microsoft YaHei UI", 9, "underline")
        link.configure(font=base_font)
        link.bind("<Button-1>", open_link)
        link.bind("<Enter>", lambda e: link.configure(font=hover_font))
        link.bind("<Leave>", lambda e: link.configure(font=base_font))

        bf = ttk.Frame(win, style="Panel.TFrame")
        bf.pack(pady=(4, 14))
        ttk.Button(bf, text="关闭", command=win.destroy).pack(side=tk.LEFT, padx=6)

        win.grab_set()
        center_window(win, self)

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

        wf = ttk.Frame(self.left_body, style="Panel.TFrame")
        wf.pack(fill=tk.X, padx=8, pady=(8, 2))
        ttk.Label(wf, text="快捷菜单", style="Dim.TLabel").pack(side=tk.LEFT, padx=(0, 6))
        self.webdav_var = tk.StringVar()
        self.webdav_cb = ttk.Combobox(wf, textvariable=self.webdav_var,
                              state="readonly", width=1)
        self.webdav_cb.pack(side=tk.LEFT, fill=tk.X, expand=True)
        self.webdav_cb.bind("<<ComboboxSelected>>", self._on_webdav_combo)
        self._webdav_combo_sources = []
        self._refresh_webdav_combo()

        sf = ttk.Frame(self.left_body, style="Panel.TFrame")
        sf.pack(fill=tk.X, padx=8, pady=(4, 4))
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
        self.ch_list.bind("<ButtonRelease-1>", self._on_channel_click)
        self.ch_list.bind("<Return>", lambda e: self.play_live())
        self.ch_list.bind("<<ListboxSelect>>", lambda e: self.on_channel_select())
        self.ch_list.bind("<Up>", self._on_arrow_up)
        self.ch_list.bind("<Down>", self._on_arrow_down)

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
        self.video.bind("<Button-1>", self._on_video_click)
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
                  style="Dim.TLabel", wraplength=260,
                  justify=tk.LEFT).pack(anchor="w", padx=8, pady=(8, 2))

        self.epg_var = tk.StringVar(value="")
        ttk.Label(self.right_body, textvariable=self.epg_var,
                  style="Dim.TLabel").pack(anchor="w", padx=8, pady=(0, 4))

        df = ttk.Frame(self.right_body, style="Panel.TFrame")
        df.pack(fill=tk.X, padx=8, pady=4)
        ttk.Label(df, text="回看", style="Dim.TLabel").pack(side=tk.LEFT, padx=(0, 4))
        self.date_var = tk.StringVar()
        self.date_cb = ttk.Combobox(df, textvariable=self.date_var, width=18,
                                    values=self.date_choices(), state="readonly")
        self.date_cb.pack(side=tk.LEFT, padx=2)
        self.date_cb.current(0)
        self.date_cb.bind("<<ComboboxSelected>>", self._on_date_selected)
        self.date_cb.bind("<Return>", lambda e: self.refresh_slots())
        self.date_cb.bind("<Up>", lambda e: self._step_date(-1))
        self.date_cb.bind("<Down>", lambda e: self._step_date(1))

        sf2 = ttk.Frame(self.right_body, style="Panel.TFrame")
        sf2.pack(fill=tk.BOTH, expand=True, padx=8, pady=4)
        sb2 = ttk.Scrollbar(sf2, orient=tk.VERTICAL)

        self.slot_list = tk.Listbox(sf2, exportselection=False, yscrollcommand=sb2.set,
                                    font=("Microsoft YaHei UI", 10),
                                    bg=COLORS["bg_input"],
                                    fg=COLORS["fg_primary"],
                                    selectbackground=COLORS["select_bg"],
                                    selectforeground=COLORS["select_fg"],
                                    highlightthickness=0, bd=0,
                                    relief="flat", width=32)

        sb2.config(command=self.slot_list.yview)
        sb2.pack(side=tk.RIGHT, fill=tk.Y)
        self.slot_list.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        self.slot_list.bind("<Double-Button-1>", lambda e: self.play_replay())
        self.slot_list.bind("<Button-1>", self._on_slot_click)
        self.slot_list.bind("<Key>", self._on_slot_key)
        self.slot_list.bind("<<ListboxSelect>>", self._on_slot_select)

        for w in (self.slot_list, self.ch_list, self.date_cb):
            w.bind("<Left>", lambda e: self._seek_arrow(-SEEK_GRANULARITY))
            w.bind("<Right>", lambda e: self._seek_arrow(SEEK_GRANULARITY))

        self.slot_list.bind("<Return>", lambda e: (self.play_replay(), "break")[1])
        self.slot_list.bind("<KP_Enter>", lambda e: (self.play_replay(), "break")[1])

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

        self.bind("<space>", self._on_space_key)
        self.bind("<KP_Space>", self._on_space_key)

        self.bind("<Left>", self._on_arrow_left)
        self.bind("<Right>", self._on_arrow_right)
        self.bind("<Up>", self._on_arrow_up)
        self.bind("<Down>", self._on_arrow_down)

        self.refresh_slots()

    def _on_channel_click(self, event=None):
        try:
            idx = self.ch_list.nearest(event.y)
        except Exception:
            return
        bbox = self.ch_list.bbox(idx)
        if not bbox:
            return
        y0 = bbox[1]
        y1 = y0 + bbox[3]
        if not (y0 <= event.y <= y1):
            return
        sel = self.ch_list.curselection()
        if not sel:
            return
        self.play_live()

    def _focus_is_text_input(self):
        try:
            w = self.focus_get()
        except Exception:
            w = None
        if w is None:
            return False
        try:
            cls = w.winfo_class()
        except Exception:
            return False
        return cls in ("TEntry", "Entry", "Text",
                       "TScale", "TSpinbox", "Spinbox")

    def _is_local_media(self, ch):
        if not ch:
            return False
        if ch.get("local"):
            return True
        return is_local_media_file(ch.get("url", ""))

    def _is_file_playback(self, ch=None):
        if ch is None:
            ch = self.current
        if not ch:
            return False
        if self._is_local_media(ch):
            return True
        if ch.get("webdav") and not ch.get("is_dir"):
            return True
        return False

    def _current_replay_pos(self):
        if (self.replay_range_start is None or self.replay_range_end is None
                or self.replay_anchor_time is None or self.replay_anchor_wall is None):
            return None
        elapsed = (self.local_now() - self.replay_anchor_wall).total_seconds()
        cur = self.replay_anchor_time + timedelta(seconds=elapsed)
        if cur > self.replay_range_end:
            cur = self.replay_range_end
        return cur

    def _seek_replay_by(self, delta_sec):
        if (self.replay_range_start is None or self.replay_range_end is None):
            return
        if self._live_rewind_mode:
            self.replay_range_end = self.local_now()
            cur = self.replay_anchor_time
        else:
            cur = self._current_replay_pos()
            if cur is None:
                cur = self.replay_range_start
        total_sec = (self.replay_range_end - self.replay_range_start).total_seconds()
        if total_sec <= 0:
            return
        cur_offset = (cur - self.replay_range_start).total_seconds()
        cur_offset = (int(cur_offset) // SEEK_GRANULARITY) * SEEK_GRANULARITY
        new_offset = cur_offset + int(delta_sec)
        max_off = max(0, int(total_sec) - SEEK_GRANULARITY)
        if new_offset < 0:
            new_offset = 0
        if new_offset > max_off:
            new_offset = max_off
        value = new_offset * 1000.0 / total_sec
        self._on_seek_bar(value, "commit")

    def _live_rewind_to(self, new_time):
        ch = self.current
        if not ch:
            return
        now = self.local_now()
        if new_time >= now:
            self._resume_live()
            return
        self.replay_anchor_time = new_time
        self.replay_anchor_wall = now
        self.replay_range_end = now
        total_sec = (self.replay_range_end - self.replay_range_start).total_seconds()
        if total_sec > 0:
            offset = (new_time - self.replay_range_start).total_seconds()
            self.seek_bar.set_value(max(0.0, min(1000.0, offset * 1000.0 / total_sec)))
        url = build_replay_url(ch["url"], new_time, now, self.cfg)
        title = "[回看] %s %s - %s" % (
            ch["name"], new_time.strftime("%Y-%m-%d %H:%M:%S"),
            now.strftime("%H:%M:%S"))
        self.play_url(url, title, live=False)
        self.status_var.set("已前进到 %s 回看" % new_time.strftime("%H:%M:%S"))

    def _find_epg_bounds_for_now(self, ch, now):
        progs = load_epg(ch["name"], now.date())
        for p in progs:
            if p["start"] <= now < p["end"]:
                return p["start"], p["end"]
        if now.hour < 6:
            for p in load_epg(ch["name"], (now - timedelta(days=1)).date()):
                if p["start"] <= now < p["end"]:
                    return p["start"], p["end"]
        past = [p for p in progs if p["end"] <= now]
        if past:
            p = max(past, key=lambda x: x["end"])
            return p["start"], p["end"]
        return None

    def _live_rewind_start(self):
        ch = self.current
        if not ch:
            return
        now = self.local_now()
        if self.replay_range_start is None:
            bounds = self._find_epg_bounds_for_now(ch, now)
            if bounds is None:
                self.status_var.set("没有可用的 EPG 数据，无法倒退")
                return
            self.replay_range_start = bounds[0]
            self.replay_anchor_time = now
            self.replay_anchor_wall = now
            self.replay_range_end = now
            try:
                self.seek_bar.set_enabled(True)
            except Exception:
                pass
        self._live_rewind_mode = True

        new_time = self.replay_anchor_time - timedelta(seconds=SEEK_GRANULARITY)
        if new_time < self.replay_range_start:
            new_time = self.replay_range_start
        if new_time >= self.replay_anchor_time:
            self.status_var.set("已到达该时段最早时间（%s），无法继续倒退" %
                                self.replay_range_start.strftime("%H:%M:%S"))
            return
        self.replay_anchor_time = new_time
        self.replay_anchor_wall = now
        self.replay_range_end = now
        total_sec = (self.replay_range_end - self.replay_range_start).total_seconds()
        if total_sec > 0:
            offset = (new_time - self.replay_range_start).total_seconds()
            try:
                self.seek_bar.set_value(max(0.0, min(1000.0, offset * 1000.0 / total_sec)))
            except Exception:
                pass
        url = build_replay_url(ch["url"], new_time, now, self.cfg)
        title = "[回看] %s %s - %s" % (
            ch["name"], new_time.strftime("%Y-%m-%d %H:%M:%S"),
            now.strftime("%H:%M:%S"))
        self.play_url(url, title, live=False)
        self.status_var.set("已倒推到 %s 回看" % new_time.strftime("%H:%M:%S"))

    def _resume_live(self):
        ch = self.current
        if not ch:
            return
        self._clear_replay_range()
        self.play_url(ch["url"], "[直播] " + ch["name"], live=True)
        self._refresh_live_bar()
        self.status_var.set("已恢复直播：%s" % ch["name"])

    def _on_arrow_left(self, event=None):
        if self._focus_is_text_input():
            return None
        if self.seek_bar.is_dragging():
            return None
        if self._is_file_playback():
            self._seek_local_media_relative(-SEEK_GRANULARITY * 1000)
            return "break"
        if self.current_live:
            self._live_rewind_start()
            return "break"
        if self.replay_range_start is None or self.replay_range_end is None:
            return None
        self._seek_replay_by(-SEEK_GRANULARITY)
        return "break"

    def _on_arrow_right(self, event=None):
        if self._focus_is_text_input():
            return None
        if self.seek_bar.is_dragging():
            return None
        if self._is_file_playback():
            self._seek_local_media_relative(SEEK_GRANULARITY * 1000)
            return "break"
        if self.current_live:
            return "break"
        if self.replay_range_start is None or self.replay_range_end is None:
            return None
        if self._live_rewind_mode:
            now = self.local_now()
            new_time = self.replay_anchor_time + timedelta(seconds=SEEK_GRANULARITY)
            if new_time >= now:
                self._resume_live()
                return "break"
            self._live_rewind_to(new_time)
            return "break"
        self._seek_replay_by(SEEK_GRANULARITY)
        return "break"

    def _seek_arrow(self, delta):
        if self.seek_bar.is_dragging():
            return "break"
        if delta < 0:
            return self._on_arrow_left()
        return self._on_arrow_right()

    def _on_arrow_up(self, event=None):
        return self._navigate_channel(-1)

    def _on_arrow_down(self, event=None):
        return self._navigate_channel(1)

    def _navigate_channel(self, delta):
        if self._focus_is_text_input():
            return None
        try:
            w = self.focus_get()
        except Exception:
            w = None
        if w is self.slot_list:
            return None
        if not self.filtered:
            return "break"

        sel = self.ch_list.curselection()
        if sel:
            idx = sel[0] + delta
            if idx < 0:
                idx = 0
            elif idx >= len(self.filtered):
                idx = len(self.filtered) - 1
            if idx == sel[0]:
                return "break"
        else:
            idx = 0

        self.ch_list.selection_clear(0, tk.END)
        self.ch_list.selection_set(idx)
        self.ch_list.activate(idx)
        self.ch_list.see(idx)
        self.on_channel_select()
        if not self.webdav_mode:
            self.play_live()
        return "break"

    def _on_slot_click(self, event=None):
        if self._slots_locked:
            self.status_var.set("该频道不支持回看，无法选择时段")
            return "break"

    def _on_slot_key(self, event=None):
        if self._slots_locked:
            if event is not None and event.keysym in ("Tab", "ISO_Left_Tab"):
                return None
            return "break"

    def _on_slot_select(self, event=None):
        if self._slots_locked and self.slot_list.curselection():
            self.slot_list.selection_clear(0, tk.END)
            self.status_var.set("该频道不支持回看，无法选择时段")

    def _replay_allowed(self):
        ch = self.selected_channel()
        if ch is None:
            return None
        if ch.get("webdav"):
            return False
        return replay_supported(ch["url"], self.cfg)

    def _on_video_right_click(self, event):
        if self.webdav_mode:
            return
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

        is_local = self._is_local_media(ch)
        if is_local:
            m.add_command(label="▶ 播放  %s" % ch["name"],
                          command=self.play_live)
        else:
            m.add_command(label="▶ 直播  %s" % ch["name"],
                          command=self.play_live)
        m.add_separator()

        if is_local:
            m.add_command(label="本地媒体文件，无回看", state="disabled")
        elif not replay_supported(ch["url"], self.cfg):
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
                slots, _ = self.build_day_slots(ch, d)
                for label, start, end in slots:
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
        self._live_rewind_mode = False
        try:
            self.seek_bar.set_value(0)
        except Exception:
            pass

    def _set_replay_range(self, start, end):
        self.replay_range_start = start
        self.replay_range_end = end
        self.replay_anchor_time = start
        self.replay_anchor_wall = self.local_now()
        self._live_rewind_mode = False
        try:
            self.seek_bar.set_enabled(True)
            self.seek_bar.set_value(0)
        except Exception:
            pass

    def _refresh_live_bar(self):
        try:
            self.seek_bar.set_enabled(True)
            self.seek_bar.set_value(1000)
            now_str = self.local_now().strftime("%H:%M:%S")
            self.time_var.set("%s / %s" % (now_str, now_str))
        except Exception:
            pass

    def _update_local_media_progress(self):
        if self.player is None:
            return
        try:
            t = self.player.get_time()
            l = self.player.get_length()
        except Exception:
            return
        if l and l > 0:
            self.seek_bar.set_enabled(True)
            pos = max(0, min(1000, int(t * 1000 / l)))
            self.seek_bar.set_value(pos)
            self.time_var.set("%s / %s" % (fmt_ms(t), fmt_ms(l)))
        else:
            self.seek_bar.set_enabled(False)
            self.seek_bar.set_value(0)
            self.time_var.set("--:--:-- / --:--:--")

    def _cancel_seek_status_timer(self):
        if self._seek_status_after_id is not None:
            try:
                self.after_cancel(self._seek_status_after_id)
            except Exception:
                pass
            self._seek_status_after_id = None

    def _flash_seek_status(self, text):
        """临时显示 seek 提示，5 秒后恢复为“正在播放”信息。"""
        self._cancel_seek_status_timer()
        self.status_var.set(text)
        self._seek_status_after_id = self.after(5000, self._restore_play_status)

    def _restore_play_status(self):
        self._seek_status_after_id = None
        if getattr(self, "_closing", False):
            return
        if not self.current_url:
            return
        if self._is_file_playback():
            self.status_var.set("正在播放：%s  %s" % (
                self.current_title, self.current_url))
        else:
            self.status_var.set("正在播放：%s" % self.current_title)

    def _seek_local_media(self, value, phase):
        if self.player is None:
            if phase == "commit":
                self.status_var.set("当前使用外部播放器，无法在程序内拖动进度")
            return
        try:
            length = self.player.get_length()
        except Exception:
            return
        if not length or length <= 0:
            if phase == "commit":
                self.status_var.set("媒体长度尚未就绪，暂时无法拖动")
            return

        target_ms = int(length * float(value) / 1000.0)
        target_ms = max(0, min(length, target_ms))

        if phase == "preview":
            self.time_var.set("%s / %s" % (fmt_ms(target_ms), fmt_ms(length)))
            return

        try:
            if not self.player.is_seekable():
                self.status_var.set("该媒体不支持跳转")
                return
        except Exception:
            pass

        try:
            ret = self.player.set_time(target_ms)
        except Exception as e:
            self.status_var.set("拖动失败：%s" % e)
            return
        if ret == -1:
            self.status_var.set("VLC 拒绝跳转")
            return
        self.time_var.set("%s / %s" % (fmt_ms(target_ms), fmt_ms(length)))
        self._flash_seek_status("已跳转到 %s" % fmt_ms(target_ms))

    def _seek_local_media_relative(self, delta_ms):
        if self.player is None:
            self.status_var.set("当前使用外部播放器，无法在程序内快进/快退")
            return
        try:
            t = self.player.get_time()
            l = self.player.get_length()
        except Exception as e:
            self.status_var.set("读取进度失败：%s" % e)
            return

        if not l or l <= 0:
            self.status_var.set("媒体长度尚未就绪或不可 seek，请稍候再试")
            return
        if t < 0:
            t = 0

        nt = max(0, min(l, t + int(delta_ms)))

        try:
            if not self.player.is_seekable():
                self.status_var.set("该媒体不支持跳转（常见于未开启 Range 的 WebDAV）")
                return
        except Exception:
            pass

        try:
            ret = self.player.set_time(nt)
        except Exception as e:
            self.status_var.set("跳转失败：%s" % e)
            return
        if ret == -1:
            self.status_var.set("VLC 拒绝跳转（媒体可能尚未完全索引）")
            return

        self.time_var.set("%s / %s" % (fmt_ms(nt), fmt_ms(l)))
        self._flash_seek_status("已跳转到 %s" % fmt_ms(nt))

    def _update_progress(self):
        self._progress_after_id = None
        try:
            if self.seek_bar.is_dragging():
                pass
            elif self._is_file_playback():
                self._update_local_media_progress()
            elif self.current_live and self.replay_range_start is None:
                if self.current is not None:
                    self._refresh_live_bar()
                else:
                    self.seek_bar.set_enabled(False)
                    self.seek_bar.set_value(0)
                    self.time_var.set("--:--:-- / --:--:--")
            elif (self.replay_range_start is not None
                  and self.replay_range_end is not None
                  and self.replay_anchor_time is not None
                  and self.replay_anchor_wall is not None):
                now = self.local_now()
                if self._live_rewind_mode:
                    self.replay_range_end = now
                    cur = self.replay_anchor_time
                    right = now
                else:
                    elapsed = (now - self.replay_anchor_wall).total_seconds()
                    cur = self.replay_anchor_time + timedelta(seconds=elapsed)
                    if cur > self.replay_range_end:
                        cur = self.replay_range_end
                    right = self.replay_range_end
                total_sec = (self.replay_range_end
                             - self.replay_range_start).total_seconds()
                if total_sec > 0:
                    offset = (cur - self.replay_range_start).total_seconds()
                    pos = max(0.0, min(1000.0, offset * 1000.0 / total_sec))
                    self.seek_bar.set_value(pos)
                    self.time_var.set("%s / %s" % (
                        cur.strftime("%H:%M:%S"),
                        right.strftime("%H:%M:%S")))
            else:
                self.seek_bar.set_enabled(False)
                self.seek_bar.set_value(0)
                self.time_var.set("--:--:-- / --:--:--")
        except Exception:
            pass
        try:
            self._progress_after_id = self.after(500, self._update_progress)
        except Exception:
            pass

    def _on_seek_bar(self, value, phase):
        if self._is_file_playback():
            self._seek_local_media(value, phase)
            return
        ch = self.current
        if self.replay_range_start is None or self.replay_range_end is None:
            if not ch:
                try:
                    self.seek_bar.set_value(1000 if self.current_live else 0)
                except Exception:
                    pass
                return
            now = self.local_now()
            bounds = self._find_epg_bounds_for_now(ch, now)
            if bounds is None:
                self.status_var.set("没有可用的 EPG 数据，无法拖动进度")
                try:
                    self.seek_bar.set_value(1000)
                except Exception:
                    pass
                return
            self.replay_range_start = bounds[0]
            self.replay_range_end = now
            self.replay_anchor_time = now
            self.replay_anchor_wall = now
            self._live_rewind_mode = True

        total_sec = (self.replay_range_end - self.replay_range_start).total_seconds()
        if total_sec <= 0:
            return
        offset_sec = total_sec * float(value) / 1000.0
        offset_sec = (int(offset_sec) // SEEK_GRANULARITY) * SEEK_GRANULARITY
        if offset_sec >= total_sec:
            offset_sec = max(0, int(total_sec) - SEEK_GRANULARITY)
        target = self.replay_range_start + timedelta(seconds=offset_sec)

        at_right = (value >= 999 or
                    target >= self.replay_range_end - timedelta(seconds=SEEK_GRANULARITY))
        if self._live_rewind_mode and at_right:
            if phase == "preview":
                end_str = self.replay_range_end.strftime("%H:%M:%S")
                self.time_var.set("%s / %s" % (end_str, end_str))
                return
            if self.current_live:
                self._clear_replay_range()
                self._refresh_live_bar()
            else:
                self._resume_live()
            return

        if phase == "preview":
            self.time_var.set("%s / %s" % (
                target.strftime("%H:%M:%S"),
                self.replay_range_end.strftime("%H:%M:%S")))
            return

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

    def build_day_slots(self, ch, day_date):
        progs = load_epg(ch["name"], day_date) if ch else []
        if progs:
            return [("%s  %s" % (fmt_range(p["start"], p["end"]), p["title"]),
                     p["start"], p["end"]) for p in progs], True
        base = datetime(day_date.year, day_date.month, day_date.day)
        slots = []
        for h in range(24):
            s = base + timedelta(hours=h)
            e = s + timedelta(hours=1)
            slots.append((fmt_range(s, e), s, e))
        return slots, False

    def auto_epg_update(self):
        if self.cfg.get("epg_auto_update", True):
            self.start_epg_update(silent=True)

    def manual_epg_update(self):
        self.start_epg_update(silent=False)

    def open_epg_settings(self):
        win = tk.Toplevel(self)
        win.title("自定义 EPG 下载参数")
        win.configure(bg=COLORS["bg_panel"])
        win.transient(self)
        win.resizable(False, False)

        sample = "00000001000000050000000000000476"
        today_str = self.local_now().strftime("%Y%m%d")
        rows = [("服务器地址", "epg_host"),
                ("路径模板", "epg_path"),
                ("日期格式（strftime）", "epg_date_fmt"),
                ("频道表 CSV（空=src/channel_epg_chongqing.csv）", "epg_csv"),
                ("下载线程数（1-10）", "epg_threads"),
                ("超时秒数（3-10）", "epg_timeout")]
        vars_ = {}
        for i, (label, key) in enumerate(rows):
            ttk.Label(win, text=label, background=COLORS["bg_panel"],
                      foreground=COLORS["fg_primary"]).grid(
                row=i, column=0, sticky="e", padx=10, pady=6)
            v = tk.StringVar(value=str(self.cfg.get(key, DEFAULT_CONFIG[key])))
            ttk.Entry(win, textvariable=v, width=54).grid(
                row=i, column=1, padx=(10, 4), pady=6)
            vars_[key] = v

        def browse():
            p = filedialog.askopenfilename(
                parent=win, filetypes=[("CSV 文件", "*.csv"), ("所有文件", "*.*")])
            if p:
                vars_["epg_csv"].set(p)
        ttk.Button(win, text="浏览…", command=browse).grid(row=3, column=2, padx=(0, 10))

        r = len(rows)
        ttk.Label(win, text="可用变量：{channelcode} 频道代码，{date} 按“日期格式”生成的日期。\n"
                            "最终地址 = 服务器地址 + 路径模板（路径模板可含 ?参数）。\n"
                            "本地文件仍统一保存为 频道代码_YYYYMMDD.json。",
                  style="Dim.TLabel", justify=tk.LEFT).grid(
            row=r, column=0, columnspan=3, sticky="w", padx=10, pady=(6, 2))
        pv = tk.StringVar()
        ttk.Label(win, text="地址预览", style="Dim.TLabel").grid(
            row=r + 1, column=0, sticky="ne", padx=10, pady=4)
        tk.Label(win, textvariable=pv, bg=COLORS["bg_panel"], fg=COLORS["accent"],
                 wraplength=460, justify=tk.LEFT, anchor="w").grid(
            row=r + 1, column=1, columnspan=2, sticky="w", padx=10, pady=4)

        def refresh_preview(*_):
            try:
                c = {k: v.get().strip() for k, v in vars_.items()}
                pv.set(_epg_url_fn(c)(sample, today_str))
            except Exception as e:
                pv.set("（格式有误：%s）" % e)
        for v in vars_.values():
            v.trace_add("write", refresh_preview)
        refresh_preview()

        def collect():
            c = {k: v.get().strip() for k, v in vars_.items()}
            c["epg_csv"] = c["epg_csv"].strip('"')
            if not c["epg_host"].lower().startswith(("http://", "https://")):
                raise ValueError("服务器地址必须以 http:// 或 https:// 开头")
            for var in ("{channelcode}", "{date}"):
                if var not in c["epg_path"]:
                    raise ValueError("路径模板必须包含 %s" % var)
            try:
                c["epg_threads"] = max(1, min(10, int(c["epg_threads"])))
                c["epg_timeout"] = max(3, min(10, int(c["epg_timeout"])))
            except ValueError:
                raise ValueError("线程数和超时秒数必须为整数")
            c["epg_date_fmt"] = c["epg_date_fmt"] or "%Y%m%d"
            try:
                _epg_url_fn(c)(sample, today_str)
            except Exception as e:
                raise ValueError("地址模板有误（含未知变量或花括号不匹配）：%s" % e)
            return c

        def save(download):
            st = self.epg_state
            if st and not st["finished"]:
                messagebox.showinfo("提示", "EPG 正在更新中，请稍后再修改。", parent=win)
                return
            try:
                c = collect()
            except ValueError as e:
                messagebox.showerror("错误", str(e), parent=win)
                return
            addr_keys = ("epg_host", "epg_path", "epg_date_fmt", "epg_csv")
            changed = any(str(self.cfg.get(k, DEFAULT_CONFIG[k])) != str(c[k])
                          for k in addr_keys)
            clear = False
            if changed and os.path.isdir(EPG_DIR):
                clear = messagebox.askyesno(
                    "下载参数已改变",
                    "地址或频道表已改变，旧的 EPG 可能属于其他地区。\n"
                    "是否清空已下载的 EPG 并重新下载？", parent=win)
            self.cfg.update(c)
            self.save_config()
            if clear:
                epg_clear_all()
            self.refresh_slots()
            win.destroy()
            if download or clear:
                self.start_epg_update(silent=not download)

        def reset():
            for k, v in vars_.items():
                v.set(str(DEFAULT_CONFIG[k]))

        bf = ttk.Frame(win, style="Panel.TFrame")
        bf.grid(row=r + 2, column=0, columnspan=3, pady=10)
        ttk.Button(bf, text="恢复默认", command=reset).pack(side=tk.LEFT, padx=6)
        ttk.Button(bf, text="保存", command=lambda: save(False)).pack(side=tk.LEFT, padx=6)
        ttk.Button(bf, text="保存并下载", command=lambda: save(True)).pack(side=tk.LEFT, padx=6)
        ttk.Button(bf, text="取消", command=win.destroy).pack(side=tk.LEFT, padx=6)

        win.grab_set()
        center_window(win, self)

    def start_epg_update(self, silent):
        st = self.epg_state
        if st and not st["finished"]:
            if not silent:
                messagebox.showinfo("提示", "EPG 正在更新中，请稍候。")
            return
        today = self.local_now().date()
        n = max(1, int(self.cfg["replay_days"]))
        force = set() if silent else {today.strftime("%Y%m%d")}
        try:
            threads = max(1, min(10, int(self.cfg.get("epg_threads", EPG_THREADS))))
            timeout = max(3, min(10, int(self.cfg.get("epg_timeout", EPG_TIMEOUT))))
        except (TypeError, ValueError):
            threads, timeout = EPG_THREADS, EPG_TIMEOUT
        self.epg_state = {"total": 0, "done": 0, "ok": 0, "fail": 0, "nodata": 0,
                          "removed": 0, "error": None, "finished": False,
                          "cancel": False, "silent": silent,
                          "csv": epg_csv_path(self.cfg), "threads": threads,
                          "timeout": timeout, "url_fn": _epg_url_fn(self.cfg)}
        threading.Thread(target=epg_update_worker,
                         args=(self.epg_state, today, n, force),
                         daemon=True).start()
        if not silent:
            self.status_var.set("正在更新 EPG…")
        self.after(500, self._poll_epg)

    def _poll_epg(self):
        st = self.epg_state
        if st is None:
            return
        if not st["finished"]:
            if not st["silent"] and st["total"]:
                self.status_var.set("正在更新 EPG… %d / %d" % (st["done"], st["total"]))
            self.after(500, self._poll_epg)
            return

        changed = st["ok"] > 0 or st["removed"] > 0
        if changed:
            self._refresh_slots_keep_selection()
        summary = "EPG 更新完成：新增 %d，无数据 %d，失败 %d，清理过期 %d" % (
            st["ok"], st["nodata"], st["fail"], st["removed"])

        if st["silent"]:
            if not st["error"] and changed and not self.current_url:
                self.status_var.set(summary)
        elif st["error"]:
            self.status_var.set("EPG 更新失败：" + st["error"])
            messagebox.showwarning("EPG 更新失败", st["error"])
        else:
            self.status_var.set(summary)
            messagebox.showinfo("EPG 下载", summary)

    def _refresh_slots_keep_selection(self):
        keep = None
        sel = self.slot_list.curselection()
        if sel and sel[0] < len(self.slot_items):
            keep = self.slot_items[sel[0]]
        self.refresh_slots()
        if keep is not None and not self._slots_locked and keep in self.slot_items:
            i = self.slot_items.index(keep)
            self.slot_list.selection_set(i)
            self.slot_list.see(i)

    def _on_date_selected(self, event=None):
        try:
            self.date_cb.selection_clear()
        except Exception:
            pass
        self.refresh_slots()

    def _step_date(self, delta):
        vals = self.date_cb["values"]
        if not vals:
            return "break"
        i = max(0, min(len(vals) - 1, self.date_cb.current() + delta))
        if i != self.date_cb.current():
            self.date_cb.current(i)
            self._on_date_selected()
        return "break"

    def refresh_slots(self):
        self.slot_list.delete(0, tk.END)
        self.slot_items = []

        if self.webdav_mode:
            self._slots_locked = True
            self.epg_var.set("WebDAV 浏览模式")
            try:
                self.slot_list.configure(cursor="")
            except Exception:
                pass
            return

        day = self.selected_date()
        now = self.local_now()
        ch = self.selected_channel()

        if ch is not None and self._is_local_media(ch):
            self._slots_locked = True
            self.epg_var.set("本地媒体文件，无回看")
            try:
                self.slot_list.configure(cursor="")
            except Exception:
                pass
            return

        self._slots_locked = (self._replay_allowed() is False)
        if day is None:
            return

        slots, from_epg = self.build_day_slots(ch, day.date())
        cur_idx = None
        for i, (label, s, e) in enumerate(slots):
            self.slot_list.insert(tk.END, "  " + label)
            self.slot_items.append((s, e))
            is_current = (s <= now < e)
            if self._slots_locked or s >= now:
                self.slot_list.itemconfig(i, fg=COLORS["disabled_fg"])
            elif is_current:
                self.slot_list.itemconfig(i, fg=COLORS["live_green"])
            if cur_idx is None and is_current:
                cur_idx = i

        if ch is None or self._slots_locked:
            self.epg_var.set("")
        elif from_epg:
            self.epg_var.set("节目单：EPG（%d 个节目）" % len(slots))
        else:
            self.epg_var.set("无 EPG 数据，按整点时段显示")

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
            if cur_idx is not None:
                self.slot_list.see(cur_idx)

    def selected_slot(self):
        if self._slots_locked or self._replay_allowed() is False:
            return None
        sel = self.slot_list.curselection()
        if not sel or sel[0] >= len(self.slot_items):
            return None
        return self.slot_items[sel[0]]

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

    def _is_media_name(self, name):
        return os.path.splitext(name)[1].lower() in MEDIA_EXTS

    def _is_audio_name(self, name):
        return os.path.splitext(name)[1].lower() in AUDIO_EXTS

    def open_webdav_dialog(self):
        win = tk.Toplevel(self)
        win.title("浏览 WebDAV")
        win.configure(bg=COLORS["bg_panel"])
        win.transient(self)
        win.resizable(False, False)

        sources = list(self.cfg.get("webdav_sources") or [])
        names = [s.get("name") or s.get("url") or "?" for s in sources]

        frm = ttk.Frame(win, style="Panel.TFrame")
        frm.pack(fill=tk.BOTH, expand=True, padx=14, pady=12)

        ttk.Label(frm, text="已保存的连接", style="Dim.TLabel").grid(
            row=0, column=0, sticky="e", padx=(0, 8), pady=4)
        saved_var = tk.StringVar()
        saved_cb = ttk.Combobox(frm, textvariable=saved_var, values=names,
                                state="readonly", width=42)
        saved_cb.grid(row=0, column=1, columnspan=2, sticky="we", pady=4)

        ttk.Label(frm, text="名称", style="Panel.TLabel").grid(
            row=1, column=0, sticky="e", padx=(0, 8), pady=4)
        name_var = tk.StringVar()
        ttk.Entry(frm, textvariable=name_var, width=42).grid(
            row=1, column=1, columnspan=2, sticky="we", pady=4)

        ttk.Label(frm, text="地址", style="Panel.TLabel").grid(
            row=2, column=0, sticky="e", padx=(0, 8), pady=4)
        url_var = tk.StringVar()
        ttk.Entry(frm, textvariable=url_var, width=42).grid(
            row=2, column=1, columnspan=2, sticky="we", pady=4)

        ttk.Label(frm, text="用户名", style="Panel.TLabel").grid(
            row=3, column=0, sticky="e", padx=(0, 8), pady=4)
        user_var = tk.StringVar()
        ttk.Entry(frm, textvariable=user_var, width=42).grid(
            row=3, column=1, columnspan=2, sticky="we", pady=4)

        ttk.Label(frm, text="密码", style="Panel.TLabel").grid(
            row=4, column=0, sticky="e", padx=(0, 8), pady=4)
        pass_var = tk.StringVar()
        ttk.Entry(frm, textvariable=pass_var, width=42, show="●").grid(
            row=4, column=1, columnspan=2, sticky="we", pady=4)

        save_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(frm, text="保存到配置", variable=save_var).grid(
            row=5, column=1, sticky="w", pady=4)

        ttk.Label(frm, text="示例：http://192.168.1.10:5005/dav  或  https://nas.local/webdav",
                  style="Dim.TLabel").grid(row=6, column=0, columnspan=3,
                                           sticky="w", pady=(0, 8))

        def load_saved(_e=None):
            i = saved_cb.current()
            if i < 0 or i >= len(sources):
                return
            s = sources[i]
            name_var.set(s.get("name", ""))
            url_var.set(s.get("url", ""))
            user_var.set(s.get("user", ""))
            pass_var.set(s.get("password", ""))
        saved_cb.bind("<<ComboboxSelected>>", load_saved)
        if names:
            saved_cb.current(0)
            load_saved()

        def do_connect():
            name = name_var.get().strip()
            url = url_var.get().strip()
            user = user_var.get().strip()
            pwd = pass_var.get()
            if not url:
                messagebox.showerror("错误", "请填写 WebDAV 地址", parent=win)
                return
            if not url.lower().startswith(("http://", "https://")):
                url = "http://" + url
                url_var.set(url)
            if not name:
                name = url
                name_var.set(name)

            if save_var.get():
                entry = {"name": name, "url": url, "user": user, "password": pwd}
                src_list = list(self.cfg.get("webdav_sources") or [])
                for i, s in enumerate(src_list):
                    if (s.get("name") or "") == name:
                        src_list[i] = entry
                        break
                else:
                    src_list.append(entry)
                self.cfg["webdav_sources"] = src_list
                self.save_config()

            self._refresh_webdav_combo()
            win.destroy()
            self._enter_webdav({"name": name, "url": url,
                                "user": user, "password": pwd})

        def do_delete():
            i = saved_cb.current()
            if i < 0 or i >= len(sources):
                return
            s = sources[i]
            if not messagebox.askyesno(
                    "确认", "删除已保存的连接“%s”？" % (s.get("name") or s.get("url")),
                    parent=win):
                return
            src_list = list(self.cfg.get("webdav_sources") or [])
            src_list.pop(i)
            self.cfg["webdav_sources"] = src_list
            self.save_config()
            self._refresh_webdav_combo()
            win.destroy()
            self.open_webdav_dialog()

        bf = ttk.Frame(frm, style="Panel.TFrame")
        bf.grid(row=7, column=0, columnspan=3, pady=(10, 0))
        ttk.Button(bf, text="连接", command=do_connect).pack(side=tk.LEFT, padx=6)
        ttk.Button(bf, text="删除", command=do_delete).pack(side=tk.LEFT, padx=6)
        ttk.Button(bf, text="取消", command=win.destroy).pack(side=tk.LEFT, padx=6)

        win.grab_set()
        center_window(win, self)

    def _refresh_webdav_combo(self):
        if not hasattr(self, "webdav_cb"):
            return
        sources = list(self.cfg.get("webdav_sources") or [])
        self._webdav_combo_sources = sources

        names = ["📄 IPTV（本地列表）"]
        for s in sources:
            names.append("🌐 " + (s.get("name") or s.get("url") or "?"))

        self.webdav_cb["values"] = names

        if self.webdav_mode and self.webdav_source:
            cur = self.webdav_source.get("name") or self.webdav_source.get("url") or ""
            try:
                idx = names.index("🌐 " + cur)
            except ValueError:
                idx = 0
            self.webdav_var.set(names[idx])
        else:
            self.webdav_var.set(names[0])

    def _on_webdav_combo(self, event=None):
        idx = self.webdav_cb.current()
        if idx < 0:
            return

        if idx == 0:
            if not os.path.isfile(DEFAULT_M3U):
                self.status_var.set("未找到 %s，请通过“文件 → 打开 m3u 文件”加载" % DEFAULT_M3U)
                self._refresh_webdav_combo()
                return
            self.load_m3u(DEFAULT_M3U)
            self.status_var.set("已切换到本地IPTV列表：%s" % DEFAULT_M3U)
            return

        src_idx = idx - 1
        if src_idx >= len(self._webdav_combo_sources):
            return
        src = self._webdav_combo_sources[src_idx]

        if (self.webdav_mode and self.webdav_source
                and self.webdav_source.get("url") == src.get("url")
                and self.webdav_source.get("user") == src.get("user")):
            return

        self._enter_webdav(dict(src))

    def _enter_webdav(self, source):
        self.webdav_mode = True
        self.webdav_source = source
        self.webdav_path = ""
        self.webdav_items = []
        if self.search_var.get():
            self.search_var.set("")
        self._refresh_webdav_combo()
        self.status_var.set("正在连接 WebDAV：%s" % source["url"])
        self._navigate_webdav("")

    def _exit_webdav(self):
        if not self.webdav_mode:
            return
        self.webdav_mode = False
        self.webdav_source = None
        self.webdav_path = ""
        self.webdav_items = []
        self.apply_filter()
        self._refresh_webdav_combo()
        self.status_var.set("已退出 WebDAV 浏览")

    def _navigate_webdav(self, path):
        src = self.webdav_source
        if not src:
            return
        self._webdav_req_id += 1
        req_id = self._webdav_req_id

        self.status_var.set("正在加载 WebDAV 目录：%s" % (path or "/"))

        def worker():
            try:
                client = WebDAVClient(src["url"], src.get("user", ""),
                                      src.get("password", ""), timeout=15)
                entries = client.list(path)
            except Exception as e:
                err_msg = str(e)
                self.after(0, lambda: self._on_webdav_error(req_id, path, err_msg))
                return
            self.after(0, lambda: self._on_webdav_loaded(req_id, path, entries))

        threading.Thread(target=worker, daemon=True).start()

    def _on_webdav_loaded(self, req_id, path, entries):
        if req_id != self._webdav_req_id:
            return

        self.webdav_path = path
        items = []

        if path:
            parent = path.rsplit("/", 1)[0] if "/" in path else ""
            items.append({
                "name": "⬅ 返回上级", "webdav": True, "is_dir": True,
                "is_back": True, "path": parent, "url": "",
            })
        else:
            items.append({
                "name": "⬅ 退出 WebDAV 浏览", "webdav": True, "is_dir": True,
                "is_back": True, "exit_browse": True, "path": "", "url": "",
            })

        dirs, files = [], []
        for e in entries:
            nm = e.get("name", "")
            if not nm or nm.startswith("."):
                continue
            if e.get("is_dir"):
                dirs.append(e)
            elif self._is_media_name(nm):
                files.append(e)

        dirs.sort(key=lambda x: x["name"].lower())
        files.sort(key=lambda x: x["name"].lower())

        for d in dirs:
            items.append({
                "name": "📁 " + d["name"], "webdav": True, "is_dir": True,
                "path": (path + "/" + d["name"]).strip("/") if path else d["name"],
                "url": d["url"], "raw_name": d["name"],
            })
        for f in files:
            mark = "🎵 " if self._is_audio_name(f["name"]) else "🎬 "
            items.append({
                "name": mark + f["name"], "webdav": True, "is_dir": False,
                "path": (path + "/" + f["name"]).strip("/") if path else f["name"],
                "url": f["url"], "raw_name": f["name"],
            })

        self.webdav_items = items
        self.apply_filter()
        self.status_var.set("WebDAV %s：%d 个子目录，%d 个媒体文件" %
                            (path or "/", len(dirs), len(files)))

    def _on_webdav_error(self, req_id, path, err):
        if req_id != self._webdav_req_id:
            return
        self.status_var.set("WebDAV 加载失败：%s" % err)
        messagebox.showerror("WebDAV 错误",
                             "无法读取目录：%s\n\n%s" % (path or "/", err))

    def _play_webdav_file(self, item):
        src = self.webdav_source or {}
        user = (src.get("user") or "").strip()
        pwd = src.get("password") or ""
        url = item.get("url") or ""
        if not url:
            self.status_var.set("WebDAV 条目缺少 URL")
            return

        if user:
            p = urllib.parse.urlparse(url)
            netloc = "%s:%s@%s" % (
                urllib.parse.quote(user, safe=""),
                urllib.parse.quote(pwd, safe=""),
                p.netloc,
            )
            url = urllib.parse.urlunparse(
                (p.scheme, netloc, p.path, p.params, p.query, p.fragment))

        self._net_paused = False
        self.current = item
        self.current_url = url
        self.current_title = "[WebDAV] " + item.get("raw_name", item["name"])
        self.current_live = False
        self._clear_replay_range()

        if self.player is None:
            self.play_external(url)
            self.status_var.set("正在使用外部播放器：%s" % item["name"])
            return

        try:
            media = self.vlc_instance.media_new(url)
            media.add_option(":http-reconnect=true")
            media.add_option(":http-continuous")
            media.add_option(":avcodec-hw=%s" %
                            ("any" if self.cfg.get("hw_decode", True) else "none"))
            self.player.set_media(media)
            self.player.play()
            self.player.audio_set_volume(self.vol_var.get())
            self.status_var.set("正在播放：%s" % item["name"])
        except Exception as e:
            self.status_var.set("播放失败：%s" % e)

    def open_media_file(self):
        all_exts = " ".join("*" + e for e in sorted(MEDIA_EXTS))
        types = [
            ("媒体文件（视频/音频）", all_exts),
            ("视频文件", "*.mp4 *.mkv *.avi *.mov *.wmv *.flv *.webm *.ts *.m4v *.mpg *.mpeg"),
            ("音频文件", "*.mp3 *.aac *.flac *.wav *.ape *.ogg *.wma *.m4a *.opus"),
            ("所有文件", "*.*"),
        ]
        paths = filedialog.askopenfilenames(title="选择媒体文件", filetypes=types)
        if not paths:
            return

        added = []
        for p in paths:
            if not os.path.isfile(p):
                continue
            if any(c["url"] == p for c in self.channels):
                continue
            name = os.path.splitext(os.path.basename(p))[0]
            ch = {"name": name, "url": p, "local": True}
            self.channels.append(ch)
            added.append(ch)

        if not added:
            self.status_var.set("未添加新的媒体文件")
            return

        if self.search_var.get():
            self.search_var.set("")
        else:
            self.apply_filter()

        first = added[0]
        try:
            idx = self.filtered.index(first)
        except ValueError:
            idx = None

        if idx is not None:
            self.ch_list.selection_clear(0, tk.END)
            self.ch_list.selection_set(idx)
            self.ch_list.activate(idx)
            self.ch_list.see(idx)

        self.current = first
        self._clear_replay_range()
        self.seek_bar.set_enabled(False)
        self.seek_bar.set_value(0)
        self.time_var.set("--:--:-- / --:--:--")
        self._play_local_file(first["url"], "[媒体] " + first["name"])
        self.status_var.set("已添加 %d 个媒体文件，正在播放：%s"
                            % (len(added), first["name"]))

    def load_m3u(self, path):
        self.webdav_mode = False
        self.webdav_source = None
        self.webdav_path = ""
        self.webdav_items = []
        try:
            self.channels = parse_m3u(path)
        except Exception as e:
            messagebox.showerror("错误", "读取 m3u 失败：%s" % e)
            return
        self.apply_filter()
        self._refresh_webdav_combo()
        self.status_var.set("已加载 %d 个频道：%s" % (len(self.channels), path))

    def apply_filter(self):
        kw = self.search_var.get().strip().lower()
        source = self.webdav_items if self.webdav_mode else self.channels
        if kw:
            self.filtered = [c for c in source
                             if c.get("is_back") or kw in c["name"].lower()]
        else:
            self.filtered = list(source)
        self.ch_list.delete(0, tk.END)
        for c in self.filtered:
            self.ch_list.insert(tk.END, c["name"])
        if self.webdav_mode:
            self.count_var.set("%d / %d 项" % (len(self.filtered), len(source)))
        else:
            self.count_var.set("%d / %d 个频道" % (len(self.filtered), len(source)))
        self.refresh_slots()

    def selected_channel(self):
        sel = self.ch_list.curselection()
        if not sel:
            return None
        return self.filtered[sel[0]]

    def on_channel_select(self):
        ch = self.selected_channel()

        if self.webdav_mode:
            if ch:
                if ch.get("is_back"):
                    self.replay_ch_var.set("返回")
                elif ch.get("is_dir"):
                    self.replay_ch_var.set("目录：%s" % ch.get("raw_name", ""))
                else:
                    self.replay_ch_var.set("文件：%s" % ch.get("raw_name", ""))
            self.refresh_slots()
            return

        if ch:
            if self._is_local_media(ch):
                tip = "（本地媒体文件）"
            elif replay_supported(ch["url"], self.cfg):
                tip = ""
            else:
                tip = "（不支持回看）"
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
            self.vlc_instance = vlc.Instance(
                "--network-caching=1500",
                "--no-video-title-show",
                "--quiet",
            )
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

            self._player_error_cb = lambda e: self.after(0, self._on_player_error)
            em = self.player.event_manager()
            em.event_attach(vlc.EventType.MediaPlayerEncounteredError,
                            self._player_error_cb)
        except Exception as e:
            self.player = None
            self.status_var.set("VLC 初始化失败，改用外部播放器：%s" % e)

    def _on_player_error(self):
        if getattr(self, "_closing", False):
            return
        try:
            self.status_var.set(
                "播放失败：无法打开该地址（请检查网络、地址或回看参数）："
                + self.current_url)
        except Exception:
            pass

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

    def _play_local_file(self, path, title):
        self._cancel_seek_status_timer()
        self._net_paused = False
        self.current_url = path
        self.current_title = title
        self.current_live = False

        if self.player is None:
            self.play_external(path)
            self.status_var.set("正在使用外部播放器：%s" % title)
            return

        if not os.path.isfile(path):
            self.status_var.set("文件不存在：%s" % path)
            messagebox.showwarning("文件不存在", path, parent=self)
            return

        media = None
        try:
            media = self.vlc_instance.media_new(self._path_to_mrl(path))
        except Exception:
            pass

        if media is None or not media.get_mrl():
            if media is not None:
                try:
                    media.release()
                except Exception:
                    pass
            try:
                media = self.vlc_instance.media_new_path(path)
            except Exception:
                media = None

        if media is None:
            self.status_var.set("VLC 无法识别该文件：%s" % path)
            messagebox.showwarning(
                "无法播放",
                "VLC 无法创建媒体对象。\n可能原因：\n"
                "• 文件被占用或损坏\n"
                "• 路径含特殊字符\n"
                "• VLC 未正确安装\n\n文件：%s" % path, parent=self)
            return

        try:
            media.add_option(":avcodec-hw=%s" %
                             ("any" if self.cfg.get("hw_decode", True) else "none"))
        except Exception:
            pass

        try:
            self.player.set_media(media)
            ret = self.player.play()
            if ret == -1:
                self.status_var.set("VLC 拒绝播放：%s" % title)
                print("[play] player.play() returned -1 for", path)
                return
            self.player.audio_set_volume(self.vol_var.get())
            self.status_var.set("正在播放：%s" % title)
        except Exception as e:
            self.status_var.set("播放异常：%s" % e)
            messagebox.showwarning("播放失败", str(e), parent=self)

    def _path_to_mrl(self, path):
        p = os.path.abspath(path).replace("\\", "/")
        if not p.startswith("/"):
            p = "/" + p
        return "file://" + urllib.parse.quote(p, safe="/:")

    def play_url(self, url, title, live=True):
        self._cancel_seek_status_timer()
        self._net_paused = False
        if is_local_media_file(url):
            self._play_local_file(url, title)
            return

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

        if ch.get("webdav"):
            if ch.get("exit_browse"):
                self._exit_webdav()
                return
            if ch.get("is_dir"):
                self._navigate_webdav(ch.get("path", ""))
                return
            self._play_webdav_file(ch)
            return

        self._clear_replay_range()
        self.current = ch
        if self._is_local_media(ch):
            self.seek_bar.set_enabled(False)
            self.seek_bar.set_value(0)
            self.time_var.set("--:--:-- / --:--:--")
            self._play_local_file(ch["url"], "[媒体] " + ch["name"])
        else:
            self.play_url(ch["url"], "[直播] " + ch["name"])
            self._refresh_live_bar()

    def replay_url_for_selection(self):
        ch = self.selected_channel() or self.current
        if not ch:
            messagebox.showinfo("提示", "请先在左侧选择频道")
            return None
        if ch.get("webdav"):
            messagebox.showinfo("提示", "WebDAV 文件不支持回看")
            return None
        if self._is_local_media(ch):
            messagebox.showinfo("提示", "本地媒体文件不支持回看")
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
        now = self.local_now()
        if start <= now < end:
            self._clear_replay_range()
            self.current = ch
            self.play_url(ch["url"], "[直播] " + ch["name"], live=True)
            self._refresh_live_bar()
            self.status_var.set("当前时段为直播：%s" % ch["name"])
            return

        if start >= now:
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
        if self.player is None:
            return
        try:
            st = self.player.get_state()
        except Exception:
            return
        if st in (vlc.State.Ended, vlc.State.Stopped, vlc.State.Error):
            return

        if self._is_file_playback():
            try:
                self.player.pause()
            except Exception:
                pass
            return

        if self._net_paused:
            self._net_paused = False
            url = self.current_url
            title = self.current_title
            live = self.current_live
            if not url:
                return
            try:
                self.player.stop()
            except Exception:
                pass
            self.play_url(url, title, live=live)
            if live:
                self._refresh_live_bar()
        else:
            try:
                self.player.pause()
            except Exception:
                pass
            self._net_paused = True
            self.status_var.set("已暂停（再按空格 / 点击画面继续）")

    def stop(self):
        self._cancel_seek_status_timer()
        self._net_paused = False
        if self.player is not None:
            self.player.stop()
        self.kill_external()
        self._clear_replay_range()
        self.current = None
        self.current_url = ""
        self.time_var.set("--:--:-- / --:--:--")
        self.status_var.set("已停止")

    def on_volume(self, _v=None):
        v = int(float(self.vol_var.get()))
        if self.player is not None:
            self.player.audio_set_volume(v)
        self.cfg["volume"] = v

    def _apply_left_width(self):
        self.left_panel.grid_propagate(False)
        self.left_panel.pack_propagate(False)
        if self.left_visible:
            self.left_panel.configure(
                width=max(150, min(600, int(self.cfg.get("left_width", 250)))))
            return
        self.update_idletasks()
        w = 20
        src = self.right_btn_col if self.right_btn_col is not None else self.left_btn_col
        if src is not None:
            rw = src.winfo_reqwidth()
            if rw > 0:
                w = rw
        self.left_panel.configure(width=w)

    def _apply_left_visible(self):
        if self.left_visible:
            self.left_body.grid(row=0, column=0, sticky="nsew")
            self.left_btn.config(text="◀")
        else:
            self.left_body.grid_remove()
            self.left_btn.config(text="▶")
        self._apply_left_width()

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

    def _on_video_click(self, event=None):
        if self._video_click_after_id is not None:
            try:
                self.after_cancel(self._video_click_after_id)
            except Exception:
                pass
        self._video_click_after_id = self.after(220, self._fire_video_click)
        return "break"

    def _fire_video_click(self):
        self._video_click_after_id = None
        if getattr(self, "_closing", False):
            return
        self.toggle_pause()

    def _on_video_double_click(self, event=None):
        if self._video_click_after_id is not None:
            try:
                self.after_cancel(self._video_click_after_id)
            except Exception:
                pass
            self._video_click_after_id = None
        self.toggle_fullscreen()
        return "break"

    def _on_space_key(self, event=None):
        if self._focus_is_text_input():
            return None
        if self.player is None:
            return None
        self.toggle_pause()
        return "break"

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
        else:
            self.left_body.grid_remove()
        self._apply_left_width()

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
            self.start_epg_update(silent=True)
            win.destroy()

        bf = ttk.Frame(win, style="Panel.TFrame")
        bf.grid(row=len(rows) + 1, column=0, columnspan=2, pady=10)
        ttk.Button(bf, text="确定", command=ok).pack(side=tk.LEFT, padx=8)
        ttk.Button(bf, text="取消", command=win.destroy).pack(side=tk.LEFT, padx=8)

        win.grab_set()
        center_window(win, self)

    def on_close(self):
        self._cancel_seek_status_timer()
        if getattr(self, "_closing", False):
            return
        self._closing = True

        if self.epg_state:
            self.epg_state["cancel"] = True

        if self._progress_after_id:
            try:
                self.after_cancel(self._progress_after_id)
            except Exception:
                pass
            self._progress_after_id = None

        if self._closing_after_id:
            try:
                self.after_cancel(self._closing_after_id)
            except Exception:
                pass
            self._closing_after_id = None

        if self._video_click_after_id is not None:
            try:
                self.after_cancel(self._video_click_after_id)
            except Exception:
                pass
            self._video_click_after_id = None

        self.save_config()

        try:
            self.withdraw()
        except Exception:
            pass

        try:
            if self.player is not None:
                try:
                    em = self.player.event_manager()
                    if self._player_error_cb is not None:
                        em.event_detach(vlc.EventType.MediaPlayerEncounteredError)
                except Exception:
                    pass
                try:
                    self.player.stop()
                except Exception:
                    pass
        except Exception:
            pass

        self._closing_after_id = self.after(450, self._final_close)

    def _final_close(self):
        self._closing_after_id = None

        try:
            if self.player is not None:
                try:
                    self.player.release()
                except Exception:
                    pass
                self.player = None
        except Exception:
            pass

        try:
            if self.vlc_instance is not None:
                try:
                    self.vlc_instance.release()
                except Exception:
                    pass
                self.vlc_instance = None
        except Exception:
            pass

        try:
            self.kill_external()
        except Exception:
            pass

        try:
            self.destroy()
        except Exception:
            pass


if __name__ == "__main__":
    IPTVApp().mainloop()
