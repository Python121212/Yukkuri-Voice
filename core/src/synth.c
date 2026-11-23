/* フォルマント合成器（Klatt 型）
 *
 *  声帯パルス (Rosenberg パルス列 + ジッタ + ビブラート)
 *        |
 *        +--> 8 段カスケード共振器 (F1-F8) --> 放射特性 --> 高域シェルフ --\
 *        |                                                                +--> AGC --> リミッタ
 *  摩擦ノイズ (帯域共振器) --> 放射特性 ---------------------------/ (別経路)
 *
 * 設計上の要点
 *  1. 母音のスペクトル包絡はフォルマントの「縦続」で決まるのでカスケード。
 *     ただし 5 極で打ち切ると 4kHz 以上が -60dB/oct で落ちて「こもる」ので、
 *     F6-F8 (幅の広い高域フォルマント) まで継ぐ。
 *  2. それでも残る高域の落ち込みは、3kHz 以上のシェルフで補正する
 *     (声帯音源の -12dB/oct を打ち消すためのもので、ノイズには掛けない)。
 *  3. 摩擦音は別バッファに貯めて、有声部のピークに対して一定の相対レベルで
 *     混ぜる。同じ正規化を掛けると母音の素の音量が小さい (/u/, /o/) ときに
 *     摩擦音だけが突出する。
 *  4. 仕上げは有声フレームの RMS を基準にしたゲイン + tanh のソフトリミッタ。
 *     「全体をピークで一律に縮める」処理はしない (スパイク 1 個で全部消える)。
 */
#include "internal.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define YK_MAX_SAMPLES (YK_SR * 30)

/* ---------------- 2 次共振器 ---------------- */
typedef struct {
    float a, b, c;   /* H(z) = a / (1 - b z^-1 - c z^-2), a = 1 - b - c (DC ゲイン 1) */
    float y1, y2;
} Reson;

/* 目標係数と、そこへ向かう 1 サンプルあたりの増分。
   係数を 64 サンプルごとに「段階的に」差し替えると、その時刻に
   波形の不連続が生まれて周期 750Hz の側帯波 (=「プシュ」という雑音)
   が出る。目標値へ 1 サンプルずつ線形に近づけることで消す。 */
typedef struct {
    Reson  r;
    float ta, tb, tc;    /* 目標 */
    float da, db, dc;    /* 増分 */
} SmReson;

static void reson_reset(Reson *r) { r->y1 = r->y2 = 0.0f; }

static void reson_set(Reson *r, float f, float bw, float sr)
{
    if (f < 60.0f) f = 60.0f;
    if (f > 0.45f * sr) f = 0.45f * sr;
    if (bw < 40.0f) bw = 40.0f;
    if (bw > 0.4f * sr) bw = 0.4f * sr;

    const float rho = expf(-(float)M_PI * bw / sr);
    const float th = 2.0f * (float)M_PI * f / sr;
    r->b = 2.0f * rho * cosf(th);
    r->c = -rho * rho;
    r->a = 1.0f - r->b - r->c;
    if (r->a < 1e-6f) r->a = 1e-6f;
}

/* ---- 係数を滑らかに動かす共振器 ---- */
static void sm_reset(SmReson *s) { s->r.y1 = s->r.y2 = 0.0f; }

/* 目標係数を決める。次の update までの sample 数で割って増分にする。 */
static void sm_target(SmReson *s, float f, float bw, float sr, float glideSamples)
{
    Reson t;
    reson_set(&t, f, bw, sr);
    s->ta = t.a; s->tb = t.b; s->tc = t.c;
    if (glideSamples < 1.0f) glideSamples = 1.0f;
    s->da = (s->ta - s->r.a) / glideSamples;
    s->db = (s->tb - s->r.b) / glideSamples;
    s->dc = (s->tc - s->r.c) / glideSamples;
}

static inline float sm_run(SmReson *s, float x)
{
    Reson *r = &s->r;
    /* 目標に近づける (行き過ぎない) */
    r->a += (s->da > 0.0f) ? ((r->a < s->ta) ? s->da : 0.0f)
                           : ((r->a > s->ta) ? s->da : 0.0f);
    r->b += (s->db > 0.0f) ? ((r->b < s->tb) ? s->db : 0.0f)
                           : ((r->b > s->tb) ? s->db : 0.0f);
    r->c += (s->dc > 0.0f) ? ((r->c < s->tc) ? s->dc : 0.0f)
                           : ((r->c > s->tc) ? s->dc : 0.0f);
    float y = r->a * x + r->b * r->y1 + r->c * r->y2;
    r->y2 = r->y1;
    r->y1 = y;
    return y;
}

/* 状態が暴走しないための安全弁。
   共振器の内部状態は定常状態で |y| < 数程度に収まるはず。
   ここでは「ピークゲイン × 入力」を大きく超えたら捨てる。 */
static inline float clampf_(float v, float lim)
{
    if (v > lim) return lim;
    if (v < -lim) return -lim;
    return v;
}

static inline float reson_run(Reson *r, float x)
{
    float y = r->a * x + r->b * r->y1 + r->c * r->y2;
    r->y2 = r->y1;
    r->y1 = y;
    return y;
}

