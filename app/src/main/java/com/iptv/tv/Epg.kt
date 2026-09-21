package com.iptv.tv

import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.format.DateTimeFormatter
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.atomic.AtomicInteger

class EpgProg(val title: String, val start: LocalDateTime, val end: LocalDateTime)

class EpgState(
    val csv: String, val threads: Int, val timeout: Int,
    val urlFn: (String, String) -> String, val silent: Boolean
) {
    @Volatile var total = 0
    val done = AtomicInteger()
    val ok = AtomicInteger()
    val fail = AtomicInteger()
    val nodata = AtomicInteger()
    @Volatile var removed = 0
    @Volatile var error: String? = null
    @Volatile var finished = false
    @Volatile var cancel = false
}

object Epg {
    private lateinit var toolDir: File
    private const val RETRY = 2
    private const val MISS_TTL_MS = 6 * 3600 * 1000L
    private val BASIC = DateTimeFormatter.ofPattern("yyyyMMdd")
    private val DT = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")
    private val FILE_RE = Regex("_(\\d{8})\\.json$", RegexOption.IGNORE_CASE)
    private val cache = ConcurrentHashMap<String, Pair<Long, List<EpgProg>>>()

    fun init(files: File) { toolDir = File(files, "src") }
    val epgDir: File get() = File(toolDir, "epg")
    val defaultCsv: File get() = File(toolDir, "channel_epg_chongqing.csv")
    private val missFile: File get() = File(epgDir, ".miss.json")

    private fun norm(s: String?) = (s ?: "").replace(Regex("[\\\\/:*?\"<>|_\\s]+"), "").lowercase()

    fun folderName(name: String?): String {
        val n = (name ?: "").replace(Regex("[\\\\/:*?\"<>|]"), "_").trim().trimEnd('.')
        return if (n.isEmpty()) "_" else n
    }

    private fun findDir(name: String): File? {
        if (!epgDir.isDirectory) return null
        val p = File(epgDir, name)
        if (p.isDirectory) return p
        val key = norm(name)
        epgDir.listFiles()?.forEach { if (it.isDirectory && norm(it.name) == key) return it }
        return null
    }

    private fun parseFile(f: File): List<EpgProg> {
        val items = ArrayList<EpgProg>()
        try {
            val data = JSONObject(decodeAuto(f.readBytes()))
            val arr = data.optJSONArray("schedules") ?: return items
            for (i in 0 until arr.length()) {
                val s = arr.optJSONObject(i) ?: continue
                try {
                    val st = LocalDateTime.parse(s.getString("starttime"), DT)
                    val et = LocalDateTime.parse(s.getString("endtime"), DT)
                    if (!et.isAfter(st)) continue
                    items.add(EpgProg(s.optString("title", "").trim(), st, et))
                } catch (_: Exception) {
                }
            }
        } catch (_: Exception) {
        }
        return items
    }

    /** 返回某日的节目列表（按开始时间排序），无数据返回空 */
    fun load(channelName: String, day: LocalDate): List<EpgProg> {
        val d = findDir(channelName) ?: return emptyList()
        val suffix = "_${day.format(BASIC)}.json"
        val files = d.listFiles { f -> f.name.lowercase().endsWith(suffix) } ?: return emptyList()
        val progs = ArrayList<EpgProg>()
        for (f in files) {
            val mt = f.lastModified()
            val hit = cache[f.path]
            val items = if (hit != null && hit.first == mt) hit.second
            else parseFile(f).also { cache[f.path] = mt to it }
            progs.addAll(items)
        }
        val ds = day.atStartOfDay()
        val de = ds.plusDays(1)
        val seen = HashSet<Triple<LocalDateTime, LocalDateTime, String>>()
        val res = ArrayList<EpgProg>()
        for (p in progs.sortedBy { it.start }) {
            if (!(p.start.isBefore(de) && p.end.isAfter(ds))) continue
            if (seen.add(Triple(p.start, p.end, p.title))) res.add(p)
        }
        return res
    }

