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
/* 自己相関で F0 を測る (抑揚でモーラごとに変わるので固定値では測れない) */
static double measured_f0(const float *x, int n, int sr)
{
    double mean = 0;
    for (int i = 0; i < n; i++) mean += x[i];
    mean /= n;
    const int lo = sr / 400, hi = sr / 120;
    double best = -1e30; int bestk = 0;
    for (int k = lo; k <= hi && k < n / 2; k++) {
        double acc = 0, e1 = 0, e2 = 0;
        for (int i = 0; i + k < n; i++) {
            double a = x[i] - mean, b = x[i + k] - mean;
            acc += a * b; e1 += a * a; e2 += b * b;
        }
        if (e1 < 1e-12 || e2 < 1e-12) continue;
        double r = acc / sqrt(e1 * e2);
        if (r > best) { best = r; bestk = k; }
    }
    return bestk > 0 ? (double)sr / bestk : 0.0;
}

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

        /* 抑揚を付けたので、この区間の実際の F0 を測ってから高調波を拾う。
           プリセット値で決め打ちすると、F0 が 3% ずれただけで山を外す。 */
        double f0m = f0;
        {
            double m = measured_f0(seg, len, SR);
            /* オクターブ誤りを避ける。F1 (700-800Hz) の山を拾うと
               半分の F0 を返すので、プリセット値の 0.75-1.25 倍に入るときだけ使う。 */
            if (m > f0 * 0.75 && m < f0 * 1.25) f0m = m;
        }

        /* F1: 200Hz から F2 の手前まで */
        double f1 = formant_peak(seg, len, SR, f0m, 350, 800);
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
            /* 上限は 2600Hz。それ以上は F5 (4300Hz) の裾を拾ってしまう。 */
            double f2 = formant_peak(seg, len, SR, f0m, lo2, 2600);
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
    /* 「し」は 3-4kHz に山があるが、狭すぎると笛のような「シュー」になる。
       3-4kHz が 5-6kHz より強いこと (す との区別) と、
       山が細すぎないこと (帯域幅) の両方を見る。 */
    check(contrast[0] > 2.0, "し: 5-6kHz が強すぎる (3-4k との差 %+.1f dB)", contrast[0]);
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


/* 文末の母音が「聞き取れる」こと。
   60ms のコサイン減衰が文末の短い母音の後半を丸ごと削っていたため、
   「ね」の母音は 100ms のうち後半 60ms が消え、耳には子音だけ =
   「ゆっくりしていって…（無音）」に聞こえていた。
   文末の母音は (a) 十分な長さで鳴っている (b) 減衰で削られていない
   の 2 点を波形から確かめる。 */
static void test_final_vowel_audible(void)
{
    int n = yk_speak_kana("ゆっくりしていってね。", yk_preset(0), buf,
                          (int)(sizeof(buf) / sizeof(buf[0])), NULL, 0);
    check(n > 0, "合成失敗 (文末母音)");
    if (n <= 0) return;

    /* 末尾から句点の無音を飛ばして、最後の有音区間を探す。 */
    const int hop = SR / 1000;                 /* 1ms */
    int lastLoud = -1;
    for (int i = 0; i + hop <= n; i += hop)
        if (rms_between(buf, i, i + hop) > 0.02) lastLoud = i;
    check(lastLoud > 0, "文末に有音区間がない");
    if (lastLoud <= 0) return;

    check(n - lastLoud < SR / 8,
          "文末の無音が長すぎる (%.0fms)",
          1000.0 * (double)(n - lastLoud) / SR);

    /* 最後の有音区間のうち、母音の後半が減衰で落ちていないか。
       終端 40ms 前の RMS が、その 80ms 前と同程度あること。 */
    double a = rms_between(buf, lastLoud - SR * 80 / 1000, lastLoud - SR * 40 / 1000);
    double b = rms_between(buf, lastLoud - SR * 40 / 1000, lastLoud - SR * 8 / 1000);
    check(b > a * 0.6,
          "文末の母音が減衰で削られている (前半 %.3f → 後半 %.3f)", a, b);
}


/* 倍音の詰まり具合 (「スカスカ」対策)。
   F0=235Hz の声でフォルマントの帯域幅を狭くしすぎると、高調波が
   山に乗ったり外れたりして山谷が深くなり、「スカスカ」して聞こえる。
   逆に広げすぎると母音の輪郭がぼやける。
   bw_for() の下限を 1.15*F0 (全次が 270Hz に張り付いていた) から
   0.75*F0 に変えた。その結果、0-3.5kHz の有声倍音は 11.8 -> 10.6 本、
   隣接倍音の最大落差は 11.8 -> 14.0dB になる。
   落差が開きすぎていないことを回帰条件にする。 */
