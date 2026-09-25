package com.iptv.tv

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.drawable.Drawable
import android.net.Uri
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.support.v4.media.MediaMetadataCompat
import android.support.v4.media.session.MediaSessionCompat
import android.support.v4.media.session.PlaybackStateCompat
import androidx.appcompat.content.res.AppCompatResources
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.graphics.drawable.toBitmap
import androidx.media.app.NotificationCompat.MediaStyle
import org.videolan.libvlc.LibVLC
import org.videolan.libvlc.Media
import org.videolan.libvlc.MediaPlayer
import java.util.ArrayList

class PlaybackService : Service() {

    companion object {
        private const val CHANNEL_ID = "iptv_playback_channel"
        private const val NOTIFICATION_ID = 1001
        private const val WAKELOCK_TIMEOUT_MS = 10 * 60 * 1000L
    }

    // ---------- Binder ----------
    inner class LocalBinder : Binder() {
        fun getService(): PlaybackService = this@PlaybackService
    }

    private val binder = LocalBinder()

    // ---------- 播放核心 ----------
    private lateinit var libVLC: LibVLC
    private lateinit var mediaPlayer: MediaPlayer

    // ---------- 媒体会话 ----------
    private lateinit var mediaSession: MediaSessionCompat

    // ---------- 电源管理 ----------
    private lateinit var wakeLock: PowerManager.WakeLock

    // ---------- 状态 ----------
    private var currentUrl: String? = null
    private var currentTitle: String = "IPTV Player"
    private var isPaused: Boolean = false

    // ============================================================
    // 生命周期
    // ============================================================

    override fun onCreate() {
        super.onCreate()

        // 1. 初始化 libVLC
        val options = ArrayList<String>().apply {
            add("--no-video-title-show")
            add("--network-caching=1500")
        }
        libVLC = LibVLC(this, options)
        mediaPlayer = MediaPlayer(libVLC)

        // 2. 初始化 MediaSession
        initMediaSession()

        // 3. 初始化通知渠道
        createNotificationChannel()

        // 4. 初始化 WakeLock
        val powerManager = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = powerManager.newWakeLock(
            PowerManager.PARTIAL_WAKE_LOCK,
            "IPTVPlayer::PlaybackWakeLock"
        ).apply {
            setReferenceCounted(false)
        }

        // 5. 监听播放器事件
        mediaPlayer.setEventListener { event ->
            when (event.type) {
                MediaPlayer.Event.Playing -> {
                    isPaused = false
                    acquireWakeLock()
                    updatePlaybackState(PlaybackStateCompat.STATE_PLAYING)
                    startForegroundWithNotification()
                }
                MediaPlayer.Event.Paused -> {
                    isPaused = true
                    releaseWakeLock()
                    updatePlaybackState(PlaybackStateCompat.STATE_PAUSED)
                    updateNotification()
                }
                MediaPlayer.Event.Stopped -> {
                    isPaused = false
                    releaseWakeLock()
                    updatePlaybackState(PlaybackStateCompat.STATE_STOPPED)
                    stopForegroundCompat()
                }
                MediaPlayer.Event.EndReached -> {
                    isPaused = false
                    releaseWakeLock()
                    updatePlaybackState(PlaybackStateCompat.STATE_STOPPED)
                    stopForegroundCompat()
                }
                MediaPlayer.Event.EncounteredError -> {
                    isPaused = false
                    releaseWakeLock()
                    updatePlaybackState(PlaybackStateCompat.STATE_ERROR)
                    stopForegroundCompat()
                }
            }
        }
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            "ACTION_PLAY" -> resume()
            "ACTION_PAUSE" -> pause()
            "ACTION_STOP" -> stop()
            else -> {
                intent?.getStringExtra("url")?.let { url ->
                    if (url.isNotEmpty()) {
                        play(url, intent.getStringExtra("title") ?: "IPTV Player")
                    }
                }
            }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        releaseWakeLock()
        try {
            mediaPlayer.stop()
            mediaPlayer.release()
            libVLC.release()
        } catch (_: Exception) { }
        mediaSession.isActive = false
        mediaSession.release()
        super.onDestroy()
    }

