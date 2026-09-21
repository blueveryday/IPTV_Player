package com.iptv.tv

import android.content.ActivityNotFoundException
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.view.KeyEvent
import android.view.View
import android.view.WindowManager
import android.widget.*
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import org.videolan.libvlc.LibVLC
import org.videolan.libvlc.Media
import org.videolan.libvlc.MediaPlayer
import org.videolan.libvlc.util.VLCVideoLayout
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.time.Duration
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import kotlin.math.max
import kotlin.math.min

data class SlotItem(val label: String, val start: LocalDateTime, val end: LocalDateTime, val color: Int)

class MainActivity : AppCompatActivity() {

    companion object {
        const val CODE_VERSION = "IPTV Player TV v2026.09.21（移植自 v2026.09.20）"
        const val GITHUB_URL = "https://github.com/blueveryday/IPTV_Player"
        const val SEEK = 5                       // 与原版 SEEK_GRANULARITY 相同
        const val WEEKDAY = "一二三四五六日"
        val RED = Color.parseColor("#ff5555")
        val GREEN = Color.parseColor("#5ecb6b")
        val T_FMT: DateTimeFormatter = DateTimeFormatter.ofPattern("HH:mm:ss")
        val DT_FMT: DateTimeFormatter = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")
    }

    private lateinit var cfg: Config
    private val ui = Handler(Looper.getMainLooper())

    // views
    private lateinit var videoLayout: VLCVideoLayout
    private lateinit var bottomBar: View
    private lateinit var titleText: TextView
    private lateinit var seekBar: SeekBarView
    private lateinit var timeText: TextView
    private lateinit var statusText: TextView
    private lateinit var leftPanel: View
    private lateinit var rightPanel: View
    private lateinit var channelList: RecyclerView
    private lateinit var slotList: RecyclerView
    private lateinit var countText: TextView
    private lateinit var searchText: TextView
    private lateinit var replayChText: TextView
    private lateinit var epgText: TextView
    private lateinit var dateBtn: TextView
    private lateinit var btnSearch: TextView
    private lateinit var btnMenu: TextView
    private lateinit var btnReplay: TextView
    private lateinit var btnCopy: TextView
    private lateinit var chAdapter: SimpleAdapter<Channel>
    private lateinit var slotAdapter: SimpleAdapter<SlotItem>

    // 频道
    private var channels: List<Channel> = emptyList()
    private var filtered: List<Channel> = emptyList()
    private var keyword = ""
    private var selIdx = -1
    private var current: Channel? = null
    private var currentUrl = ""
    private var currentTitle = ""
    private var currentLive = true

    // 播放器
    private var libVlc: LibVLC? = null
    private var player: MediaPlayer? = null
    private var pendingStart: Runnable? = null
    private var resumeOnStart = false

    // 回看状态（与原版一致）
    private var rangeStart: LocalDateTime? = null
    private var rangeEnd: LocalDateTime? = null
    private var anchorTime: LocalDateTime? = null
    private var anchorWall: LocalDateTime? = null
    private var liveRewind = false

    // 节目单
    private var dateIdx = 0
    private var slotsLocked = false
    private var slotSel = -1
    private var epgState: EpgState? = null

    private var lastBack = 0L

