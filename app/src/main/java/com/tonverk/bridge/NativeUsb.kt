package com.tonverk.bridge

/** Brücke zum nativen libusb-UAC2-Treiber (usbaudio.so). */
object NativeUsb {
    @Volatile var loaded = false

    init {
        try {
            try { System.loadLibrary("usb1.0") } catch (_: Throwable) {} // Abhängigkeit zuerst
            System.loadLibrary("usbaudio")
            loaded = true
        } catch (e: Throwable) { Diag.log("native lib load error: ${e.message}") }
    }

    /** Öffnet den Stream auf dem USB-fd (0 = ok, negativ = Fehlercode). */
    external fun start(fd: Int, sampleRate: Int): Int

    /** Wie start(), aber speist einen reinen Sinus (Diagnose: isoliert den USB-Pfad). */
    external fun startTone(fd: Int, sampleRate: Int): Int

    /** 16-Bit-LE-Stereo-PCM in den Treiber schieben (wird intern nach 24 Bit gewandelt). */
    external fun write(pcm: ByteArray, len: Int)

    /** Float-Stereo-PCM (voller Mixer-Dynamikumfang) -> intern 24 Bit, gerundet. */
    external fun writeFloat(pcm: FloatArray, len: Int)

    external fun stop()

    external fun stats(): String

    /** Wird vom nativen Code aufgerufen -> landet in der Bildschirm-Diagnose. */
    @JvmStatic
    fun log(msg: String) { Diag.log("[native] $msg") }
}
