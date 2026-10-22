package com.tonverk.bridge

import android.os.Handler
import android.os.Looper
import android.util.Log

/** Sammelt Diagnose-Zeilen und spiegelt sie live in die UI (kein adb/logcat nötig). */
object Diag {
    private const val TAG = "BRIDGE"
    private val sb = StringBuilder()
    private val main = Handler(Looper.getMainLooper())

    @Volatile var listener: ((String) -> Unit)? = null

    /** Live-Pegelmeter (Service -> UI), läuft getrennt vom Log. */
    @Volatile var meterListener: ((String) -> Unit)? = null

    fun meter(text: String) {
        meterListener?.let { l -> main.post { l(text) } }
    }

    @Synchronized fun log(msg: String) {
        Log.i(TAG, msg)
        sb.append(msg).append('\n')
        val snap = sb.toString()
        listener?.let { l -> main.post { l(snap) } }
    }

    @Synchronized fun snapshot(): String = sb.toString()

    @Synchronized fun clear() { sb.setLength(0) }
}