/* ---------------- 二次ローパス (バターワース) ---------------- */
/* 破裂音の破裂を 1-4kHz に収めるために使う。一次だと -6dB/oct しか
   落ちないので、高域が残って「プスッ」と金属的に鳴る。 */
typedef struct {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} BiquadLp;

static void biquad_lp_reset(BiquadLp *f) { f->x1 = f->x2 = f->y1 = f->y2 = 0.0f; }

static void biquad_lp_set(BiquadLp *f, float fc, float sr)
{
    const float w0 = 2.0f * (float)M_PI * fc / sr;
    const float cw = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * 0.7071f);
    const float a0 = 1.0f + alpha;
    f->b0 = (1.0f - cw) * 0.5f / a0;
    f->b1 = (1.0f - cw) / a0;
    f->b2 = (1.0f - cw) * 0.5f / a0;
    f->a1 = -2.0f * cw / a0;
    f->a2 = (1.0f - alpha) / a0;
}

static inline float biquad_lp_run(BiquadLp *f, float x)
{
    float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2
            - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x;
    f->y2 = f->y1; f->y1 = y;
    return y;
}

/* ---------------- 一次フィルタ ---------------- */
typedef struct { float y, a; } OnePole;

static inline void onep_set(OnePole *s, float f, float sr)
{
    if (f < 20.0f) f = 20.0f;
    if (f > 0.45f * sr) f = 0.45f * sr;
    s->a = 1.0f - expf(-2.0f * (float)M_PI * f / sr);
}
static inline float onep_lp(OnePole *s, float x)
{
    s->y += s->a * (x - s->y);
    return s->y;
}

/* ---------------- 乱数 ---------------- */
typedef struct { uint32_t s; } Rnd;
static inline float rnd_next(Rnd *r)
{
    r->s ^= r->s << 13; r->s ^= r->s >> 17; r->s ^= r->s << 5;
    return (float)((int32_t)r->s) * (1.0f / 2147483648.0f);
}

/* ---------------- 声帯パルス (Rosenberg 型) ----------------
 * 実際の声帯は「開く→閉じる」で空気流量が滑らかに変化し、
 * その微分 (声帯励振) は -12dB/oct で減衰する。
 * ノコギリ波 (-6dB/oct) だと倍音が多すぎて金属的に聞こえる。
 *
 *   phase 0 .. tp   : 開く (1 - cos)
 *   phase tp..tp+tn : 閉じる (cos, 速い)
 *   以降            : 0
 * 微分したものを励振源として使う。
 */
typedef struct {
    float phase;
    float prev;
    float tp, tn;     /* 開く区間と閉じる区間の比率 (合計 <1) */
} Glottal;

static void glottal_init(Glottal *g)
{
    g->phase = 0.0f;
    g->prev = 0.0f;
    g->tp = 0.42f;
    g->tn = 0.16f;
}

static inline float rosenberg_flow(float ph, float tp, float tn)
{
    if (ph < tp) {
        /* 開く: 0 -> 1 */
        return 0.5f * (1.0f - cosf((float)M_PI * ph / tp));
    }
    if (ph < tp + tn) {
        /* 閉じる: 1 -> 0 (速い) */
        return cosf((float)M_PI * (ph - tp) / (2.0f * tn));
    }
    return 0.0f;
}

/* 1 サンプル進めて、励振 (流量の微分) を返す */
static inline float glottal_step(Glottal *g, float f0, float sr)
{
    g->phase += f0 / sr;
    if (g->phase >= 1.0f) g->phase -= 1.0f;
    float flow = rosenberg_flow(g->phase, g->tp, g->tn);
    float d = flow - g->prev;
    g->prev = flow;
    return d;
}

/* ---------------- 目標パラメータ ---------------- */
/* 高次フォルマント F6-F8 (固定)。実声道の F6 以上は幅が広い。 */
static const float YK_HF_F[3] = { 4300.0f, 5800.0f, 7500.0f };
static const float YK_HF_B[3] = { 1100.0f, 1400.0f, 1800.0f };

typedef struct {
    float f[8];
    float bw;
    float voiced;     /* 声帯振幅 */
    float noise;      /* 摩擦/破裂ノイズ振幅 */
    float noiseF;
    float noiseBw;
    float amp;
    float gain;
} Frame;

static void frame_for_phone(YkPhone p, const YkPreset *ps, Frame *fr)
{
    const YkPhoneInfo *pi = yk_phone_info(p);
    memset(fr, 0, sizeof(*fr));
    fr->bw = pi->bw > 0 ? pi->bw : 80.0f;
    fr->voiced = pi->voiced ? pi->amp : 0.0f;
    fr->noise = pi->noise;
    fr->amp = pi->amp > 0 ? pi->amp : 1.0f;
    fr->gain = pi->gain > 0 ? pi->gain : 1.0f;
    fr->noiseF = pi->noiseF > 0 ? pi->noiseF : 4000.0f;
    fr->noiseBw = pi->noiseBw > 0 ? pi->noiseBw : 2500.0f;

    if (p <= YK_P_O) {
        for (int i = 0; i < 5; i++) fr->f[i] = YK_VOWEL_F[p][i] * ps->formantScale;
    } else if (pi->formant[0] > 0) {
        for (int i = 0; i < 5; i++) fr->f[i] = (float)pi->formant[i] * ps->formantScale;
    } else {
        for (int i = 0; i < 5; i++) fr->f[i] = 500.0f * ps->formantScale;
    }
    for (int i = 0; i < 3; i++) fr->f[5 + i] = YK_HF_F[i];
}

