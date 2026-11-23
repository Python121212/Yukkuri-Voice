/* フォルマント合成器本体
 *
 *   声帯パルス (ノコギリ波を歪ませたもの + ジッタ/ビブラート) --\
 *   ノイズ源 (摩擦音 / 息成分) -----------------------------------> 5 段の共振器 -> 放射特性 -> 自動ゲイン
 *
 *  - 共振器はピークゲインを 1 に正規化する (48kHz では Q が非常に高くなるため必須)
 *  - パラメータはサンプルごとに補間するので、子音→母音のフォルマント遷移がなめらか
 *  - 最後に自動ゲイン (有声区間のピークを -1.5dBFS 付近へ) を掛ける
 */
#include "internal.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ *
 * 2 次共振器 (Klatt 型, ピークゲイン 1 正規化)
 * ------------------------------------------------------------------ */
typedef struct {
    float a, b, c;
    float y1, y2;
} Reson;

static void reson_clear(Reson *r) { r->y1 = r->y2 = 0.0f; }

static void reson_set(Reson *r, float f, float bw, float sr)
{
    const float t = 1.0f / sr;
    const float rho = expf(-(float)M_PI * bw * t);
    const float th = 2.0f * (float)M_PI * f * t;
    r->b = 2.0f * rho * cosf(th);
    r->c = -rho * rho;
    /* |1 - b e^{-jθ} - c e^{-2jθ}| を計算して a に与えると、
       共振周波数でのゲインがちょうど 1 になる */
    const float c1 = cosf(th), s1 = sinf(th);
    const float c2 = cosf(2.0f * th), s2 = sinf(2.0f * th);
    const float zr = 1.0f - r->b * c1 - r->c * c2;
    const float zi = r->b * s1 + r->c * s2;
    float mag = sqrtf(zr * zr + zi * zi);
    if (mag < 1e-4f) mag = 1e-4f;
    r->a = mag;
}

static inline float reson_run(Reson *r, float x)
{
    float y = r->a * x + r->b * r->y1 + r->c * r->y2;
    r->y2 = r->y1;
    r->y1 = y;
    return y;
}

typedef struct { float y, a; } OneS;
static inline float onep_lp(OneS *s, float x) { s->y += s->a * (x - s->y); return s->y; }
static inline void onep_set(OneS *s, float f, float sr)
{ s->a = 1.0f - expf(-2.0f * (float)M_PI * f / sr); }

typedef struct { uint32_t s; } Rnd;
static inline float rnd_next(Rnd *r)
{
    r->s ^= r->s << 13; r->s ^= r->s >> 17; r->s ^= r->s << 5;
    return (float)((int32_t)r->s) * (1.0f / 2147483648.0f);
}

/* ------------------------------------------------------------------ *
 * 目標フレーム
 * ------------------------------------------------------------------ */
typedef struct {
    float f[5];
    float bw;
    float voiced;
    float noise;
    float noiseHp;
    float amp;
    float noiseF;     /* ノイズの中心周波数 (摩擦音の帯域) */
    float noiseBw;
} Frame;

