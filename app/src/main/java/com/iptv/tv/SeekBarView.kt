package com.iptv.tv

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View

/** 进度条：0..1000。TV 上仅显示，用遥控器左右键控制；触屏设备可拖动。 */
class SeekBarView @JvmOverloads constructor(c: Context, a: AttributeSet? = null) : View(c, a) {
    var onSeek: ((Double, String) -> Unit)? = null
    var value = 0.0
        private set
    var dragging = false
        private set
    var seekEnabled = false
        set(v) { field = v; invalidate() }

    private val p = Paint(Paint.ANTI_ALIAS_FLAG)

    fun setValue(v: Double) {
        value = v.coerceIn(0.0, 1000.0)
        invalidate()
    }

    override fun onDraw(cv: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 1 || h <= 1) return
        val cy = h / 2
        val tr = h / 8
        p.style = Paint.Style.FILL
        p.color = Color.parseColor("#3f3f46")
        cv.drawRect(0f, cy - tr, w, cy + tr, p)
        val fw = (w * value / 1000.0).toFloat()
        if (fw > 0) {
            p.color = if (seekEnabled) Color.parseColor("#4a90d9") else Color.parseColor("#6b6b6b")
            cv.drawRect(0f, cy - tr, fw, cy + tr, p)
        }
        val r = h / 2 - 2
        val tx = fw.coerceIn(r, w - r)
        p.color = Color.parseColor("#252526")
        cv.drawCircle(tx, cy, r, p)
        p.style = Paint.Style.STROKE
        p.strokeWidth = 3f
        p.color = Color.parseColor("#4a90d9")
        cv.drawCircle(tx, cy, r, p)
    }

    private fun xToValue(x: Float): Double =
        if (width <= 1) 0.0 else (x * 1000.0 / width).coerceIn(0.0, 1000.0)

    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (!seekEnabled) return false
        when (e.action) {
            MotionEvent.ACTION_DOWN -> {
                dragging = true
                setValue(xToValue(e.x)); onSeek?.invoke(value, "preview")
            }
            MotionEvent.ACTION_MOVE -> if (dragging) {
                setValue(xToValue(e.x)); onSeek?.invoke(value, "preview")
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> if (dragging) {
                dragging = false
                setValue(xToValue(e.x)); onSeek?.invoke(value, "commit")
            }
        }
        return true
    }
}
