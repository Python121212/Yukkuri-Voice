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
        nativeSynth(kana, "", presetIndex, readingOut)

    /** プリセット名で合成する。 */
    fun synthesize(kana: String, preset: String, readingOut: Array<String>? = null): FloatArray? =
        nativeSynth(kana, preset, -1, readingOut)

    external fun nativeSynth(
        kana: String, preset: String, presetIndex: Int, readingOut: Array<String>?
    ): FloatArray?

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
