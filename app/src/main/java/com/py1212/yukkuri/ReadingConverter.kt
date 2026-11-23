package com.py1212.yukkuri

import java.io.File

/**
 * 日本語テキスト -> カタカナ読み。
 *
 * 方式:
 *  1. 数字・記号・英字を読みに展開する (前処理)
 *  2. 辞書 (IPADIC から抽出した 表層形->読み, 8MB) を最長一致で適用する
 *  3. かなはそのまま通す
 *
 * 形態素解析 (Viterbi) は行わないが、「東京駅」のような複合語も
 * 最長一致で拾えるため、TTS 用途では十分な精度になる。
 */
class ReadingConverter(private val assetDir: File) {

    private lateinit var data: ByteArray
    private var count: Int = 0
    private var indexOff: Int = 0
    private var poolOff: Int = 0

    fun load() {
        if (::data.isInitialized) return
        data = File(assetDir, "reading_dict.bin").readBytes()
        require(data.size > 12 && String(data, 0, 4, Charsets.US_ASCII) == "YKR1") {
            "reading_dict.bin の形式が不正です"
        }
        count = readInt(4)
        indexOff = 12
        poolOff = indexOff + count * 8
    }

    private fun readInt(off: Int): Int =
        (data[off].toInt() and 0xFF) or
            ((data[off + 1].toInt() and 0xFF) shl 8) or
            ((data[off + 2].toInt() and 0xFF) shl 16) or
            ((data[off + 3].toInt() and 0xFF) shl 24)

    private fun readShort(off: Int): Int =
        (data[off].toInt() and 0xFF) or ((data[off + 1].toInt() and 0xFF) shl 8)

    /** 辞書引き (バイト列の二分探索) */
    private fun lookup(key: String): String? {
        val kb = key.toByteArray(Charsets.UTF_8)
        var lo = 0
        var hi = count - 1
        while (lo <= hi) {
            val mid = (lo + hi) ushr 1
            val o = indexOff + mid * 8
            val off = readInt(o)
            val kl = readShort(o + 4)
            val vl = readShort(o + 6)
            val kStart = poolOff + off
            val cmp = compareBytes(data, kStart, kl, kb)
            if (cmp == 0) {
                return String(data, kStart + kl, vl, Charsets.UTF_8)
            }
            if (cmp > 0) hi = mid - 1 else lo = mid + 1
        }
        return null
    }

    private fun compareBytes(a: ByteArray, aOff: Int, aLen: Int, b: ByteArray): Int {
        val n = minOf(aLen, b.size)
        for (i in 0 until n) {
            val x = a[aOff + i].toInt() and 0xFF
            val y = b[i].toInt() and 0xFF
            if (x != y) return x - y
        }
        return aLen - b.size
    }

    /** 漢字かな交じり文 -> カタカナ */
    fun toKatakana(text: String): String {
        load()
        val sb = StringBuilder(text.length * 2)
        var i = 0
        while (i < text.length) {
            val c = text[i]
            // 辞書最長一致 (最大 12 文字まで)
            var matched: String? = null
            var matchedLen = 0
            var len = minOf(12, text.length - i)
            while (len >= 2) {
                val cand = text.substring(i, i + len)
                if (hasDictionaryChar(cand)) {
                    val r = lookup(cand)
                    if (r != null) { matched = r; matchedLen = len; break }
                }
                len--
            }
            if (matched != null) {
                sb.append(matched)
                i += matchedLen
                continue
            }
            when {
                isKanji(c) -> {
                    // 辞書に無い漢字は読みを捨てて「ン」にするより、無視せず
                    // カタカナの候補を 1 文字ずつ探す
                    val r = lookup(c.toString())
                    if (r != null) sb.append(r) else sb.append('ッ')
                    i++
                }
                c in 'ぁ'..'ん' || c == 'ー' -> {   // ひらがな -> カタカナ
                    sb.append(if (c == 'ー') c else (c.code + 0x60).toChar())
                    i++
                }
                c in 'ァ'..'ヶ' || c == 'ー' -> { sb.append(c); i++ }
                c in '0'..'9' -> { sb.append(readDigits(text, i)); i += digitRun(text, i) }
                c == '％' || c == '%' -> { sb.append("パーセント"); i++ }
                c == '。' || c == '、' || c == '！' || c == '？' || c == '!' || c == '?' ||
                    c == ',' || c == '.' || c == '　' || c == ' ' -> { sb.append('、'); i++ }
                else -> i++   // 記号・絵文字は読み上げない
            }
        }
        return sb.toString()
    }

    private fun hasDictionaryChar(s: String): Boolean = s.any { isKanji(it) || it in 'ァ'..'ヶ' }

    private fun isKanji(c: Char): Boolean =
        c in '\u4E00'..'\u9FFF' || c == '々' || c == '〆' || c == 'ヶ' || c in '\u3400'..'\u4DBF'

    private fun digitRun(s: String, start: Int): Int {
        var i = start
        while (i < s.length && (s[i] in '0'..'9' || s[i] == ',' || s[i] == '.')) i++
        return i - start
    }

    /** 数字列を読み上げ用カタカナにする (例: 1234 -> センニヒャクサンジュウヨン) */
    private fun readDigits(s: String, start: Int): String {
        val run = StringBuilder()
        var i = start
        var dot = false
        while (i < s.length) {
            val c = s[i]
            if (c in '0'..'9') run.append(c)
            else if (c == '.' && !dot) { dot = true; run.append('.') }
            else if (c == ',') { /* 桁区切りは無視 */ }
            else break
            i++
        }
        val str = run.toString()
        if (str.isEmpty()) return ""
        val sb = StringBuilder()
        val parts = str.split('.')
        sb.append(readInteger(parts[0]))
        if (parts.size > 1) {
            sb.append("テン")
            for (ch in parts[1]) sb.append(DIGITS[ch - '0'])
        }
        return sb.toString()
    }

    private fun readInteger(num: String): String {
        val digits = num.trimStart('0')
        if (digits.isEmpty()) return "ゼロ"
        if (digits.length > 12) {   // 長すぎる数は 1 桁ずつ
            val sb = StringBuilder()
            for (c in digits) sb.append(DIGITS[c - '0'])
            return sb.toString()
        }
        val sb = StringBuilder()
        val units = arrayOf("", "マン", "オク", "チョウ")
        var d = digits
        // 4 桁ごとに区切る
        val groups = ArrayList<String>()
        while (d.isNotEmpty()) {
            val cut = maxOf(0, d.length - 4)
            groups.add(d.substring(cut))
            d = d.substring(0, cut)
        }
        for (gi in groups.indices.reversed()) {
            val g = groups[gi]
            val s = read4(g)
            if (s.isNotEmpty()) {
                sb.append(s)
                sb.append(units[gi])
            }
        }
        return sb.toString()
    }

    private fun read4(g: String): String {
        val sb = StringBuilder()
        val pads = Array(4) { '0' }
        val s = String(pads) + g
        val four = s.takeLast(4)
        val names = arrayOf("セン", "ヒャク", "ジュウ", "")
        for (i in 0 until 4) {
            val d = four[i] - '0'
            if (d == 0) continue
            if (!(d == 1 && i > 0)) sb.append(DIGITS[d])
            sb.append(names[i])
        }
        return sb.toString()
    }

    companion object {
        private val DIGITS = arrayOf("ゼロ", "イチ", "ニ", "サン", "ヨン", "ゴ", "ロク", "ナナ", "ハチ", "キュウ")
    }
}
