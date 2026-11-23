/* かな列 -> 音素列 (発話プラン) */
#include "internal.h"
#include <string.h>
#include <stdlib.h>

int yk_plan_push(YkPlan *p, YkPhone ph, float dur, float f0, float amp)
{
    if (p->count >= YK_MAX_PHONEMES) return -1;
    YkPhoneme *x = &p->items[p->count++];
    x->phone = (uint8_t)ph;
    x->dur = dur;
    x->f0 = f0;
    x->amp = amp;
    return 0;
}

/* --- UTF-8 デコード（かな列専用の最小実装） --- */
static const char *next_char(const char *s, uint32_t *cp)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *cp = u[0]; return s + 1; }
    if ((u[0] & 0xE0) == 0xC0 && u[1]) { *cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); return s + 2; }
    if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) {
        *cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        return s + 3;
    }
    *cp = 0; return s + 1;
}

/* かな (ひらがな/カタカナ) の合成定義 */
typedef struct {
    uint32_t cp;       /* 代表コードポイント */
    uint8_t  cons;     /* 子音音素 (なければ 0xFF) */
    uint8_t  vowel;    /* 母音音素 */
    uint8_t  smallY;   /* 拗音の母音として使うか (ゃゅょ) */
} KanaDef;

#define NN 0xFF
static const KanaDef KANA[] = {
    {0x3042, NN, YK_P_A, 0}, {0x3044, NN, YK_P_I, 0}, {0x3046, NN, YK_P_U, 0},
    {0x3048, NN, YK_P_E, 0}, {0x304A, NN, YK_P_O, 0},
    {0x304B, YK_P_K, YK_P_A, 0}, {0x304D, YK_P_K, YK_P_I, 0}, {0x304F, YK_P_K, YK_P_U, 0},
    {0x3051, YK_P_K, YK_P_E, 0}, {0x3053, YK_P_K, YK_P_O, 0},
    {0x3055, YK_P_S, YK_P_A, 0}, {0x3057, YK_P_SH, YK_P_I, 0}, {0x3059, YK_P_S, YK_P_U, 0},
    {0x305B, YK_P_S, YK_P_E, 0}, {0x305D, YK_P_S, YK_P_O, 0},
    {0x305F, YK_P_T, YK_P_A, 0}, {0x3061, YK_P_T, YK_P_I, 0}, {0x3064, YK_P_T, YK_P_U, 0},
    {0x3066, YK_P_T, YK_P_E, 0}, {0x3068, YK_P_T, YK_P_O, 0},
    {0x306A, YK_P_NG, YK_P_A, 0}, {0x306B, YK_P_NG, YK_P_I, 0}, {0x306C, YK_P_NG, YK_P_U, 0},
    {0x306D, YK_P_NG, YK_P_E, 0}, {0x306E, YK_P_NG, YK_P_O, 0},
    {0x306F, YK_P_H, YK_P_A, 0}, {0x3072, YK_P_H, YK_P_I, 0}, {0x3075, YK_P_F, YK_P_U, 0},
    {0x3078, YK_P_H, YK_P_E, 0}, {0x307B, YK_P_H, YK_P_O, 0},
    {0x307E, YK_P_M, YK_P_A, 0}, {0x307F, YK_P_M, YK_P_I, 0}, {0x3080, YK_P_M, YK_P_U, 0},
    {0x3081, YK_P_M, YK_P_E, 0}, {0x3082, YK_P_M, YK_P_O, 0},
    {0x3084, YK_P_Y, YK_P_A, 0}, {0x3086, YK_P_Y, YK_P_U, 0}, {0x3088, YK_P_Y, YK_P_O, 0},
    {0x3089, YK_P_R, YK_P_A, 0}, {0x308A, YK_P_R, YK_P_I, 0}, {0x308B, YK_P_R, YK_P_U, 0},
    {0x308C, YK_P_R, YK_P_E, 0}, {0x308D, YK_P_R, YK_P_O, 0},
    {0x308F, YK_P_W, YK_P_A, 0}, {0x3092, YK_P_W, YK_P_O, 0},
    {0x3093, YK_P_N, YK_P_N, 0},                        /* ん */
    {0x3063, YK_P_CL, YK_P_CL, 0},                      /* っ */
    {0x3041, NN, YK_P_A, 1}, {0x3043, NN, YK_P_I, 1}, {0x3045, NN, YK_P_U, 1},
    {0x3047, NN, YK_P_E, 1}, {0x3049, NN, YK_P_O, 1},
    {0x3083, YK_P_Y, YK_P_A, 1}, {0x3085, YK_P_Y, YK_P_U, 1}, {0x3087, YK_P_Y, YK_P_O, 1},
    {0x308E, YK_P_W, YK_P_A, 1},
    /* ゔ行などは簡略化 */
    {0x3094, YK_P_W, YK_P_U, 0},
    {0x30FC, 0xFE, 0, 0},   /* 長音記号: 直前の母音を伸ばす (特別扱い) */
};
static const int NKANA = (int)(sizeof(KANA) / sizeof(KANA[0]));

static const KanaDef *find_kana(uint32_t cp)
{
    /* カタカナはひらがなへ写像 */
    if (cp >= 0x30A1 && cp <= 0x30F6) cp -= 0x60;
    if (cp == 0x30FC) cp = 0x30FC;
    for (int i = 0; i < NKANA; i++)
        if (KANA[i].cp == cp) return &KANA[i];
    return NULL;
}

/* カタカナ -> ひらがな & 読み文字列の追記 */
static int append_reading_n(char *dst, int cap, int pos, const char *src, int n)
{
    if (n < 0) n = (int)strlen(src);
    if (pos + n >= cap) n = cap - pos - 1;
    if (n > 0) memcpy(dst + pos, src, (size_t)n);
    return pos + (n > 0 ? n : 0);
}

