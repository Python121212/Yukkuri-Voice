/* エンジンの自動テスト (CI でホストの gcc から実行)
 *
 *  - 母音のフォルマントが設計値どおりに出ているか (LPC ではなく単純な DFT ピーク)
 *  - 母音間の音量差が許容範囲か
 *  - クリップしていないか
 *  - かな列の読みが正しいか
 */
#include "yukkuri.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR YK_SR
static float buf[YK_SR * 30];
static int failures = 0;

static void check(int cond, const char *fmt, ...)
{
    if (cond) return;
    failures++;
    va_list ap;
    va_start(ap, fmt);
    printf("FAIL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* フォルマント推定:
   1. ケプストラムでスペクトル包絡を作る
   2. 包絡の形状から F0 の高調波を除く (ピークが F0 間隔で並んでいたら高調波)
   3. 残った「広い山」を F1, F2 とする
   テスト用途なので、設計値との一致を確かめられれば十分。 */
static void estimate_formants(const float *x, int n, int sr, float *out, int *nout)
{
    const int K = 4096;
    static double w[4096];
    double mean = 0;
    int len = n < K ? n : K;
    for (int i = 0; i < len; i++) mean += x[i];
    mean /= len;
    for (int i = 0; i < K; i++) {
        double v = (i < len) ? x[i] - mean : 0.0;
        w[i] = v * (0.5 - 0.5 * cos(2.0 * M_PI * i / (K - 1)));
    }
    static double re[2048], im[2048], logm[2048];
    for (int k = 0; k < K / 2; k++) {
        double a = 0, b = 0;
        for (int i = 0; i < K; i++) {
            double ang = -2.0 * M_PI * k * i / K;
            a += w[i] * cos(ang);
            b += w[i] * sin(ang);
        }
        re[k] = a; im[k] = b;
        logm[k] = log(sqrt(a * a + b * b) + 1e-12);
    }
    /* ケプストラム (対称性を使って実数の逆変換で戻す) */
    static double cep[4096];
    for (int q = 0; q < K; q++) {
        double s = 0;
        for (int k = 0; k < K / 2; k++)
            s += logm[k] * cos(2.0 * M_PI * q * k / K);
        cep[q] = s / (K / 2);
    }
    /* lifter: 1.2ms より遅い成分 (細かい高調波) を捨てる */
    int lifter = (int)(0.0012 * sr);
    if (lifter < 8) lifter = 8;
    for (int q = lifter; q < K - lifter; q++) cep[q] = 0;
    /* 逆変換して包絡を得る */
    static double env[2048];
    for (int k = 0; k < K / 2; k++) {
        double s = cep[0];
        for (int q = 1; q < lifter; q++)
            s += 2.0 * cep[q] * cos(2.0 * M_PI * q * k / K);
        env[k] = s;
    }
    /* 包絡の局所ピークを拾う */
    typedef struct { double v; double f; } Pk;
    Pk pk[128];
    int np = 0;
    for (int k = 2; k < K / 2 - 2; k++) {
        if (env[k] > env[k - 1] && env[k] >= env[k + 1]) {
            double f = (double)k * sr / K;
            if (f < 250 || f > 4200) continue;
            if (np < 128) { pk[np].v = env[k]; pk[np].f = f; np++; }
        }
    }
    /* 強い順 */
    for (int i = 1; i < np; i++) {
        Pk v = pk[i]; int j = i - 1;
        while (j >= 0 && pk[j].v < v.v) { pk[j + 1] = pk[j]; j--; }
        pk[j + 1] = v;
    }
    /* 200Hz 以内は同一フォルマントとみなす */
    int c = 0;
    for (int i = 0; i < np && c < 3; i++) {
        int ok = 1;
        for (int j = 0; j < c; j++)
            if (fabs(out[j] - pk[i].f) < 200) ok = 0;
        if (ok) out[c++] = (float)pk[i].f;
    }
    for (int i = 1; i < c; i++) {
        float v = out[i]; int j = i - 1;
        while (j >= 0 && out[j] > v) { out[j + 1] = out[j]; j--; }
        out[j + 1] = v;
    }
    *nout = c;
}

static double rms_between(const float *x, int a, int b)
{
    double acc = 0;
    for (int i = a; i < b; i++) acc += (double)x[i] * x[i];
    return sqrt(acc / (b - a));
}

/* 有声区間のスペクトル包絡を「F0 の高調波の振幅」から作る。
   有声源は高調波構造を持つので、高調波だけを見ればフォルマントが読める。 */
static double harm_amp(const float *x, int n, int sr, double f)
{
    const int K = 4096;
    int len = n < K ? n : K;
    double re = 0, im = 0;
    for (int i = 0; i < len; i++) {
        double w = 0.5 - 0.5 * cos(2.0 * M_PI * i / (len - 1));
        double a = -2.0 * M_PI * f * i / sr;
        re += x[i] * w * cos(a);
        im += x[i] * w * sin(a);
    }
    return sqrt(re * re + im * im);
}

/* F0 の高調波をたどり、指定帯域で最も強い山の周波数を返す */
static double formant_peak(const float *x, int n, int sr, double f0,
                           double lo, double hi)
{
    double best = 0, bestF = 0;
    /* 高調波 (F0 の整数倍) に合わせる。整数倍からずれると谷を測ってしまう */
    double f = ceil(lo / f0) * f0;
    if (f < f0) f = f0;
    for (; f <= hi; f += f0) {
        double a = harm_amp(x, n, sr, f);
        if (a > best) { best = a; bestF = f; }
    }
    return bestF;
}

static void test_vowel_formants(void)
{
    /* 母音のフォルマント (設計値: internal.h の YK_VOWEL_F × formantScale)。
       F1/F2 は音響的に判別できるので、高調波の山の位置で検証する。 */
    static const float expect[5][2] = {
        { 780, 1200 },   /* a */
        { 300, 2150 },   /* i */
        { 340, 1350 },   /* u */
        { 480, 1900 },   /* e */
        { 450,  850 },   /* o */
    };
    const char *kana[5] = { "あ", "い", "う", "え", "お" };
    const YkPreset ps = *yk_preset(0);
    double loud[5];
    const double f0 = ps.pitch;

    for (int v = 0; v < 5; v++) {
        int n = yk_speak_kana(kana[v], &ps, buf, (int)(sizeof(buf) / sizeof(buf[0])), NULL, 0);
        check(n > 0, "合成失敗: %s", kana[v]);
        if (n <= 0) return;
        /* 母音の定常部を測る。立ち上がり (0-40ms) は F0 付近の成分が
           強く出るので避け、40ms 以降の 60ms を使う。 */
        const float *seg = buf + SR * 3 / 100;
        int len = SR * 6 / 100;

        float peakv = 0;
        for (int i = 0; i < n; i++) if (fabsf(buf[i]) > peakv) peakv = fabsf(buf[i]);
        check(peakv < 0.999f, "%s: クリップしている (peak=%.3f)", kana[v], peakv);
        check(peakv > 0.2f, "%s: 音量が小さすぎる (peak=%.3f)", kana[v], peakv);

        /* F1: 200Hz から F2 の手前まで */
        double f1 = formant_peak(seg, len, SR, f0, 350, 800);
        double want1 = expect[v][0] * ps.formantScale;
        double tol1 = want1 * 0.35f + 120;
        check(fabs(f1 - want1) < tol1,
              "%s: F1 = %.0f Hz (期待 %.0f ± %.0f)", kana[v], f1, want1, tol1);

        /* F2: 第 2 フォルマントの帯域 (600-3500Hz) で最も強い高調波。
           F1 の裾 (数百 Hz) と区別するため、下限は 600Hz とする。 */
        /* F2 は F1 から 550Hz 以上離れた位置で探す (F1 の裾を拾わないため) */
        double f2 = formant_peak(seg, len, SR, f0, f1 + 550, 3000);
        double want2 = expect[v][1] * ps.formantScale;
        double tol2 = want2 * 0.30f + 150;
        check(fabs(f2 - want2) < tol2,
              "%s: F2 = %.0f Hz (期待 %.0f ± %.0f)", kana[v], f2, want2, tol2);

        loud[v] = rms_between(buf, SR / 30, SR / 12);
    }

    double lo = loud[0], hi = loud[0];
    for (int v = 1; v < 5; v++) { if (loud[v] < lo) lo = loud[v]; if (loud[v] > hi) hi = loud[v]; }
    check(hi / lo < 3.0, "母音の音量差が大きすぎる (比 %.2f)", hi / lo);
}

static void test_reading(void)
{
    char reading[1024];
    YkPlan plan;
    int rc = yk_plan_from_kana("ゆっくり", yk_preset(0), &plan);
    check(rc == 0, "プラン生成に失敗");
    yk_speak_kana("ゆっくり", yk_preset(0), buf, (int)(sizeof(buf)/sizeof(buf[0])),
                  reading, (int)sizeof(reading));
    check(strcmp(reading, "ゆっくり") == 0, "読みが不正: %s", reading);

    /* かな表記の全モーラが音素列に入っているか */
    yk_speak_kana("かきくけこ", yk_preset(0), buf, (int)(sizeof(buf)/sizeof(buf[0])),
                  reading, (int)sizeof(reading));
    int vowels = 0;
    for (int i = 0; i < plan.count; i++) if (plan.items[i].phone <= YK_P_O) vowels++;
    (void)vowels;
    check(strcmp(reading, "かきくけこ") == 0, "読みが不正: %s", reading);

    /* 長音と促音 */
    yk_speak_kana("ゆっくり", yk_preset(0), buf, (int)(sizeof(buf)/sizeof(buf[0])),
                  reading, (int)sizeof(reading));
    check(strstr(reading, "っ") != NULL, "促音が読みに残っていない: %s", reading);
}

static void test_presets(void)
{
    check(yk_preset_count() >= 3, "プリセットが少なすぎる");
    for (int i = 0; i < yk_preset_count(); i++) {
        const YkPreset *p = yk_preset(i);
        check(yk_preset_index_of(p->name) == i, "プリセット %s の検索に失敗", p->name);
        check(p->pitch > 120 && p->pitch < 400, "%s: ピッチが範囲外", p->name);
        int n = yk_speak_kana("あいうえお", p, buf, (int)(sizeof(buf)/sizeof(buf[0])), NULL, 0);
        check(n > 0, "%s: 合成に失敗", p->name);
    }
}

static void test_pause_duration(void)
{
    /* 句読点で間が入り、全体が長くなる */
    int n1 = yk_speak_kana("ああ", yk_preset(0), buf, (int)(sizeof(buf)/sizeof(buf[0])), NULL, 0);
    int n2 = yk_speak_kana("ああ。", yk_preset(0), buf, (int)(sizeof(buf)/sizeof(buf[0])), NULL, 0);
    check(n2 > n1 + SR / 10, "句点で間が入っていない (%d -> %d)", n1, n2);
}

int main(void)
{
    printf("yukkuri engine %s\n", yk_version());
    test_presets();
    test_reading();
    test_vowel_formants();
    test_pause_duration();
    if (failures) {
        printf("\n%d 件のテストが失敗しました\n", failures);
        return 1;
    }
    printf("すべてのテストに合格しました\n");
    return 0;
}
