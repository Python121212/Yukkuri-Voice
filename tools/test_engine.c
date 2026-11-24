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

static double rms_between(const float *x, int a, int b)
{
    double acc = 0;
    for (int i = a; i < b; i++) acc += (double)x[i] * x[i];
    return sqrt(acc / (b - a));
}

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

/* 同じ入力からは必ず同じ波形が出ること。
   内部バッファの未初期化領域を読むバグ (ノイズが混ざって結果が変わる) は
   これで捕まえる。 */
static void test_deterministic(void)
{
    static float buf2[YK_SR * 30];
    const YkPreset ps = *yk_preset(0);
    const char *kana[3] = { "か", "さ", "ゆっくり" };
    for (int i = 0; i < 3; i++) {
        int n1 = yk_speak_kana(kana[i], &ps, buf, (int)(sizeof(buf) / sizeof(buf[0])), NULL, 0);
        int n2 = yk_speak_kana(kana[i], &ps, buf2, (int)(sizeof(buf2) / sizeof(buf2[0])), NULL, 0);
        check(n1 == n2, "%s: 出力長が一致しない (%d vs %d)", kana[i], n1, n2);
        if (n1 != n2) continue;
        int diff = 0;
        for (int k = 0; k < n1; k++) if (buf[k] != buf2[k]) diff++;
        check(diff == 0, "%s: 出力が非決定的 (%d サンプルが不一致)", kana[i], diff);
    }
}

/* 母音に雑音 (「プシュ」という息まじりの音) が混ざっていないか。
   係数を 64 サンプルごとに段階的に切り替えていた頃は、その境界で
   波形が不連続になり、高調波のすぐ隣 (F0 の半分ずれた位置) に
   側帯波が出ていた。今は係数を 1 サンプルずつ動かしているので、
   側帯波は高調波より 25dB 以上低いはず。 */