    fun clearAll() {
        epgDir.deleteRecursively()
        cache.clear()
    }

    fun validDates(today: LocalDate, n: Int): List<String> =
        (n - 1 downTo 0).map { today.minusDays(it.toLong()).format(BASIC) }

    fun urlFn(host: String, path: String, dateFmt: String): (String, String) -> String {
        val h = host.trim().trimEnd('/')
        var p = path.trim()
        if (p.isNotEmpty() && !p.startsWith("/") && !p.startsWith("?")) p = "/$p"
        val fmt = dateFmt.trim().ifEmpty { "%Y%m%d" }
        return { code, d ->
            val date = LocalDate.parse(d, BASIC)
            (h + p).replace("{channelcode}", code).replace("{date}", strftimeDate(date, fmt))
        }
    }

    fun csvPath(cfg: Config, appFiles: File): String {
        val p = cfg.s("epg_csv").trim().trim('"')
        if (p.isEmpty()) return defaultCsv.path
        if (p.startsWith("http://", true) || p.startsWith("https://", true)) return p
        val f = File(p)
        return if (f.isAbsolute) p else File(appFiles, p).path
    }

    private fun parseCsv(t: String): List<List<String>> {
        val rows = ArrayList<List<String>>()
        var row = ArrayList<String>()
        val sb = StringBuilder()
        var q = false
        var i = 0
        while (i < t.length) {
            val c = t[i]
            if (q) {
                if (c == '"') {
                    if (i + 1 < t.length && t[i + 1] == '"') { sb.append('"'); i++ } else q = false
                } else sb.append(c)
            } else when (c) {
                '"' -> q = true
                ',' -> { row.add(sb.toString()); sb.setLength(0) }
                '\r' -> {}
                '\n' -> { row.add(sb.toString()); sb.setLength(0); rows.add(row); row = ArrayList() }
                else -> sb.append(c)
            }
            i++
        }
        if (sb.isNotEmpty() || row.isNotEmpty()) { row.add(sb.toString()); rows.add(row) }
        return rows
    }

    fun loadChannels(text: String): List<Pair<String, String>> {
        val rows = parseCsv(text.removePrefix("\uFEFF"))
        if (rows.isEmpty()) throw RuntimeException("频道表为空")
        val hdr = rows[0].map { it.trim().lowercase() }
        val ci = hdr.indexOf("channelcode")
        val ti = hdr.indexOf("title")
        if (ci < 0 || ti < 0) throw RuntimeException("channel_epg_chongqing.csv 缺少 channelcode 或 title 字段")
        val seen = HashSet<String>()
        val out = ArrayList<Pair<String, String>>()
        for (r in rows.drop(1)) {
            val code = r.getOrNull(ci)?.trim() ?: ""
            val title = r.getOrNull(ti)?.trim() ?: ""
            if (code.isEmpty() || !seen.add(code)) continue
            out.add(code to title.ifEmpty { code })
        }
        return out
    }

    private fun fetch(url: String, timeoutSec: Int): Pair<String, ByteArray?> {
        for (attempt in 0 until RETRY) {
            try {
                val c = URL(url).openConnection() as HttpURLConnection
                c.connectTimeout = timeoutSec * 1000
                c.readTimeout = timeoutSec * 1000
                c.setRequestProperty("User-Agent", "Mozilla/5.0 (IPTVPlayer)")
                val code = c.responseCode
                if (code == 404) return "nodata" to null
                if (code in 200..299) return "ok" to c.inputStream.use { it.readBytes() }
            } catch (_: Exception) {
            }
            if (attempt < RETRY - 1) Thread.sleep(1000)
        }
        return "error" to null
    }

    private fun loadMiss(): MutableMap<String, Long> {
        val m = HashMap<String, Long>()
        try {
            val o = JSONObject(missFile.readText())
            for (k in o.keys()) m[k] = o.getLong(k)
        } catch (_: Exception) {
        }
        return m
    }

