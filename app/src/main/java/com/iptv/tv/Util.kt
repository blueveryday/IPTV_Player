package com.iptv.tv

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.nio.ByteBuffer
import java.nio.charset.Charset
import java.nio.charset.CodingErrorAction
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.format.DateTimeFormatter

data class Channel(
    val name: String,
    val url: String,
    val local: Boolean = false,
    val webdav: Boolean = false,
    val isDir: Boolean = false,
    val isBack: Boolean = false,
    val exitBrowse: Boolean = false,
    val rawName: String = "",
    val path: String = "",
    val webdavUser: String = "",
    val webdavPass: String = ""
)

// 本地/WebDAV 媒体文件后缀，与 Python 版 MEDIA_EXTS / AUDIO_EXTS 一致
val MEDIA_EXTS = setOf(
    "mp4", "mkv", "avi", "mov", "wmv", "flv", "webm", "ts", "m4v",
    "mpg", "mpeg", "m2ts", "mts", "3gp", "rmvb", "rm", "vob", "ogv",
    "mp3", "aac", "flac", "wav", "ape", "ogg", "wma", "m4a", "opus",
    "ac3", "dts", "aiff", "aif", "alac", "mka", "mp2", "mpc", "wv"
)
val AUDIO_EXTS = setOf(
    "mp3", "aac", "flac", "wav", "ape", "ogg", "wma", "m4a", "opus",
    "ac3", "dts", "aiff", "aif", "alac", "mka", "mp2", "mpc", "wv"
)
fun isMediaName(name: String): Boolean = MEDIA_EXTS.contains(name.substringAfterLast('.', "").lowercase())
fun isAudioName(name: String): Boolean = AUDIO_EXTS.contains(name.substringAfterLast('.', "").lowercase())

fun Context.dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

/** 与原版 iptv_config.json 相同的配置项 */
class Config(private val file: File) {
    private val j = JSONObject()

    companion object {
        fun defaults(): LinkedHashMap<String, Any> = linkedMapOf(
            "userid" to "gf001",
            "authinfo" to "xxx",
            "cfg_ver" to 2,
            "hw_decode" to true,
            "rtsp_tcp" to true,
            "http_replay_keys" to listOf("/rtsp/"),
            "template" to "{base}?AuthInfo={authinfo}&userid={userid}&playseek={seek}",
            "tz_offset" to 8,
            "replay_days" to 7,
            "epg_auto_update" to true,
            "epg_host" to "http://123.147.117.163:8081",
            "epg_path" to "/resource/schedules_v2/{channelcode}_{date}.json",
            "epg_date_fmt" to "%Y%m%d",
            "epg_csv" to "",
            "epg_threads" to 1,
            "epg_timeout" to 10,
            "m3u_path" to "",
            "m3u_url" to "",
            "webdav_sources" to emptyList<Any>()
        )
    }

    init {
        defaults().forEach { (k, v) -> put(k, v) }
        try {
            val o = JSONObject(file.readText())
            for (k in o.keys()) j.put(k, o.get(k))
        } catch (_: Exception) {
        }
    }

    fun put(k: String, v: Any?) {
        j.put(k, if (v is List<*>) JSONArray(v) else v)
    }

    fun s(k: String): String = j.optString(k, "")
    fun i(k: String): Int = j.optInt(k, 0)
    fun b(k: String, def: Boolean = true): Boolean = j.optBoolean(k, def)

    fun webdavSources(): List<JSONObject> {
        val arr = j.optJSONArray("webdav_sources") ?: JSONArray()
        return (0 until arr.length()).mapNotNull { arr.optJSONObject(it) }
    }

    fun setWebdavSources(list: List<JSONObject>) {
        j.put("webdav_sources", JSONArray(list))
    }

    fun replayKeys(): List<String> {
        val v = j.opt("http_replay_keys")
        return when (v) {
            is JSONArray -> (0 until v.length()).map { v.optString(it) }.filter { it.isNotBlank() }
            is String -> v.split(",").map { it.trim() }.filter { it.isNotEmpty() }
            else -> listOf("/rtsp/")
        }
    }

    fun save() {
        try {
            file.writeText(j.toString(2))
        } catch (_: Exception) {
        }
    }
}

