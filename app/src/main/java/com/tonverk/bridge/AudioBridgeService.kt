package com.tonverk.bridge

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
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
import android.net.Uri
import android.os.Build
import android.os.IBinder
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

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
        const val ACTION_REC_START = "rec_start"
        const val ACTION_REC_STOP = "rec_stop"
        const val EXTRA_CODE = "code"
        const val EXTRA_DATA = "data"
        const val EXTRA_DEVICE_ID = "device_id"
        const val EXTRA_NATIVE = "native"
        const val PREFS = "rec_prefs"
        const val PREF_TREE_URI = "tree_uri"       // SAF-Zielordner ("" = Default)
        const val PREF_FILE_NAME = "file_name"     // Wunschname ("" = Datum+Uhrzeit)
        const val PREF_GAIN_DB = "gain_db"         // Aufnahme-Gain in dB (-24..0)
        private const val CH_ID = "bridge"
        private const val NOTE_ID = 1
        private const val SR = 48_000          // Tonverk läuft auf 48 kHz

        /** Laufende Instanz (gleicher Prozess): erlaubt der Activity, eine Aufnahme
         *  synchron zu finalisieren, BEVOR sie den nativen Treiber stoppt. */
        @Volatile var instance: AudioBridgeService? = null
            private set
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
        instance = this
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            // Stoppt nur die Bridge; eine laufende Aufnahme läuft weiter (der Treiber
            // gehört der Activity und bleibt im Monitor-Betrieb ohnehin offen).
            ACTION_STOP -> { stopBridge(); stopIfIdle(); return START_NOT_STICKY }
            ACTION_REC_START -> {
                // Aufnahme funktioniert auch ohne laufende Bridge (nur USB-Treiber nötig);
                // dann läuft der Foreground-Service über den Typ "connectedDevice".
                // NOT_STICKY: Ein System-Restart ohne Intent könnte die Aufnahme ohnehin
                // nicht wieder aufnehmen (Treiber-fd weg) und dürfte kein startForeground rufen.
                if (!running) startForegroundRec()
                startRecording()
                return if (running) START_STICKY else START_NOT_STICKY
            }
            ACTION_REC_STOP -> {
                stopRecording()   // finishRecording() im Writer-Thread ruft stopIfIdle()
                return if (running) START_STICKY else START_NOT_STICKY
            }
            ACTION_START -> {
                if (running) stopBridge()   // evtl. alte Capture-Session sauber beenden
                val code = intent.getIntExtra(EXTRA_CODE, 0)
                val data = intent.getParcelableExtra<Intent>(EXTRA_DATA)
                targetDeviceId = intent.getIntExtra(EXTRA_DEVICE_ID, -1)
                nativeMode = intent.getBooleanExtra(EXTRA_NATIVE, false)
                if (data == null) { Diag.log("ERROR: no projection data"); stopSelf(); return START_NOT_STICKY }

                startForegroundNote()
                val mpm = getSystemService(MediaProjectionManager::class.java)
                projection = mpm.getMediaProjection(code, data)
                projection?.registerCallback(object : MediaProjection.Callback() {
                    override fun onStop() { stopBridge(); stopIfIdle() }
                }, null)
                startCapture()
            }
            else -> {   // z. B. Null-Intent nach System-Restart: nie ohne startForeground weiterlaufen
                stopIfIdle()
                return START_NOT_STICKY
            }
        }
        return START_STICKY
    }

    private fun goForeground(text: String, type: Int, recording: Boolean) {
        val nm = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(
                NotificationChannel(CH_ID, "Audio bridge", NotificationManager.IMPORTANCE_LOW)
            )
        }
        val note = buildNote(text, recording)
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTE_ID, note, type)
        } else {
            startForeground(NOTE_ID, note)
        }
    }

    private fun startForegroundNote() =
        goForeground("System audio → USB", ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION, false)

    /** Notification mit Zustand, „App öffnen" (Tippen) und einer Stop-Aktion. */
    private fun buildNote(text: String, recording: Boolean): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE
        )
        val stopAction = if (recording) ACTION_REC_STOP else ACTION_STOP
        val stopPi = PendingIntent.getService(
            this, if (recording) 2 else 1,
            Intent(this, AudioBridgeService::class.java).setAction(stopAction),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val title = if (recording) "● Recording from Tonverk" else "● Streaming to Tonverk"
        return Notification.Builder(this, CH_ID)
            .setContentTitle(title)
            .setContentText(text)
            .setSmallIcon(if (recording) android.R.drawable.ic_btn_speak_now else android.R.drawable.ic_media_play)
            .setOnlyAlertOnce(true)
            .setOngoing(true)
            .setContentIntent(open)
            .addAction(
                Notification.Action.Builder(
                    null, if (recording) "Stop recording" else "Stop bridge", stopPi
                ).build()
            )
            .build()
    }

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
                if (running) try { nm.notify(NOTE_ID, buildNote(txt, false)) } catch (_: Exception) {}
            }
            Diag.meter("")
        }.apply { name = "audio-meter"; isDaemon = true; start() }
    }

    // ---------------- Aufnahme (Tonverk -> WAV-Datei) ----------------

    @Volatile private var recRunning = false
    private var recThread: Thread? = null

    /** Foreground ohne MediaProjection (Aufnahme ohne laufende Bridge): Typ connectedDevice. */
    private fun startForegroundRec() =
        goForeground("● 0:00", ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE, true)

    /** Service beenden, sobald weder Bridge noch Aufnahme läuft. Von jedem Thread aufrufbar. */
    private fun stopIfIdle() {
        if (!running && !recRunning) {
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
        }
    }

    private fun startRecording() {
        if (recRunning) return
        if (!NativeUsb.loaded) { RecStatus.update(false, "Native library missing"); return }

        val r = NativeUsb.captureStart()
        if (r != 0) {
            Diag.log("REC: captureStart => $r")
            val why = if (r == -3) " – device has no capture interface?" else ""
            RecStatus.update(false, "Recording failed (driver error $r)$why")
            stopIfIdle()
            return
        }
        NativeUsb.captureFlush()   // Monitor-Altbestand (bis ~2 s) gehört nicht in die Datei
        val ch = NativeUsb.captureChannels().coerceAtLeast(1)
        val sub = NativeUsb.captureSubslot().coerceAtLeast(1)
        val rate = NativeUsb.captureRate().takeIf { it > 0 } ?: SR

        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        RecGain.db = prefs.getInt(PREF_GAIN_DB, 0).coerceIn(-24, 0)
        val treeUri = prefs.getString(PREF_TREE_URI, "")!!
            .takeIf { it.isNotEmpty() }?.let { Uri.parse(it) }
        val base = WavRecorder.sanitizeName(prefs.getString(PREF_FILE_NAME, ""))
            ?: SimpleDateFormat("yyyy-MM-dd HH-mm-ss", Locale.US).format(Date())
        val name = if (base.endsWith(".wav", true)) base else "$base.wav"

        val w = WavRecorder(contentResolver)
        if (!w.start(ch, rate, sub, name, treeUri)) {
            NativeUsb.captureStop()
            RecStatus.update(false, "Could not create the recording file")
            stopIfIdle()
            return
        }
        recRunning = true
        Diag.log("REC: start -> ${w.label} (${ch}ch / ${sub * 8} bit / $rate Hz)")
        RecStatus.update(true, "● 0:00")
        recThread = Thread { recLoop(w, ch, sub, rate) }
            .apply { setName("rec-writer"); priority = Thread.NORM_PRIORITY + 2; start() }
    }

    private fun recLoop(w: WavRecorder, ch: Int, sub: Int, rate: Int) {
        val frame = ch * sub
        val buf = ByteArray(rate / 10 * frame)      // ~100 ms pro Lesevorgang
        val nm = getSystemService(NotificationManager::class.java)
        var peak = 0
        var sincePatch = 0L
        var lastUi = 0L
        var lastNote = 0L
        while (recRunning) {
            val n = NativeUsb.captureRead(buf, buf.size)
            if (n < 0) { Diag.log("REC: capture ended by driver"); break }
            if (n == 0) { try { Thread.sleep(20) } catch (_: InterruptedException) { break }; continue }
            // Live-Gain (0 dB = No-op, Bytes bleiben bit-exakt)
            Pcm.applyGainInPlace(buf, n, sub, Pcm.dbToFactor(RecGain.db))
            try { w.write(buf, n) } catch (e: Exception) { Diag.log("REC: write error: ${e.message}"); break }

            // Peak über alle Kanäle (nach Gain): das MSB jedes Samples reicht fürs Meter.
            var i = sub - 1
            while (i < n) {
                val v = buf[i].toInt()
                val a = if (v < 0) -(v + 1) else v
                if (a > peak) peak = a
                i += sub
            }

            // Header regelmäßig patchen: Datei bleibt auch nach App-Tod abspielbar.
            sincePatch += n
            if (sincePatch >= 5L * rate * frame) {
                try { w.patchHeader() } catch (_: Exception) {}
                sincePatch = 0
            }

            val now = System.currentTimeMillis()
            if (now - lastUi >= 500) {
                lastUi = now
                val secs = w.dataBytes / (rate.toLong() * frame)
                val db = if (peak > 0) 20.0 * Math.log10(peak / 127.0) else -100.0
                peak = 0
                val lvl = if (db <= -90.0) "silent" else String.format(Locale.US, "%.0f dB", db)
                val txt = String.format(Locale.US, "● %d:%02d  %s", secs / 60, secs % 60, lvl)
                RecStatus.update(true, txt)
                // Notification sparsamer (alle 2 s): jede Aktualisierung ist ein IPC + Shade-Redraw.
                if (now - lastNote >= 2000) {
                    lastNote = now
                    try { nm.notify(NOTE_ID, buildNote(txt, true)) } catch (_: Exception) {}
                }
            }
        }
        finishRecording(w, sub)
    }

    /** Läuft im rec-writer-Thread: Ring leeren, Capture stoppen, Datei abschließen. */
    private fun finishRecording(w: WavRecorder, sub: Int) {
        val buf = ByteArray(32768)
        while (true) {
            val n = NativeUsb.captureRead(buf, buf.size)
            if (n <= 0) break
            Pcm.applyGainInPlace(buf, n, sub, Pcm.dbToFactor(RecGain.db))
            try { w.write(buf, n) } catch (_: Exception) { break }
        }
        NativeUsb.captureStop()
        val label = w.finish()
        recRunning = false
        Diag.log("REC: done -> $label (${w.dataBytes} bytes)")
        RecStatus.update(false, if (label != null) "Saved: $label" else "Recording failed")
        stopIfIdle()   // endet die Aufnahme von selbst (z. B. Unplug), Service nicht hängen lassen
    }

    /** Beendet die Aufnahme und wartet (begrenzt), bis die Datei finalisiert ist.
     *  Vom UI-Thread und von MainActivity (via instance) aufrufbar. */
    fun stopRecording() {
        if (!recRunning && recThread == null) return
        recRunning = false
        recThread?.interrupt()
        try { recThread?.join(1500) } catch (_: InterruptedException) {}
        recThread = null
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

    /** Beendet nur den Bridge-Teil (Projection-Capture + Pump). Aufnahme bleibt unberührt. */
    private fun stopBridge() {
        if (!running) return
        running = false
        // Recorder zuerst stoppen: der Pump-Thread hängt sonst im blockierenden
        // recorder.read() und join() läuft in den Timeout -> Pump überlebt den Treiber.
        try { record?.stop() } catch (_: Exception) {}
        try { pump?.interrupt(); pump?.join(800) } catch (_: InterruptedException) {}
        pump = null
        try { meterThread?.interrupt(); meterThread?.join(300) } catch (_: InterruptedException) {}
        meterThread = null
        Diag.meter("")
        try { audioManager.unregisterAudioDeviceCallback(deviceCallback) } catch (_: Exception) {}
        record?.release(); record = null
        try { track?.stop() } catch (_: Exception) {}
        track?.release(); track = null
        try { projection?.stop() } catch (_: Exception) {}
        projection = null
        // Läuft noch eine Aufnahme, Foreground-Status/Notification darauf umstellen.
        if (recRunning) startForegroundRec()
        BridgeStatus.update(false)   // UI informieren (z. B. Projection extern beendet)
        Diag.log("bridge stopped")
    }

    override fun onDestroy() {
        instance = null
        stopRecording()     // zuerst: finalisiert eine evtl. laufende WAV-Datei sauber
        stopBridge()
        super.onDestroy()
    }
    override fun onBind(intent: Intent?): IBinder? = null
}
