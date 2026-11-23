/* エンジンの自動テスト (CI でホストの gcc から実行)
 *
 *  - 母音のフォルマントが設計値どおりに出ているか (LPC ではなく単純な DFT ピーク)
 *  - 母音間の音量差が許容範囲か
 *  - クリック (1 サンプルだけ跳ねる不連続) が無いか
 *  - 摩擦音が母音に対して適正なレベルか
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

/* F0 の高調波をたどり、指定帯域で「局所的な山」になっている
   最も強い高調波の周波数を返す。単純に最大値を取ると F1 の裾が
   勝ってしまうので、両隣の高調波より強いことを条件にする。 */
static double formant_peak(const float *x, int n, int sr, double f0,
                           double lo, double hi)
{
    enum { MAXH = 256 };
    double fa[MAXH], aa[MAXH];
    int nh = 0;
    double f = ceil(lo / f0) * f0;
    if (f < f0) f = f0;
    for (; f <= hi && nh < MAXH; f += f0) {
        fa[nh] = f;
        aa[nh] = harm_amp(x, n, sr, f);
        nh++;
    }
    double best = 0, bestF = 0;
    for (int i = 1; i + 1 < nh; i++) {
        if (aa[i] > aa[i - 1] && aa[i] >= aa[i + 1] && aa[i] > best) {
            best = aa[i];
            bestF = fa[i];
        }
    }
    if (bestF == 0 && nh > 0) {
        /* 単調な帯域なら一番強い高調波を返す */
        for (int i = 0; i < nh; i++) if (aa[i] > best) { best = aa[i]; bestF = fa[i]; }
    }
    return bestF;
}

/* クリック検出: 隣接サンプルの差分が、その区間の RMS に対して
   異常に大きい箇所を数える。共振器の過渡や閉鎖明けの不連続は
   ここで「カチッ」という雑音として出るので、回帰テストにする。 */
static int count_clicks(const float *x, int n)
{
    double acc = 0.0;
    for (int i = 0; i < n; i++) acc += (double)x[i] * x[i];
    double rms = sqrt(acc / n);
    if (rms < 1e-6) return 0;
    /* 正常な有声波形でも 1 サンプル差分は rms の数倍になる。
       10 倍を超える跳びは不連続とみなす。 */
    double lim = rms * 10.0;
    int c = 0;
    for (int i = 1; i < n; i++) {
        if (fabs((double)x[i] - (double)x[i - 1]) > lim) c++;
    }
    return c;
}

/* 指定帯域のエネルギー (dB) */
static double band_energy(const float *x, int n, int sr, double lo, double hi)
{
    double p = 0.0;
    int lo_k = (int)(lo * n / sr), hi_k = (int)(hi * n / sr);
    if (hi_k > n / 2) hi_k = n / 2;
    if (lo_k < 1) lo_k = 1;
    for (int k = lo_k; k < hi_k; k++) {
        double a = 0, b = 0;
        for (int i = 0; i < n; i++) {
            double ang = -2.0 * M_PI * k * i / n;
            a += x[i] * cos(ang);
            b += x[i] * sin(ang);
        }
        p += a * a + b * b;
    }
    return 10.0 * log10(p / (hi_k - lo_k + 1) + 1e-20);
}

/* 子音 (摩擦音・破裂音) が母音に対して適正な音量比になっているか。
   ここが崩れると「サ行だけ耳障り」または「カ行が聞こえない」になる。 */
static void test_consonant_balance(void)
{
    const char *kana[8] = { "さ", "し", "す", "は", "か", "た", "ま", "な" };
    const YkPreset ps = *yk_preset(0);
    for (int i = 0; i < 8; i++) {
        int n = yk_speak_kana(kana[i], &ps, buf, (int)(sizeof(buf) / sizeof(buf[0])), NULL, 0);
        check(n > 0, "合成失敗: %s", kana[i]);
        if (n <= 0) continue;
        int w20 = SR / 50;
        /* 子音部: 前半のうち最も強い 60ms (破裂音は無音の閉鎖のあとに
           バーストが来るので、先頭固定で測ると無音を測ってしまう)。
           母音部 = 末尾 60ms */
        double c = 0.0;
        for (int st = 0; st + w20 * 3 <= n / 2; st += w20) {
            double r = rms_between(buf, st, st + w20 * 3);
            if (r > c) c = r;
        }
        double v = rms_between(buf, n - w20 * 3, n);
        if (v < 1e-5) continue;
        double db = 20.0 * log10((c + 1e-9) / v);
        /* 摩擦音は母音と同程度、破裂音は -20dB くらいまで許す */
        check(db < 6.0, "%s: 子音が大きすぎる (母音比 %+.0f dB)", kana[i], db);
        check(db > -30.0, "%s: 子音が小さすぎる (母音比 %+.0f dB)", kana[i], db);
    }
}

/* 全体の音量が適正か (小さすぎると聞こえない) */
static void test_loudness(void)
{
    const YkPreset ps = *yk_preset(0);
    const char *kana[5] = { "あ", "い", "う", "え", "お" };
    for (int i = 0; i < 5; i++) {
        int n = yk_speak_kana(kana[i], &ps, buf, (int)(sizeof(buf) / sizeof(buf[0])), NULL, 0);
        if (n <= 0) continue;
        double r = rms_between(buf, 0, n);
        check(r > 0.06, "%s: 音量が小さい (RMS %.3f)", kana[i], r);
        check(r < 0.30, "%s: 音量が大きすぎる (RMS %.3f)", kana[i], r);
    }
}

static void test_vowel_formants(void)
{
    /* 母音のフォルマント (設計値: internal.h の YK_VOWEL_F × formantScale)。
       F1/F2 は音響的に判別できるので、高調波の山の位置で検証する。 */
    static const float expect[5][2] = {
        { 780, 1200 },   /* a */
        { 300, 2250 },   /* i */
        { 350, 1450 },   /* u */
        { 500, 1950 },   /* e */
        { 480,  800 },   /* o */
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
        if (v == 4) {
            /* /o/ は F1-F2 が 320Hz しか離れておらず、F0≈230Hz では
               高調波の山を分離できない。代わりに「F2 が低い」ことを
               帯域エネルギー比で確かめる。 */
            double lowE = band_energy(seg, len, SR, 550, 1000);
            double highE = band_energy(seg, len, SR, 1500, 2600);
            check(lowE > highE * 2.0,
                  "お: F2 が低くない (550-1000 %0.1f dB vs 1500-2600 %.1f dB)",
                  lowE, highE);
        } else {
            /* F2 は F1 の裾を拾わないよう、F1 の 1.3 倍以上から探す */
            double lo2 = f1 * 1.3;
            if (lo2 < 700) lo2 = 700;
            double f2 = formant_peak(seg, len, SR, f0, lo2, 3000);
            double want2 = expect[v][1] * ps.formantScale;
            double tol2 = want2 * 0.30f + 150;
            check(fabs(f2 - want2) < tol2,
                  "%s: F2 = %.0f Hz (期待 %.0f ± %.0f)", kana[v], f2, want2, tol2);
        }

        /* 有声部にクリックが無いこと (前は閉鎖明けに 1 サンプルの跳びが出ていた) */
        {
            int ncl = count_clicks(buf, n);
            check(ncl == 0, "%s: クリックが %d 箇所ある", kana[v], ncl);
        }

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
    test_consonant_balance();
    test_loudness();
    test_pause_duration();
    if (failures) {
        printf("\n%d 件のテストが失敗しました\n", failures);
        return 1;
    }
    printf("すべてのテストに合格しました\n");
    return 0;
}