static int find_vowel_back(const YkPlan *pl, int idx)
{
    for (int i = idx - 1; i >= 0; i--) {
        int p = pl->items[i].phone;
        if (p == YK_P_PAUSE) return -1;
        if (p <= YK_P_O) return p;
    }
    return -1;
}

static int find_vowel_fwd(const YkPlan *pl, int idx)
{
    for (int i = idx + 1; i < pl->count; i++) {
        int p = pl->items[i].phone;
        if (p == YK_P_PAUSE) return -1;
        if (p <= YK_P_O) return p;
    }
    return -1;
}

static float closure_ms(YkPhone p)
{
    switch (p) {
    case YK_P_P: case YK_P_T: case YK_P_K: return 55.0f;
    case YK_P_B: case YK_P_D: case YK_P_G: return 40.0f;
    case YK_P_GLOTTAL: return 20.0f;
    default: return 0.0f;
    }
}

static int is_stop(YkPhone p)
{
    return p == YK_P_B || p == YK_P_D || p == YK_P_G ||
           p == YK_P_P || p == YK_P_T || p == YK_P_K;
}

/* フォルマント帯域幅: 低次ほど狭い。F1 は 60-90Hz, F3 以上は 150-250Hz 程度が自然。 */
/* フォルマントの帯域幅。
   重要な制約: 帯域幅が F0 (この声で 235Hz) より狭いと、高調波がフォルマントの
   山を「飛び越えて」しまい、フォルマントが聞こえのピークにならない。
   帯域幅は F0 と同程度以上にし、中心周波数に比例させて広げる
   (実声道でも高いフォルマントほど帯域が広い)。 */
static float bw_for(int i, float base, float f0)
{
    float minbw = f0 * 1.15f;                 /* 高調波が必ず 1 本は山に乗る */
    float bw = base + 0.06f * (float)i * base
             + (i >= 2 ? 0.12f * (float)(i - 1) * base : 0.0f);
    (void)minbw;
    static const float mult[5] = { 1.0f, 1.15f, 1.9f, 2.6f, 3.4f };
    bw = base * mult[i];
    if (bw < minbw) bw = minbw;
    return bw;
}

