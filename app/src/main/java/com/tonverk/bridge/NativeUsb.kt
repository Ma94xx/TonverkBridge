package com.tonverk.bridge

import android.hardware.usb.UsbDeviceConnection

/** Prozessweiter Halter der USB-Verbindung: der native Treiber lebt prozessweit,
 *  also darf die Verbindung nicht an eine Activity-Instanz gebunden sein
 *  (Rotation/Recreate würde sonst fd leaken und den Monitor verlieren). */
object UsbLink {
    @Volatile var conn: UsbDeviceConnection? = null
}

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

    // ---- Capture (Tonverk -> Handy) ----

    /** Startet den ISO-IN-Capture-Pfad; der Treiber (start) muss laufen. 0 = ok, negativ = Fehler. */
    external fun captureStart(): Int

    external fun captureStop()

    /** Holt bis zu max Bytes ab (ganze Frames, gepackt wie vom Gerät, z. B. 24-Bit-LE).
     *  0 = gerade nichts da, -1 = Capture gestoppt oder tot (z. B. Gerät abgezogen). */
    external fun captureRead(buf: ByteArray, max: Int): Int

    /** Pegel-Poll fürs Live-Meter: >=0 Betrags-Peak (16-Bit-Auflösung) seit dem letzten
     *  Aufruf, -1 = Capture tot (Gerät weg), -2 = kein Capture aktiv. */
    external fun capturePeak(): Int

    /** Ring leeren (Monitor-Altbestand verwerfen, bevor eine Aufnahme startet). */
    external fun captureFlush()

    /** Format des laufenden Captures (0 solange kein Capture läuft). */
    external fun captureChannels(): Int
    external fun captureSubslot(): Int
    external fun captureRate(): Int

    external fun captureStats(): String

    /** Wird vom nativen Code aufgerufen -> landet in der Bildschirm-Diagnose. */
    @JvmStatic
    fun log(msg: String) { Diag.log("[native] $msg") }
}
