package com.tonverk.bridge

import kotlin.math.roundToLong

/** Reine PCM-Byte-Verarbeitung (JVM-testbar, kein Android). */
object Pcm {

    /**
     * Skaliert gepackte LE-PCM-Samples in-place (subslot = Bytes pro Sample, 2/3/4).
     * factor 1.0 lässt die Bytes unangetastet (bit-exakter Pfad); geklemmt wird
     * symmetrisch auf den Wertebereich des Subslots. Ein angebrochenes Sample am
     * Ende (n nicht aligned) bleibt unverändert.
     */
    fun applyGainInPlace(buf: ByteArray, n: Int, subslot: Int, factor: Float) {
        if (factor == 1.0f || subslot < 2 || subslot > 4) return
        val bits = subslot * 8
        val max = (1L shl (bits - 1)) - 1
        val min = -(1L shl (bits - 1))
        var s = 0
        while (s + subslot <= n) {
            var v = 0L
            for (b in 0 until subslot) v = v or ((buf[s + b].toLong() and 0xff) shl (8 * b))
            // Vorzeichen ausweiten
            if (v and (1L shl (bits - 1)) != 0L) v -= 1L shl bits
            v = (v * factor.toDouble()).roundToLong().coerceIn(min, max)
            for (b in 0 until subslot) buf[s + b] = ((v shr (8 * b)) and 0xff).toByte()
            s += subslot
        }
    }

    /** dB (<= 0) in linearen Faktor; 0 dB -> exakt 1.0f. */
    fun dbToFactor(db: Int): Float =
        if (db >= 0) 1.0f else Math.pow(10.0, db / 20.0).toFloat()
}
