package com.py1212.yukkuri

import android.Manifest
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.media.MediaScannerConnection
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.textfield.TextInputEditText
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

class MainActivity : AppCompatActivity() {

    private lateinit var inputText: TextInputEditText
    private lateinit var readingText: TextView
    private lateinit var infoText: TextView
    private lateinit var presetSpinner: Spinner
    private lateinit var speakButton: Button
    private lateinit var stopButton: Button
    private lateinit var saveButton: Button

    private lateinit var converter: ReadingConverter
    private var player: AudioPlayer? = null
    private var lastSamples: FloatArray? = null
    private var lastReading: String = ""

    private val presetNames: Array<String> by lazy {
        Array(NativeEngine.presetCount) { NativeEngine.presetName(it) }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        inputText = findViewById(R.id.inputText)
        readingText = findViewById(R.id.readingText)
        infoText = findViewById(R.id.infoText)
        presetSpinner = findViewById(R.id.presetSpinner)
        speakButton = findViewById(R.id.speakButton)
        stopButton = findViewById(R.id.stopButton)
        saveButton = findViewById(R.id.saveButton)

        converter = ReadingConverter(File(applicationInfo.sourceDir))

        val labels = (0 until NativeEngine.presetCount).map { NativeEngine.presetLabel(it) }
        presetSpinner.adapter = ArrayAdapter(
            this, android.R.layout.simple_spinner_dropdown_item, labels
        )

        inputText.setText(getString(R.string.sample_text))

        speakButton.setOnClickListener { speak() }
        stopButton.setOnClickListener { stop() }
        saveButton.setOnClickListener { saveWav() }

        presetSpinner.onItemSelectedListener = object : AdapterViewListener() {
            override fun onSelected(position: Int) { updateInfo() }
        }
        updateInfo()
    }

    override fun onDestroy() {
        super.onDestroy()
        player?.release()
    }

    private fun updateInfo() {
        val i = presetSpinner.selectedItemPosition.coerceAtLeast(0)
        val name = presetNames.getOrElse(i) { "yukkuri" }
        infoText.text = "engine ${NativeEngine.version} / ${NativeEngine.sampleRate}Hz / preset=$name / " +
            "${NativeEngine.presetCount} voices"
    }

    private fun speak() {
        val text = inputText.text?.toString()?.trim().orEmpty()
        if (text.isEmpty()) {
            Toast.makeText(this, "文章を入力してください", Toast.LENGTH_SHORT).show()
            return
        }
        stop()
        speakButton.isEnabled = false
        speakButton.text = "合成中…"

        Thread {
            val kana = converter.toKatakana(text)
            val presetIndex = presetSpinner.selectedItemPosition.coerceAtLeast(0)
            val readingOut = arrayOf("")
            val samples = NativeEngine.synthesize(kana, presetIndex, readingOut)
            lastReading = readingOut[0]
            runOnUiThread {
                speakButton.isEnabled = true
                speakButton.text = getString(R.string.btn_speak)
                if (samples == null || samples.isEmpty()) {
                    Toast.makeText(this, "合成に失敗しました（かな・漢字を入れてください）",
                        Toast.LENGTH_SHORT).show()
                    return@runOnUiThread
                }
                lastSamples = samples
                readingText.visibility = View.VISIBLE
                readingText.text = getString(R.string.label_reading) + "：" + lastReading
                readingText.alpha = 0.55f
                play(samples)
            }
        }.start()
    }

    private fun play(samples: FloatArray) {
        player = AudioPlayer(samples, onDone = { done ->
            Handler(Looper.getMainLooper()).post {
                stopButton.isEnabled = false
                readingText.alpha = 0.9f
            }
        })
        stopButton.isEnabled = true
        player?.start()
    }

    private fun stop() {
        player?.stop()
        player?.release()
        player = null
        stopButton.isEnabled = false
    }

    // ---- WAV 保存 -------------------------------------------------------

