package com.tonverk.bridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** Auto-Split-Logik für lange Jams (>4 GiB WAV-Grenze). */
class WavSplitTest {

    @Test
    fun partNames() {
        assertEquals("jam.wav", WavRecorder.partName("jam", 1))
        assertEquals("jam-02.wav", WavRecorder.partName("jam", 2))
        assertEquals("jam-10.wav", WavRecorder.partName("jam", 10))
    }

    @Test
    fun splitThresholdIsBlockAlignedAndUnder4GiB() {
        for (blockAlign in intArrayOf(4, 6, 8, 12, 16)) {
            val t = WavRecorder.splitThreshold(blockAlign)
            assertEquals("Vielfaches von blockAlign", 0L, t % blockAlign)
            // Datei = 44-Byte-Header + Daten muss unter 2^32 bleiben.
            assertTrue("unter 4 GiB inkl. Header", 44L + t < 0xFFFF_FFFFL)
            // nahe am Maximum (keine unnötig kleinen Dateien)
            assertTrue("nicht zu klein", t > 3_900_000_000L)
        }
    }
}
