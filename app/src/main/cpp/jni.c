/* JNI ブリッジ: Kotlin <-> 自作フォルマント合成エンジン */
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include "yukkuri.h"

#define YK_MAX_SAMPLES (YK_SR * 30)   /* 最大 30 秒 */

static jfloatArray synth(JNIEnv *env, jstring jkana, jstring jpreset,
                         jint presetIndex, jobjectArray jreadingOut, jint useSymbols)
{
    YkPreset ps;
    if (presetIndex >= 0) {
        const YkPreset *p = yk_preset(presetIndex);
        ps = *p;
    } else {
        /* Kotlin 側から名前で指定された場合 */
        const char *name = jpreset ? (*env)->GetStringUTFChars(env, jpreset, NULL) : NULL;
        int idx = name ? yk_preset_index_of(name) : -1;
        if (name) (*env)->ReleaseStringUTFChars(env, jpreset, name);
        if (idx < 0) idx = 0;
        ps = *yk_preset(idx);
    }

    const char *kana = (*env)->GetStringUTFChars(env, jkana, NULL);
    if (!kana) return NULL;

    float *buf = (float *)malloc(sizeof(float) * YK_MAX_SAMPLES);
    if (!buf) { (*env)->ReleaseStringUTFChars(env, jkana, kana); return NULL; }

    char reading[1024];
    int n = useSymbols
          ? yk_speak_symbols(kana, &ps, buf, YK_MAX_SAMPLES, reading, (int)sizeof(reading))
          : yk_speak_kana(kana, &ps, buf, YK_MAX_SAMPLES, reading, (int)sizeof(reading));
    (*env)->ReleaseStringUTFChars(env, jkana, kana);
    if (n <= 0) { free(buf); return NULL; }

    /* 読みを返す */
    if (jreadingOut) {
        jstring jr = (*env)->NewStringUTF(env, reading);
        (*env)->SetObjectArrayElement(env, jreadingOut, 0, jr);
        (*env)->DeleteLocalRef(env, jr);
    }

    jfloatArray out = (*env)->NewFloatArray(env, n);
    if (out) (*env)->SetFloatArrayRegion(env, out, 0, n, buf);
    free(buf);
    return out;
}

JNIEXPORT jfloatArray JNICALL
Java_com_py1212_yukkuri_NativeEngine_nativeSynth(JNIEnv *env, jclass cls,
                                                    jstring kana, jstring preset,
                                                    jint presetIndex,
                                                    jobjectArray readingOut, jint useSymbols)
{
    (void)cls;
    return synth(env, kana, preset, presetIndex, readingOut, useSymbols);
}

/* かな列を音素記号列に自動変換する (UI の「自動変換」表示用)。 */
JNIEXPORT jstring JNICALL
Java_com_py1212_yukkuri_NativeEngine_nativeKanaToSymbols(JNIEnv *env, jclass cls, jstring jkana)
{
    (void)cls;
    if (!jkana) return NULL;
    const char *kana = (*env)->GetStringUTFChars(env, jkana, NULL);
    if (!kana) return NULL;
    char out[2048];
    int n = yk_kana_to_symbols(kana, out, (int)sizeof(out));
    (*env)->ReleaseStringUTFChars(env, jkana, kana);
    if (n <= 0) out[0] = '\0';
    return (*env)->NewStringUTF(env, out);
}

/* 音素記号の一覧 (UI のヘルプ表示用)。 */
JNIEXPORT jstring JNICALL
Java_com_py1212_yukkuri_NativeEngine_nativePhoneSymbol(JNIEnv *env, jclass cls, jint i)
{
    (void)cls;
    if (i < 0 || i >= YK_P_COUNT) return NULL;
    return (*env)->NewStringUTF(env, yk_phone_symbol((YkPhone)i));
}

JNIEXPORT jint JNICALL
Java_com_py1212_yukkuri_NativeEngine_nativePhoneCount(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return YK_P_COUNT;
}

JNIEXPORT jstring JNICALL
Java_com_py1212_yukkuri_NativeEngine_nativeVersion(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, yk_version());
}

JNIEXPORT jint JNICALL
Java_com_py1212_yukkuri_NativeEngine_nativeSampleRate(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return YK_SR;
}

JNIEXPORT jint JNICALL
Java_com_py1212_yukkuri_NativeEngine_nativePresetCount(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return yk_preset_count();
}

JNIEXPORT jstring JNICALL
Java_com_py1212_yukkuri_NativeEngine_nativePresetName(JNIEnv *env, jclass cls, jint i)
{
    (void)cls;
    return (*env)->NewStringUTF(env, yk_preset(i)->name);
}

JNIEXPORT jstring JNICALL
Java_com_py1212_yukkuri_NativeEngine_nativePresetLabel(JNIEnv *env, jclass cls, jint i)
{
    (void)cls;
    return (*env)->NewStringUTF(env, yk_preset(i)->label);
}
