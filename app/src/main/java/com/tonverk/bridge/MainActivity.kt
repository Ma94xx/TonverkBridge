package com.tonverk.bridge

import android.Manifest
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.method.ScrollingMovementMethod
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat

class MainActivity : ComponentActivity() {

    companion object {
        private const val DONATE_URL = "https://ko-fi.com/buyabeerforthos"
        private const val REC_LABEL = "●  Record"
        private const val REC_STOP_LABEL = "■  Stop recording"

        // Farbwelt
        private const val C_BG = "#0F0F13"
        private const val C_CARD = "#1A1A20"
        private const val C_TEXT = "#ECECF0"
        private const val C_DIM = "#9B9BA6"
        private const val C_ACCENT = "#FF8A3C"
        private const val C_GREEN = "#81C784"
        private const val C_RED = "#E05548"
        private const val C_AMBER = "#FFB300"
    }

    private lateinit var status: TextView
    private lateinit var meterView: TextView
    private lateinit var hint: TextView
    private lateinit var primaryBtn: Button
    private lateinit var levelMeter: LevelMeterView
    private lateinit var recBtn: Button
    private lateinit var recView: TextView
    private lateinit var nameEdit: EditText
    private lateinit var folderView: TextView
    private lateinit var gainLabel: TextView
    private lateinit var detailsBox: LinearLayout
    private lateinit var logView: TextView
    private lateinit var spinner: Spinner
    private lateinit var projectionManager: MediaProjectionManager
    private lateinit var audioManager: AudioManager

    private var devices: List<AudioDeviceInfo> = emptyList()
    private var active = false
    private var starting = false
    private var monitorEnabled = true   // Diagnose-Schalter: Live-Monitor pausieren (A/B-Test)

    private val ui = Handler(Looper.getMainLooper())