    private fun saveWav() {
        val samples = lastSamples
        if (samples == null) {
            Toast.makeText(this, "先に「ゆっくり話す」で合成してください", Toast.LENGTH_SHORT).show()
            return
        }
        if (Build.VERSION.SDK_INT < 29 &&
            checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.WRITE_EXTERNAL_STORAGE), 100)
            return
        }
        try {
            val dir = if (Build.VERSION.SDK_INT >= 29)
                File(getExternalFilesDir(Environment.DIRECTORY_MUSIC), "")
            else
                File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_MUSIC), "Yukkuri")
            dir.mkdirs()
            val name = "yukkuri_${System.currentTimeMillis()}.wav"
            val file = File(dir, name)
            writeWav(file, samples, NativeEngine.sampleRate)
            MediaScannerConnection.scanFile(this, arrayOf(file.absolutePath), null, null)
            Toast.makeText(this, "保存しました: ${file.absolutePath}", Toast.LENGTH_LONG).show()
        } catch (e: Exception) {
            Toast.makeText(this, "保存に失敗: ${e.message}", Toast.LENGTH_LONG).show()
        }
    }

    private fun writeWav(file: File, samples: FloatArray, sr: Int) {
        val n = samples.size
        val buf = ByteBuffer.allocate(44 + n * 2).order(ByteOrder.LITTLE_ENDIAN)
        buf.put("RIFF".toByteArray())
        buf.putInt(36 + n * 2)
        buf.put("WAVEfmt ".toByteArray())
        buf.putInt(16)
        buf.putShort(1)
        buf.putShort(1)
        buf.putInt(sr)
        buf.putInt(sr * 2)
        buf.putShort(2)
        buf.putShort(16)
        buf.put("data".toByteArray())
        buf.putInt(n * 2)
        for (s in samples) {
            val v = (s.coerceIn(-1f, 1f) * 32767).toInt()
            buf.putShort(v.toShort())
        }
        FileOutputStream(file).use { it.write(buf.array()) }
    }

    // ---- 小さなユーティリティ -------------------------------------------

    /** Spinner の選択変更をラムダで受けるための薄いラッパー */
    private abstract class AdapterViewListener : android.widget.AdapterView.OnItemSelectedListener {
        abstract fun onSelected(position: Int)
        override fun onItemSelected(
            parent: android.widget.AdapterView<*>?, view: View?, position: Int, id: Long
        ) = onSelected(position)

        override fun onNothingSelected(parent: android.widget.AdapterView<*>?) {}
    }
}

/**
 * FloatArray を AudioTrack (ストリームモード) で再生する。
 * 16bit PCM に変換して書き込む。
 */
class AudioPlayer(
    private val samples: FloatArray,
    private val onDone: (Boolean) -> Unit
) {
    private var track: AudioTrack? = null
    @Volatile private var running = false
    private var thread: Thread? = null

    fun start() {
        val sr = NativeEngine.sampleRate
        val minBuf = AudioTrack.getMinBufferSize(
            sr, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT
        ).coerceAtLeast(sr / 4)

        val t = AudioTrack(
            AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                .build(),
            AudioFormat.Builder()
                .setSampleRate(sr)
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                .build(),
            minBuf,
            AudioTrack.MODE_STREAM,
            AudioManager.AUDIO_SESSION_ID_GENERATE
        )
        track = t
        t.play()
        running = true
        thread = Thread {
            val chunk = 1024
            val pcm = ShortArray(chunk)
            var i = 0
            while (running && i < samples.size) {
                val n = minOf(chunk, samples.size - i)
                for (j in 0 until n) {
                    pcm[j] = (samples[i + j].coerceIn(-1f, 1f) * 32767).toInt().toShort()
                }
                t.write(pcm, 0, n)
                i += n
            }
            if (running) {
                t.stop()
            }
            onDone(true)
        }.also { it.start() }
    }

    fun stop() {
        running = false
        try { track?.pause(); track?.flush() } catch (_: Exception) {}
    }

    fun release() {
        running = false
        try { track?.stop() } catch (_: Exception) {}
        try { track?.release() } catch (_: Exception) {}
        track = null
    }
}