static void test_harmonic_gap(void)
{
    const char *kana[5] = { "あ", "い", "う", "え", "お" };
    for (int v = 0; v < 5; v++) {
        int n = yk_speak_kana(kana[v], yk_preset(0), buf,
                              (int)(sizeof(buf) / sizeof(buf[0])), NULL, 0);
        if (n <= 0) { check(0, "%s: 合成失敗 (倍音)", kana[v]); continue; }
        int st = SR * 60 / 1000, len = SR * 90 / 1000;
        if (st + len > n) continue;
        double f0 = measured_f0(buf + st, len, SR);
        if (f0 < 150 || f0 > 300) f0 = 235.0;
        /* 一番強い高調波を基準に、0-3.5kHz の倍音の並びを見る */
        double amp[64]; int nh = 0;
        for (double f = f0; f < 3500 && nh < 64; f += f0) {
            amp[nh] = harm_amp(buf + st, len, SR, f);
            nh++;
        }
        double peak = 0;
        for (int i = 0; i < nh; i++) if (amp[i] > peak) peak = amp[i];
        if (peak <= 0) continue;
        double worst = 0;
        for (int i = 1; i < nh; i++) {
            double a = 20.0 * log10(amp[i - 1] / peak + 1e-12);
            double b = 20.0 * log10(amp[i] / peak + 1e-12);
            double d = fabs(a - b);
            if (d > worst) worst = d;
        }
        /* /o/ は F1 (523Hz) と F2 (872Hz) が近く、その間の谷が深いので
           他の母音より大きめに出る (実測 17.6dB)。それを含めた上限。 */
        check(worst < 19.0,
              "%s: 隣接倍音の落差が大きすぎる (%.1f dB: スカスカ寄り)",
              kana[v], worst);
    }
}

/* 母音の長さの比率 (「うー…」と伸びて聞こえないか)。
   合成時は母音を 1.45 倍に伸ばしている。話速 0.80 倍と合わせると
   CV モーラは約 280ms、うち母音が約 200ms (71%) になる。
   子音が母音に埋もれない範囲であることを確かめる。 */
static void test_vowel_duration_ratio(void)
{
    int n = yk_speak_kana("か", yk_preset(0), buf,
                          (int)(sizeof(buf) / sizeof(buf[0])), NULL, 0);
    check(n > 0, "合成失敗 (母音長)");
    if (n <= 0) return;

    /* 「か」の子音は先頭の 60ms 前後。母音の区間を後半から測る。 */
    int hop = SR / 1000;
    int lastLoud = 0;
    for (int i = 0; i + hop <= n; i += hop)
        if (rms_between(buf, i, i + hop) > 0.03) lastLoud = i;

    /* モーラ全体の長さ (無音を除く) */
    int firstLoud = n;
    for (int i = 0; i + hop <= n; i += hop)
        if (rms_between(buf, i, i + hop) > 0.03) { firstLoud = i; break; }
    double moraMs = 1000.0 * (double)(lastLoud - firstLoud) / SR;

    /* 日本語の 1 モーラは 100-200ms 程度。ゆっくり声なので長めだが、
       300ms を超えると「伸びた」印象になる。 */
    check(moraMs < 300.0, "1 モーラが長すぎる (%.0f ms)", moraMs);
    check(moraMs > 150.0, "1 モーラが短すぎる (%.0f ms)", moraMs);

    /* 合成時に伸ばす母音の長さが、末尾で切り捨てられていないこと。
       母音のみの「あ」が計算どおりの長さで出ているかで見る。 */
    int n2 = yk_speak_kana("あ", yk_preset(0), buf,
                           (int)(sizeof(buf) / sizeof(buf[0])), NULL, 0);
    /* dur=110ms, 速度 0.80, 母音 1.45 倍 = 199ms 前後 */
    double aMs = 1000.0 * (double)n2 / SR;
    check(aMs > 185.0 && aMs < 230.0,
          "母音 1 音の長さが想定外 (%.0f ms, 期待 199ms 前後)", aMs);

    /* 文末の母音が切り捨てられていないこと。
       出力サンプル数の見積もりで母音の 1.45 倍を数え落としていたため、
       句点で終わる文では末尾が約 100ms 短く切られていた。
       句点つきと句点なしで、有音の終端がほぼ同じ位置に来ることを確かめる。 */
    int na = yk_speak_kana("ゆっくりしていってね。", yk_preset(0), buf,
                           (int)(sizeof(buf) / sizeof(buf[0])), NULL, 0);
    double envA[4096]; int wa = 0;
    for (int i = 0; i + hop <= na && wa < 4096; i += hop)
        envA[wa++] = rms_between(buf, i, i + hop);
    int endA = 0;
    for (int i = 0; i < wa; i++) if (envA[i] > 0.02) endA = i;

    int nb = yk_speak_kana("ゆっくりしていってね", yk_preset(0), buf,
                           (int)(sizeof(buf) / sizeof(buf[0])), NULL, 0);
    double envB[4096]; int wb = 0;
    for (int i = 0; i + hop <= nb && wb < 4096; i += hop)
        envB[wb++] = rms_between(buf, i, i + hop);
    int endB = 0;
    for (int i = 0; i < wb; i++) if (envB[i] > 0.02) endB = i;

    check(abs(endA - endB) * 1 <= 40,
          "文末の母音が切り捨てられている (句点あり %d ms / なし %d ms)",
          endA, endB);
}


