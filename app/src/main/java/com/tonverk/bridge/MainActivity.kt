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
import android.graphics.Color
import android.graphics.Typeface
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.text.method.ScrollingMovementMethod
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat

class MainActivity : ComponentActivity() {

    companion object {
        private const val DONATE_URL = "https://ko-fi.com/buyabeerforthos"
    }

    private lateinit var status: TextView
    private lateinit var meterView: TextView
    private lateinit var hint: TextView
    private lateinit var primaryBtn: Button
    private lateinit var detailsBox: LinearLayout
    private lateinit var logView: TextView
    private lateinit var spinner: Spinner
    private lateinit var projectionManager: MediaProjectionManager
    private lateinit var audioManager: AudioManager

    private var devices: List<AudioDeviceInfo> = emptyList()
    private var usbConn: UsbDeviceConnection? = null
    private var active = false
    private var starting = false

    // --- Ergebnis des System-Dialogs "Aufnahme/Übertragung starten?" ---
    private val projectionLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        starting = false
        if (result.resultCode == RESULT_OK && result.data != null) {
            ContextCompat.startForegroundService(this, Intent(this, AudioBridgeService::class.java).apply {
                action = AudioBridgeService.ACTION_START
                putExtra(AudioBridgeService.EXTRA_CODE, result.resultCode)
                putExtra(AudioBridgeService.EXTRA_DATA, result.data)
                putExtra(AudioBridgeService.EXTRA_NATIVE, true)
            })
            active = true
            setState("● USB audio running", "All capturable sound now goes to the Tonverk.", true)
        } else {
            teardown()
            setState("Ready", "Capture permission cancelled. Tap \"Start\" to try again.", false)
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
        buildUi()
        Diag.listener = { text -> logView.text = text }
        Diag.meterListener = { text -> meterView.text = text }
        logView.text = Diag.snapshot()
        maybeAutoStart()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        maybeAutoStart()   // z.B. beim Einstecken des Tonverk
    }

    override fun onDestroy() {
        Diag.listener = null
        Diag.meterListener = null
        usbReceiver?.let { try { unregisterReceiver(it) } catch (_: Exception) {} }
        usbReceiver = null
        super.onDestroy()
    }

    // ---------------- Oberfläche ----------------

    private fun buildUi() {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(56, 120, 56, 48)
        }
        val title = TextView(this).apply {
            text = "Tonverk Bridge"
            textSize = 24f
            setTypeface(typeface, Typeface.BOLD)
            setPadding(0, 0, 0, 40)
        }
        status = TextView(this).apply { textSize = 22f; setTypeface(typeface, Typeface.BOLD) }
        meterView = TextView(this).apply {
            typeface = Typeface.MONOSPACE
            textSize = 16f
            setTextColor(Color.parseColor("#FFB300"))
            setPadding(0, 12, 0, 0)
        }
        hint = TextView(this).apply { textSize = 15f; setPadding(0, 12, 0, 40) }
        primaryBtn = Button(this).apply {
            text = "Start"
            textSize = 18f
            setOnClickListener { if (active) stopAll() else startFlow() }
        }
        val about = TextView(this).apply {
            textSize = 13f
            setTextColor(Color.parseColor("#9E9E9E"))
            setPadding(0, 40, 0, 8)
            text = """
                HOW IT WORKS
                This app captures your phone's media audio (YouTube, browser, games, music apps) and streams it to the Elektron Tonverk over USB as a 48 kHz / 24-bit class-compliant audio stream, driven directly via libusb. No root required. Apps that opt out of audio capture (e.g. Spotify) stay silent by design.

                PERMISSIONS
                • Record audio + the system "start recording/casting" dialog (MediaProjection): Android requires both to let an app capture the phone's own media sound. Nothing is recorded, stored or sent anywhere — the audio goes straight to your Tonverk.
                • Notifications: shows the ongoing notification while the bridge is running.
                • USB access: requested when the Tonverk is attached, to talk to its USB audio interface.

                This is an unofficial tool, not affiliated with Elektron.
            """.trimIndent()
        }
        val donateBtn = Button(this).apply {
            text = "☕ Buy me a beer (Ko-fi)"
            textSize = 14f
            setOnClickListener {
                try {
                    startActivity(Intent(Intent.ACTION_VIEW, android.net.Uri.parse(DONATE_URL)))
                } catch (_: Exception) { toast("No browser found") }
            }
        }
        val detailsToggle = TextView(this).apply {
            text = "Diagnostics ▾"
            textSize = 14f
            setPadding(0, 48, 0, 8)
            setOnClickListener {
                detailsBox.visibility =
                    if (detailsBox.visibility == View.GONE) View.VISIBLE else View.GONE
                if (detailsBox.visibility == View.VISIBLE) refreshDevices()
            }
        }
        detailsBox = buildDetails()
        listOf(title, status, meterView, hint, primaryBtn, about, donateBtn, detailsToggle, detailsBox)
            .forEach { root.addView(it) }
        setContentView(ScrollView(this).apply { addView(root) })
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
        val statsBtn = dbtn("Driver status") { if (NativeUsb.loaded) Diag.log("STATUS: ${NativeUsb.stats()}") }
        val copyBtn = dbtn("Copy diagnostics") {
            getSystemService(ClipboardManager::class.java)
                .setPrimaryClip(ClipData.newPlainText("diag", Diag.snapshot()))
            toast("Diagnostics copied")
        }
        logView = TextView(this).apply {
            textSize = 11f
            setTextIsSelectable(true)
            typeface = Typeface.MONOSPACE
            movementMethod = ScrollingMovementMethod()
        }
        val scroll = ScrollView(this).apply { addView(logView) }

