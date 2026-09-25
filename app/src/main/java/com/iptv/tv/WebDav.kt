package com.iptv.tv

import android.util.Base64
import android.util.Xml
import org.xmlpull.v1.XmlPullParser
import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.net.URI
import java.net.URLDecoder
import java.net.URLEncoder
import javax.net.ssl.SSLSocketFactory

data class WebDavEntry(val name: String, val isDir: Boolean, val url: String, val size: Long)

class WebDavException(msg: String) : Exception(msg)

/**
 * 极简 WebDAV 客户端：只实现浏览目录所需的 PROPFIND（Depth:1）。
 * Android 的 HttpURLConnection 不支持 PROPFIND 这种非标准方法，这里用原始 Socket 手写一次
 * 最简单的 HTTP/1.1 请求；播放文件仍走普通 http(s):// 地址交给 VLC，不经过这里。
 */
class WebDavClient(
    rawUrl: String,
    private val user: String = "",
    private val password: String = "",
    private val timeoutMs: Int = 15000
) {
    private val uri: URI
    val baseUrl: String

    init {
        var u = rawUrl.trim()
        if (!u.startsWith("http://", true) && !u.startsWith("https://", true)) u = "http://$u"
        baseUrl = u.trimEnd('/')
        uri = URI(baseUrl)
    }

    private fun encodeSeg(seg: String): String = URLEncoder.encode(seg, "UTF-8").replace("+", "%20")

    /** rel 是用 "/" 分隔、不以 "/" 开头或结尾的相对路径 */
    fun list(rel: String): List<WebDavEntry> {
        val relClean = rel.trim('/')
        val encRel = if (relClean.isEmpty()) "" else relClean.split("/").joinToString("/") { encodeSeg(it) }
        val basePath = (uri.rawPath ?: "").let { if (it.isEmpty()) "/" else it.trimEnd('/') + "/" }
        val reqPath = (basePath + encRel).ifEmpty { "/" }

        val body = ("<?xml version=\"1.0\" encoding=\"utf-8\"?>" +
                "<d:propfind xmlns:d=\"DAV:\"><d:prop>" +
                "<d:resourcetype/><d:getcontentlength/></d:prop></d:propfind>").toByteArray(Charsets.UTF_8)

        val resp = rawRequest("PROPFIND", reqPath, mapOf("Depth" to "1", "Content-Type" to "application/xml; charset=utf-8"), body)
        if (resp.code !in 200..299) throw WebDavException("HTTP ${resp.code}")
        return parseMultiStatus(resp.body, reqPath)
    }

    private class RawResponse(val code: Int, val body: ByteArray)

    private fun rawRequest(method: String, path: String, extraHeaders: Map<String, String>, body: ByteArray?): RawResponse {
        val host = uri.host ?: throw WebDavException("地址缺少主机名")
        val https = uri.scheme.equals("https", true)
        val port = if (uri.port > 0) uri.port else if (https) 443 else 80
        val socket: Socket = if (https) SSLSocketFactory.getDefault().createSocket() else Socket()
        socket.connect(InetSocketAddress(host, port), timeoutMs)
        socket.soTimeout = timeoutMs
        try {
            val out = socket.getOutputStream()
            val sb = StringBuilder()
            sb.append("$method $path HTTP/1.1\r\n")
            sb.append("Host: $host\r\n")
            sb.append("User-Agent: IPTVPlayer-Android\r\n")
            sb.append("Connection: close\r\n")
            if (user.isNotEmpty() || password.isNotEmpty()) {
                val tok = Base64.encodeToString("$user:$password".toByteArray(Charsets.UTF_8), Base64.NO_WRAP)
                sb.append("Authorization: Basic $tok\r\n")
            }
            for ((k, v) in extraHeaders) sb.append("$k: $v\r\n")
            if (body != null) sb.append("Content-Length: ${body.size}\r\n")
            sb.append("\r\n")
            out.write(sb.toString().toByteArray(Charsets.UTF_8))
            if (body != null) out.write(body)
            out.flush()

            val ins = BufferedInputStream(socket.getInputStream())
            val statusLine = readLine(ins) ?: throw WebDavException("服务器无响应")
            val code = Regex("HTTP/\\d\\.\\d\\s+(\\d+)").find(statusLine)?.groupValues?.get(1)?.toIntOrNull()
                ?: throw WebDavException("非法响应：$statusLine")
            val headers = LinkedHashMap<String, String>()
            while (true) {
                val line = readLine(ins) ?: break
                if (line.isEmpty()) break
                val idx = line.indexOf(':')
                if (idx > 0) headers[line.substring(0, idx).trim().lowercase()] = line.substring(idx + 1).trim()
            }
            val respBody = if (headers["transfer-encoding"]?.contains("chunked", true) == true)
                readChunked(ins) else readByLength(ins, headers["content-length"]?.toIntOrNull() ?: -1)
            return RawResponse(code, respBody)
        } finally {
            try { socket.close() } catch (_: Exception) {}
        }
    }

    private fun readLine(ins: BufferedInputStream): String? {
        val buf = ByteArrayOutputStream()
        var prev = -1
        while (true) {
            val b = ins.read()
            if (b == -1) return if (buf.size() == 0) null else buf.toString("UTF-8")
            if (prev == '\r'.code && b == '\n'.code) {
                val bytes = buf.toByteArray()
                return String(bytes, 0, bytes.size - 1, Charsets.UTF_8)
            }
            buf.write(b)
            prev = b
        }
    }

    private fun readByLength(ins: BufferedInputStream, len: Int): ByteArray {
        if (len < 0) return ins.readBytes()
        val out = ByteArray(len)
        var off = 0
        while (off < len) {
            val n = ins.read(out, off, len - off)
            if (n < 0) break
            off += n
        }
        return if (off == len) out else out.copyOf(off)
    }

    private fun readChunked(ins: BufferedInputStream): ByteArray {
        val out = ByteArrayOutputStream()
        while (true) {
            val sizeLine = readLine(ins) ?: break
            val size = sizeLine.trim().substringBefore(';').toIntOrNull(16) ?: break
            if (size == 0) { readLine(ins); break }
            out.write(readByLength(ins, size))
            readLine(ins) // 结尾的 CRLF
        }
        return out.toByteArray()
    }

    private fun parseMultiStatus(xml: ByteArray, reqPath: String): List<WebDavEntry> {
        val parser = Xml.newPullParser()
        parser.setFeature(XmlPullParser.FEATURE_PROCESS_NAMESPACES, false)
        parser.setInput(xml.inputStream(), "UTF-8")
        val items = ArrayList<WebDavEntry>()
        var href = ""; var isDir = false; var size = 0L; var inResponse = false
        val basePathDecoded = URLDecoder.decode((uri.rawPath ?: "/").ifEmpty { "/" }, "UTF-8").trimEnd('/')
        val reqPathDecoded = URLDecoder.decode(reqPath, "UTF-8").trimEnd('/')
        var event = parser.eventType
        while (event != XmlPullParser.END_DOCUMENT) {
            when (event) {
                XmlPullParser.START_TAG -> when (parser.name.substringAfter(':')) {
                    "response" -> { inResponse = true; href = ""; isDir = false; size = 0 }
                    "href" -> href = parser.nextText()
                    "collection" -> isDir = true
                    "getcontentlength" -> size = parser.nextText().toLongOrNull() ?: 0
                }
                XmlPullParser.END_TAG -> if (parser.name.substringAfter(':') == "response" && inResponse) {
                    inResponse = false
                    val hrefPath = try { URI(href).rawPath ?: href } catch (_: Exception) { href }
                    val hrefDecoded = URLDecoder.decode(hrefPath, "UTF-8").trimEnd('/')
                    if (hrefDecoded.isNotEmpty() && hrefDecoded != reqPathDecoded && hrefDecoded != basePathDecoded) {
                        val parent = hrefDecoded.substringBeforeLast('/', "")
                        if (parent == reqPathDecoded) {
                            val nm = hrefDecoded.substringAfterLast('/')
                            if (nm.isNotEmpty()) items.add(WebDavEntry(nm, isDir, resolveHrefUrl(href), size))
                        }
                    }
                }
            }
            event = parser.next()
        }
        return items
    }

    private fun resolveHrefUrl(href: String): String = try {
        if (href.startsWith("http://", true) || href.startsWith("https://", true)) href else uri.resolve(href).toString()
    } catch (_: Exception) { href }
}

/** 把用户名密码编码进 URL 的 userinfo 部分，交给 VLC 走标准 HTTP Basic Auth 播放 */
fun webDavAuthUrl(url: String, user: String, pass: String): String {
    if (user.isEmpty() && pass.isEmpty()) return url
    return try {
        val u = URI(url)
        val userinfo = URLEncoder.encode(user, "UTF-8") + ":" + URLEncoder.encode(pass, "UTF-8")
        URI(u.scheme, userinfo, u.host, u.port, u.path, u.query, u.fragment).toString()
    } catch (_: Exception) { url }
}
