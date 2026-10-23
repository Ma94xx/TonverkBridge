package com.tonverk.bridge

import android.content.ContentResolver
import android.content.ContentValues
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import android.provider.MediaStore
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Schreibt eine WAV-Datei mit den rohen Capture-Bytes (gepackt wie vom USB-Gerät,
 * z. B. 24-Bit-LE interleaved – das ist exakt das WAV-Datenformat, keine Wandlung nötig).
 *
 * Ziel ist wahlweise ein vom Nutzer gewählter SAF-Ordner (ACTION_OPEN_DOCUMENT_TREE)
 * oder per Default MediaStore unter Music/Tonverk Bridge. Der Header wird periodisch
 * nachgepatcht, damit die Datei auch nach einem Absturz abspielbar bleibt.
 */
class WavRecorder(private val resolver: ContentResolver) {

    companion object {
        const val DEFAULT_DIR = "Music/Tonverk Bridge"

        /** Klassischer 44-Byte-PCM-Header; reine Funktion (testbar). */
        fun header(channels: Int, rate: Int, bytesPerSample: Int, dataBytes: Long): ByteArray {
            // RIFF adressiert max. 4 GB; darüber hinaus bleibt der Header beim
            // Maximum stehen statt überzulaufen (die Bytes selbst sind auf der Platte).
            val data = dataBytes.coerceAtMost(0xFFFF_FFFFL - 44)
            val blockAlign = channels * bytesPerSample
            val b = ByteBuffer.allocate(44).order(ByteOrder.LITTLE_ENDIAN)
            b.put("RIFF".toByteArray(Charsets.US_ASCII))
            b.putInt((36 + data).toInt())
            b.put("WAVE".toByteArray(Charsets.US_ASCII))
            b.put("fmt ".toByteArray(Charsets.US_ASCII))
            b.putInt(16)                              // fmt-Chunk-Größe
            b.putShort(1)                             // PCM
            b.putShort(channels.toShort())
            b.putInt(rate)
            b.putInt(rate * blockAlign)               // Byte-Rate
            b.putShort(blockAlign.toShort())
            b.putShort((bytesPerSample * 8).toShort())
            b.put("data".toByteArray(Charsets.US_ASCII))
            b.putInt(data.toInt())
            return b.array()
        }

        /** Verbotene Dateinamen-Zeichen raus; leer -> null (Aufrufer nimmt Default). */
        fun sanitizeName(raw: String?): String? {
            val s = (raw ?: "").replace(Regex("[\\\\/:*?\"<>|]"), "-").trim()
            return s.ifEmpty { null }
        }
    }

    private var uri: Uri? = null
    private var pfd: ParcelFileDescriptor? = null
    private var out: FileOutputStream? = null
    private var isMediaStore = false
    private var channels = 2
    private var rate = 48_000
    private var bytesPerSample = 3

    var dataBytes = 0L; private set
    var label = ""; private set            // Anzeigename inkl. Ort, für die UI

    /**
     * Legt die Datei an und schreibt den (vorläufigen) Header.
     * treeUri = vom Nutzer gewählter SAF-Ordner oder null für MediaStore-Default.
     */
    fun start(channels: Int, rate: Int, bytesPerSample: Int, name: String, treeUri: Uri?): Boolean {
        this.channels = channels; this.rate = rate; this.bytesPerSample = bytesPerSample
        dataBytes = 0

        if (treeUri != null && startSaf(treeUri, name)) return true
        if (treeUri != null) Diag.log("REC: chosen folder not writable – falling back to $DEFAULT_DIR")
        return startMediaStore(name)
    }

    private fun startSaf(treeUri: Uri, name: String): Boolean = try {
        val parent = DocumentsContract.buildDocumentUriUsingTree(
            treeUri, DocumentsContract.getTreeDocumentId(treeUri)
        )
        val u = DocumentsContract.createDocument(resolver, parent, "audio/x-wav", name)
            ?: throw IllegalStateException("createDocument returned null")
        openAndWriteHeader(u)
        // Seek-Probe: Provider ohne Seek (manche Cloud-Ordner) könnten den Header
        // später nie patchen -> Datei wäre als 0 Samples getaggt. Dann lieber
        // gleich scheitern und auf MediaStore zurückfallen.
        patchHeader()
        isMediaStore = false
        label = name
        true
    } catch (e: Exception) {
        Diag.log("REC: SAF create failed: ${e.message}")
        discardOpenFile(deleteDoc = true)
        false
    }

    private fun startMediaStore(name: String): Boolean = try {
        val cv = ContentValues().apply {
            put(MediaStore.Audio.Media.DISPLAY_NAME, name)
            put(MediaStore.Audio.Media.MIME_TYPE, "audio/x-wav")
            put(MediaStore.Audio.Media.RELATIVE_PATH, DEFAULT_DIR)
            put(MediaStore.Audio.Media.IS_PENDING, 1)
        }
        val u = resolver.insert(
            MediaStore.Audio.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY), cv
        ) ?: throw IllegalStateException("MediaStore insert returned null")
        openAndWriteHeader(u)
        isMediaStore = true
        label = "$DEFAULT_DIR/$name"
        true
    } catch (e: Exception) {
        Diag.log("REC: MediaStore create failed: ${e.message}")
        discardOpenFile(deleteDoc = false)   // Pending-Einträge räumt das System selbst ab
        false
    }

    /** Räumt eine halb angelegte Datei weg (nur im Fehlerfall von start()). */
    private fun discardOpenFile(deleteDoc: Boolean) {
        try { out?.close() } catch (_: Exception) {}
        try { pfd?.close() } catch (_: Exception) {}
        out = null; pfd = null
        val u = uri; uri = null
        if (u != null && deleteDoc) {
            try { DocumentsContract.deleteDocument(resolver, u) } catch (_: Exception) {}
        }
    }

    private fun openAndWriteHeader(u: Uri) {
        val p = resolver.openFileDescriptor(u, "rw")
            ?: throw IllegalStateException("openFileDescriptor returned null")
        uri = u; pfd = p
        out = FileOutputStream(p.fileDescriptor).also {
            it.write(header(channels, rate, bytesPerSample, 0))
        }
    }

    fun write(buf: ByteArray, n: Int) {
        out?.write(buf, 0, n)
        dataBytes += n
    }

    /** Header mit aktuellen Größen überschreiben; Schreibposition bleibt am Dateiende. */
    fun patchHeader() {
        val o = out ?: return
        o.flush()
        val ch = o.channel
        val end = ch.position()
        ch.position(0)
        ch.write(ByteBuffer.wrap(header(channels, rate, bytesPerSample, dataBytes)))
        ch.position(end)
    }

    /** Finalisiert die Datei; gibt den Anzeigenamen zurück (null wenn nichts offen war). */
    fun finish(): String? {
        val hadFile = out != null
        try { patchHeader() } catch (e: Exception) { Diag.log("REC: header patch failed: ${e.message}") }
        try { out?.close() } catch (_: Exception) {}
        try { pfd?.close() } catch (_: Exception) {}
        out = null; pfd = null
        val u = uri; uri = null
        if (u != null && isMediaStore) {
            try {
                resolver.update(u, ContentValues().apply { put(MediaStore.Audio.Media.IS_PENDING, 0) }, null, null)
            } catch (e: Exception) { Diag.log("REC: clear pending failed: ${e.message}") }
        }
        return if (hadFile) label else null
    }
}