static void frame_for_phone(YkPhone p, const YkPreset *ps, Frame *fr)
{
    const YkPhoneInfo *pi = yk_phone_info(p);
    memset(fr, 0, sizeof(*fr));
    fr->bw = pi->bw;
    fr->voiced = pi->voiced ? pi->amp : 0.0f;
    fr->noise = pi->noise;
    fr->noiseHp = pi->noiseHp;
    fr->amp = pi->amp;
    fr->noiseF = 4500.0f;
    fr->noiseBw = 2500.0f;
    if (p <= YK_P_O) {
        for (int i = 0; i < 5; i++) fr->f[i] = YK_VOWEL_F[p][i] * ps->formantScale;
    } else if (pi->formant[0] > 0) {
        for (int i = 0; i < 5; i++) fr->f[i] = (float)pi->formant[i] * ps->formantScale;
    } else {
        for (int i = 0; i < 5; i++) fr->f[i] = 500.0f * ps->formantScale;
    }
    const float lim = 0.44f * (float)YK_SR;
    for (int i = 0; i < 5; i++)
        if (fr->f[i] > lim) fr->f[i] = lim;
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

/* 有声/無声の破裂音は閉鎖区間 (完全な無音) を持つ */
static float closure_ms(YkPhone p)
{
    switch (p) {
    case YK_P_P: case YK_P_T: case YK_P_K: return 60.0f;
    case YK_P_B: case YK_P_D: case YK_P_G: return 45.0f;
    default: return 0.0f;
    }
}

static int is_stop(YkPhone p)
{
    return p == YK_P_B || p == YK_P_D || p == YK_P_G ||
           p == YK_P_P || p == YK_P_T || p == YK_P_K;
}

/* ------------------------------------------------------------------ */
int yk_synthesize(const YkPlan *plan, const YkPreset *preset, float *out, int out_cap)
{
    if (!plan || !preset || !out || out_cap <= 0 || plan->count == 0) return -1;

    const float sr = (float)YK_SR;
    const float spd = preset->speed > 0.1f ? preset->speed : 0.8f;

    /* ---- 必要なサンプル数 ---- */
    long total = 0;
    for (int i = 0; i < plan->count; i++) {
        YkPhone p = (YkPhone)plan->items[i].phone;
        float ms = plan->items[i].dur / spd + closure_ms(p);
        if (p == YK_P_CL) ms = plan->items[i].dur / spd;   /* 促音は閉鎖の伸長 */
        total += (long)(ms * sr / 1000.0f);
    }
    total += (long)(0.12f * sr);
    if (total > out_cap) total = out_cap;

    /* ---- 状態 ---- */
    Reson r[5];
    for (int i = 0; i < 5; i++) { reson_clear(&r[i]); reson_set(&r[i], 500, 90, sr); }
    Reson nres; reson_clear(&nres); reson_set(&nres, 4500, 2500, sr);
    OneS srcLp, outLp, nzHp;
    onep_set(&srcLp, 1400, sr);
    onep_set(&outLp, 9000, sr);
    onep_set(&nzHp, 1500, sr);

    Rnd rnd = { 0x2545F491u };
    float phase = 0.0f;
    Frame cur, target;
    frame_for_phone(YK_P_O, preset, &cur);
    memset(&target, 0, sizeof(target));

    const float glide    = 1.0f - expf(-1.0f / (0.016f * sr));
    const float glideAmp = 1.0f - expf(-1.0f / (0.010f * sr));

    long pos = 0;
    float f0 = preset->pitch;
    float ampLevel = 0.0f;
    float clickEnv = 0.0f;
    float prevY = 0.0f;
    float jitter = 0.0f;
    long vibPhase = 0;
    long closureLeft = 0;

    for (int idx = 0; idx < plan->count && pos < total; idx++) {
        const YkPhoneme *ph = &plan->items[idx];
        YkPhone p = (YkPhone)ph->phone;

        /* 促音: 次の破裂音の閉鎖を伸ばす */
        if (p == YK_P_CL) { closureLeft += (long)(ph->dur / spd * sr / 1000.0f); continue; }

        frame_for_phone(p, preset, &target);
        if (p != YK_P_PAUSE && p > YK_P_O) {
            int pv = find_vowel_back(plan, idx);
            int nv = find_vowel_fwd(plan, idx);
            float tmp[5]; int have = 0;
            if (pv >= 0) { for (int i = 0; i < 5; i++) tmp[i] = YK_VOWEL_F[pv][i] * preset->formantScale; have = 1; }
            if (nv >= 0) {
                for (int i = 0; i < 5; i++) {
                    float v = YK_VOWEL_F[nv][i] * preset->formantScale;
                    tmp[i] = have ? (tmp[i] + v) * 0.5f : v;
                }
                have = 1;
            }
            if (have) {
                float k = (p == YK_P_N || p == YK_P_M || p == YK_P_NG || p == YK_P_R) ? 0.9f : 0.65f;
                for (int i = 0; i < 5; i++)
                    target.f[i] = target.f[i] * (1.0f - k) + tmp[i] * k;
            }
        }

        /* 破裂音は閉鎖 + 促音分の無音から始まる */
        long n_clo = (long)(closure_ms(p) * sr / 1000.0f) + closureLeft;
        closureLeft = 0;
        for (long s = 0; s < n_clo && pos < total; s++, pos++) {
            cur.voiced *= (1.0f - glideAmp);
            cur.noise  *= (1.0f - glideAmp);
            ampLevel   *= (1.0f - glideAmp);
            float y = 0.0f;
            for (int i = 0; i < 5; i++) y = reson_run(&r[i], y);
            out[pos] = onep_lp(&outLp, y) * 0.05f;
            prevY = 0.0f;
        }
        if (is_stop(p))
            clickEnv = (p == YK_P_P || p == YK_P_T || p == YK_P_K) ? 0.55f : 0.22f;
        else if (p == YK_P_Z || p == YK_P_J) clickEnv = 0.10f;

        long n = (long)(ph->dur / spd * sr / 1000.0f);
        const int isVowel = (p <= YK_P_O);
        const int isPause = (p == YK_P_PAUSE);

        for (long s = 0; s < n && pos < total; s++, pos++) {
            /* ---- パラメータ補間 ---- */
            for (int i = 0; i < 5; i++) cur.f[i] += (target.f[i] - cur.f[i]) * glide;
            cur.bw += (target.bw - cur.bw) * glide;
            float tVoiced = isPause ? 0.0f : target.voiced;
            cur.voiced += (tVoiced - cur.voiced) * (isVowel ? glideAmp * 0.6f : glideAmp);
            cur.amp += (target.amp - cur.amp) * glideAmp;
            cur.noise += (target.noise - cur.noise) * glideAmp;
            cur.noiseHp += (target.noiseHp - cur.noiseHp) * glideAmp;
            cur.noiseF += (target.noiseF - cur.noiseF) * glide; /* 摩擦音の帯域移動は無視できる程度 */
            cur.noiseBw += (target.noiseBw - cur.noiseBw) * glide;

            /* ---- 共振器 ---- */
            if ((s & 31) == 0) {
                for (int i = 0; i < 5; i++) {
                    float bw = cur.bw * (1.0f + 0.5f * (float)i);
                    float f = cur.f[i];
                    if (f < 120.0f) f = 120.0f;
                    reson_set(&r[i], f, bw, sr);
                }
            }

            /* ---- F0 ---- */
            float tf = ph->f0 > 0 ? ph->f0 : preset->pitch;
            f0 += (tf - f0) * (1.0f - expf(-1.0f / (0.040f * sr)));
            vibPhase++;
            float vib = 0.0f;
            if (preset->vibrato > 0.0f)
                vib = preset->vibrato * sinf(2.0f * (float)M_PI * 5.4f * (float)vibPhase / sr);
            jitter = jitter * 0.99f + rnd_next(&rnd) * 0.012f;
            float fnow = f0 * powf(2.0f, (vib + jitter) / 12.0f);

            /* ---- 声帯パルス ---- */
            phase += fnow / sr;
            if (phase >= 1.0f) phase -= 1.0f;
            float saw = 2.0f * phase - 1.0f;
            float pulse = saw - 0.4f * saw * saw * saw;   /* 声帯の開閉に近い形 */
            float src = onep_lp(&srcLp, pulse) * cur.voiced;

            /* 破裂音のバースト */
            if (clickEnv > 0.001f) {
                src += rnd_next(&rnd) * clickEnv;
                clickEnv *= 0.9985f;
            }

            /* ---- ノイズ ---- */
            float nz = rnd_next(&rnd);
            if (cur.noiseHp > 0.001f) {
                float hp = nz - onep_lp(&nzHp, nz);
                nz = hp * cur.noiseHp + nz * (1.0f - cur.noiseHp) * 0.6f;
            }
            float noise = nz * cur.noise;

            /* 息成分 */
            src += rnd_next(&rnd) * preset->breath * 0.05f * cur.voiced;

            float x = src + noise;

            /* ---- カスケード共振器 ---- */
            float y = x;
            for (int i = 0; i < 5; i++) y = reson_run(&r[i], y);

            /* ---- 放射特性 (微分) ---- */
            float outS = onep_lp(&outLp, y - prevY);
            prevY = y;

            float tAmp = cur.amp * ph->amp;
            ampLevel += (tAmp - ampLevel) * glideAmp;
            out[pos] = outS * ampLevel;
        }
    }

    /* ---- DC 除去 (1 次ハイパス) ---- */
    float dcX = 0.0f, dcY = 0.0f;
    for (long i = 0; i < pos; i++) {
        float x = out[i];
        float y = x - dcX + 0.9995f * dcY;
        dcX = x; dcY = y;
        out[i] = y;
    }

    /* ---- 自動ゲイン: 有声区間の 99.5% ピークを 0.80 に ---- */
    float peak = 0.0f;
    for (long i = 0; i < pos; i++) { float a = fabsf(out[i]); if (a > peak) peak = a; }
    if (peak > 1e-5f) {
        float g = 0.80f / peak;
        for (long i = 0; i < pos; i++) out[i] *= g;
    }

    /* ---- ソフトクリップ ---- */
    for (long i = 0; i < pos; i++) {
        float y = out[i];
        if (y > 1.0f) y = 1.0f;
        if (y < -1.0f) y = -1.0f;
        out[i] = y * (1.0f - 0.18f * y * y);
    }

    long fade = (long)(0.008f * sr);
    for (long i = 0; i < fade && i < pos; i++) out[i] *= (float)i / (float)fade;
    for (long i = 0; i < fade && i < pos; i++) out[pos - 1 - i] *= (float)i / (float)fade;

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
        memcpy(reading, plan.reading, (size_t)n);
        reading[n] = '\0';
    }
    if (rc != 0) return rc;
    return yk_synthesize(&plan, preset, out, out_cap);
}

const char *yk_version(void) { return "0.1.0"; }