    private fun saveMiss(m: Map<String, Long>) {
        try {
            epgDir.mkdirs()
            val o = JSONObject()
            m.forEach { (k, v) -> o.put(k, v) }
            missFile.writeText(o.toString())
        } catch (_: Exception) {
        }
    }

    fun cleanup(valid: List<String>): Int {
        var removed = 0
        val keep = valid.toSet()
        epgDir.listFiles()?.forEach { sp ->
            if (!sp.isDirectory) return@forEach
            sp.listFiles()?.forEach { f ->
                val m = FILE_RE.find(f.name)
                val stale = (m != null && m.groupValues[1] !in keep) || f.name.lowercase().endsWith(".json.tmp")
                if (stale && f.delete()) { removed++; cache.remove(f.path) }
            }
            sp.delete() // 空目录才会删除成功
        }
        return removed
    }

    private class Job(val code: String, val d: String, val folder: File, val fp: File)

    fun updateWorker(st: EpgState, today: LocalDate, n: Int, force: Set<String>) {
        try {
            val dates = validDates(today, n)
            st.removed = cleanup(dates)

            val text: String = if (st.csv.startsWith("http://", true) || st.csv.startsWith("https://", true)) {
                val (s, body) = fetch(st.csv, st.timeout)
                if (s != "ok" || body == null) { st.error = "无法下载频道表：${st.csv}"; return }
                decodeAuto(body)
            } else {
                val f = File(st.csv)
                if (!f.isFile) { st.error = "未找到频道表：${st.csv}"; return }
                decodeAuto(f.readBytes())
            }
            val channels = loadChannels(text)

            val miss = loadMiss()
            val nowMs = System.currentTimeMillis()
            val q = ConcurrentLinkedQueue<Job>()
            for ((code, title) in channels) {
                val folder = File(epgDir, folderName(title))
                for (d in dates) {
                    val fp = File(folder, "${code}_$d.json")
                    if (d !in force) {
                        if (fp.isFile) continue
                        if (nowMs - (miss["${code}_$d"] ?: 0L) < MISS_TTL_MS) continue
                    }
                    q.add(Job(code, d, folder, fp))
                }
            }
            st.total = q.size
            val lock = Any()

            val ths = (0 until st.threads).map {
                Thread {
                    while (!st.cancel) {
                        val job = q.poll() ?: break
                        val key = "${job.code}_${job.d}"
                        var status = "error"
                        try {
                            val (s, body) = fetch(st.urlFn(job.code, job.d), st.timeout)
                            status = s
                            if (status == "ok" && body != null) {
                                try {
                                    val data = JSONObject(String(body, Charsets.UTF_8).removePrefix("\uFEFF"))
                                    val arr = data.optJSONArray("schedules")
                                    if (arr == null || arr.length() == 0) status = "nodata"
                                } catch (_: Exception) {
                                    status = "error"
                                }
                            }
                            if (status == "ok" && body != null) {
                                job.folder.mkdirs()
                                val tmp = File(job.fp.path + ".tmp")
                                tmp.writeBytes(body)
                                if (!tmp.renameTo(job.fp)) { job.fp.delete(); tmp.renameTo(job.fp) }
                            }
                        } catch (_: Exception) {
                            status = "error"
                        }
                        synchronized(lock) {
                            when (status) {
                                "ok" -> { st.ok.incrementAndGet(); miss.remove(key) }
                                "nodata" -> { st.nodata.incrementAndGet(); miss[key] = System.currentTimeMillis() }
                                else -> st.fail.incrementAndGet()
                            }
                            st.done.incrementAndGet()
                        }
                    }
                }.also { it.isDaemon = true; it.start() }
            }
            ths.forEach { it.join() }

            val keep = dates.toSet()
            saveMiss(miss.filterKeys { it.substringAfterLast('_') in keep })
        } catch (e: Exception) {
            st.error = e.message ?: e.toString()
        } finally {
            st.finished = true
        }
    }
}
