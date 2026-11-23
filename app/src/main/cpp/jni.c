/* JNI ブリッジ: Kotlin <-> 自作フォルマント合成エンジン */
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include "yukkuri.h"

#define YK_MAX_SAMPLES (YK_SR * 30)   /* 最大 30 秒 */

static jfloatArray synth(JNIEnv *env, jstring jkana, jstring jpreset,
                         jint presetIndex, jobjectArray jreadingOut)
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
    int n = yk_speak_kana(kana, &ps, buf, YK_MAX_SAMPLES, reading, (int)sizeof(reading));
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
                                                    jobjectArray readingOut)
{
    (void)cls;
    return synth(env, kana, preset, presetIndex, readingOut);
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