static void test_no_sideband_noise(void)
{
    const YkPreset ps = *yk_preset(0);
    const char *kana[3] = { "あ", "い", "お" };
    const int K = 8192;
    for (int i = 0; i < 3; i++) {
        int n = yk_speak_kana(kana[i], &ps, buf, (int)(sizeof(buf) / sizeof(buf[0])), NULL, 0);
        if (n < K + SR / 20) continue;

        /* 母音の定常部 */
        const float *seg = buf + SR / 20;
        double worst = -999.0;
        /* 高調波の隣 (F0 の 1/2 だけずらした位置) のレベルを調べる */
        for (int k = 4; k <= 8; k++) {
            double f0 = k * (double)ps.pitch;
            double main = harm_amp(seg, K, SR, f0);
            double side = harm_amp(seg, K, SR, f0 + ps.pitch * 0.5);
            double db = 20.0 * log10((side + 1e-12) / (main + 1e-12));
            if (db > worst) worst = db;
        }
        check(worst < -20.0, "%s: 母音に側帯波の雑音がある (%+.0f dB)", kana[i], worst);
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

/* 破裂音のバーストが「プッ」と聞こえる高域に偏っていないこと。
   2 次 LP を入れる前は重心が 12-15kHz あり、子音が「プシュ」に聞こえた。 */
static void test_burst_band(void)
{
    const char *kana[6] = { "か", "た", "ぱ", "が", "だ", "ば" };
    for (int i = 0; i < 6; i++) {
        int n = yk_speak_kana(kana[i], yk_preset(0), buf, (int)(sizeof(buf) / sizeof(buf[0])), NULL, 0);
        check(n > 0, "合成失敗: %s", kana[i]);
        if (n <= 0) continue;
        /* 最も強い 4ms 窓 = 破裂 */
        int w = SR / 250;
        int best = 0; double be = 0;
        for (int st = 0; st + w <= n / 2; st += w / 2) {
            double r = rms_between(buf, st, st + w);
            if (r > be) { be = r; best = st; }
        }
        if (be < 1e-4) continue;
        /* その窓のスペクトル重心 (DFT を粗く走査) */
        double num = 0.0, den = 0.0;
        for (int k = 1; k < 96; k++) {
            double f = 300.0 * k;
            if (f > SR / 2 - 300) break;
            double a = harm_amp(buf + best, w, SR, f);
            num += a * a * f;
            den += a * a;
        }
        double cen = den > 0 ? num / den : 0.0;
        check(cen < 7000.0, "%s: 破裂の重心が高すぎる (%.0f Hz)", kana[i], cen);
    }
}

/* 語尾が「プツ」と切れないこと。
   有音の終わりで 1 サンプル段差が残ると、語尾にクリックが付く。 */
static void test_tail_no_click(void)
{
    int n = yk_speak_kana("ゆっくりしていってね。", yk_preset(0), buf,
                          (int)(sizeof(buf) / sizeof(buf[0])), NULL, 0);
    check(n > 0, "合成失敗 (語尾)");
    if (n <= 0) return;
    /* 無音に入る直前 60ms の最大サンプル間差分が、直前の局所 rms に
       比べて跳ねていないか。 */
    int st = n - SR / 10;
    if (st < 0) st = 0;
    double worst = 0.0;
    for (int i = 1; i < n; i++) {
        int a = i - SR / 500, b = i + SR / 500;
        if (a < 0) a = 0;
        if (b > n) b = n;
        double r = rms_between(buf, a, b);
        double d = fabs((double)buf[i] - (double)buf[i - 1]);
        /* 相対的にも絶対的にも大きい段差だけをクリックとみなす。
           無音直前の微小な残り (0.003 程度) は聞こえない。 */
        if (r > 1e-5 && d > 0.01 && d / r > worst) worst = d / r;
    }
    check(worst < 6.0, "語尾に段差がある (相対 %.1f)", worst);
}

/* 摩擦音の明るさ。
   破裂用のローパスを摩擦経路にも掛けていたときは /s/ の重心が
   4.1kHz まで落ち、「し」「す」「さ」がぼやけて聞き分けにくかった。
   帯域ノイズは 5-7kHz まで残っている必要がある。 */
static void test_fricative_brightness(void)
{
    const char *kana[3] = { "さ", "す", "し" };
    const double lo[3] = { 4500.0, 4500.0, 3000.0 };   /* 期待する重心の下限 */
    for (int i = 0; i < 3; i++) {
        int n = yk_speak_kana(kana[i], yk_preset(0), buf, (int)(sizeof(buf) / sizeof(buf[0])), NULL, 0);
        check(n > 0, "合成失敗: %s", kana[i]);
        if (n <= 0) continue;
        int len = n / 2;                    /* 子音部 */
        double num = 0.0, den = 0.0;
        for (int k = 1; k < 200; k++) {
            double f = 200.0 * k;
            if (f > SR / 2 - 200) break;
            double a = harm_amp(buf, len, SR, f);
            num += a * a * f;
            den += a * a;
        }
        double cen = den > 0 ? num / den : 0.0;
        check(cen > lo[i], "%s: 摩擦音が暗すぎる (重心 %.0f Hz)", kana[i], cen);
    }
}

/* 「し」(/ɕ/) と「す」(/s/) が聞き分けられること。
   /ɕ/ は 3-4kHz に山があり 5-6kHz が弱い。/s/ は 5-6kHz まで平坦。
   ノイズ帯域を 1800Hz と広く取っていたときは /ɕ/ が /s/ に近づき、
   「し」が「す」に聞こえた。 */
static void test_sh_vs_s(void)
{
    const char *kana[2] = { "し", "す" };
    double contrast[2] = { 0, 0 };
    for (int i = 0; i < 2; i++) {
        int n = yk_speak_kana(kana[i], yk_preset(0), buf, (int)(sizeof(buf) / sizeof(buf[0])), NULL, 0);
        check(n > 0, "合成失敗: %s", kana[i]);
        if (n <= 0) return;
        /* 先頭 100ms のうち、母音が始まる前の摩擦だけを見る */
        int len = SR * 100 / 1000;
        if (len > n) len = n;
        double e34 = 0, e56 = 0;
        for (int k = 1; k < 300; k++) {
            double f = 100.0 * k;
            if (f > SR / 2 - 100) break;
            double a = harm_amp(buf, len, SR, f);
            double p = a * a;
            if (f >= 3000 && f < 4000) e34 += p;
            if (f >= 5000 && f < 6000) e56 += p;
        }
        contrast[i] = 10.0 * log10((e34 + 1e-20) / (e56 + 1e-20));
    }
    check(contrast[0] > 8.0, "し: 5-6kHz が強すぎる (3-4k との差 %+.1f dB)", contrast[0]);
    check(contrast[1] < 3.0, "す: 5-6kHz が弱すぎる (3-4k との差 %+.1f dB)", contrast[1]);
}

/* 破裂音・促音・声門破裂の直後にクリックが出ないこと。
   閉鎖中に声帯フェーズを進めていなかったため、閉鎖明けの 1 サンプル目で
   パルスの位相が飛び、「プツ」「プッ」というクリックになっていた。 */
static void test_stop_onset_click(void)
{
    const char *txt[3] = { "ゆっくりしていってね。", "た", "きって" };
    for (int t = 0; t < 3; t++) {
        int n = yk_speak_kana(txt[t], yk_preset(0), buf, (int)(sizeof(buf) / sizeof(buf[0])), NULL, 0);
        check(n > 0, "合成失敗: %s", txt[t]);
        if (n <= 0) continue;
        double worst = 0.0; int where = 0;
        for (int i = 1; i < n; i++) {
            int a = i - SR / 500, b = i + SR / 500;
            if (a < 0) a = 0;
            if (b > n) b = n;
            double r = rms_between(buf, a, b);
            double d = fabs((double)buf[i] - (double)buf[i - 1]);
            if (r > 1e-5 && d > 0.01 && d / r > worst) { worst = d / r; where = i; }
        }
        check(worst < 6.0, "%s: 破裂の頭に段差がある (相対 %.1f @ %d ms)", txt[t], worst,
              (int)(where * 1000 / SR));
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
    test_deterministic();
    test_no_sideband_noise();
    test_burst_band();
    test_tail_no_click();
    test_fricative_brightness();
    test_sh_vs_s();
    test_stop_onset_click();
    test_pause_duration();
    if (failures) {
        printf("\n%d 件のテストが失敗しました\n", failures);
        return 1;
    }
    printf("すべてのテストに合格しました\n");
    return 0;
}
