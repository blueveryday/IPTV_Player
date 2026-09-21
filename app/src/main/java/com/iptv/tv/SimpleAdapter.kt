package com.iptv.tv

import android.graphics.Color
import android.text.TextUtils
import android.view.ViewGroup
import android.widget.TextView
import androidx.core.content.ContextCompat
import androidx.recyclerview.widget.RecyclerView

/** 频道 / 时段列表通用适配器：每项一个可聚焦 TextView */
class SimpleAdapter<T>(
    private val text: (T) -> String,
    private val color: (T) -> Int?,
    private val onClick: (Int) -> Unit,
    private val onFocus: (Int) -> Unit
) : RecyclerView.Adapter<SimpleAdapter.VH>() {

    class VH(val tv: TextView) : RecyclerView.ViewHolder(tv)

    var items: List<T> = emptyList()
        set(v) { field = v; notifyDataSetChanged() }
    var selected = -1
        private set

    fun setSelectedPos(p: Int) {
        val old = selected
        selected = p
        if (old in items.indices) notifyItemChanged(old)
        if (p in items.indices) notifyItemChanged(p)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH {
        val c = parent.context
        val tv = TextView(c).apply {
            layoutParams = RecyclerView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            setPadding(c.dp(12), c.dp(10), c.dp(12), c.dp(10))
            textSize = 18f
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
            isFocusable = true
            isClickable = true
            background = ContextCompat.getDrawable(c, R.drawable.item_bg)
        }
        return VH(tv)
    }

    override fun getItemCount() = items.size

    override fun onBindViewHolder(h: VH, position: Int) {
        val it = items[position]
        h.tv.text = text(it)
        h.tv.setTextColor(color(it) ?: Color.parseColor("#e0e0e0"))
        h.tv.isActivated = position == selected
        h.tv.setOnClickListener { onClick(h.bindingAdapterPosition) }
        h.tv.setOnFocusChangeListener { _, f -> if (f) onFocus(h.bindingAdapterPosition) }
    }
}