/* ------------------------------------------------------------------ */
int yk_synthesize(const YkPlan *plan, const YkPreset *preset, float *out, int out_cap)
{
    if (!plan || !preset || !out || out_cap <= 0 || plan->count == 0) return -1;

    const float sr = (float)YK_SR;
    const float spd = preset->speed > 0.1f ? preset->speed : 0.8f;

    /* ---- 出力サンプル数 ---- */
    long total = 0;
    for (int i = 0; i < plan->count; i++) {
        YkPhone p = (YkPhone)plan->items[i].phone;
        float ms = plan->items[i].dur / spd;
        if (p != YK_P_CL) ms += closure_ms(p);
        total += (long)(ms * sr / 1000.0f);
    }
    total += (long)(0.15f * sr);
    if (total > out_cap) total = out_cap;
    if (total > YK_MAX_SAMPLES) total = YK_MAX_SAMPLES;

    /* ---- 状態 ---- */
    SmReson r[8];
    Reson nres;
    for (int i = 0; i < 8; i++) {
        sm_reset(&r[i]);
        reson_set(&r[i].r, 700, 80, sr);
        r[i].ta = r[i].r.a; r[i].tb = r[i].r.b; r[i].tc = r[i].r.c;
        r[i].da = r[i].db = r[i].dc = 0.0f;
    }
    reson_reset(&nres);
    reson_set(&nres, 4000, 2000, sr);

    OnePole glottalLp, radLp, noiseLp;
    BiquadLp radNLp;        /* 摩擦/破裂ノイズの帯域制限 */
    memset(&glottalLp, 0, sizeof(glottalLp));
    memset(&radLp, 0, sizeof(radLp));
    memset(&noiseLp, 0, sizeof(noiseLp));
    biquad_lp_reset(&radNLp);
    onep_set(&glottalLp, 900.0f, sr);     /* 声帯パルスの丸め */
    onep_set(&radLp, 15000.0f, sr);       /* 放射後の高域の歯止め */
    onep_set(&noiseLp, 2500.0f, sr);
    /* 破裂音の破裂は 1-4kHz が主成分。高域まで通すと「プスッ」と
       金属的に鳴るので 2 次で 3.5kHz から落とす。 */
    biquad_lp_set(&radNLp, 3500.0f, sr);

    Rnd rnd = { 0x9E3779B9u };

    Glottal glottal;
    glottal_init(&glottal);
    float f0 = preset->pitch;
    float jitter = 0.0f;
    float prevRad = 0.0f;
    long vibPhase = 0;

    Frame cur, target;
    frame_for_phone(YK_P_O, preset, &cur);
    for (int i = 0; i < 5; i++) cur.f[i] = YK_VOWEL_F[YK_P_O][i] * preset->formantScale;
    for (int i = 0; i < 3; i++) cur.f[5 + i] = YK_HF_F[i];
    memset(&target, 0, sizeof(target));
    target.voiced = cur.voiced = 0.0f;
    target.gain = cur.gain = 1.0f;
    target.amp = cur.amp = 1.0f;
    target.bw = cur.bw = 80.0f;
    target.noiseF = cur.noiseF = 4000.0f;
    target.noiseBw = cur.noiseBw = 2000.0f;


    /* 時定数 */
    const float glideF   = 1.0f - expf(-1.0f / (0.020f * sr));   /* フォルマント遷移 20ms */
    /* 振幅の時定数は短く (3ms)。長いと「子音の直後に母音が遅れて
       立ち上がる」ように聞こえて、音節がばらばらに聞こえる。 */
    const float glideAmp = 1.0f - expf(-1.0f / (0.003f * sr));
    const float glideF0  = 1.0f - expf(-1.0f / (0.040f * sr));

    /* 明るさ補正: 3kHz 以上を何倍に持ち上げるか。
       声道の縦続だけでは 4kHz 以上が -40dB まで落ちて「こもる」ので、
       耳が子音の識別に使う 3-8kHz を補正する。 */
    const float kShelf = 3.2f;

    for (int j = 0; j < 3; j++) {
        sm_reset(&r[5 + j]);
        reson_set(&r[5 + j].r, YK_HF_F[j], YK_HF_B[j], sr);
        r[5 + j].ta = r[5 + j].r.a; r[5 + j].tb = r[5 + j].r.b; r[5 + j].tc = r[5 + j].r.c;
        r[5 + j].da = r[5 + j].db = r[5 + j].dc = 0.0f;
    }

    OnePole shelfLp;
    memset(&shelfLp, 0, sizeof(shelfLp));
    onep_set(&shelfLp, 3000.0f, sr);   /* 高域シェルフの折れ点 */

    /* 20ms 窓ごとの「有声だったか」を記録しておく。音量の正規化を
       有声音だけで決めるため (摩擦音だけで決めると母音が埋もれる)。 */
    float *winVoiced = NULL;
    float *voicedHist = (float *)calloc((size_t)(total > 0 ? total : 1), sizeof(float));
    /* 摩擦音は有声部と別に貯めて、最後に「母音に対する相対レベル」で混ぜる。
       同じバッファで正規化すると、母音の素の音量が小さい母音 (う, お) の
       ときに摩擦音だけが持ち上がって耳障りになる。 */
    float *noiseBuf = (float *)calloc((size_t)(total > 0 ? total : 1), sizeof(float));
    float prevRadN = 0.0f;

    float clickEnv = 0.0f;
    float clickAtk = 0.0f;                 /* 破裂の立ち上がり包絡 */
    const float clickAtkStep = 1.0f / (0.0008f * sr);
    float burst = 0.0f;
    float burstHp = 0.0f;
    float hFriction = 0.0f;    /* 摩擦音の「息の強さ」を別経路でも反映 */
    long pos = 0;
    long closureLeft = 0;

    for (int idx = 0; idx < plan->count && pos < total; idx++) {
        const YkPhoneme *ph = &plan->items[idx];
        const YkPhone p = (YkPhone)ph->phone;

        if (p == YK_P_CL) {   /* 促音: 次の子音の閉鎖を延ばす */
            closureLeft += (long)(ph->dur / spd * sr / 1000.0f);
            continue;
        }

        /* ---- 目標フレーム ---- */
        frame_for_phone(p, preset, &target);
        /* 破裂音のノイズは「破裂の瞬間」だけ。音素テーブルの noise を
           そのまま使うと、子音の長さぶん (60-80ms) ずっとノイズが鳴り、
           「ぷしゅ」という息の音になる。破裂音の音は閉鎖 + 短い破裂。 */
        if (is_stop(p)) target.noise = 0.0f;
        if (p != YK_P_PAUSE && p > YK_P_O) {
            int pv = find_vowel_back(plan, idx);
            int nv = find_vowel_fwd(plan, idx);
            float tmp[5]; int have = 0;
            if (pv >= 0) {
                for (int i = 0; i < 5; i++) tmp[i] = YK_VOWEL_F[pv][i] * preset->formantScale;
                have = 1;
            }
            if (nv >= 0) {
                for (int i = 0; i < 5; i++) {
                    float v = YK_VOWEL_F[nv][i] * preset->formantScale;
                    tmp[i] = have ? (tmp[i] + v) * 0.5f : v;
                }
                have = 1;
            }
            if (have) {
                float k = (p == YK_P_N || p == YK_P_M || p == YK_P_NG || p == YK_P_R) ? 0.85f : 0.6f;
                for (int i = 0; i < 5; i++)
                    target.f[i] = target.f[i] * (1.0f - k) + tmp[i] * k;
            }
        }

        /* ---- 破裂音の閉鎖 ---- */
        long n_clo = (long)(closure_ms(p) * sr / 1000.0f) + closureLeft;
        closureLeft = 0;
        for (long s = 0; s < n_clo && pos < total; s++, pos++) {
            cur.voiced += (0.0f - cur.voiced) * glideAmp * 4.0f;
            cur.noise  += (0.0f - cur.noise) * glideAmp * 4.0f;
            cur.amp    += (1.0f - cur.amp) * glideAmp;
            /* 共振器を空回しして状態を落とし、放射特性の前回値も更新しておく。
               ここを飛ばすと、閉鎖明けの 1 サンプルだけ放射差分が跳ねて
               「カチッ」というクリックになる。 */
            float yy = 0.0f;
            for (int i = 0; i < 8; i++) yy = sm_run(&r[i], 0.0f);
            prevRad = yy;
            /* ノイズ経路の状態も落とす。ここを残すと、閉鎖明けの
               1 サンプル目で radN = noise - 0.9*prevRadN が古い値との
               差で跳ね、「プッ」というクリックになる。 */
            prevRadN = 0.0f;
            burst = 0.0f;
            burstHp = 0.0f;
            biquad_lp_reset(&radNLp);
            out[pos] = 0.0f;
            if (noiseBuf) noiseBuf[pos] = 0.0f;
            if (voicedHist) voicedHist[pos] = 0.0f;
        }
        if (is_stop(p)) {
            clickEnv = (p == YK_P_P || p == YK_P_T || p == YK_P_K) ? 1.00f : 0.45f;
            clickAtk = 0.0f;
        }
        else if (p == YK_P_Z || p == YK_P_J) clickEnv = 0.10f;
        /* 有声破裂音 (が/だ/ば) は閉鎖明けに声帯が最大振幅でいきなり入る。
           その 1 サンプルの段差が「プッ」というクリックになるので、
           本体の先頭 4ms に立ち上がりを付ける。無声破裂のバーストは
           clickAtk 側で別に立ち上げているので、ここでは触らない。 */
        const int voiceAtk = (p == YK_P_B || p == YK_P_D || p == YK_P_G);
        const long bodyStart = pos;

        /* ---- 本体 ---- */
        /* 母音は長めに取る。短すぎると共鳴が立ち上がりきる前に次の
           子音が来て、こもった「プ」のような音になる。 */
        float durMs = ph->dur / spd;
        if (p <= YK_P_O) durMs *= 1.45f;
        const long n = (long)(durMs * sr / 1000.0f);
        const int isPause = (p == YK_P_PAUSE);
        const int isFric = (p == YK_P_S || p == YK_P_SH || p == YK_P_H || p == YK_P_F ||
                            p == YK_P_Z || p == YK_P_J);

        for (long s = 0; s < n && pos < total; s++, pos++) {

            /* パラメータ補間 */
            for (int i = 0; i < 8; i++) cur.f[i] += (target.f[i] - cur.f[i]) * glideF;
            cur.bw += (target.bw - cur.bw) * glideF;
            /* 母音は子音の直後からすぐ声を出す (頼りない立ち上がりを防ぐ)。
               子音自身はノイズで作るので、有声を早く入れても濁らない。 */
            float tV = isPause ? 0.0f : (p <= YK_P_O ? target.voiced : target.voiced);
            cur.voiced += (tV - cur.voiced) * glideAmp;
            cur.amp += (target.amp - cur.amp) * glideAmp;
            cur.gain += (target.gain - cur.gain) * glideAmp;
            cur.noise += (target.noise - cur.noise) * glideAmp;
            cur.noiseF += (target.noiseF - cur.noiseF) * glideF;
            cur.noiseBw += (target.noiseBw - cur.noiseBw) * glideF;

            hFriction += ((isFric ? 1.0f : 0.0f) - hFriction) * glideAmp;

            /* 摩擦音など無声子音は、有声の重なりでノイズが打ち消されないよう
               子音区間では声帯を止める (VOT ぶんの遅れは自然)。 */
            /* 係数更新。目標値は 64 サンプルごとに決め直し、実際の係数は
               1 サンプルずつ目標へ近づける (段差があると 750Hz の側帯波が
               出て「プシュ」という雑音になる)。 */
            if ((s & 63) == 0) {
                for (int i = 0; i < 5; i++)
                    sm_target(&r[i], cur.f[i], bw_for(i, cur.bw, f0), sr, 64.0f);
                reson_set(&nres, cur.noiseF, cur.noiseBw, sr);
            }

            /* F0 */
            float tf = ph->f0 > 0 ? ph->f0 : preset->pitch;
            f0 += (tf - f0) * glideF0;
            vibPhase++;
            float vib = preset->vibrato * sinf(2.0f * (float)M_PI * 5.2f * (float)vibPhase / sr);
            /* ジッタは「揺らぎ」なので 20ms 程度のゆっくりした動きにとどめる。
               ここを速く動かすとフォルマントの狭い山を横切るたびに
               側帯波が出て、母音に「プシュ」という雑音が混ざる。 */
            jitter = jitter * 0.9995f + rnd_next(&rnd) * 0.0006f;
            float fnow = f0 * powf(2.0f, (vib + jitter) / 12.0f);

            /* ---- 声帯励振 (Rosenberg パルスの微分) ----
               微分すると -12dB/oct で減衰する自然なスペクトルになる。
               サンプル微分は 1 サンプルあたりの変化量なので、感度を上げるため
               係数を掛ける。 */
            float exc = glottal_step(&glottal, fnow, sr) * 130.0f;
            /* 声帯パルスの高域を軽く丸める (実際の声道の放射前の減衰) */
            float src = onep_lp(&glottalLp, exc) * cur.voiced;
            /* 息成分 (有声時のブレス)。声道を通すので「ハスキーさ」になり、
               生のノイズを足すより自然。 */
            src += rnd_next(&rnd) * preset->breath * 0.010f * cur.voiced;

            /* ---- 摩擦/破裂ノイズ ----
               摩擦ノイズ。帯域共振器の出力は入力 (白色雑音, rms≈0.58) に
               対して Q のぶんだけ大きくなるので、生成後に正規化する。 */
            float nz = rnd_next(&rnd);
            float nzBand = reson_run(&nres, nz);
            float nzLow = nz - onep_lp(&noiseLp, nz);
            /* 帯域ノイズ: 中心周波数のまわりに集める (実効 rms を一定に) */
            float noise = (nzBand * 1.35f + nzLow * 0.12f) * cur.noise * 4.0f;

            /* ---- 破裂音のバースト ----
               実際の破裂は 5-10ms の短いノイズ。長く鳴らすと「ぷしゅ」
               「ふぁ」という息の音になるので、時定数を 6ms にする。
               帯域は高めに寄せて「カ」「タ」らしくする。 */
            if (clickEnv > 0.0001f) {
                burst += (rnd_next(&rnd) - burst) * 0.90f;
                burstHp += (burst - burstHp) * 0.20f;
                /* 立ち上がり (1.5ms) を付ける。いきなり最大振幅の乱数から
                   始めると、その 1 サンプルの段差が「プッ」というクリックに
                   なる。実際の破裂も 1-2ms かけて立ち上がる。 */
                if (clickAtk < 1.0f) clickAtk += clickAtkStep;
                if (clickAtk > 1.0f) clickAtk = 1.0f;
                noise += (burst - burstHp) * clickEnv * clickAtk;
                clickEnv *= 0.99270f;                        /* τ ≈ 2.5ms */
            }

            /* ---- 並列フォルマントバンク ----
               各フォルマントを独立した共振器で作り、振幅を dB で与えて
               足し合わせる。カスケード (縦続) だと 5 極で打ち切った時点で
               4kHz 以上が -60dB/oct まで落ちて「こもる」が、並列なら
               高次の枝をいくらでも足せて、しかも各枝の山の高さを直接
               設計できる。枝ごとに中心周波数でゲイン 1 に正規化してある。 */
            /* ---- 並列フォルマントバンク ----
               各フォルマントを独立の共振器で作り、その振幅を包絡の設計値に
               する。枝はそれぞれ「自分の中心周波数でゲイン 1」に正規化して
               あるので、振幅はそのまま目標レベルになる。
               カスケード (縦続) は物理的には正しいが、5 極で打ち切ると
               4kHz 以上が -60dB/oct で落ちて「こもる」。
               並列なら高次の枝を足すだけで高域まで設計できる。 */
            float y = src;
            for (int i = 0; i < 8; i++) y = sm_run(&r[i], y);
            /* ---- 放射特性 (唇): +6dB/oct 相当 ---- */
            float rad = y - 0.90f * prevRad;
            prevRad = y;
            /* 摩擦音は別経路 (放射だけ通して貯める) */
            float radN = 0.0f;
            if (noiseBuf) {
                radN = noise - 0.90f * prevRadN;
                prevRadN = noise;
                radN = biquad_lp_run(&radNLp, radN);
            }
            /* 明るさ補正: 折れ点より上を一定量持ち上げるシェルフ。
               減算で作ったハイパスを足すと、高域でゲインが kShelf に
               飽和する (プラトー) ので、帯域全体を均等に持ち上げられる。
               これは「声帯音源の -12dB/oct」を打ち消すためのものなので、
               もともと平らなノイズには掛けない (掛けると歯擦音が耳障り)。 */
            {
                float hp = rad - onep_lp(&shelfLp, rad);
                rad += hp * (kShelf - 1.0f);
            }
            rad = onep_lp(&radLp, rad);

            out[pos] = rad * cur.amp * ph->amp * cur.gain;
            if (noiseBuf) noiseBuf[pos] = radN * cur.amp;
            if (voicedHist) voicedHist[pos] = cur.voiced;
        }

        if (voiceAtk) {
            long len = (long)(0.004f * sr);
            if (len > pos - bodyStart) len = pos - bodyStart;
            for (long k = 0; k < len; k++) {
                float w = 0.5f - 0.5f * cosf((float)M_PI * (float)k / (float)(len > 1 ? len - 1 : 1));
                out[bodyStart + k] *= w;
            }
        }
    }

    /* ---- 窓ごとの有声度 (音量正規化のため) ---- */
    {
        const long win = (long)(0.020f * sr);
        const long nwin = pos / win;
        if (nwin > 0) {
            winVoiced = (float *)calloc((size_t)nwin, sizeof(float));
            if (winVoiced) {
                /* 有声かどうかは「その窓に声帯励振が入っていたか」で判定する。
                   励振は src に比例するので、出力の低域ではなく
                   声帯振幅の履歴で代用する。ここでは出力の高域/低域比ではなく、
                   単純に voicedAmpHist を使う。 */
                for (long w = 0; w < nwin; w++) {
                    double a = 0.0;
                    for (long i = w * win; i < (w + 1) * win && i < pos; i++)
                        a += (double)voicedHist[i];
                    winVoiced[w] = (float)(a / (double)win);
                }
            }
        }
    }

    /* ---- 緩やかな AGC ----
       母音どうしの音量差 (あ と う で 8dB ほど) をならす。時定数は長め
       (立ち上がり 30ms / 戻り 200ms) にして、音節ごとの自然な強弱は残す。
       無音 (閉鎖) には掛けない。 */
    if (pos > 0) {
        const long win = (long)(0.010f * sr);
        /* 20ms 移動平均の RMS 包絡 */
        float env = 0.0f;
        const float aAtk = 1.0f - expf(-(float)win / (0.030f * sr));
        const float aRel = 1.0f - expf(-(float)win / (0.200f * sr));
        const float target = 0.080f;
        for (long i = 0; i + win <= pos; i += win) {
            double acc = 0.0;
            for (long j = 0; j < win; j++) acc += (double)out[i + j] * out[i + j];
            float r = (float)sqrt(acc / (double)win);
            env += (r - env) * (r > env ? aAtk : aRel);
            if (env < 1e-4f) continue;
            float g = target / env;
            /* 持ち上げるのは弱い母音だけ。増やしすぎると無音の残響まで
               大きくなるので、上限は控えめにする。 */
            if (g > 1.25f) g = 1.25f;
            if (g < 0.50f) g = 0.50f;
            /* 窓内でゲインを線形補間して滑らかにつなぐ */
            for (long j = 0; j < win; j++) out[i + j] *= g;
        }
    }

    /* ---- 摩擦音のミックス ----
       摩擦音の最も強い 20ms 窓が、有声部の最強窓より 8dB 下になるように
       そろえる。子音ごとのノイズ量のばらつきをここで吸収する。 */
    if (noiseBuf) {
        const long win = (long)(0.020f * sr);
        double peakV = 0.0, peakN = 0.0;
        for (long i = 0; i + win <= pos; i += win) {
            double av = 0.0, an = 0.0;
            for (long j = 0; j < win; j++) {
                av += (double)out[i + j] * out[i + j];
                an += (double)noiseBuf[i + j] * noiseBuf[i + j];
            }
            if (av > peakV) peakV = av;
            if (an > peakN) peakN = an;
        }
        if (peakN > 1e-12 && peakV > 1e-12) {
            float g = (float)(sqrt(peakV / peakN) * 0.62);   /* -4dB */
            for (long i = 0; i < pos; i++) out[i] += noiseBuf[i] * g;
        }
        free(noiseBuf);
        noiseBuf = NULL;
    }

    /* ---- DC 除去 ---- */
    {
        float dcX = 0.0f, dcY = 0.0f;
        for (long i = 0; i < pos; i++) {
            float x = out[i];
            float y = x - dcX + 0.9998f * dcY;
            dcX = x; dcY = y;
            out[i] = y;
        }
    }

    /* ---- 基準レベル: 有声フレームの RMS 中央値 ---- */
    {
        const long win = (long)(0.020f * sr);
        const long nwin = pos / win;
        if (nwin > 0) {
            float *rms = (float *)malloc(sizeof(float) * (size_t)nwin);
            if (rms) {
                long nv = 0;
                for (long w = 0; w < nwin; w++) {
                    /* 有声でない窓 (摩擦音・閉鎖) は除外する。
                       有声窓は位相がばらけるので 20ms 平均の RMS は
                       母音らしい値になる。 */
                    if (winVoiced) {
                        float vv = winVoiced[w];
                        if (vv < 0.85f) { rms[w] = 0.0f; continue; }
                    }
                    double acc = 0.0;
                    for (long i = w * win; i < (w + 1) * win; i++) acc += (double)out[i] * out[i];
                    float v = (float)sqrt(acc / (double)win);
                    rms[w] = v;
                    if (v > 1e-4f) nv++;
                }
                if (nv == 0) {   /* 有声窓が無ければ全窓で */
                    for (long w = 0; w < nwin; w++) {
                        double acc = 0.0;
                        for (long i = w * win; i < (w + 1) * win; i++) acc += (double)out[i] * out[i];
                        float v = (float)sqrt(acc / (double)win);
                        rms[w] = v;
                        if (v > 1e-4f) nv++;
                    }
                }
                if (nv > 0) {
                    float *tmp = (float *)malloc(sizeof(float) * (size_t)nv);
                    if (tmp) {
                        long c = 0;
                        for (long w = 0; w < nwin; w++) if (rms[w] > 1e-4f) tmp[c++] = rms[w];
                        for (long i = 1; i < c; i++) {
                            float v = tmp[i]; long j = i - 1;
                            while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; }
                            tmp[j + 1] = v;
                        }
                        /* 有声フレームの中央値だと、無音 (閉鎖) の直前直後の
                           小さいフレームに引きずられる。75 パーセンタイルを
                           使うと「母音の実力」に合わせられる。 */
                        float med = tmp[(c * 3) / 4];
                        free(tmp);
                        if (med > 0.0f) {
                            float g = 0.145f / med;    /* 有声フレームの RMS を -21dBFS 程度に */
                            if (g > 1e4f) g = 1e4f;
                            if (g < 1e-3f) g = 1e-3f;
                            for (long i = 0; i < pos; i++) out[i] *= g;
                        }
                    }
                }
                free(rms);
            }
        }
        free(winVoiced);
        winVoiced = NULL;
    }
    free(voicedHist);
    voicedHist = NULL;

    /* ---- ピークリミッタ (包絡追従型) ----
       サンプルごとの tanh は、共振器の過渡で 1 サンプルだけ跳ねたときに
       波形を歪ませる。包絡を見てゆっくりゲインを下げる方式にすると、
       母音の立ち上がりの「カチッ」を潰さずにピークだけ抑えられる。 */
    {
        const float thresh = 0.85f;
        const float aAtk = 1.0f - expf(-1.0f / (0.002f * sr));   /* 2ms */
        const float aRel = 1.0f - expf(-1.0f / (0.150f * sr));   /* 150ms */
        float env = 0.0f;
        for (long i = 0; i < pos; i++) {
            float ax = fabsf(out[i]);
            env += (ax - env) * (ax > env ? aAtk : aRel);
            float g = 1.0f;
            if (env > thresh) g = thresh / env;
            out[i] *= g;
        }
    }

    /* ---- 語尾の減衰 (「プツ」対策) ----
       母音や摩擦音が大きい振幅のまま切れて無音に入ると、波形が
       いきなり 0 になる段差が「プツ」というクリックになる。
       1ms ごとの RMS 包絡で無音区間を見つけ、その直前 40ms を
       コサインで滑らかに落とす。サンプル値ではなく包絡で判定するので、
       ゼロクロスを無音と誤認しない。立ち上がりには触らない。 */
    if (pos > 0) {
        const long hop = (long)(0.001f * sr) > 0 ? (long)(0.001f * sr) : 1;
        const long nh = pos / hop + 1;
        const long rel = (long)(0.040f * sr);
        for (long h = 1; h < nh; h++) {
            long b = h * hop;
            if (b >= pos) break;
            /* この 1ms が無音か */
            double e = 0.0;
            for (long i = b; i < b + hop && i < pos; i++) e += (double)out[i] * out[i];
            if (sqrt(e / (double)hop) > 0.0015) continue;
            /* 直前に続いていた有音の終端 = b。その手前を下げる。 */
            long len = rel;
            if (len > b) len = b;
            /* k=0 が無音の直前 (= 一番新しい) なので、そこを最小に、
               過去 (k=len-1) に向かって 1 に戻す。 */
            for (long k = 0; k < len; k++) {
                long idx = b - 1 - k;
                float w = 0.5f - 0.5f * cosf((float)M_PI * (float)k / (float)(len > 1 ? len - 1 : 1));
                if (w > 1.0f) w = 1.0f;
                if (w < 0.0f) w = 0.0f;
                out[idx] *= w;
            }
            /* 無音が続く限り同じ場所を何度も削らないよう、次の有音まで飛ばす */
            while (h < nh) {
                long bb = h * hop;
                if (bb + hop >= pos) { h = nh; break; }
                double e2 = 0.0;
                for (long i = bb; i < bb + hop; i++) e2 += (double)out[i] * out[i];
                if (sqrt(e2 / (double)hop) > 0.0015) break;
                h++;
            }
        }
    }

    /* ---- 前後フェード ---- */
    {
        long fade = (long)(0.006f * sr);
        if (fade > pos / 4) fade = pos / 4;
        for (long i = 0; i < fade; i++) {
            float w = (float)i / (float)fade;
            out[i] *= w;
            out[pos - 1 - i] *= w;
        }
    }

    return (int)pos;
}

int yk_speak_kana(const char *kana, const YkPreset *preset, float *out, int out_cap,
                  char *reading, int reading_cap)
{
    static YkPlan plan;   /* 大きいので静的領域 (単一スレッド前提) */
    int rc = yk_plan_from_kana(kana, preset, &plan);
    if (reading && reading_cap > 0) {
        int n = (int)strlen(plan.reading);
        if (n >= reading_cap) n = reading_cap - 1;
        if (n < 0) n = 0;
        memcpy(reading, plan.reading, (size_t)n);
        reading[n] = '\0';
    }
    if (rc != 0) return rc;
    return yk_synthesize(&plan, preset, out, out_cap);
}

const char *yk_version(void) { return "0.1.0"; }
