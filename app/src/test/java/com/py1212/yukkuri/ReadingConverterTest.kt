package com.py1212.yukkuri

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * 読み変換のテスト。CI では kuromoji から生成した辞書を使って実行する。
 * (辞書がない環境では skip)
 */
class ReadingConverterTest {

    private fun converter(): ReadingConverter? {
        val f = File("src/main/assets/reading_dict.bin")
        if (!f.exists()) return null
        return ReadingConverter { f.readBytes() }
    }

    private fun conv(text: String): String? = converter()?.toKatakana(text)

    @Test
    fun katakanaPassthrough() {
        val c = conv("ユックリ") ?: return
        assertEquals("ユックリ", c)
    }

    @Test
    fun hiraganaToKatakana() {
        val c = conv("ゆっくり") ?: return
        assertEquals("ユックリ", c)
    }

    @Test
    fun kanjiWord() {
        val c = conv("東京") ?: return
        assertEquals("トウキョウ", c)
    }

    @Test
    fun compoundWord() {
        val c = conv("東京駅") ?: return
        assertTrue("東京駅 -> $c", c.startsWith("トウキョウ") && c.length > 5)
    }

    @Test
    fun numbers() {
        val c = conv("123") ?: return
        assertEquals("ヒャクニジュウサン", c)
    }

    @Test
    fun sentence() {
        val c = conv("ゆっくりしていってね！") ?: return
        assertEquals("ユックリシテイッテネ、", c)
    }
}