int yk_plan_from_kana(const char *kana, const YkPreset *preset, YkPlan *out)
{
    if (!kana || !preset || !out) return -1;
    memset(out, 0, sizeof(*out));

    const char *s = kana;
    uint32_t cp;
    YkPhone last_vowel = YK_P_A;
    int have_vowel = 0;
    int reading_pos = 0;

    /* モーラ単位で一度集めてから、アクセント/抑扬を割り当てる */
    while (*s) {
        const char *prev = s;
        s = next_char(s, &cp);
        if (cp == 0) break;
        if (cp == ' ' || cp == '\t' || cp == '\n') {
            yk_plan_push(out, YK_P_PAUSE, 160, preset->pitch, 1.0f);
            continue;
        }
        if (cp == 0x3001 || cp == 0x3002 || cp == ',' || cp == '.' ||
            cp == '!' || cp == '?' || cp == 0xFF01 || cp == 0xFF1F ||
            cp == 0xFF0C || cp == 0xFF0E) {
            /* 句読点は長めの休止 */
            yk_plan_push(out, YK_P_PAUSE, 320, preset->pitch, 1.0f);
            continue;
        }
        /* 長音 */
        if (cp == 0x30FC || cp == 0x301C) {
            if (have_vowel) {
                const YkPhoneInfo *vi = yk_phone_info(last_vowel);
                yk_plan_push(out, last_vowel, vi->dur * 0.9f, preset->pitch, 1.0f);
                reading_pos = append_reading_n(out->reading, (int)sizeof(out->reading), reading_pos, "ー", -1);
            }
            continue;
        }
        const KanaDef *k = find_kana(cp);
        if (!k) { continue; }
        reading_pos = append_reading_n(out->reading, (int)sizeof(out->reading),
                                     reading_pos, prev, (int)(s - prev));
        if (k->cons == 0xFE) continue; /* 長音は上で処理済み */

        if (k->cons == YK_P_CL) {          /* っ: 次の子音の閉鎖を伸ばす */
            yk_plan_push(out, YK_P_CL, 90, preset->pitch, 0.0f);
            continue;
        }
        if (k->cp == 0x3093) {             /* ん */
            yk_plan_push(out, YK_P_N, yk_phone_info(YK_P_N)->dur, preset->pitch, 1.0f);
            have_vowel = 0;
            continue;
        }
        /* 子音 */
        if (k->cons != NN) {
            /* ぱ行・半濁は p、ば行は b など、テーブル側で音素が決まっている。
               濁点・半濁点は事前に正規化される想定。 */
            yk_plan_push(out, (YkPhone)k->cons, yk_phone_info(k->cons)->dur,
                         preset->pitch, 1.0f);
        }
        /* 拗音 (ゃゅょ) は直前の子音の遷移として扱う */
        const char *peek = s;
        uint32_t cp2;
        if (*peek) {
            const char *after = next_char(peek, &cp2);
            const KanaDef *k2 = find_kana(cp2);
            if (k2 && k2->smallY && k->cons != NN && k->cons != YK_P_CL) {
                s = after;
                reading_pos = append_reading_n(out->reading, (int)sizeof(out->reading),
                                             reading_pos, peek, (int)(s - peek));
                /* 拗音化: 子音のフォルマントを近づけ、母音を小書きのものに */
                yk_plan_push(out, k2->vowel, yk_phone_info(k2->vowel)->dur * 1.05f,
                             preset->pitch, 1.0f);
                last_vowel = (YkPhone)k2->vowel;
                have_vowel = 1;
                continue;
            }
        }
        yk_plan_push(out, (YkPhone)k->vowel, yk_phone_info(k->vowel)->dur,
                     preset->pitch, 1.0f);
        last_vowel = (YkPhone)k->vowel;
        have_vowel = 1;
    }

    if (out->count == 0) return -2;

    /* --- 韻律: モーラごとのピッチを決める -----------------------------
       ゆっくり声は「ほぼ平坦＋文末で軽く下がる」のが特徴。
       ここでは 2 モーラのアクセント単位を擬似的に作り、
       文末 2 モーラを下降させる。 */
    float base = preset->pitch;
    int momo = 0;
    int total = out->count;
    int last_pause = -1;
    for (int i = 0; i < total; i++)
        if (out->items[i].phone == YK_P_PAUSE) last_pause = i;

    for (int i = 0; i < total; i++) {
        YkPhoneme *p = &out->items[i];
        if (p->phone == YK_P_PAUSE) { momo = 0; continue; }
        if (p->phone == YK_P_CL) continue;
        int is_vowel = p->phone <= YK_P_O || p->phone == YK_P_Y ||
                       p->phone == YK_P_W || p->phone == YK_P_R;
        if (!is_vowel) { p->amp = 0.7f; continue; }
        /* 呼気の山 (アクセント核) */
        float acc = (momo % 2 == 0) ? 1.0f : 0.86f;
        /* 全体をゆるく下げる + 文末で下げ */
        float decl = 1.0f - 0.06f * ((float)i / (float)total);
        float tail = 1.0f;
        if (last_pause < 0) last_pause = total;
        int remain = 0;
        for (int j = i + 1; j < total; j++)
            if (out->items[j].phone == YK_P_PAUSE) break; else remain++;
        if (remain <= 1) tail = 0.88f;
        else if (remain <= 3) tail = 0.94f;
        float semis = -2.2f * (1.0f - acc) - 1.6f * (1.0f - decl) - 4.0f * (1.0f - tail);
        p->f0 = base * (float)(1.0 + preset->intonation * semis / 12.0);
        p->amp = 1.0f;
        momo++;
    }
    return 0;
}
