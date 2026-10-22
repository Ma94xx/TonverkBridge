package com.tonverk.bridge

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.AudioAttributes
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.IBinder

/**
 * Fängt das System-Medien-Audio per AudioPlaybackCapture ab (legal, weil wir damit
 * selbst zum Produzenten werden) und schreibt es über einen AudioTrack, dessen
 * bevorzugtes Ausgabegerät das gewählte USB-Gerät (Tonverk) ist. Den UAC2-Treiber
 * macht Android selbst – wir erzwingen nur das Ziel. Diagnose läuft über Diag -> UI.
 */
class AudioBridgeService : Service() {

    companion object {
        const val ACTION_START = "start"
        const val ACTION_STOP = "stop"
        const val EXTRA_CODE = "code"
        const val EXTRA_DATA = "data"
        const val EXTRA_DEVICE_ID = "device_id"
        const val EXTRA_NATIVE = "native"
        private const val CH_ID = "bridge"
        private const val NOTE_ID = 1
        private const val SR = 48_000          // Tonverk läuft auf 48 kHz
    }

    @Volatile private var running = false
    @Volatile private var meterPeak = 0f      // Max-Hold seit letztem Meter-Tick
    private var meterThread: Thread? = null
    private var nativeMode = false
    private var targetDeviceId = -1
    private var projection: MediaProjection? = null
    private var record: AudioRecord? = null
    private var track: AudioTrack? = null
    private var pump: Thread? = null
    private lateinit var audioManager: AudioManager