// ---------- 文本解码 / m3u ----------

fun decodeAuto(raw: ByteArray): String {
    for (cs in listOf("UTF-8", "GBK", "UTF-16")) {
        try {
            val dec = Charset.forName(cs).newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
            val s = dec.decode(ByteBuffer.wrap(raw)).toString()
            return s.removePrefix("\uFEFF")
        } catch (_: Exception) {
        }
    }
    return String(raw, Charsets.UTF_8).removePrefix("\uFEFF")
}

private val MD_LINK = Regex("^\\[(.*?)\\]\\((.*?)\\)$")

fun cleanUrl(line: String): String {
    val l = line.trim()
    MD_LINK.find(l)?.let { return it.groupValues[2].trim() }
    return l.trim('<', '>')
}

private val EXTINF = Regex("^#EXTINF:[^,\"]*(?:\"[^\"]*\"[^,\"]*)*,(.*)$", RegexOption.IGNORE_CASE)
private val TVG_NAME = Regex("tvg-name=\"([^\"]*)\"")

fun parseM3u(text: String): List<Channel> {
    val out = ArrayList<Channel>()
    var name: String? = null
    for (raw in text.lines()) {
        val line = raw.trim()
        if (line.isEmpty()) continue
        if (line.uppercase().startsWith("#EXTINF")) {
            val m = EXTINF.find(line)
            name = m?.groupValues?.get(1)?.trim() ?: "未知频道"
            if (name.isNullOrEmpty()) {
                name = TVG_NAME.find(line)?.groupValues?.get(1) ?: "未知频道"
            }
        } else if (line.startsWith("#")) {
            continue
        } else {
            val url = cleanUrl(line)
            if (url.isNotEmpty()) out.add(Channel(if (name.isNullOrEmpty()) url else name, url))
            name = null
        }
    }
    return out
}

// ---------- 回看地址 ----------

fun replaySupported(url: String, cfg: Config): Boolean {
    val u = url.lowercase()
    if (u.startsWith("rtsp://")) return true
    if (!(u.startsWith("http://") || u.startsWith("https://"))) return false
    val keys = cfg.replayKeys().map { it.lowercase() }
    if (keys.isEmpty()) return true
    return keys.any { u.contains(it) }
}

private val SEEK_FMT = DateTimeFormatter.ofPattern("yyyyMMddHHmmss")
private val CLEAN_EMPTY = Regex("(?<=[?&])(AuthInfo|userid)=(&|$)")

fun buildReplayUrl(base: String, start: LocalDateTime, end: LocalDateTime, cfg: Config): String {
    val off = cfg.i("tz_offset").toLong()
    val seek = "${start.minusHours(off).format(SEEK_FMT)}-${end.minusHours(off).format(SEEK_FMT)}"
    val auth = cfg.s("authinfo").trim()
    val userid = cfg.s("userid").trim()
    val tpl = cfg.s("template").trim()
    if (tpl.isNotEmpty()) {
        var url = tpl.replace("{base}", base).replace("{authinfo}", auth)
            .replace("{userid}", userid).replace("{seek}", seek)
        url = CLEAN_EMPTY.replace(url, "")
        return url.trimEnd('&', '?')
    }
    val params = ArrayList<String>()
    if (auth.isNotEmpty()) params.add("AuthInfo=$auth")
    if (userid.isNotEmpty()) params.add("userid=$userid")
    params.add("playseek=$seek")
    val sep = if (base.contains("?")) "&" else "?"
    return base + sep + params.joinToString("&")
}

fun fmtRange(start: LocalDateTime, end: LocalDateTime): String {
    val t = DateTimeFormatter.ofPattern("HH:mm")
    val e = if (end.hour == 0 && end.minute == 0 && end.toLocalDate().isAfter(start.toLocalDate())) "24:00"
    else end.format(t)
    return "${start.format(t)}-$e"
}

fun strftimeDate(d: LocalDate, fmt: String): String =
    fmt.replace("%Y", "%04d".format(d.year))
        .replace("%m", "%02d".format(d.monthValue))
        .replace("%d", "%02d".format(d.dayOfMonth))
        .replace("%y", "%02d".format(d.year % 100))