        fun rowParams(gapTop: Int = 0, gapBottom: Int = 20) =
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = gapTop; bottomMargin = gapBottom }

        box.addView(TextView(this).apply { text = "Target device (for test tone):" }, rowParams(gapBottom = 8))
        box.addView(spinner, rowParams(gapBottom = 24))
        listOf(refreshBtn, toneBtn, dumpBtn, statsBtn, copyBtn).forEach { box.addView(it, rowParams()) }
        box.addView(scroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 700)
            .apply { topMargin = 8 })
        return box
    }

    private fun dbtn(label: String, onClick: () -> Unit) =
        Button(this).apply {
            text = label
            textSize = 13f
            isAllCaps = false
            setOnClickListener { onClick() }
        }

    private fun setState(big: String, sub: String, on: Boolean) {
        status.text = big
        status.setTextColor(if (on) Color.parseColor("#81C784") else Color.parseColor("#B0B0B0"))
        hint.text = sub
        primaryBtn.text = if (on) "Stop" else "Start"
    }

    private fun toast(m: String) {
        android.widget.Toast.makeText(this, m, android.widget.Toast.LENGTH_SHORT).show()
    }

    // ---------------- Start/Stop-Ablauf ----------------

    private fun maybeAutoStart() {
        if (active || starting) return
        val um = getSystemService(UsbManager::class.java)
        if (findTonverk(um) != null) startFlow()
        else setState("Plug in your Tonverk", "The bridge starts automatically once the Tonverk is connected via USB.", false)
    }

    private fun startFlow() {
        if (!NativeUsb.loaded) { setState("Error", "Native library missing (broken build).", false); return }
        if (starting) return
        starting = true
        cleanupSession()
        setState("Connecting…", "Starting the USB driver.", false)
        ensureUsbPermission { um, dev ->
            val conn = um.openDevice(dev) ?: run {
                starting = false; setState("Error", "Could not open the USB device.", false); return@ensureUsbPermission
            }
            usbConn = conn
            val r = NativeUsb.start(conn.fileDescriptor, 48_000)
            Diag.log("NativeUsb.start => $r")
            if (r == 0) requestCaptureThenProjection()
            else {
                starting = false
                teardown()
                setState("Error", "Driver start failed ($r). Unplug and replug the Tonverk.", false)
            }
        }
    }

    private fun stopAll() {
        cleanupSession()
        active = false
        setState("Ready", "Stopped. Tap \"Start\" to stream again.", false)
    }

    /** USB-Verbindung + nativen Treiber + Capture-Service sauber beenden. */
    private fun cleanupSession() {
        startService(Intent(this, AudioBridgeService::class.java).apply { action = AudioBridgeService.ACTION_STOP })
        teardown()
    }

    private fun teardown() {
        try { NativeUsb.stop() } catch (_: Throwable) {}
        try { usbConn?.close() } catch (_: Exception) {}
        usbConn = null
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

    private fun findTonverk(um: UsbManager): UsbDevice? =
        um.deviceList.values.firstOrNull {
            it.productName?.contains("Tonverk", true) == true ||
            it.productName?.contains("Elektron", true) == true
        } ?: um.deviceList.values.firstOrNull()

    private fun ensureUsbPermission(onReady: (UsbManager, UsbDevice) -> Unit) {
        val um = getSystemService(UsbManager::class.java)
        val dev = findTonverk(um)
        if (dev == null) { starting = false; setState("Plug in your Tonverk", "No USB device found.", false); return }
        if (um.hasPermission(dev)) { onReady(um, dev); return }

        onUsbReady = onReady
        if (usbReceiver == null) {
            usbReceiver = object : BroadcastReceiver() {
                override fun onReceive(c: Context, i: Intent) {
                    if (i.action == usbPermAction) {
                        if (i.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) onUsbReady?.invoke(um, dev)
                        else { starting = false; setState("Ready", "USB permission denied.", false) }
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