    private val pickM3u = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) importM3u(uri)
    }

    // ================= 生命周期 =================

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        hideSystemBars()

        cfg = Config(File(filesDir, "iptv_config.json"))
        Epg.init(filesDir)
        ensureDefaultCsv()

        videoLayout = findViewById(R.id.videoLayout)
        bottomBar = findViewById(R.id.bottomBar)
        titleText = findViewById(R.id.titleText)
        seekBar = findViewById(R.id.seekBar)
        timeText = findViewById(R.id.timeText)
        statusText = findViewById(R.id.statusText)
        leftPanel = findViewById(R.id.leftPanel)
        rightPanel = findViewById(R.id.rightPanel)
        channelList = findViewById(R.id.channelList)
        slotList = findViewById(R.id.slotList)
        countText = findViewById(R.id.countText)
        searchText = findViewById(R.id.searchText)
        replayChText = findViewById(R.id.replayChText)
        epgText = findViewById(R.id.epgText)
        dateBtn = findViewById(R.id.dateBtn)
        btnSearch = findViewById(R.id.btnSearch)
        btnMenu = findViewById(R.id.btnMenu)
        btnReplay = findViewById(R.id.btnReplay)
        btnCopy = findViewById(R.id.btnCopy)

        chAdapter = SimpleAdapter({ it.name }, { null }, { pos -> onChannelPicked(pos) }, { pos -> onChannelFocused(pos) })
        channelList.layoutManager = LinearLayoutManager(this)
        channelList.adapter = chAdapter
        channelList.itemAnimator = null

        slotAdapter = SimpleAdapter({ "  " + it.label }, { it.color }, { pos -> onSlotPicked(pos) }, { pos -> onSlotFocused(pos) })
        slotList.layoutManager = LinearLayoutManager(this)
        slotList.adapter = slotAdapter
        slotList.itemAnimator = null

        seekBar.onSeek = { v, ph -> onSeekBar(v, ph) }
        btnSearch.setOnClickListener { openSearch() }
        btnMenu.setOnClickListener { showMenu() }
        btnReplay.setOnClickListener { playReplay() }
        btnCopy.setOnClickListener { copyReplayUrl() }
        dateBtn.setOnClickListener { chooseDate() }
        updateDateBtn()

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (panelsVisible()) { hidePanels(); return }
                val now = System.currentTimeMillis()
                if (now - lastBack < 2000) finish()
                else { lastBack = now; Toast.makeText(this@MainActivity, "再按一次返回键退出", Toast.LENGTH_SHORT).show() }
            }
        })

        initPlayer()
        ui.postDelayed({ loadDefault() }, 200)
        ui.postDelayed({ updateProgress() }, 500)
        ui.postDelayed({ autoEpgUpdate() }, 1500)
        ui.postDelayed({ showLeft() }, 400)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    private fun hideSystemBars() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val c = WindowInsetsControllerCompat(window, window.decorView)
        c.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        c.hide(WindowInsetsCompat.Type.systemBars())
    }

    override fun onStop() {
        super.onStop()
        if (currentUrl.isNotEmpty()) {
            resumeOnStart = true
            cancelPending()
            player?.stop()
        }
    }

    override fun onStart() {
        super.onStart()
        if (resumeOnStart) {
            resumeOnStart = false
            val ch = current
            val end = rangeEnd
            if (!currentLive && ch != null && end != null) {
                val cur = curPos()
                if (cur != null) {
                    anchorTime = cur; anchorWall = localNow()
                    playUrl(buildReplayUrl(ch.url, cur, end, cfg), currentTitle, false)
                }
            } else if (currentUrl.isNotEmpty()) startMedia(currentUrl)
        }
    }

    override fun onDestroy() {
        epgState?.cancel = true
        cfg.save()
        try { player?.stop(); player?.detachViews(); player?.release(); libVlc?.release() } catch (_: Exception) {}
        super.onDestroy()
    }

    // ================= 遥控器按键 =================

    private fun panelsVisible() = leftPanel.visibility == View.VISIBLE || rightPanel.visibility == View.VISIBLE

    private fun isDescendant(parent: View, child: View?): Boolean {
        var v: View? = child
        while (v != null) { if (v === parent) return true; v = v.parent as? View }
        return false
    }

    private fun isCenter(k: Int) = k == KeyEvent.KEYCODE_DPAD_CENTER || k == KeyEvent.KEYCODE_ENTER || k == KeyEvent.KEYCODE_NUMPAD_ENTER

    override fun onKeyDown(keyCode: Int, ev: KeyEvent): Boolean {
        val step = SEEK * (1 + min(ev.repeatCount / 4, 12))   // 长按加速，仍是 5 秒的整数倍
        when (keyCode) {
            KeyEvent.KEYCODE_MENU -> { showMenu(); return true }
            KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE, KeyEvent.KEYCODE_MEDIA_PLAY, KeyEvent.KEYCODE_MEDIA_PAUSE -> { togglePause(); return true }
            KeyEvent.KEYCODE_MEDIA_STOP -> { stopPlay(); return true }
            KeyEvent.KEYCODE_CHANNEL_UP, KeyEvent.KEYCODE_PAGE_UP -> { navigateChannel(-1); return true }
            KeyEvent.KEYCODE_CHANNEL_DOWN, KeyEvent.KEYCODE_PAGE_DOWN -> { navigateChannel(1); return true }
            KeyEvent.KEYCODE_MEDIA_REWIND -> { arrowLeft(step); return true }
            KeyEvent.KEYCODE_MEDIA_FAST_FORWARD -> { arrowRight(step); return true }
        }
        if (!panelsVisible()) {
            when (keyCode) {
                KeyEvent.KEYCODE_DPAD_UP -> { navigateChannel(-1); return true }
                KeyEvent.KEYCODE_DPAD_DOWN -> { navigateChannel(1); return true }
                KeyEvent.KEYCODE_DPAD_LEFT -> { arrowLeft(step); return true }
                KeyEvent.KEYCODE_DPAD_RIGHT -> { arrowRight(step); return true }
            }
            if (isCenter(keyCode)) { ev.startTracking(); return true }
        } else {
            val f = currentFocus
            if (leftPanel.visibility == View.VISIBLE && keyCode == KeyEvent.KEYCODE_DPAD_RIGHT && isDescendant(channelList, f)) {
                showRight(); return true
            }
            if (rightPanel.visibility == View.VISIBLE && keyCode == KeyEvent.KEYCODE_DPAD_LEFT) {
                showLeft(); return true
            }
        }
        return super.onKeyDown(keyCode, ev)
    }

    override fun onKeyLongPress(keyCode: Int, ev: KeyEvent): Boolean {
        if (isCenter(keyCode) && !panelsVisible()) { showMenu(); return true }
        return super.onKeyLongPress(keyCode, ev)
    }

    override fun onKeyUp(keyCode: Int, ev: KeyEvent): Boolean {
        if (isCenter(keyCode) && !panelsVisible() && ev.isTracking && !ev.isCanceled) { showLeft(); return true }
        return super.onKeyUp(keyCode, ev)
    }

    // ================= 面板 =================

    private fun hidePanels() {
        leftPanel.visibility = View.GONE
        rightPanel.visibility = View.GONE
    }

    private fun showLeft() {
        rightPanel.visibility = View.GONE
        leftPanel.visibility = View.VISIBLE
        val pos = if (selIdx in filtered.indices) selIdx else 0
        (channelList.layoutManager as LinearLayoutManager).scrollToPositionWithOffset(pos, channelList.height / 3)
        ui.postDelayed({
            (channelList.findViewHolderForAdapterPosition(pos)?.itemView ?: btnSearch).requestFocus()
        }, 80)
    }

    private fun showRight() {
        leftPanel.visibility = View.GONE
        rightPanel.visibility = View.VISIBLE
        refreshSlots()
        val target = slotAdapter.items.indexOfFirst { it.color == GREEN }.let { if (it < 0) 0 else it }
        (slotList.layoutManager as LinearLayoutManager).scrollToPositionWithOffset(target, slotList.height / 3)
        ui.postDelayed({
            (slotList.findViewHolderForAdapterPosition(target)?.itemView ?: dateBtn).requestFocus()
        }, 80)
    }

    private fun showBar(ms: Long = 5000) {
        bottomBar.visibility = View.VISIBLE
        titleText.text = currentTitle
        ui.removeCallbacks(hideBarRunnable)
        ui.postDelayed(hideBarRunnable, ms)
    }

    private val hideBarRunnable = Runnable { bottomBar.visibility = View.GONE }

    private fun setStatus(s: String) {
        statusText.text = s
        showBar()
    }

    private fun alert(title: String, msg: String) {
        AlertDialog.Builder(this).setTitle(title).setMessage(msg).setPositiveButton("确定", null).show()
    }

    // ================= 频道 =================

    private fun ensureDefaultCsv() {
        try {
            val f = Epg.defaultCsv
            if (!f.exists()) {
                f.parentFile?.mkdirs()
                assets.open("channel_epg_chongqing.csv").use { i -> f.outputStream().use { o -> i.copyTo(o) } }
            }
        } catch (_: Exception) { /* 没有内置也没关系，可在“EPG 参数”里指定 */ }
    }

    private fun loadDefault() {
        val saved = cfg.s("m3u_path").takeIf { it.isNotEmpty() }?.let { File(it) }
        val ext = getExternalFilesDir(null)?.let { File(it, "iptv.m3u") }
        val internal = File(filesDir, "iptv.m3u")
        val f = listOfNotNull(saved, ext, internal).firstOrNull { it.isFile }
        if (f != null) { loadM3u(f); return }
        try {
            assets.open("iptv.m3u").use { i -> internal.outputStream().use { o -> i.copyTo(o) } }
            loadM3u(internal)
        } catch (_: Exception) {
            setStatus("未找到 iptv.m3u。请在菜单中“打开 m3u 文件”或“从网址加载 m3u”，或用 adb 放到 ${ext?.path}")
        }
    }

    private fun loadM3u(f: File) {
        try {
            channels = parseM3u(decodeAuto(f.readBytes()))
        } catch (e: Exception) {
            alert("错误", "读取 m3u 失败：${e.message}"); return
        }
        applyFilter()
        setStatus("已加载 ${channels.size} 个频道：${f.path}")
    }

    private fun importM3u(uri: Uri) {
        try {
            val dst = File(filesDir, "iptv.m3u")
            contentResolver.openInputStream(uri)!!.use { i -> dst.outputStream().use { o -> i.copyTo(o) } }
            cfg.put("m3u_path", dst.path); cfg.save()
            loadM3u(dst)
        } catch (e: Exception) {
            alert("错误", "导入失败：${e.message}")
        }
    }

    private fun openM3uFile() {
        try { pickM3u.launch(arrayOf("*/*")) }
        catch (e: ActivityNotFoundException) {
            alert("无法选择文件", "本机没有文件选择器。请改用“从网址加载 m3u”，或用 adb 把 iptv.m3u 推送到：\n${getExternalFilesDir(null)?.path}")
        }
    }

    private fun loadM3uFromUrl() {
        val et = EditText(this).apply {
            setText(cfg.s("m3u_url")); hint = "http://…/iptv.m3u"
            inputType = InputType.TYPE_TEXT_VARIATION_URI; setSingleLine()
            setTextColor(Color.WHITE); setHintTextColor(Color.GRAY)
        }
        AlertDialog.Builder(this).setTitle("从网址加载 m3u").setView(pad(et))
            .setPositiveButton("下载") { _, _ ->
                val url = et.text.toString().trim()
                if (url.isEmpty()) return@setPositiveButton
                cfg.put("m3u_url", url); cfg.save()
                setStatus("正在下载 m3u…")
                Thread {
                    try {
                        val c = URL(url).openConnection() as HttpURLConnection
                        c.connectTimeout = 20000; c.readTimeout = 20000
                        c.setRequestProperty("User-Agent", "Mozilla/5.0 (IPTVPlayer)")
                        val bytes = c.inputStream.use { it.readBytes() }
                        val dst = File(filesDir, "iptv.m3u")
                        dst.writeBytes(bytes)
                        ui.post { cfg.put("m3u_path", dst.path); cfg.save(); loadM3u(dst) }
                    } catch (e: Exception) {
                        ui.post { alert("下载失败", e.message ?: e.toString()) }
                    }
                }.start()
            }.setNegativeButton("取消", null).show()
    }

    private fun openSearch() {
        val et = EditText(this).apply {
            setText(keyword); hint = "输入频道名关键字"; setSingleLine()
            setTextColor(Color.WHITE); setHintTextColor(Color.GRAY)
        }
        AlertDialog.Builder(this).setTitle("搜索频道").setView(pad(et))
            .setPositiveButton("搜索") { _, _ -> keyword = et.text.toString().trim(); applyFilter(); showLeft() }
            .setNeutralButton("清除") { _, _ -> keyword = ""; applyFilter(); showLeft() }
            .setNegativeButton("取消", null).show()
    }

    private fun applyFilter() {
        val kw = keyword.lowercase()
        filtered = channels.filter { it.name.lowercase().contains(kw) }
        selIdx = -1
        chAdapter.items = filtered
        chAdapter.setSelectedPos(-1)
        countText.text = "${filtered.size} / ${channels.size} 个频道"
        searchText.text = if (keyword.isEmpty()) "" else "搜索：$keyword"
        refreshSlots()
    }

    private fun selectedChannel(): Channel? = filtered.getOrNull(selIdx)

    private val selectRunnable = Runnable { onChannelSelect() }

    private fun onChannelFocused(pos: Int) {
        selIdx = pos
        ui.post { chAdapter.setSelectedPos(pos) }
        ui.removeCallbacks(selectRunnable)
        ui.postDelayed(selectRunnable, 200)
    }

    private fun onChannelPicked(pos: Int) {
        selIdx = pos
        onChannelSelect()
        hidePanels()
        playLive()
    }

    private fun onChannelSelect() {
        val ch = selectedChannel()
        if (ch != null) {
            val tip = if (replaySupported(ch.url, cfg)) "" else "（不支持回看）"
            replayChText.text = "${ch.name} $tip"
        }
        refreshSlots()
    }

    private fun navigateChannel(delta: Int) {
        if (filtered.isEmpty()) return
        val idx = if (selIdx >= 0) (selIdx + delta).coerceIn(0, filtered.size - 1) else 0
        if (idx == selIdx && selIdx >= 0) return
        selIdx = idx
        chAdapter.setSelectedPos(idx)
        onChannelSelect()
        playLive(350)
    }

    // ================= 播放 =================

    private fun initPlayer() {
        try {
            libVlc = LibVLC(this, arrayListOf("--network-caching=1500", "--quiet"))
            player = MediaPlayer(libVlc).also { p ->
                p.attachViews(videoLayout, null, false, false)
                p.setEventListener { ev ->
                    if (ev.type == MediaPlayer.Event.EncounteredError)
                        runOnUiThread { setStatus("播放失败：无法打开该地址（请检查网络、地址或回看参数）：$currentUrl") }
                }
            }
        } catch (e: Throwable) {
            player = null
            setStatus("VLC 初始化失败：${e.message}")
        }
    }

    private fun cancelPending() {
        pendingStart?.let { ui.removeCallbacks(it) }
        pendingStart = null
    }

    private fun playUrl(url: String, title: String, live: Boolean = true, delayMs: Long = 0) {
        currentUrl = url; currentTitle = title; currentLive = live
        cancelPending()
        if (delayMs > 0) {
            val r = Runnable { pendingStart = null; startMedia(url) }
            pendingStart = r
            ui.postDelayed(r, delayMs)
        } else startMedia(url)
        setStatus("正在播放：$title")
    }

    private fun startMedia(url: String) {
        val p = player ?: run { setStatus("播放器未初始化，无法播放"); return }
        try {
            val m = Media(libVlc, Uri.parse(url))
            if (url.lowercase().startsWith("rtsp://")) {
                if (cfg.b("rtsp_tcp")) m.addOption(":rtsp-tcp")
            } else m.addOption(":http-reconnect=true")
            m.setHWDecoderEnabled(cfg.b("hw_decode"), false)
            p.media = m
            m.release()
            p.play()
        } catch (e: Exception) {
            setStatus("播放出错：${e.message}")
        }
    }

    private fun playLive(delayMs: Long = 0) {
        val ch = selectedChannel()
        if (ch == null) { Toast.makeText(this, "请先选择频道", Toast.LENGTH_SHORT).show(); return }
        clearReplayRange()
        current = ch
        playUrl(ch.url, "[直播] " + ch.name, true, delayMs)
        refreshLiveBar()
    }

    private fun togglePause() {
        val p = player ?: return
        if (p.isPlaying) p.pause() else p.play()
    }

    private fun stopPlay() {
        cancelPending()
        player?.stop()
        clearReplayRange()
        current = null
        currentUrl = ""
        timeText.text = "--:--:-- / --:--:--"
        setStatus("已停止")
    }

    private fun onDecodeOptionChanged() {
        cfg.save()
        if (player == null || currentUrl.isEmpty()) return
        if (player!!.isPlaying) {
            player!!.stop()
            startMedia(currentUrl)
            setStatus("已切换（硬解=${if (cfg.b("hw_decode")) "开" else "关"}, RTSP-TCP=${if (cfg.b("rtsp_tcp")) "开" else "关"}）并重新播放：$currentTitle")
        }
    }

    // ================= 回看 / 进度（移植自原版） =================

    private fun localNow(): LocalDateTime = LocalDateTime.now(ZoneOffset.UTC).plusHours(cfg.i("tz_offset").toLong())
    private fun secs(a: LocalDateTime, b: LocalDateTime): Double = Duration.between(a, b).toMillis() / 1000.0

    private fun curPos(): LocalDateTime? {
        val a = anchorTime ?: return null
        val w = anchorWall ?: return null
        val e = rangeEnd ?: return null
        val now = localNow()
        val cur = a.plusNanos(Duration.between(w, now).toNanos())
        val cap = if (liveRewind) now else e
        return if (cur.isAfter(cap)) cap else cur
    }

    private fun clearReplayRange() {
        rangeStart = null; rangeEnd = null; anchorTime = null; anchorWall = null; liveRewind = false
        seekBar.setValue(0.0)
    }

    private fun setReplayRange(s: LocalDateTime, e: LocalDateTime) {
        rangeStart = s; rangeEnd = e; anchorTime = s; anchorWall = localNow(); liveRewind = false
        seekBar.seekEnabled = true
        seekBar.setValue(0.0)
    }

    private fun refreshLiveBar() {
        seekBar.seekEnabled = true
        seekBar.setValue(1000.0)
        val n = localNow().format(T_FMT)
        timeText.text = "$n / $n"
    }

    private fun updateProgress() {
        try {
            val rs = rangeStart
            val re0 = rangeEnd
            if (seekBar.dragging) {
            } else if (currentLive && rs == null) {
                if (current != null) refreshLiveBar()
                else { seekBar.seekEnabled = false; seekBar.setValue(0.0); timeText.text = "--:--:-- / --:--:--" }
            } else if (rs != null && re0 != null && anchorTime != null && anchorWall != null) {
                if (liveRewind) rangeEnd = localNow()
                val right = rangeEnd!!
                val cur = curPos()!!
                val total = secs(rs, right)
                if (total > 0) {
                    seekBar.setValue((secs(rs, cur) * 1000.0 / total).coerceIn(0.0, 1000.0))
                    timeText.text = "${cur.format(T_FMT)} / ${right.format(T_FMT)}"
                }
            } else {
                seekBar.seekEnabled = false; seekBar.setValue(0.0); timeText.text = "--:--:-- / --:--:--"
            }
        } catch (_: Exception) {
        }
        ui.postDelayed({ updateProgress() }, 500)
    }

    private fun findEpgBoundsForNow(ch: Channel, now: LocalDateTime): Pair<LocalDateTime, LocalDateTime>? {
        val progs = Epg.load(ch.name, now.toLocalDate())
        progs.firstOrNull { !it.start.isAfter(now) && now.isBefore(it.end) }?.let { return it.start to it.end }
        if (now.hour < 6) {
            Epg.load(ch.name, now.toLocalDate().minusDays(1))
                .firstOrNull { !it.start.isAfter(now) && now.isBefore(it.end) }?.let { return it.start to it.end }
        }
        val past = progs.filter { !it.end.isAfter(now) }
        if (past.isNotEmpty()) { val p = past.maxByOrNull { it.end }!!; return p.start to p.end }
        return null
    }

    private fun replayTitle(ch: Channel, t: LocalDateTime, e: LocalDateTime) =
        "[回看] ${ch.name} ${t.format(DT_FMT)} - ${e.format(T_FMT)}"

    private fun arrowLeft(step: Int = SEEK) {
        if (seekBar.dragging) return
        showBar()
        if (currentLive) { liveRewindStart(step); return }
        if (rangeStart == null || rangeEnd == null) return
        seekReplayBy(-step)
    }

    private fun arrowRight(step: Int = SEEK) {
        if (seekBar.dragging) return
        showBar()
        if (currentLive) return
        if (rangeStart == null || rangeEnd == null) return
        if (liveRewind) {
            val now = localNow()
            val base = curPos() ?: return
            val nt = base.plusSeconds(step.toLong())
            if (!nt.isBefore(now)) resumeLive() else liveRewindTo(nt)
            return
        }
        seekReplayBy(step)
    }

    private fun seekReplayBy(delta: Int) {
        val rs = rangeStart ?: return
        if (rangeEnd == null) return
        if (liveRewind) rangeEnd = localNow()
        val cur = curPos() ?: rs
        val total = secs(rs, rangeEnd!!)
        if (total <= 0) return
        val curOff = (secs(rs, cur).toLong() / SEEK) * SEEK
        val maxOff = max(0L, total.toLong() - SEEK)
        val newOff = (curOff + delta).coerceIn(0L, maxOff)
        onSeekBar(newOff * 1000.0 / total, "commit")
    }

    private fun liveRewindTo(newTime: LocalDateTime) {
        val ch = current ?: return
        val rs = rangeStart ?: return
        val now = localNow()
        if (!newTime.isBefore(now)) { resumeLive(); return }
        anchorTime = newTime; anchorWall = now; rangeEnd = now
        val total = secs(rs, now)
        if (total > 0) seekBar.setValue((secs(rs, newTime) * 1000.0 / total).coerceIn(0.0, 1000.0))
        playUrl(buildReplayUrl(ch.url, newTime, now, cfg), replayTitle(ch, newTime, now), false, 400)
        setStatus("已前进到 ${newTime.format(T_FMT)} 回看")
    }

    private fun liveRewindStart(step: Int) {
        val ch = current ?: return
        val now = localNow()
        if (rangeStart == null) {
            val b = findEpgBoundsForNow(ch, now)
            if (b == null) { setStatus("没有可用的 EPG 数据，无法倒退"); return }
            rangeStart = b.first; anchorTime = now; anchorWall = now; rangeEnd = now
            seekBar.seekEnabled = true
        }
        liveRewind = true
        val rs = rangeStart!!
        val base = curPos() ?: now
        var nt = base.minusSeconds(step.toLong())
        if (nt.isBefore(rs)) nt = rs
        if (!nt.isBefore(base)) { setStatus("已到达该时段最早时间（${rs.format(T_FMT)}），无法继续倒退"); return }
        anchorTime = nt; anchorWall = now; rangeEnd = now
        val total = secs(rs, now)
        if (total > 0) seekBar.setValue((secs(rs, nt) * 1000.0 / total).coerceIn(0.0, 1000.0))
        playUrl(buildReplayUrl(ch.url, nt, now, cfg), replayTitle(ch, nt, now), false, 400)
        setStatus("已倒推到 ${nt.format(T_FMT)} 回看")
    }

    private fun resumeLive() {
        val ch = current ?: return
        clearReplayRange()
        playUrl(ch.url, "[直播] " + ch.name, true)
        refreshLiveBar()
        setStatus("已恢复直播：${ch.name}")
    }

    private fun onSeekBar(value: Double, phase: String) {
        val ch = current
        if (rangeStart == null || rangeEnd == null) {
            if (ch == null) { seekBar.setValue(if (currentLive) 1000.0 else 0.0); return }
            val now = localNow()
            val b = findEpgBoundsForNow(ch, now)
            if (b == null) { setStatus("没有可用的 EPG 数据，无法拖动进度"); seekBar.setValue(1000.0); return }
            rangeStart = b.first; rangeEnd = now; anchorTime = now; anchorWall = now; liveRewind = true
        }
        val rs = rangeStart!!
        val re = rangeEnd!!
        val total = secs(rs, re)
        if (total <= 0) return
        var off = (total * value / 1000.0).toLong() / SEEK * SEEK
        if (off >= total) off = max(0L, total.toLong() - SEEK)
        val target = rs.plusSeconds(off)
        val atRight = value >= 999 || !target.isBefore(re.minusSeconds(SEEK.toLong()))

        if (liveRewind && atRight) {
            if (phase == "preview") { val e = re.format(T_FMT); timeText.text = "$e / $e"; return }
            if (currentLive) { clearReplayRange(); refreshLiveBar() } else resumeLive()
            return
        }
        if (phase == "preview") { timeText.text = "${target.format(T_FMT)} / ${re.format(T_FMT)}"; return }
        if (ch == null || player == null) return
        anchorTime = target; anchorWall = localNow()
        playUrl(buildReplayUrl(ch.url, target, re, cfg), replayTitle(ch, target, re), false, 400)
        setStatus("已跳转到 ${target.format(T_FMT)} 继续回看")
    }

    // ================= 节目单 / 回看选择 =================

    private fun dateList(): List<Pair<LocalDate, String>> {
        val today = localNow().toLocalDate()
        val n = max(1, cfg.i("replay_days"))
        return (0 until n).map { i ->
            val d = today.minusDays(i.toLong())
            d to "${d} 星期${WEEKDAY[d.dayOfWeek.value - 1]}"
        }
    }

    private fun earliestDate(): LocalDate = localNow().toLocalDate().minusDays((max(1, cfg.i("replay_days")) - 1).toLong())

    private fun updateDateBtn() {
        val l = dateList()
        if (dateIdx !in l.indices) dateIdx = 0
        dateBtn.text = "回看日期：${l[dateIdx].second}   ▾"
    }

    private fun chooseDate() {
        val l = dateList()
        AlertDialog.Builder(this).setTitle("选择回看日期")
            .setSingleChoiceItems(l.map { it.second }.toTypedArray(), dateIdx) { d, w ->
                dateIdx = w; updateDateBtn(); refreshSlots(); d.dismiss()
                ui.postDelayed({ slotList.findViewHolderForAdapterPosition(0)?.itemView?.requestFocus() }, 100)
            }.show()
    }

    private fun buildDaySlots(ch: Channel?, day: LocalDate): Pair<List<Triple<String, LocalDateTime, LocalDateTime>>, Boolean> {
        val progs = if (ch != null) Epg.load(ch.name, day) else emptyList()
        if (progs.isNotEmpty())
            return progs.map { Triple("${fmtRange(it.start, it.end)}  ${it.title}", it.start, it.end) } to true
        val base = day.atStartOfDay()
        return (0 until 24).map { h ->
            val s = base.plusHours(h.toLong()); val e = s.plusHours(1)
            Triple(fmtRange(s, e), s, e)
        } to false
    }

    private fun refreshSlots(keep: Boolean = false) {
        val keepItem = if (keep) slotAdapter.items.getOrNull(slotSel) else null
        val l = dateList()
        if (dateIdx !in l.indices) dateIdx = 0
        updateDateBtn()
        val day = l[dateIdx].first
        val now = localNow()
        val ch = selectedChannel()
        slotsLocked = ch != null && !replaySupported(ch.url, cfg)
        val (slots, fromEpg) = buildDaySlots(ch, day)
        val items = slots.map { (label, s, e) ->
            val isCur = !s.isAfter(now) && now.isBefore(e)
            val color = if (slotsLocked || !s.isBefore(now)) RED else if (isCur) GREEN else Color.parseColor("#e0e0e0")
            SlotItem(label, s, e, color)
        }
        slotAdapter.items = items
        slotSel = -1
        if (keepItem != null && !slotsLocked) {
            slotSel = items.indexOfFirst { it.start == keepItem.start && it.end == keepItem.end }
        }
        slotAdapter.setSelectedPos(slotSel)
        epgText.text = when {
            ch == null || slotsLocked -> ""
            fromEpg -> "节目单：EPG（${items.size} 个节目）"
            else -> "无 EPG 数据，按整点时段显示"
        }
    }

    private fun onSlotFocused(pos: Int) {
        slotSel = pos
        ui.post { slotAdapter.setSelectedPos(pos) }
    }

    private fun onSlotPicked(pos: Int) {
        if (slotsLocked) { setStatus("该频道不支持回看，无法选择时段"); return }
        slotSel = pos
        playReplay()
    }

    private fun selectedSlot(): Pair<LocalDateTime, LocalDateTime>? {
        if (slotsLocked) return null
        val it = slotAdapter.items.getOrNull(slotSel) ?: return null
        return it.start to it.end
    }

    private fun replayUrlForSelection(): List<Any>? {
        val ch = selectedChannel() ?: current
        if (ch == null) { Toast.makeText(this, "请先选择频道", Toast.LENGTH_SHORT).show(); return null }
        if (!replaySupported(ch.url, cfg)) { Toast.makeText(this, "该频道不支持回看", Toast.LENGTH_SHORT).show(); return null }
        val slot = selectedSlot()
        if (slot == null) { Toast.makeText(this, "请选择正确的日期和回看时间段", Toast.LENGTH_SHORT).show(); return null }
        return listOf(ch, slot.first, slot.second, buildReplayUrl(ch.url, slot.first, slot.second, cfg))
    }

    private fun playReplay() {
        val r = replayUrlForSelection() ?: return
        playReplayAt(r[0] as Channel, r[1] as LocalDateTime, r[2] as LocalDateTime)
    }

    private fun playReplayAt(ch: Channel, start: LocalDateTime, end: LocalDateTime) {
        val now = localNow()
        if (!start.isAfter(now) && now.isBefore(end)) {
            clearReplayRange(); current = ch
            playUrl(ch.url, "[直播] " + ch.name, true)
            refreshLiveBar()
            hidePanels()
            setStatus("当前时段为直播：${ch.name}")
            return
        }
        if (!start.isBefore(now)) { alert("提示", "所选时间段尚未开始，无法回看"); return }
        if (start.toLocalDate().isBefore(earliestDate())) {
            alert("提示", "只能回看包含今天在内的 ${cfg.i("replay_days")} 天内的节目（最早 ${earliestDate()}）"); return
        }
        val url = buildReplayUrl(ch.url, start, end, cfg)
        current = ch
        currentUrl = url
        setReplayRange(start, end)
        hidePanels()
        playUrl(url, "[回看] ${ch.name} ${start.format(DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm"))} - ${end.format(DateTimeFormatter.ofPattern("HH:mm"))}", false)
    }

    private fun copyReplayUrl() {
        val r = replayUrlForSelection() ?: return
        val url = r[3] as String
        (getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager)
            .setPrimaryClip(ClipData.newPlainText("replay", url))
        setStatus("回看地址已复制：$url")
    }

    // ================= EPG 下载 =================

    private fun autoEpgUpdate() { if (cfg.b("epg_auto_update")) startEpgUpdate(true) }

    private fun startEpgUpdate(silent: Boolean) {
        val st0 = epgState
        if (st0 != null && !st0.finished) {
            if (!silent) Toast.makeText(this, "EPG 正在更新中，请稍候", Toast.LENGTH_SHORT).show()
            return
        }
        val today = localNow().toLocalDate()
        val n = max(1, cfg.i("replay_days"))
        val force = if (silent) emptySet() else setOf(today.format(DateTimeFormatter.ofPattern("yyyyMMdd")))
        val threads = cfg.i("epg_threads").coerceIn(1, 10)
        val timeout = cfg.i("epg_timeout").coerceIn(3, 10)
        val st = EpgState(Epg.csvPath(cfg, filesDir), threads, timeout,
            Epg.urlFn(cfg.s("epg_host"), cfg.s("epg_path"), cfg.s("epg_date_fmt")), silent)
        epgState = st
        Thread { Epg.updateWorker(st, today, n, force) }.also { it.isDaemon = true }.start()
        if (!silent) setStatus("正在更新 EPG…")
        ui.postDelayed({ pollEpg() }, 500)
    }

    private fun pollEpg() {
        val st = epgState ?: return
        if (!st.finished) {
            if (!st.silent && st.total > 0) setStatus("正在更新 EPG… ${st.done.get()} / ${st.total}")
            ui.postDelayed({ pollEpg() }, 500)
            return
        }
        val changed = st.ok.get() > 0 || st.removed > 0
        if (changed) refreshSlots(keep = true)
        val summary = "EPG 更新完成：新增 ${st.ok.get()}，无数据 ${st.nodata.get()}，失败 ${st.fail.get()}，清理过期 ${st.removed}"
        if (st.silent) {
            if (st.error == null && changed && currentUrl.isEmpty()) setStatus(summary)
        } else if (st.error != null) {
            setStatus("EPG 更新失败：${st.error}")
            alert("EPG 更新失败", st.error!!)
        } else {
            setStatus(summary)
            alert("EPG 下载", summary)
        }
    }

    // ================= 菜单 / 设置 =================

    private fun pad(v: View): View = FrameLayout(this).also {
        it.setPadding(dp(24), dp(8), dp(24), dp(0)); it.addView(v)
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun showMenu() {
        val items = arrayOf(
            "直播当前选择的频道", "暂停 / 继续", "停止",
            "频道列表", "回看时段（节目单）",
            "打开 m3u 文件…", "从网址加载 m3u…", "重新加载 m3u",
            "回看参数设置…", "播放选项（硬解 / RTSP-TCP）…",
            "下载 EPG", "自定义 EPG 下载参数…",
            "关于", "退出程序")
        AlertDialog.Builder(this).setTitle("菜单").setItems(items) { _, w ->
            when (w) {
                0 -> { hidePanels(); playLive() }
                1 -> togglePause()
                2 -> stopPlay()
                3 -> showLeft()
                4 -> showRight()
                5 -> openM3uFile()
                6 -> loadM3uFromUrl()
                7 -> loadDefault()
                8 -> openSettings()
                9 -> openPlayOptions()
                10 -> startEpgUpdate(false)
                11 -> openEpgSettings()
                12 -> showAbout()
                13 -> finish()
            }
        }.show()
    }

    private fun showAbout() {
        alert("关于", "$CODE_VERSION\n\nGitHub：$GITHUB_URL\n\n" +
                "遥控器：\n确定键=频道列表（长按=菜单）\n上/下=换台　左/右=倒退/快进（5 秒）\n" +
                "频道列表中 → 进入回看节目单，← 返回\n菜单键=菜单　返回键=关闭面板")
    }

    private fun openPlayOptions() {
        val checked = booleanArrayOf(cfg.b("hw_decode"), cfg.b("rtsp_tcp"))
        AlertDialog.Builder(this).setTitle("播放选项")
            .setMultiChoiceItems(arrayOf("硬件解码", "RTSP 使用 TCP（仅对 rtsp:// 生效）"), checked) { _, i, c -> checked[i] = c }
            .setPositiveButton("确定") { _, _ ->
                cfg.put("hw_decode", checked[0]); cfg.put("rtsp_tcp", checked[1]); onDecodeOptionChanged()
            }.setNegativeButton("取消", null).show()
    }

    private fun formView(labels: List<String>, values: List<String>): Pair<LinearLayout, List<EditText>> {
        val box = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(20), dp(4), dp(20), 0) }
        val edits = ArrayList<EditText>()
        labels.forEachIndexed { i, l ->
            box.addView(TextView(this).apply { text = l; setTextColor(Color.parseColor("#9e9e9e")); textSize = 14f; setPadding(0, dp(8), 0, 0) })
            val et = EditText(this).apply { setText(values[i]); setSingleLine(); setTextColor(Color.WHITE); textSize = 16f }
            box.addView(et); edits.add(et)
        }
        return box to edits
    }

    private fun openSettings() {
        val rows = listOf(
            "userid" to "userid", "AuthInfo" to "authinfo",
            "时区偏移（小时，重庆=UTC+8）" to "tz_offset", "可回看天数（含今天）" to "replay_days",
            "回看地址模板（{base} {authinfo} {userid} {seek}）" to "template",
            "HTTP 回看关键字（逗号分隔，空=全部）" to "http_replay_keys")
        val vals = rows.map { (_, k) -> if (k == "http_replay_keys") cfg.replayKeys().joinToString(",") else cfg.s(k).ifEmpty { cfg.i(k).toString().takeIf { k == "tz_offset" || k == "replay_days" } ?: "" } }
        val (box, edits) = formView(rows.map { it.first }, vals)
        val tcp = CheckBox(this).apply { text = "RTSP 使用 TCP（仅对 rtsp:// 生效）"; isChecked = cfg.b("rtsp_tcp"); setTextColor(Color.WHITE) }
        box.addView(tcp)
        val dlg = AlertDialog.Builder(this).setTitle("回看参数设置").setView(ScrollView(this).apply { addView(box) })
            .setPositiveButton("确定", null).setNegativeButton("取消", null).create()
        dlg.show()
        dlg.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
            val m = rows.indices.associate { rows[it].second to edits[it].text.toString().trim() }
            val tz = m["tz_offset"]!!.toIntOrNull()
            val days = m["replay_days"]!!.toIntOrNull()
            if (tz == null || days == null) { Toast.makeText(this, "时区偏移与回看天数必须为整数", Toast.LENGTH_LONG).show(); return@setOnClickListener }
            cfg.put("userid", m["userid"]); cfg.put("authinfo", m["authinfo"])
            cfg.put("tz_offset", tz); cfg.put("replay_days", max(1, days))
            cfg.put("template", m["template"])
            cfg.put("http_replay_keys", m["http_replay_keys"]!!.split(",").map { it.trim() }.filter { it.isNotEmpty() })
            cfg.put("rtsp_tcp", tcp.isChecked)
            cfg.save()
            updateDateBtn(); refreshSlots(); startEpgUpdate(true)
            dlg.dismiss()
        }
    }

    private fun openEpgSettings() {
        val sample = "00000001000000050000000000000476"
        val todayStr = localNow().format(DateTimeFormatter.ofPattern("yyyyMMdd"))
        val rows = listOf(
            "服务器地址" to "epg_host", "路径模板" to "epg_path",
            "日期格式（strftime，如 %Y%m%d）" to "epg_date_fmt",
            "频道表 CSV（空=内置；可填路径或 http 网址）" to "epg_csv",
            "下载线程数（1-10）" to "epg_threads", "超时秒数（3-10）" to "epg_timeout")
        val defs = Config.defaults()
        val (box, edits) = formView(rows.map { it.first }, rows.map { (_, k) -> cfg.s(k).ifEmpty { if (k == "epg_threads" || k == "epg_timeout") cfg.i(k).toString() else "" } })
        box.addView(TextView(this).apply {
            text = "可用变量：{channelcode} 频道代码，{date} 按“日期格式”生成的日期。\n最终地址 = 服务器地址 + 路径模板。"
            setTextColor(Color.parseColor("#9e9e9e")); textSize = 13f; setPadding(0, dp(10), 0, 0)
        })
        val pv = TextView(this).apply { setTextColor(Color.parseColor("#4a90d9")); textSize = 13f; setPadding(0, dp(6), 0, dp(6)) }
        box.addView(pv)
        val reset = TextView(this).apply {
            text = "恢复默认"; setPadding(dp(12), dp(10), dp(12), dp(10)); setTextColor(Color.WHITE)
            isFocusable = true; isClickable = true; setBackgroundResource(R.drawable.item_bg)
            setOnClickListener { rows.forEachIndexed { i, (_, k) -> edits[i].setText(defs[k].toString()) } }
        }
        box.addView(reset)

        fun preview() {
            try {
                val m = rows.indices.associate { rows[it].second to edits[it].text.toString().trim() }
                pv.text = "地址预览：" + Epg.urlFn(m["epg_host"]!!, m["epg_path"]!!, m["epg_date_fmt"]!!)(sample, todayStr)
            } catch (e: Exception) { pv.text = "（格式有误：${e.message}）" }
        }
        edits.forEach { it.addTextChangedListener(object : TextWatcher {
            override fun afterTextChanged(s: Editable?) { preview() }
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
        }) }
        preview()

        val dlg = AlertDialog.Builder(this).setTitle("自定义 EPG 下载参数").setView(ScrollView(this).apply { addView(box) })
            .setPositiveButton("保存并下载", null).setNeutralButton("保存", null).setNegativeButton("取消", null).create()
        dlg.show()

        fun save(download: Boolean) {
            val st = epgState
            if (st != null && !st.finished) { Toast.makeText(this, "EPG 正在更新中，请稍后再修改", Toast.LENGTH_LONG).show(); return }
            val m = rows.indices.associate { rows[it].second to edits[it].text.toString().trim() }.toMutableMap()
            m["epg_csv"] = m["epg_csv"]!!.trim('"')
            if (!m["epg_host"]!!.lowercase().startsWith("http://") && !m["epg_host"]!!.lowercase().startsWith("https://")) {
                Toast.makeText(this, "服务器地址必须以 http:// 或 https:// 开头", Toast.LENGTH_LONG).show(); return
            }
            for (v in listOf("{channelcode}", "{date}")) if (!m["epg_path"]!!.contains(v)) {
                Toast.makeText(this, "路径模板必须包含 $v", Toast.LENGTH_LONG).show(); return
            }
            val th = m["epg_threads"]!!.toIntOrNull(); val to = m["epg_timeout"]!!.toIntOrNull()
            if (th == null || to == null) { Toast.makeText(this, "线程数和超时秒数必须为整数", Toast.LENGTH_LONG).show(); return }
            if (m["epg_date_fmt"]!!.isEmpty()) m["epg_date_fmt"] = "%Y%m%d"
            val changed = listOf("epg_host", "epg_path", "epg_date_fmt", "epg_csv").any { cfg.s(it) != m[it] }
            fun commit(clear: Boolean) {
                for (k in listOf("epg_host", "epg_path", "epg_date_fmt", "epg_csv")) cfg.put(k, m[k])
                cfg.put("epg_threads", th.coerceIn(1, 10)); cfg.put("epg_timeout", to.coerceIn(3, 10))
                cfg.save()
                if (clear) Epg.clearAll()
                refreshSlots()
                dlg.dismiss()
                if (download || clear) startEpgUpdate(!download)
            }
            if (changed && Epg.epgDir.isDirectory) {
                AlertDialog.Builder(this).setTitle("下载参数已改变")
                    .setMessage("地址或频道表已改变，旧的 EPG 可能属于其他地区。\n是否清空已下载的 EPG 并重新下载？")
                    .setPositiveButton("是") { _, _ -> commit(true) }
                    .setNegativeButton("否") { _, _ -> commit(false) }.show()
            } else commit(false)
        }
        dlg.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener { save(true) }
        dlg.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener { save(false) }
    }
}