    // --- Zielordner für Aufnahmen (SAF); ohne Auswahl: Music/Tonverk Bridge ---
    private val folderLauncher = registerForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri ->
        if (uri != null) {
            try {
                contentResolver.takePersistableUriPermission(
                    uri, Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                )
            } catch (e: Exception) { Diag.log("REC: persist uri failed: ${e.message}") }
            recPrefs().edit().putString(AudioBridgeService.PREF_TREE_URI, uri.toString()).apply()
            updateFolderLabel()
        }
    }

    // --- Ergebnis des System-Dialogs "Aufnahme/Übertragung starten?" ---
    private val projectionLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        starting = false
        if (result.resultCode == RESULT_OK && result.data != null) {
            ContextCompat.startForegroundService(this, svc(AudioBridgeService.ACTION_START).apply {
                putExtra(AudioBridgeService.EXTRA_CODE, result.resultCode)
                putExtra(AudioBridgeService.EXTRA_DATA, result.data)
                putExtra(AudioBridgeService.EXTRA_NATIVE, true)
            })
            active = true
            pauseMonitor()   // IN-Stream aus: volle USB-Bandbreite fürs Playback (sonst Aussetzer)
            setState("● USB audio running", "All capturable sound now goes to the Tonverk.", true)
        } else {
            // Treiber/Monitor bewusst weiterlaufen lassen: Pegel + Aufnahme gehen auch ohne Bridge.
            setState("Ready", "Capture cancelled. Level meter and recording still work.", false)
        }
    }

    private val permLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { perms ->
        if (perms[Manifest.permission.RECORD_AUDIO] == true) askProjection()
        else { starting = false; setState("Ready", "Can't work without the record-audio permission.", false) }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        projectionManager = getSystemService(MediaProjectionManager::class.java)
        audioManager = getSystemService(AudioManager::class.java)
        RecGain.db = recPrefs().getInt(AudioBridgeService.PREF_GAIN_DB, 0).coerceIn(-24, 0)
        buildUi()
        window.statusBarColor = Color.parseColor(C_BG)
        window.navigationBarColor = Color.parseColor(C_BG)
        Diag.listener = { text -> logView.text = text }
        Diag.meterListener = { text -> meterView.text = text }
        RecStatus.listener = { rec, text ->
            recView.text = text
            styleRecButton(rec)
            nameEdit.isEnabled = !rec
            refreshButtons()
            // Nach Aufnahme-Ende (Service ruft captureStop) den Monitor wieder anwerfen.
            if (!rec) ensureMonitor()
        }
        BridgeStatus.listener = { on ->
            // Extern beendete Bridge (z. B. Projection vom System gestoppt): UI nachziehen.
            if (!on && active) {
                active = false
                ensureMonitor()   // Pegelmeter wieder anwerfen (Bandbreite ist jetzt frei)
                setState("Ready", "Bridge stopped. Tap \"Start\" to stream again.", false)
            }
        }
        styleRecButton(RecStatus.recording)
        refreshButtons()
        logView.text = Diag.snapshot()
        maybeAutoStart()
    }

    override fun onStart() {
        super.onStart()
        maybeStartMonitor()
        ui.post(meterTick)
    }

    override fun onStop() {
        ui.removeCallbacks(meterTick)
        // Monitor-Treiber freigeben, wenn nichts Aktives mehr dranhängt. Während
        // System-Dialogen (starting) und bei Bridge/Aufnahme bleibt er offen.
        if (!active && !starting && !RecStatus.recording) teardownAsync()
        super.onStop()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        maybeAutoStart()   // z.B. beim Einstecken des Tonverk
    }

    override fun onDestroy() {
        Diag.listener = null
        Diag.meterListener = null
        RecStatus.listener = null
        BridgeStatus.listener = null
        usbReceiver?.let { try { unregisterReceiver(it) } catch (_: Exception) {} }
        usbReceiver = null
        super.onDestroy()
    }

    private fun svc(action: String) =
        Intent(this, AudioBridgeService::class.java).apply { this.action = action }

    // ---------------- Live-Monitor (Pegel ohne Aufnahme) ----------------

    /** Startet den Capture-Pfad fürs Pegelmeter, sofern der Treiber läuft (idempotent).
     *  NICHT während aktiver Bridge: der 4-Kanal-ISO-IN-Stream würde mit dem OUT-Stream
     *  um die USB-Isochron-Bandbreite konkurrieren und das Playback am Tonverk gaten.
     *  Läuft im Hintergrund: captureStart macht blockierende USB-Control-Transfers. */
    private fun ensureMonitor() {
        if (!monitorEnabled || !NativeUsb.loaded || UsbLink.conn == null || active) return
        Thread {
            val r = NativeUsb.captureStart()
            if (r != 0) Diag.log("monitor: captureStart => $r")
        }.apply { isDaemon = true; start() }
    }

    /** Monitor-IN-Stream anhalten (Bandbreite für den OUT-Stream freigeben). */
    private fun pauseMonitor() {
        if (!NativeUsb.loaded) return
        Thread { NativeUsb.captureStop() }.apply { isDaemon = true; start() }
    }

    /** Treiber nur fürs Monitoring hochfahren (App offen + Tonverk dran, nichts läuft). */
    private fun maybeStartMonitor() {
        if (!NativeUsb.loaded || UsbLink.conn != null || active || starting || RecStatus.recording) return
        val um = getSystemService(UsbManager::class.java)
        if (findTonverk(um) == null) return
        startDriver(
            onFail = { msg -> Diag.log("monitor: $msg") },
            onReady = { }
        )
    }

    private val meterTick = object : Runnable {
        override fun run() {
            updateLevelMeter()
            ui.postDelayed(this, 150)
        }
    }

    private fun updateLevelMeter() {
        if (!NativeUsb.loaded || UsbLink.conn == null) {
            levelMeter.setIdle("connect Tonverk"); return
        }
        // Während aktiver Bridge ist der IN-Monitor bewusst aus (Bandbreite); nur bei
        // laufender Aufnahme kommt dann trotzdem ein Pegel (die Aufnahme nutzt IN).
        if (active && !RecStatus.recording) {
            levelMeter.setIdle("streaming to Tonverk"); return
        }
        when (val p = NativeUsb.capturePeak()) {
            -2 -> levelMeter.setIdle(if (monitorEnabled) "starting…" else "monitor paused")
            -1 -> {
                // Alle IN-Transfers tot: Gerät weg. Treiber freigeben, Zustand zurücksetzen.
                Diag.log("monitor: capture dead – releasing driver")
                levelMeter.setIdle("device lost – replug")
                if (active) { startService(svc(AudioBridgeService.ACTION_STOP)); active = false }
                teardownAsync()
                setState("Plug in your Tonverk", "Connection lost. Replug the USB cable.", false)
            }
            else -> {
                val db = if (p > 0) 20.0 * Math.log10(p / 32767.0) else -100.0
                levelMeter.setLevelDb((db + RecGain.db).coerceAtMost(0.0))
            }
        }
    }

    // ---------------- Oberfläche ----------------

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun card() = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        background = GradientDrawable().apply {
            cornerRadius = dp(20).toFloat()
            setColor(Color.parseColor(C_CARD))
        }
        setPadding(dp(18), dp(16), dp(18), dp(16))
    }

    private fun sectionLabel(text: String) = TextView(this).apply {
        this.text = text
        textSize = 11f
        letterSpacing = 0.12f
        setTextColor(Color.parseColor(C_DIM))
    }

    private fun gap(h: Int) = View(this).apply {
        layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(h))
    }

    private fun tintButton(b: Button, color: String, textColor: Int = Color.BLACK) {
        b.backgroundTintList = ColorStateList.valueOf(Color.parseColor(color))
        b.setTextColor(textColor)
        b.isAllCaps = false
    }

    private fun styleRecButton(rec: Boolean) {
        recBtn.text = if (rec) REC_STOP_LABEL else REC_LABEL
        tintButton(recBtn, if (rec) "#6E2A24" else C_RED, if (rec) Color.parseColor(C_TEXT) else Color.WHITE)
    }

    private fun buildUi() {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(18), dp(40), dp(18), dp(20))
        }
        fun add(v: View, topGap: Int = 0) {
            if (topGap > 0) root.addView(gap(topGap))
            root.addView(v, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }

        val title = TextView(this).apply {
            text = "Tonverk Bridge"
            textSize = 24f
            setTypeface(typeface, Typeface.BOLD)
            setTextColor(Color.parseColor(C_TEXT))
        }
        add(title)

        // ---- Karte 1: Bridge (Handy -> Tonverk) ----
        val bridgeCard = card()
        status = TextView(this).apply { textSize = 19f; setTypeface(typeface, Typeface.BOLD) }
        hint = TextView(this).apply { textSize = 13f; setTextColor(Color.parseColor(C_DIM)) }
        meterView = TextView(this).apply {
            typeface = Typeface.MONOSPACE
            textSize = 14f
            setTextColor(Color.parseColor(C_AMBER))
        }
        primaryBtn = Button(this).apply {
            text = "Start"
            textSize = 16f
            setOnClickListener { if (active) stopAll() else startFlow() }
        }
        tintButton(primaryBtn, C_ACCENT)
        bridgeCard.addView(sectionLabel("BRIDGE · PHONE → TONVERK"))
        bridgeCard.addView(gap(10)); bridgeCard.addView(status)
        bridgeCard.addView(gap(4));  bridgeCard.addView(hint)
        bridgeCard.addView(gap(8));  bridgeCard.addView(meterView)
        bridgeCard.addView(gap(12)); bridgeCard.addView(primaryBtn)
        add(bridgeCard, 18)

        // ---- Karte 2: Aufnahme (Tonverk -> Handy) ----
        val recCard = card()
        levelMeter = LevelMeterView(this)
        gainLabel = TextView(this).apply { textSize = 13f; setTextColor(Color.parseColor(C_DIM)) }
        val gainSeek = SeekBar(this).apply {
            max = 24
            progress = 24 + RecGain.db        // -24..0 dB -> 0..24
            progressTintList = ColorStateList.valueOf(Color.parseColor(C_ACCENT))
            thumbTintList = ColorStateList.valueOf(Color.parseColor(C_ACCENT))
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(s: SeekBar?, pr: Int, fromUser: Boolean) {
                    RecGain.db = pr - 24
                    updateGainLabel()
                }
                override fun onStartTrackingTouch(s: SeekBar?) {}
                override fun onStopTrackingTouch(s: SeekBar?) {
                    recPrefs().edit().putInt(AudioBridgeService.PREF_GAIN_DB, RecGain.db).apply()
                }
            })
        }
        updateGainLabel()
        recBtn = Button(this).apply {
            textSize = 16f
            setOnClickListener { toggleRecord() }
        }
        recView = TextView(this).apply {
            typeface = Typeface.MONOSPACE
            textSize = 13f
            setTextColor(Color.parseColor(C_RED))
        }
        nameEdit = EditText(this).apply {
            hint = "File name (empty = date & time)"
            textSize = 14f
            isSingleLine = true
            setTextColor(Color.parseColor(C_TEXT))
            setHintTextColor(Color.parseColor("#6A6A74"))
            backgroundTintList = ColorStateList.valueOf(Color.parseColor(C_DIM))
            setText(recPrefs().getString(AudioBridgeService.PREF_FILE_NAME, ""))
        }
        folderView = TextView(this).apply {
            textSize = 13f
            setTextColor(Color.parseColor(C_DIM))
        }
        val chooseBtn = Button(this).apply {
            text = "Choose folder…"
            textSize = 13f
            setOnClickListener { folderLauncher.launch(null) }
        }
        tintButton(chooseBtn, "#2A2A32", Color.parseColor(C_TEXT))
        val resetBtn = Button(this).apply {
            text = "Default"
            textSize = 13f
            setOnClickListener {
                recPrefs().edit().remove(AudioBridgeService.PREF_TREE_URI).apply()
                updateFolderLabel()
                toast("Saving to ${WavRecorder.DEFAULT_DIR}")
            }
        }
        tintButton(resetBtn, "#2A2A32", Color.parseColor(C_TEXT))
        val folderRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(chooseBtn, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            addView(gapW(8))
            addView(resetBtn, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        }
        updateFolderLabel()

        recCard.addView(sectionLabel("RECORD · TONVERK → PHONE"))
        recCard.addView(gap(12)); recCard.addView(levelMeter)
        recCard.addView(gap(10)); recCard.addView(gainLabel)
        recCard.addView(gainSeek)
        recCard.addView(gap(10)); recCard.addView(recBtn)
        recCard.addView(gap(4));  recCard.addView(recView)
        recCard.addView(gap(10)); recCard.addView(nameEdit)
        recCard.addView(gap(10)); recCard.addView(folderView)
        recCard.addView(gap(6));  recCard.addView(folderRow)
        add(recCard, 14)

        // ---- Fußbereich: Diagnose, Infos, Spende ----
        val detailsToggle = TextView(this).apply {
            text = "Diagnostics ▾"
            textSize = 14f
            setTextColor(Color.parseColor(C_DIM))
            setPadding(dp(4), dp(10), dp(4), dp(4))
            setOnClickListener {
                detailsBox.visibility =
                    if (detailsBox.visibility == View.GONE) View.VISIBLE else View.GONE
                if (detailsBox.visibility == View.VISIBLE) refreshDevices()
            }
        }
        detailsBox = buildDetails()
        val about = TextView(this).apply {
            textSize = 12f
            setTextColor(Color.parseColor("#6A6A74"))
            text = """
                HOW IT WORKS
                The bridge streams your phone's media audio (YouTube, browser, games, music apps) to the Elektron Tonverk over USB as a 48 kHz / 24-bit class-compliant stream, driven directly via libusb. No root required. Apps that opt out of audio capture (e.g. Spotify) stay silent by design.

                RECORDING
                "Record" captures the Tonverk's USB audio output into a WAV file (48 kHz / 24-bit). At 0 dB gain the file is bit-exact, clocked by the Tonverk itself. The level meter shows the incoming signal whenever the Tonverk is connected. Works with or without the bridge running.

                PERMISSIONS
                • Record audio + the system "start recording/casting" dialog (MediaProjection): Android requires both to let an app capture the phone's own media sound. Nothing is stored or sent anywhere — only the explicit "Record" button writes a file, and only to the folder you chose.
                • Notifications: the ongoing notification while the bridge or a recording is running.
                • USB access: requested when the Tonverk is attached.

                This is an unofficial tool, not affiliated with Elektron.
            """.trimIndent()
            setPadding(dp(4), dp(12), dp(4), dp(4))
        }
        val donateBtn = Button(this).apply {
            text = "☕ Buy me a beer (Ko-fi)"
            textSize = 13f
            setOnClickListener {
                try {
                    startActivity(Intent(Intent.ACTION_VIEW, android.net.Uri.parse(DONATE_URL)))
                } catch (_: Exception) { toast("No browser found") }
            }
        }
        tintButton(donateBtn, "#2A2A32", Color.parseColor(C_TEXT))

        add(detailsToggle, 10)
        add(detailsBox)
        add(about, 6)
        add(donateBtn, 8)

        setContentView(ScrollView(this).apply {
            setBackgroundColor(Color.parseColor(C_BG))
            isFillViewport = true
            addView(root)
        })
    }

    private fun gapW(w: Int) = View(this).apply {
        layoutParams = LinearLayout.LayoutParams(dp(w), 1)
    }

    private fun updateGainLabel() {
        gainLabel.text = if (RecGain.db == 0) "Recording gain:  0 dB (bit-exact)"
                         else "Recording gain:  ${RecGain.db} dB"
    }

    private fun buildDetails(): LinearLayout {
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
        }
        spinner = Spinner(this)
        val refreshBtn = dbtn("Refresh USB devices") { refreshDevices() }
        val toneBtn = dbtn("Test tone via USB (diagnostic)") { playTestTone() }
        val dumpBtn = dbtn("Dump USB descriptors") { dumpUsbDescriptors() }
        val statsBtn = dbtn("Driver status") {
            if (NativeUsb.loaded) Diag.log("STATUS: ${NativeUsb.stats()}  REC: ${NativeUsb.captureStats()}")
        }
        // A/B-Diagnose: pausiert den Live-Monitor (ISO-IN-Stream), um zu prüfen,
        // ob die Gleichzeitigkeit beider Richtungen den OUT-Pfad stört.
        lateinit var monitorBtn: Button
        monitorBtn = dbtn("Live monitor: ON (tap to pause)") {
            if (RecStatus.recording) { toast("Not while recording"); return@dbtn }
            monitorEnabled = !monitorEnabled
            if (monitorEnabled) {
                ensureMonitor()
                Diag.log("monitor: resumed by user")
            } else {
                Thread { NativeUsb.captureStop() }.apply { isDaemon = true }.start()
                Diag.log("monitor: paused by user (A/B test)")
            }
            monitorBtn.text = if (monitorEnabled) "Live monitor: ON (tap to pause)"
                              else "Live monitor: PAUSED (tap to resume)"
        }
        val copyBtn = dbtn("Copy diagnostics") {
            getSystemService(ClipboardManager::class.java)
                .setPrimaryClip(ClipData.newPlainText("diag", Diag.snapshot()))
            toast("Diagnostics copied")
        }
        logView = TextView(this).apply {
            textSize = 11f
            setTextIsSelectable(true)
            typeface = Typeface.MONOSPACE
            setTextColor(Color.parseColor(C_DIM))
            movementMethod = ScrollingMovementMethod()
        }
        val scroll = ScrollView(this).apply { addView(logView) }

        fun rowParams(gapTop: Int = 0, gapBottom: Int = 20) =
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = gapTop; bottomMargin = gapBottom }

        box.addView(TextView(this).apply {
            text = "Target device (for test tone):"
            setTextColor(Color.parseColor(C_DIM))
        }, rowParams(gapBottom = 8))
        box.addView(spinner, rowParams(gapBottom = 24))
        listOf(refreshBtn, toneBtn, dumpBtn, statsBtn, monitorBtn, copyBtn).forEach { box.addView(it, rowParams()) }
        box.addView(scroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 700)
            .apply { topMargin = 8 })
        return box
    }

    private fun dbtn(label: String, onClick: () -> Unit) =
        Button(this).apply {
            text = label
            textSize = 13f
            setOnClickListener { onClick() }
            tintButton(this, "#2A2A32", Color.parseColor(C_TEXT))
        }

    private fun setState(big: String, sub: String, on: Boolean) {
        status.text = big
        status.setTextColor(Color.parseColor(if (on) C_GREEN else "#B0B0B0"))
        hint.text = sub
        primaryBtn.text = if (on) "Stop" else "Start"
        refreshButtons()
    }

    /** Bridge und Aufnahme schließen sich gegenseitig aus (sonst konkurrieren beide
     *  ISO-Streams um die USB-Bandbreite). Die jeweils blockierte Aktion wird ausgegraut;
     *  die gerade laufende Aktion bleibt bedienbar, damit man sie stoppen kann. */
    private fun refreshButtons() {
        val rec = RecStatus.recording
        primaryBtn.isEnabled = active || (!rec && !starting)
        recBtn.isEnabled = rec || (!active && !starting)
        primaryBtn.alpha = if (primaryBtn.isEnabled) 1f else 0.4f
        recBtn.alpha = if (recBtn.isEnabled) 1f else 0.4f
        hintForLock(rec)
    }

    private fun hintForLock(rec: Boolean) {
        if (active && !rec) recView.text = "Stop the bridge to record"
        else if (rec) { /* recView zeigt den Aufnahme-Status */ }
    }

    private fun toast(m: String) {
        android.widget.Toast.makeText(this, m, android.widget.Toast.LENGTH_SHORT).show()
    }

    // ---------------- Aufnahme (Tonverk -> WAV-Datei) ----------------

    private fun recPrefs() = getSharedPreferences(AudioBridgeService.PREFS, MODE_PRIVATE)

    private fun updateFolderLabel() {
        val tree = recPrefs().getString(AudioBridgeService.PREF_TREE_URI, "") ?: ""
        val where = if (tree.isEmpty()) WavRecorder.DEFAULT_DIR
                    else android.net.Uri.parse(tree).lastPathSegment?.substringAfter(':') ?: "chosen folder"
        folderView.text = "Saves to:  $where"
    }

    private fun toggleRecord() {
        if (RecStatus.recording) {
            startService(svc(AudioBridgeService.ACTION_REC_STOP))
            return
        }
        if (!NativeUsb.loaded) { toast("Native library missing (broken build)"); return }
        if (active) { toast("Stop the bridge first"); return }   // nur eins von beiden gleichzeitig
        recPrefs().edit()
            .putString(AudioBridgeService.PREF_FILE_NAME, nameEdit.text.toString()).apply()

        if (UsbLink.conn != null) {   // Treiber läuft schon (Monitor oder Bridge)
            ContextCompat.startForegroundService(this, svc(AudioBridgeService.ACTION_REC_START))
            return
        }
        if (starting) { recView.text = "Busy – still connecting, try again."; return }
        starting = true
        recView.text = "Connecting…"
        onUsbFail = { recView.text = "No USB access (permission denied or no device)." }
        startDriver(
            onFail = { msg -> recView.text = msg },
            onReady = {
                starting = false
                ContextCompat.startForegroundService(this, svc(AudioBridgeService.ACTION_REC_START))
            }
        )
    }

    // ---------------- Start/Stop-Ablauf ----------------

    private fun maybeAutoStart() {
        // Nicht automatisch starten, wenn schon etwas läuft (inkl. laufender Aufnahme:
        // der Bridge-Start würde die alte Session im Service beenden).
        if (active || starting || RecStatus.recording) return
        val um = getSystemService(UsbManager::class.java)
        if (findTonverk(um) != null) startFlow()
        else setState("Plug in your Tonverk", "The bridge starts automatically once the Tonverk is connected via USB.", false)
    }

    private fun startFlow() {
        if (!NativeUsb.loaded) { setState("Error", "Native library missing (broken build).", false); return }
        if (RecStatus.recording) { toast("Stop the recording first"); return }   // gegenseitig ausschließend
        if (starting) return
        starting = true
        startService(svc(AudioBridgeService.ACTION_STOP))   // alte Bridge-Session beenden (Aufnahme läuft weiter)
        setState("Connecting…", "Starting the USB driver.", false)
        if (UsbLink.conn != null) {
            requestCaptureThenProjection()   // Treiber läuft schon im Monitor-Betrieb
        } else {
            startDriver(
                onFail = { msg -> setState("Error", msg, false) },
                onReady = { requestCaptureThenProjection() }
            )
        }
    }

    /** Gemeinsamer Treiber-Start für Bridge, Aufnahme und Monitor:
     *  USB-Gerät öffnen + nativen Treiber hochfahren + Capture-Pfad fürs Meter. */
    private fun startDriver(onFail: (String) -> Unit, onReady: () -> Unit) {
        ensureUsbPermission { um, dev ->
            teardown()   // evtl. alten/kaputten Treiber sauber ersetzen
            val conn = um.openDevice(dev) ?: run {
                starting = false; onFail("Could not open the USB device."); return@ensureUsbPermission
            }
            UsbLink.conn = conn
            val r = NativeUsb.start(conn.fileDescriptor, 48_000)
            Diag.log("NativeUsb.start => $r")
            if (r == 0) {
                ensureMonitor()
                onReady()
            } else {
                starting = false
                teardown()
                onFail("Driver start failed ($r). Unplug and replug the Tonverk.")
            }
        }
    }

    private fun stopAll() {
        // Nur die Bridge stoppen; Treiber (und eine evtl. Aufnahme) laufen weiter.
        startService(svc(AudioBridgeService.ACTION_STOP))
        active = false
        ensureMonitor()   // Pegelmeter wieder aktivieren
        setState("Ready", "Stopped. Tap \"Start\" to stream again.", false)
    }

    /** Synchroner Teardown (nur dort, wo gleich danach neu gestartet wird). */
    private fun teardown() {
        val c = UsbLink.conn
        UsbLink.conn = null
        try { NativeUsb.stop() } catch (_: Throwable) {}
        try { c?.close() } catch (_: Exception) {}
    }

    /** Teardown im Hintergrund: native_stop wartet bis ~0,5 s auf Transfer-Drain —
     *  das gehört nicht auf den Main-Thread (onStop, Meter-Tick). */
    private fun teardownAsync() {
        val c = UsbLink.conn
        UsbLink.conn = null
        Thread {
            try { NativeUsb.stop() } catch (_: Throwable) {}
            try { c?.close() } catch (_: Exception) {}
        }.apply { isDaemon = true; start() }
    }

    private fun requestCaptureThenProjection() {
        val needed = mutableListOf(Manifest.permission.RECORD_AUDIO)
        if (Build.VERSION.SDK_INT >= 33) needed.add(Manifest.permission.POST_NOTIFICATIONS)
        val missing = needed.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) askProjection() else permLauncher.launch(missing.toTypedArray())
    }

    private fun askProjection() {
        setState("Almost there…", "Please confirm \"Start now\" in the system dialog.", false)
        projectionLauncher.launch(projectionManager.createScreenCaptureIntent())
    }

    // ---------------- USB-Berechtigung ----------------

    private val usbPermAction = "com.tonverk.bridge.USB_PERMISSION"
    private var usbReceiver: BroadcastReceiver? = null
    private var onUsbReady: ((UsbManager, UsbDevice) -> Unit)? = null
    private var onUsbFail: (() -> Unit)? = null   // optionales Zusatz-Feedback (z. B. Aufnahme-Statuszeile)

    private fun findTonverk(um: UsbManager): UsbDevice? =
        um.deviceList.values.firstOrNull {
            it.productName?.contains("Tonverk", true) == true ||
            it.productName?.contains("Elektron", true) == true
        } ?: um.deviceList.values.firstOrNull()

    private fun ensureUsbPermission(onReady: (UsbManager, UsbDevice) -> Unit) {
        val um = getSystemService(UsbManager::class.java)
        val dev = findTonverk(um)
        if (dev == null) {
            starting = false
            onUsbFail?.invoke(); onUsbFail = null
            setState("Plug in your Tonverk", "No USB device found.", false)
            return
        }
        if (um.hasPermission(dev)) { onUsbFail = null; onReady(um, dev); return }

        onUsbReady = onReady
        if (usbReceiver == null) {
            usbReceiver = object : BroadcastReceiver() {
                override fun onReceive(c: Context, i: Intent) {
                    if (i.action == usbPermAction) {
                        if (i.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                            onUsbFail = null
                            onUsbReady?.invoke(um, dev)
                        } else {
                            starting = false
                            onUsbFail?.invoke(); onUsbFail = null
                            setState("Ready", "USB permission denied.", false)
                        }
                    }
                }
            }
            val filter = IntentFilter(usbPermAction)
            if (Build.VERSION.SDK_INT >= 33)
                registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
            else
                @Suppress("UnspecifiedRegisterReceiverFlag") registerReceiver(usbReceiver, filter)
        }
        val pi = PendingIntent.getBroadcast(
            this, 0, Intent(usbPermAction).setPackage(packageName), PendingIntent.FLAG_IMMUTABLE
        )
        um.requestPermission(dev, pi)
    }

    // ---------------- Diagnose-Funktionen (Details) ----------------

    private fun refreshDevices() {
        devices = audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS).filter {
            it.type == AudioDeviceInfo.TYPE_USB_DEVICE ||
            it.type == AudioDeviceInfo.TYPE_USB_HEADSET ||
            it.type == AudioDeviceInfo.TYPE_USB_ACCESSORY
        }
        val labels = if (devices.isEmpty()) listOf("– no USB output –")
                     else devices.map { "${it.productName} [${it.type}]" }
        spinner.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, labels)
    }

    private fun playTestTone() {
        val dev = devices.getOrNull(spinner.selectedItemPosition)
        if (dev == null) { toast("Select a USB device first"); return }
        Diag.log("=== TEST TONE (AudioTrack) to ${dev.productName} ===")
        Thread {
            val sr = 48_000
            val min = AudioTrack.getMinBufferSize(sr, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT)
            val at = AudioTrack.Builder()
                .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(sr).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build())
                .setBufferSizeInBytes(min * 4).setTransferMode(AudioTrack.MODE_STREAM).build()
            at.setPreferredDevice(dev); at.play()
            val chunk = ShortArray(960); var frame = 0
            while (frame < sr * 4) {
                for (i in 0 until 480) {
                    val s = (9000 * Math.sin(2.0 * Math.PI * 1000.0 * (frame + i) / sr)).toInt().toShort()
                    chunk[i * 2] = s; chunk[i * 2 + 1] = s
                }
                at.write(chunk, 0, chunk.size); frame += 480
            }
            at.stop(); at.release()
        }.start()
    }

    private fun dumpUsbDescriptors() {
        Diag.clear(); logView.text = ""
        ensureUsbPermission { um, dev ->
            Diag.log("USB: ${dev.productName} VID=${hex4(dev.vendorId)} PID=${hex4(dev.productId)}")
            val conn = um.openDevice(dev) ?: run { Diag.log("openDevice failed"); return@ensureUsbPermission }
            try {
                val raw = conn.rawDescriptors ?: ByteArray(0)
                Diag.log("rawDescriptors: ${raw.size} bytes")
                parseDescriptors(raw)
            } catch (e: Exception) { Diag.log("Error: ${e.message}") } finally { conn.close() }
        }
    }

    private fun parseDescriptors(d: ByteArray) {
        var i = 0
        while (i + 1 < d.size) {
            val len = d[i].toInt() and 0xff
            if (len == 0) break
            when (d[i + 1].toInt() and 0xff) {
                0x04 -> {
                    val cls = b(d, i + 5); val sub = b(d, i + 6)
                    val tag = if (cls == 1) " <- AUDIO" +
                        when (sub) { 2 -> " STREAMING"; 1 -> " CONTROL"; else -> "" } else ""
                    Diag.log("IFACE num=${b(d, i + 2)} alt=${b(d, i + 3)} cls=$cls sub=$sub$tag")
                }
                0x05 -> {
                    val addr = b(d, i + 2); val attr = b(d, i + 3)
                    val dir = if (addr and 0x80 != 0) "IN" else "OUT"
                    val tt = when (attr and 0x03) { 1 -> "ISO"; 2 -> "BULK"; 3 -> "INT"; else -> "CTRL" }
                    Diag.log("  EP ${hex2(addr)} $dir $tt mps=${b(d, i + 4) or (b(d, i + 5) shl 8)} iv=${b(d, i + 6)}")
                }
                0x24 -> Diag.log("  CS_IFACE sub=${b(d, i + 2)} raw=${hexRange(d, i, len)}")
            }
            i += len
        }
        Diag.log("=== done ===")
    }

    private fun b(a: ByteArray, idx: Int) = if (idx < a.size) a[idx].toInt() and 0xff else 0
    private fun hex2(v: Int) = "0x%02X".format(v)
    private fun hex4(v: Int) = "0x%04X".format(v)
    private fun hexRange(a: ByteArray, off: Int, len: Int): String {
        val sb = StringBuilder(); var k = off
        while (k < off + len && k < a.size) { sb.append("%02X ".format(a[k].toInt() and 0xff)); k++ }
        return sb.toString().trim()
    }
}