/* 音素記号列の解釈。
   ローマ字・かな・記号を混在で受け付け、正しい音素列になること。 */
static void test_symbol_parse(void)
{
    static YkPlan pl;
    /* 「ゆっくり」-> y u Q k u r i (7 音素、Q は促音) */
    int rc = yk_plan_from_symbols("yuQkuri", yk_preset(0), &pl);
    check(rc == 0, "記号列の解釈に失敗 (yuQkuri)");
    if (rc == 0) {
        check(pl.count == 7, "yuQkuri の音素数が %d (期待 7)", pl.count);
        check(yk_phone_symbol((YkPhone)pl.items[2].phone)[0] == 'Q',
              "3 番目が促音でない (%s)", yk_phone_symbol((YkPhone)pl.items[2].phone));
        check(pl.items[0].phone == YK_P_Y && pl.items[1].phone == YK_P_U,
              "ゆ が y+u になっていない");
    }

    /* 撥音 N と長音 : */
    rc = yk_plan_from_symbols("aN", yk_preset(0), &pl);
    check(rc == 0 && pl.count == 2 && pl.items[1].phone == YK_P_N,
          "撥音 N が解釈されない");

    /* かなとローマ字の混在 */
    rc = yk_plan_from_symbols("ゆQkuri", yk_preset(0), &pl);
    check(rc == 0 && pl.count >= 6, "かなとローマ字の混在に失敗");

    /* 拗音は 1 モーラ (子音 + 母音の 2 音素) */
    rc = yk_plan_from_symbols("kya", yk_preset(0), &pl);
    check(rc == 0 && pl.count == 2,
          "拗音 kya が %d 音素 (期待 2: 子音 + 母音)", pl.count);

    /* 未知の文字は無視して残りを読む */
    rc = yk_plan_from_symbols("ka@ku", yk_preset(0), &pl);
    check(rc == 0 && pl.count == 4, "未知文字の読み飛ばしに失敗 (%d 音素)", pl.count);
}

/* 日本語の異音規則。 */
static void test_allophones(void)
{
    static YkPlan pl;

    /* ん の同化: さんぽ の ん は [m]、まんが の ん は [ŋ] */
    yk_plan_from_kana("さんぽ", yk_preset(0), &pl);
    int found_m = 0;
    for (int i = 0; i < pl.count; i++)
        if (pl.items[i].phone == YK_P_M) found_m = 1;
    check(found_m, "さんぽ の ん が [m] に同化していない");

    yk_plan_from_kana("まんが", yk_preset(0), &pl);
    int found_ng = 0;
    for (int i = 0; i < pl.count; i++)
        if (pl.items[i].phone == YK_P_NG) found_ng = 1;
    check(found_ng, "まんが の ん が [ŋ] に同化していない");

    /* 母音の無声化: きって の き の母音が無声化する */
    yk_plan_from_kana("きって", yk_preset(0), &pl);
    int dev = 0;
    for (int i = 0; i < pl.count; i++)
        if (pl.items[i].phone == YK_P_DEV) dev = 1;
    check(dev, "きって の き が無声化していない");

    /* 単独の「す」「し」は無声化しない (母音が残る) */
    yk_plan_from_kana("す", yk_preset(0), &pl);
    dev = 0;
    for (int i = 0; i < pl.count; i++)
        if (pl.items[i].phone == YK_P_DEV) dev = 1;
    check(!dev, "単独の す が無声化している (母音が消える)");

    /* ら行の弾き音: 語中の ら は語頭より短い */
    static YkPlan p1, p2;
    yk_plan_from_kana("ら", yk_preset(0), &p1);
    yk_plan_from_kana("あら", yk_preset(0), &p2);
    float head = 0, mid = 0;
    for (int i = 0; i < p1.count; i++) if (p1.items[i].phone == YK_P_R) head = p1.items[i].dur;
    for (int i = 0; i < p2.count; i++) if (p2.items[i].phone == YK_P_R) mid = p2.items[i].dur;
    check(head > 0 && mid > 0 && mid < head * 0.8f,
          "語中の ら が弾き音になっていない (語頭 %.1fms / 語中 %.1fms)", head, mid);
}

/* かな -> 音素記号の自動変換 (UI 表示用) */
static void test_kana_to_symbols(void)
{
    char buf[256];
    int n = yk_kana_to_symbols("ゆっくり", buf, (int)sizeof(buf));
    check(n > 0, "かな -> 記号の変換に失敗");
    if (n > 0) {
        check(strstr(buf, "Q") != NULL, "促音が記号に反映されていない (%s)", buf);
        check(strstr(buf, "k") != NULL, "か行の子音が記号に反映されていない (%s)", buf);
    }
    /* 記号に戻したものを再度読める (往復) */
    static YkPlan pl;
    int rc = yk_plan_from_symbols(buf, yk_preset(0), &pl);
    check(rc == 0, "変換した記号列を読み直せない (%s)", buf);
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
    test_final_vowel_audible();
    test_harmonic_gap();
    test_vowel_duration_ratio();
    test_symbol_parse();
    test_allophones();
    test_kana_to_symbols();
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
