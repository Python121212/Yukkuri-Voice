package com.py1212.yukkuri

import android.content.res.AssetManager
import java.io.File
import java.io.InputStream

/** ReadingConverter に辞書データを渡すための Android 側ヘルパー。 */
object ReadingAssets {

    /** APK の assets/reading_dict.bin から読み込む */
    fun fromAssets(assets: AssetManager): ReadingConverter =
        ReadingConverter { assets.open("reading_dict.bin").use(InputStream::readBytes) }

    /** 開発用: ローカルのファイルから読み込む */
    fun fromFile(file: File): ReadingConverter =
        ReadingConverter { file.readBytes() }
}
