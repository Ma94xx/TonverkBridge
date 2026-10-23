package com.tonverk.bridge

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import android.os.SystemClock
import android.view.View
import java.util.Locale

/**
 * Horizontales Pegelmeter (−60…0 dBFS) mit Farbverlauf grün→gelb→rot,
 * 1,5-s-Peak-Hold und Text rechts. Ohne Signal/Gerät zeigt es einen Status-Text.
 */
class LevelMeterView(context: Context) : View(context) {

    private enum class State { IDLE, LIVE }

    private val density = resources.displayMetrics.density
    private fun dp(v: Float) = v * density

    private val bgPaint = Paint().apply { color = Color.parseColor("#26262C"); isAntiAlias = true }
    private val fillPaint = Paint().apply { isAntiAlias = true }
    private val peakPaint = Paint().apply { color = Color.parseColor("#F2F2F2") }
    private val textPaint = Paint().apply {
        color = Color.parseColor("#B8B8C0")
        textSize = dp(12f)
        isAntiAlias = true
        typeface = Typeface.MONOSPACE
        textAlign = Paint.Align.RIGHT
    }

    private var state = State.IDLE
    private var idleText = "no signal"
    private var db = -100.0
    private var peakDb = -100.0
    private var peakAt = 0L
    private var shader: Shader? = null

    fun setLevelDb(v: Double) {
        state = State.LIVE
        db = v
        val now = SystemClock.elapsedRealtime()
        if (v >= peakDb || now - peakAt > 1500) { peakDb = v; peakAt = now }
        invalidate()
    }

    fun setIdle(text: String) {
        state = State.IDLE
        idleText = text
        peakDb = -100.0
        invalidate()
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        setMeasuredDimension(w, dp(30f).toInt())
    }

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
        // Farbverlauf über die volle Breite: grün bis −12 dB, gelb bis −3, dann rot
        shader = LinearGradient(
            0f, 0f, w.toFloat(), 0f,
            intArrayOf(
                Color.parseColor("#45C465"), Color.parseColor("#45C465"),
                Color.parseColor("#E8C545"), Color.parseColor("#E05548")
            ),
            floatArrayOf(0f, 0.65f, 0.90f, 1f),
            Shader.TileMode.CLAMP
        )
        fillPaint.shader = shader
    }

    private fun frac(d: Double) = (((d + 60.0) / 60.0).coerceIn(0.0, 1.0)).toFloat()

    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat(); val h = height.toFloat()
        val r = dp(6f)
        canvas.drawRoundRect(RectF(0f, 0f, w, h), r, r, bgPaint)

        if (state == State.LIVE) {
            val fw = w * frac(db)
            if (fw > 1f) {
                canvas.save()
                canvas.clipRect(0f, 0f, fw, h)
                canvas.drawRoundRect(RectF(0f, 0f, w, h), r, r, fillPaint)
                canvas.restore()
            }
            val px = w * frac(peakDb)
            if (px > 2f) canvas.drawRect(px - dp(1f), dp(2f), px + dp(1f), h - dp(2f), peakPaint)
            val label = if (db <= -90.0) "-inf dB"
                        else String.format(Locale.US, "%5.1f dB", db)
            canvas.drawText(label, w - dp(8f), h / 2 + textPaint.textSize / 2 - dp(1.5f), textPaint)
        } else {
            canvas.drawText(idleText, w - dp(8f), h / 2 + textPaint.textSize / 2 - dp(1.5f), textPaint)
        }
    }
}