    // ============================================================
    // 对外播放控制
    // ============================================================

    fun play(url: String, title: String = "IPTV Player") {
        currentUrl = url
        currentTitle = title

        try {
            mediaPlayer.stop()
            val media = Media(libVLC, Uri.parse(url))
            media.setHWDecoderEnabled(true, false)
            mediaPlayer.media = media
            media.release()
            mediaPlayer.play()

            updateMetadata(title)
            startForegroundWithNotification()
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    fun pause() {
        if (mediaPlayer.isPlaying) {
            mediaPlayer.pause()
            isPaused = true
            releaseWakeLock()
            updatePlaybackState(PlaybackStateCompat.STATE_PAUSED)
            updateNotification()
        }
    }

    fun resume() {
        mediaPlayer.play()
        isPaused = false
        acquireWakeLock()
        updatePlaybackState(PlaybackStateCompat.STATE_PLAYING)
        updateNotification()
    }

    fun stop() {
        mediaPlayer.stop()
        isPaused = false
        releaseWakeLock()
        updatePlaybackState(PlaybackStateCompat.STATE_STOPPED)
        stopForegroundCompat()
    }

    fun isPlaying(): Boolean = mediaPlayer.isPlaying

    // ============================================================
    // WakeLock
    // ============================================================

    private fun acquireWakeLock() {
        if (::wakeLock.isInitialized && !wakeLock.isHeld) {
            wakeLock.acquire(WAKELOCK_TIMEOUT_MS)
        }
    }

    private fun releaseWakeLock() {
        if (::wakeLock.isInitialized && wakeLock.isHeld) {
            try { wakeLock.release() } catch (_: Exception) { }
        }
    }

    // ============================================================
    // Logo 加载（兼容自适应图标）
    // ============================================================

    private fun loadAppLogo(sizePx: Int = 256): Bitmap? {
        return try {
            val drawable: Drawable? =
                AppCompatResources.getDrawable(this, R.mipmap.ic_launcher)
            drawable?.toBitmap(sizePx, sizePx, Bitmap.Config.ARGB_8888)
        } catch (e: Exception) {
            try {
                BitmapFactory.decodeResource(resources, R.mipmap.ic_launcher)
            } catch (_: Exception) {
                null
            }
        }
    }

    // ============================================================
    // MediaSession
    // ============================================================

    private fun initMediaSession() {
        mediaSession = MediaSessionCompat(this, "IPTVPlayerSession").apply {
            setCallback(object : MediaSessionCompat.Callback() {
                override fun onPlay() = resume()
                override fun onPause() = pause()
                override fun onStop() = stop()
                override fun onSkipToNext() { }
                override fun onSkipToPrevious() { }
            })
            isActive = true
        }
    }

    private fun updateMetadata(title: String) {
        val logoBitmap = loadAppLogo()

        val builder = MediaMetadataCompat.Builder()
            .putString(MediaMetadataCompat.METADATA_KEY_TITLE, title)
            .putString(MediaMetadataCompat.METADATA_KEY_ARTIST, "IPTV Player")
            .putString(MediaMetadataCompat.METADATA_KEY_ALBUM, "IPTV")

        if (logoBitmap != null) {
            builder
                .putBitmap(MediaMetadataCompat.METADATA_KEY_ALBUM_ART, logoBitmap)
                .putBitmap(MediaMetadataCompat.METADATA_KEY_ART, logoBitmap)
                .putBitmap(MediaMetadataCompat.METADATA_KEY_DISPLAY_ICON, logoBitmap)
        }

        mediaSession.setMetadata(builder.build())
    }

    private fun updatePlaybackState(state: Int) {
        val playbackState = PlaybackStateCompat.Builder()
            .setActions(
                PlaybackStateCompat.ACTION_PLAY or
                        PlaybackStateCompat.ACTION_PAUSE or
                        PlaybackStateCompat.ACTION_PLAY_PAUSE or
                        PlaybackStateCompat.ACTION_STOP or
                        PlaybackStateCompat.ACTION_SKIP_TO_NEXT or
                        PlaybackStateCompat.ACTION_SKIP_TO_PREVIOUS
            )
            .setState(state, PlaybackStateCompat.PLAYBACK_POSITION_UNKNOWN, 1.0f)
            .build()

        mediaSession.setPlaybackState(playbackState)
    }

    // ============================================================
    // 通知
    // ============================================================

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "播放控制",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "后台播放控制"
                setShowBadge(false)
            }
            val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            nm.createNotificationChannel(channel)
        }
    }

    private fun buildNotification(): Notification {
        val contentIntent = PendingIntent.getActivity(
            this,
            0,
            packageManager.getLaunchIntentForPackage(packageName),
            PendingIntent.FLAG_UPDATE_CURRENT or
                    (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
                        PendingIntent.FLAG_IMMUTABLE else 0)
        )

        val logoBitmap = loadAppLogo()

        val builder = NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(currentTitle)
            .setContentText(if (isPaused) "已暂停" else "正在播放")
            .setSubText("IPTV Player")
            .setSmallIcon(R.drawable.ic_notification)   // 单色白色小图标
            .setContentIntent(contentIntent)
            .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
            .setOngoing(mediaPlayer.isPlaying)
            .setShowWhen(false)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setStyle(
                MediaStyle()
                    .setMediaSession(mediaSession.sessionToken)
                    .setShowActionsInCompactView(0, 1, 2)
            )

        if (logoBitmap != null) {
            builder.setLargeIcon(logoBitmap)
        }

        // 上一首
        builder.addAction(
            NotificationCompat.Action(
                android.R.drawable.ic_media_previous,
                "上一首",
                mediaAction(PlaybackStateCompat.ACTION_SKIP_TO_PREVIOUS)
            )
        )

        // 播放/暂停
        if (mediaPlayer.isPlaying) {
            builder.addAction(
                NotificationCompat.Action(
                    android.R.drawable.ic_media_pause,
                    "暂停",
                    mediaAction(PlaybackStateCompat.ACTION_PAUSE)
                )
            )
        } else {
            builder.addAction(
                NotificationCompat.Action(
                    android.R.drawable.ic_media_play,
                    "播放",
                    mediaAction(PlaybackStateCompat.ACTION_PLAY)
                )
            )
        }

        // 停止
        builder.addAction(
            NotificationCompat.Action(
                android.R.drawable.ic_menu_close_clear_cancel,
                "停止",
                mediaAction(PlaybackStateCompat.ACTION_STOP)
            )
        )

        return builder.build()
    }

    private fun mediaAction(action: Long): PendingIntent {
        val intent = Intent(this, PlaybackService::class.java).apply {
            this.action = when (action) {
                PlaybackStateCompat.ACTION_PLAY -> "ACTION_PLAY"
                PlaybackStateCompat.ACTION_PAUSE -> "ACTION_PAUSE"
                PlaybackStateCompat.ACTION_STOP -> "ACTION_STOP"
                PlaybackStateCompat.ACTION_SKIP_TO_PREVIOUS -> "ACTION_PREV"
                PlaybackStateCompat.ACTION_SKIP_TO_NEXT -> "ACTION_NEXT"
                else -> "ACTION_UNKNOWN"
            }
        }
        return PendingIntent.getService(
            this,
            action.toInt(),
            intent,
            PendingIntent.FLAG_UPDATE_CURRENT or
                    (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
                        PendingIntent.FLAG_IMMUTABLE else 0)
        )
    }

    private fun startForegroundWithNotification() {
        val notification = buildNotification()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    private fun updateNotification() {
        val nm = NotificationManagerCompat.from(this)
        try {
            nm.notify(NOTIFICATION_ID, buildNotification())
        } catch (_: SecurityException) { }
    }

    private fun stopForegroundCompat() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE)
        } else {
            @Suppress("DEPRECATION")
            stopForeground(true)
        }
        NotificationManagerCompat.from(this).cancel(NOTIFICATION_ID)
    }
}