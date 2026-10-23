package com.tonverk.bridge

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** JVM-Tests für die reine Audio-/WAV-Logik (kein Android nötig). */
class PcmTest {

    private fun b24(v: Int) = byteArrayOf(
        (v and 0xff).toByte(), ((v shr 8) and 0xff).toByte(), ((v shr 16) and 0xff).toByte()
    )

    @Test
    fun gain24BitHalvesSamples() {
        val buf = b24(1000000) + b24(-1000000)
        Pcm.applyGainInPlace(buf, buf.size, 3, 0.5f)
        assertArrayEquals(b24(500000) + b24(-500000), buf)
    }

    @Test
    fun gainUnityLeavesBytesUntouched() {
        val buf = b24(0x123456) + b24(-0x123456)
        val before = buf.copyOf()
        Pcm.applyGainInPlace(buf, buf.size, 3, 1.0f)
        assertArrayEquals(before, buf)
    }

    @Test
    fun gain24BitClampsAtFullScale() {
        // factor > 1 kommt im UI nicht vor, aber die Funktion darf nie überlaufen
        val buf = b24(8000000)
        Pcm.applyGainInPlace(buf, buf.size, 3, 1.5f)
        assertArrayEquals(b24(8388607), buf)
    }

    @Test
    fun gain16BitWorks() {
        val buf = byteArrayOf(0x10, 0x00, 0x00, 0x80.toByte())   // +16, -32768
        Pcm.applyGainInPlace(buf, buf.size, 2, 0.5f)
        assertArrayEquals(byteArrayOf(0x08, 0x00, 0x00, 0xC0.toByte()), buf)  // +8, -16384
    }

    @Test
    fun gainIgnoresPartialTrailingSample() {
        // n nicht Sample-aligned: der angebrochene Rest bleibt unangetastet
        val buf = b24(1000000) + byteArrayOf(0x7F)
        Pcm.applyGainInPlace(buf, buf.size, 3, 0.5f)
        assertArrayEquals(b24(500000) + byteArrayOf(0x7F), buf)
    }

    // ---- WAV-Header ----

    private fun le32(b: ByteArray, off: Int): Long =
        (b[off].toLong() and 0xff) or ((b[off + 1].toLong() and 0xff) shl 8) or
        ((b[off + 2].toLong() and 0xff) shl 16) or ((b[off + 3].toLong() and 0xff) shl 24)

    private fun le16(b: ByteArray, off: Int): Int =
        (b[off].toInt() and 0xff) or ((b[off + 1].toInt() and 0xff) shl 8)

    @Test
    fun wavHeaderFields() {
        val h = WavRecorder.header(channels = 2, rate = 48000, bytesPerSample = 3, dataBytes = 1000)
        assertEquals(44, h.size)
        assertEquals("RIFF", String(h, 0, 4, Charsets.US_ASCII))
        assertEquals(1036L, le32(h, 4))               // 36 + data
        assertEquals("WAVE", String(h, 8, 4, Charsets.US_ASCII))
        assertEquals(1, le16(h, 20))                  // PCM
        assertEquals(2, le16(h, 22))                  // Kanäle
        assertEquals(48000L, le32(h, 24))             // Rate
        assertEquals(48000L * 6, le32(h, 28))         // Byte-Rate
        assertEquals(6, le16(h, 32))                  // Block-Align
        assertEquals(24, le16(h, 34))                 // Bits
        assertEquals("data", String(h, 36, 4, Charsets.US_ASCII))
        assertEquals(1000L, le32(h, 40))
    }

    @Test
    fun wavHeaderClampsAbove4GiB() {
        val h = WavRecorder.header(2, 48000, 3, dataBytes = 6_000_000_000L)
        val data = le32(h, 40)
        val riff = le32(h, 4)
        assertEquals(0xFFFF_FFFFL - 44, data)         // geklemmt statt übergelaufen
        assertEquals(data + 36, riff)
    }

    // ---- Dateinamen ----

    @Test
    fun sanitizeNameStripsForbiddenChars() {
        assertEquals("a-b-c", WavRecorder.sanitizeName("a/b\\c"))
        assertNull(WavRecorder.sanitizeName("   "))
        assertNull(WavRecorder.sanitizeName(null))
    }
}
