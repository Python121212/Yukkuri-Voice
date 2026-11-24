package com.py1212.yukkuri

/**
 * ネイティブ合成エンジン (libyukkuri.so) への薄いラッパー。
 * エンジン本体はリポジトリ直下の core/ (C99) にあり、Android に依存しない。
 */
object NativeEngine {

    init {
        System.loadLibrary("yukkuri")
    }

    /** かな文字列を合成し、-1..1 のモノラル波形を返す。失敗時は null。 */
    fun synthesize(kana: String, presetIndex: Int, readingOut: Array<String>? = null): FloatArray? =
        nativeSynth(kana, "", presetIndex, readingOut, 0)

    /** プリセット名で合成する。 */
    fun synthesize(kana: String, preset: String, readingOut: Array<String>? = null): FloatArray? =
        nativeSynth(kana, preset, -1, readingOut, 0)

    /**
     * 音素記号列から合成する。ローマ字・かな・記号を混在で受け付ける
     * (例: "yuQkuri shite iQte ne")。日本語の異音規則は自動で当たる。
     */
    fun synthesizeSymbols(text: String, presetIndex: Int,
                          readingOut: Array<String>? = null): FloatArray? =
        nativeSynth(text, "", presetIndex, readingOut, 1)

    /** かな列を音素記号列に自動変換する (UI 表示用)。失敗時は空文字。 */
    fun kanaToSymbols(kana: String): String = nativeKanaToSymbols(kana) ?: ""

    external fun nativeSynth(
        kana: String, preset: String, presetIndex: Int, readingOut: Array<String>?, useSymbols: Int
    ): FloatArray?

    external fun nativeKanaToSymbols(kana: String): String
    external fun nativePhoneSymbol(i: Int): String
    external fun nativePhoneCount(): Int

    external fun nativeVersion(): String
    external fun nativeSampleRate(): Int
    external fun nativePresetCount(): Int
    external fun nativePresetName(i: Int): String
    external fun nativePresetLabel(i: Int): String

    val version: String get() = nativeVersion()
    val sampleRate: Int get() = nativeSampleRate()
    val presetCount: Int get() = nativePresetCount()
    fun presetName(i: Int): String = nativePresetName(i)
    fun presetLabel(i: Int): String = nativePresetLabel(i)
}