    private val deviceCallback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(added: Array<out AudioDeviceInfo>?) {
            Diag.log("Device(s) attached – re-applying route")
            routeToUsb(); logRouted()
        }
    }

    override fun onCreate() {
        super.onCreate()
        audioManager = getSystemService(AudioManager::class.java)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> { stopEverything(); stopSelf(); return START_NOT_STICKY }
            ACTION_START -> {
                if (running) stopEverything()   // evtl. alte Capture-Session sauber beenden
                val code = intent.getIntExtra(EXTRA_CODE, 0)
                val data = intent.getParcelableExtra<Intent>(EXTRA_DATA)
                targetDeviceId = intent.getIntExtra(EXTRA_DEVICE_ID, -1)
                nativeMode = intent.getBooleanExtra(EXTRA_NATIVE, false)
                if (data == null) { Diag.log("ERROR: no projection data"); stopSelf(); return START_NOT_STICKY }

                startForegroundNote()
                val mpm = getSystemService(MediaProjectionManager::class.java)
                projection = mpm.getMediaProjection(code, data)
                projection?.registerCallback(object : MediaProjection.Callback() {
                    override fun onStop() { stopEverything(); stopSelf() }
                }, null)
                startCapture()
            }
        }
        return START_STICKY
    }

    private fun startForegroundNote() {
        val nm = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(
                NotificationChannel(CH_ID, "Audio bridge", NotificationManager.IMPORTANCE_LOW)
            )
        }
        val note = buildNote("System audio → USB")
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTE_ID, note, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
        } else {
            startForeground(NOTE_ID, note)
        }
    }

    private fun buildNote(text: String): Notification =
        Notification.Builder(this, CH_ID)
            .setContentTitle("Tonverk Bridge active")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setOnlyAlertOnce(true)
            .build()

    /** Pegelmeter: aktualisiert Notification + UI 2x/Sekunde mit Peak und Stream-Zahl. */
    private fun startMeter() {
        meterThread = Thread {
            val nm = getSystemService(NotificationManager::class.java)
            while (running) {
                try { Thread.sleep(500) } catch (_: InterruptedException) { break }
                val pk = meterPeak
                meterPeak = 0f
                val db = if (pk > 1e-5f) 20.0 * Math.log10(pk.toDouble()) else -100.0
                val segs = ((db + 60.0) / 6.0).toInt().coerceIn(0, 10)
                val bar = "▰".repeat(segs) + "▱".repeat(10 - segs)
                val streams = try {
                    audioManager.activePlaybackConfigurations.count {
                        it.audioAttributes.usage == AudioAttributes.USAGE_MEDIA ||
                        it.audioAttributes.usage == AudioAttributes.USAGE_GAME ||
                        it.audioAttributes.usage == AudioAttributes.USAGE_UNKNOWN
                    }
                } catch (_: Exception) { 0 }
                val lvl = if (db <= -90.0) "silent"
                          else String.format(java.util.Locale.US, "%.0f dB", db)
                val txt = "$bar  $lvl · $streams media stream" + if (streams == 1) "" else "s"
                Diag.meter(txt)
                if (running) try { nm.notify(NOTE_ID, buildNote(txt)) } catch (_: Exception) {}
            }
            Diag.meter("")
        }.apply { name = "audio-meter"; isDaemon = true; start() }
    }

    private fun startCapture() {
        val mp = projection ?: return

        val config = AudioPlaybackCaptureConfiguration.Builder(mp)
            .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
            .addMatchingUsage(AudioAttributes.USAGE_GAME)
            .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
            .build()

        val inFormat = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(SR)
            .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
            .build()
        val minIn = AudioRecord.getMinBufferSize(SR, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT)
            .coerceAtLeast(SR / 5 * 4)   // mind. ~200 ms Capture-Puffer gegen Lastspitzen

        // Im Nativ-Modus bevorzugt in Float aufnehmen: voller Dynamikumfang des Mixers
        // (keine 16-Bit-Quantisierung), der Treiber wandelt gerundet nach 24 Bit.
        var floatMode = false
        var rec: AudioRecord? = null
        if (nativeMode) {
            try {
                val minInF = AudioRecord.getMinBufferSize(
                    SR, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_FLOAT
                ).coerceAtLeast(SR / 5 * 8)
                val r = AudioRecord.Builder()
                    .setAudioFormat(
                        AudioFormat.Builder()
                            .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                            .setSampleRate(SR)
                            .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                            .build()
                    )
                    .setBufferSizeInBytes(minInF * 4)
                    .setAudioPlaybackCaptureConfig(config)
                    .build()
                if (r.state == AudioRecord.STATE_INITIALIZED) { rec = r; floatMode = true }
                else { r.release(); Diag.log("Float capture failed to initialize – falling back to 16-bit") }
            } catch (e: Exception) {
                Diag.log("Float capture not available (${e.message}) – falling back to 16-bit")
            }
        }
        if (rec == null) {
            rec = AudioRecord.Builder()
                .setAudioFormat(inFormat)
                .setBufferSizeInBytes(minIn * 4)
                .setAudioPlaybackCaptureConfig(config)
                .build()
        }

        val minOut = AudioTrack.getMinBufferSize(SR, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT)
        var trk: AudioTrack? = null
        if (!nativeMode) {
            trk = AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .setAllowedCapturePolicy(AudioAttributes.ALLOW_CAPTURE_BY_NONE)
                        .build()
                )
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(SR)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                        .build()
                )
                .setBufferSizeInBytes(minOut * 4)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
        }

        val recorder = rec ?: return
        record = recorder
        track = trk
        running = true
        recorder.startRecording()

        if (nativeMode) {
            Diag.log("Capture in NATIVE mode -> libusb" + if (floatMode) " (float/24-bit path)" else " (16-bit)")
        } else {
            audioManager.registerAudioDeviceCallback(deviceCallback, null)
            logOutputs()
            routeToUsb()
            trk!!.play()
            routeToUsb()
            logRouted()
        }

        pump = Thread {
            var iter = 0
            var logged = 0
            if (floatMode) {
                val fbuf = FloatArray(4096)
                while (running) {
                    val n = recorder.read(fbuf, 0, fbuf.size, AudioRecord.READ_BLOCKING)
                    when {
                        n > 0 -> NativeUsb.writeFloat(fbuf, n)
                        n < 0 -> { Diag.log("AudioRecord.read error $n"); break }
                    }
                    if (n > 0) {
                        var peak = 0f
                        for (i in 0 until n) {
                            val a = if (fbuf[i] < 0) -fbuf[i] else fbuf[i]
                            if (a > peak) peak = a
                        }
                        if (peak > meterPeak) meterPeak = peak
                        if (++iter % 50 == 0 && logged < 8) {
                            Diag.log("capture peak=${(peak * 32767).toInt()}  ${NativeUsb.stats()}")
                            logged++
                        }
                    }
                }
            } else {
                val buf = ByteArray(minOut.coerceAtLeast(4096))
                while (running) {
                    val n = recorder.read(buf, 0, buf.size)
                    when {
                        n > 0 -> if (nativeMode) NativeUsb.write(buf, n) else trk!!.write(buf, 0, n)
                        n < 0 -> { Diag.log("AudioRecord.read error $n"); break }
                    }
                    if (n > 0) {
                        var peak = 0
                        var i = 0
                        while (i + 1 < n) {
                            val s = ((buf[i].toInt() and 0xff) or (buf[i + 1].toInt() shl 8)).toShort().toInt()
                            val a = if (s < 0) -s else s
                            if (a > peak) peak = a
                            i += 2
                        }
                        val pf = peak / 32768f
                        if (pf > meterPeak) meterPeak = pf
                        if (++iter % 50 == 0 && logged < 8) {
                            val extra = if (nativeMode) NativeUsb.stats() else "underrun=${trk!!.underrunCount}"
                            Diag.log("capture peak=$peak  $extra")
                            logged++
                        }
                    }
                }
            }
        }.apply { name = "audio-pump"; priority = Thread.MAX_PRIORITY; start() }

        startMeter()
    }

    private fun routeToUsb() {
        val outs = audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
        val usb = outs.firstOrNull { it.id == targetDeviceId && targetDeviceId != -1 }
            ?: outs.firstOrNull {
                it.type == AudioDeviceInfo.TYPE_USB_DEVICE ||
                it.type == AudioDeviceInfo.TYPE_USB_HEADSET ||
                it.type == AudioDeviceInfo.TYPE_USB_ACCESSORY
            }
        if (usb != null) {
            val ok = track?.setPreferredDevice(usb)
            Diag.log("Route -> ${usb.productName} (id=${usb.id}, type=${usb.type}) ok=$ok")
        } else {
            Diag.log("No USB output found – plugged in and in audio mode?")
        }
    }

    private fun logOutputs() {
        Diag.log("targetDeviceId=$targetDeviceId")
        audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS).forEach {
            Diag.log("OUT id=${it.id} type=${it.type} name=${it.productName} rates=${it.sampleRates?.toList()}")
        }
    }

    private fun logRouted() {
        val r = track?.routedDevice
        Diag.log(">>> ROUTED -> name=${r?.productName} type=${r?.type} id=${r?.id}")
        if (r != null && r.type != AudioDeviceInfo.TYPE_USB_DEVICE &&
            r.type != AudioDeviceInfo.TYPE_USB_HEADSET &&
            r.type != AudioDeviceInfo.TYPE_USB_ACCESSORY) {
            Diag.log(">>> WARNING: audio is NOT going to USB (type=${r.type}) = speaker fallback")
        }
    }

    private fun stopEverything() {
        running = false
        try { meterThread?.interrupt(); meterThread?.join(300) } catch (_: InterruptedException) {}
        meterThread = null
        Diag.meter("")
        try { pump?.join(500) } catch (_: InterruptedException) {}
        pump = null
        try { audioManager.unregisterAudioDeviceCallback(deviceCallback) } catch (_: Exception) {}
        try { record?.stop() } catch (_: Exception) {}
        record?.release(); record = null
        try { track?.stop() } catch (_: Exception) {}
        track?.release(); track = null
        try { projection?.stop() } catch (_: Exception) {}
        projection = null
        Diag.log("stopped")
    }

    override fun onDestroy() { stopEverything(); super.onDestroy() }
    override fun onBind(intent: Intent?): IBinder? = null
}
