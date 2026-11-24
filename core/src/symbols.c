/* 音素記号列 -> 音素列 (発話プラン)。
 *
 * 「かなを渡す」のではなく「発音そのもの」を指定できるようにする層。
 * ローマ字・かな・ISO 風の何でも受け付け、日本語の異音規則
 * (ん の同化、母音の無声化、ら行の弾き音、つ/ち/ず の破擦化、
 *  語末の長音) を自動で当てる。
 *
 * 対応する書き方 (大文字小文字は区別しない):
 *   母音      a i u e o
 *   かな      あ い う え お / ア イ ウ エ オ
 *   子音+母音 ka ki ku ke ko, sha shu sho, cha chu cho, tsu, ...
 *   撥音      N  n'  ん  ン
 *   促音      Q  (または「っ」「ッ」) -> 次の子音の閉鎖を伸ばす
 *   長音      :  (または「ー」) -> 直前の母音を 1 モーラ伸ばす
 *   休止      ,  .  、 。 / (または「-」) -> 休止
 *   アクセント 母音の直後の ' (上げ) と ` (下げ)
 *
 * かなを混ぜてもよい (「ゆっくりして ne」)。変換できない文字は読み飛ばす。
 */
#include "internal.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* ポータブルな大文字小文字無視の比較 (C99 に strcasecmp が無い)         */
/* ------------------------------------------------------------------ */
static int lower_ascii(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int cmp_ci_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int x = lower_ascii((unsigned char)a[i]);
        int y = lower_ascii((unsigned char)b[i]);
        if (x != y) return x - y;
        if (x == 0) return 0;
    }
    return 0;
}

static int cmp_ci(const char *a, const char *b)
{
    return cmp_ci_n(a, b, strlen(b) + 1);
}

/* ------------------------------------------------------------------ */
/* 音素記号の逆引き (表示用)                                            */
/* ------------------------------------------------------------------ */
static const char *const PHONE_SYM[YK_P_COUNT] = {
    "a", "i", "u", "e", "o",
    "N", "Q",
    "b", "d", "g", "p", "t", "k",
    "m", "ng",
    "n",
    "s", "sh", "h", "f",
    "z", "j",
    "r", "y", "w",
    "\xCA\x94",   /* ʔ (声門破裂)。表示用 */
    "ts", "ch", "dz", "U",
    "_"
};

const char *yk_phone_symbol(YkPhone p)
{
    if (p < 0 || p >= YK_P_COUNT) return "?";
    return PHONE_SYM[p];
}

/* 音素記号 -> 音素。見つからなければ YK_P_COUNT を返す。 */
YkPhone yk_phone_from_symbol(const char *sym)
{
    if (!sym || !*sym) return YK_P_COUNT;
    /* 大文字小文字を無視して比較する (N と n は別物なので先に完全一致) */
    for (int i = 0; i < YK_P_COUNT; i++) {
        if (strcmp(sym, PHONE_SYM[i]) == 0) return (YkPhone)i;
    }
    /* 声門破裂は "?" でも受け付ける (表示は ʔ) */
    if (strcmp(sym, "?") == 0) return YK_P_GLOTTAL;
    for (int i = 0; i < YK_P_COUNT; i++) {
        if (cmp_ci(sym, PHONE_SYM[i]) == 0) {
            /* 小文字の n は「な行の n」、大文字の N は撥音。ここに来るのは
               完全一致しなかった場合だけなので、そのまま返してよい。 */
            return (YkPhone)i;
        }
    }
    return YK_P_COUNT;
}

/* ------------------------------------------------------------------ */
/* かな -> 音素 (自動変換)                                              */
/* ------------------------------------------------------------------ */
#include "internal.h"

/* かな 1 文字を (子音, 母音) に分解する。plan.c の KANA 表と同じ内容だが、
   ここでは「記号列の中に混ざったかな」を扱うために独立して持つ。 */
static int kana_to_cv(uint32_t cp, int *cons, int *vowel)
{
    if (cp >= 0x30A1 && cp <= 0x30F6) cp -= 0x60;   /* カタカナ -> ひらがな */
    switch (cp) {
    /* 母音 */
    case 0x3042: *cons = -1; *vowel = YK_P_A; return 1;
    case 0x3044: *cons = -1; *vowel = YK_P_I; return 1;
    case 0x3046: *cons = -1; *vowel = YK_P_U; return 1;
    case 0x3048: *cons = -1; *vowel = YK_P_E; return 1;
    case 0x304A: *cons = -1; *vowel = YK_P_O; return 1;
    /* か行 */
    case 0x304B: *cons = YK_P_K; *vowel = YK_P_A; return 1;
    case 0x304D: *cons = YK_P_K; *vowel = YK_P_I; return 1;
    case 0x304F: *cons = YK_P_K; *vowel = YK_P_U; return 1;
    case 0x3051: *cons = YK_P_K; *vowel = YK_P_E; return 1;
    case 0x3053: *cons = YK_P_K; *vowel = YK_P_O; return 1;
    /* が行 */
    case 0x304C: *cons = YK_P_G; *vowel = YK_P_A; return 1;
    case 0x304E: *cons = YK_P_G; *vowel = YK_P_I; return 1;
    case 0x3050: *cons = YK_P_G; *vowel = YK_P_U; return 1;
    case 0x3052: *cons = YK_P_G; *vowel = YK_P_E; return 1;
    case 0x3054: *cons = YK_P_G; *vowel = YK_P_O; return 1;
    /* さ行 */
    case 0x3055: *cons = YK_P_S; *vowel = YK_P_A; return 1;
    case 0x3057: *cons = YK_P_SH; *vowel = YK_P_I; return 1;
    case 0x3059: *cons = YK_P_S; *vowel = YK_P_U; return 1;
    case 0x305B: *cons = YK_P_S; *vowel = YK_P_E; return 1;
    case 0x305D: *cons = YK_P_S; *vowel = YK_P_O; return 1;
    /* ざ行 */
    case 0x3056: *cons = YK_P_Z; *vowel = YK_P_A; return 1;
    case 0x3058: *cons = YK_P_J; *vowel = YK_P_I; return 1;
    case 0x305A: *cons = YK_P_Z; *vowel = YK_P_U; return 1;
    case 0x305C: *cons = YK_P_Z; *vowel = YK_P_E; return 1;
    case 0x305E: *cons = YK_P_Z; *vowel = YK_P_O; return 1;
    /* た行 */
    case 0x305F: *cons = YK_P_T; *vowel = YK_P_A; return 1;
    case 0x3061: *cons = YK_P_CH; *vowel = YK_P_I; return 1;
    case 0x3064: *cons = YK_P_TS; *vowel = YK_P_U; return 1;
    case 0x3066: *cons = YK_P_T; *vowel = YK_P_E; return 1;
    case 0x3068: *cons = YK_P_T; *vowel = YK_P_O; return 1;
    /* だ行 */
    case 0x3060: *cons = YK_P_D; *vowel = YK_P_A; return 1;
    case 0x3062: *cons = YK_P_J; *vowel = YK_P_I; return 1;
    case 0x3065: *cons = YK_P_DZ; *vowel = YK_P_U; return 1;
    case 0x3067: *cons = YK_P_D; *vowel = YK_P_E; return 1;
    case 0x3069: *cons = YK_P_D; *vowel = YK_P_O; return 1;
    /* な行 */
    case 0x306A: *cons = YK_P_NA; *vowel = YK_P_A; return 1;
    case 0x306B: *cons = YK_P_NA; *vowel = YK_P_I; return 1;
    case 0x306C: *cons = YK_P_NA; *vowel = YK_P_U; return 1;
    case 0x306D: *cons = YK_P_NA; *vowel = YK_P_E; return 1;
    case 0x306E: *cons = YK_P_NA; *vowel = YK_P_O; return 1;
    /* は行 */
    case 0x306F: *cons = YK_P_H; *vowel = YK_P_A; return 1;
    case 0x3072: *cons = YK_P_H; *vowel = YK_P_I; return 1;
    case 0x3075: *cons = YK_P_F; *vowel = YK_P_U; return 1;
    case 0x3078: *cons = YK_P_H; *vowel = YK_P_E; return 1;
    case 0x307B: *cons = YK_P_H; *vowel = YK_P_O; return 1;
    /* ば行 */
    case 0x3070: *cons = YK_P_B; *vowel = YK_P_A; return 1;
    case 0x3073: *cons = YK_P_B; *vowel = YK_P_I; return 1;
    case 0x3076: *cons = YK_P_B; *vowel = YK_P_U; return 1;
    case 0x3079: *cons = YK_P_B; *vowel = YK_P_E; return 1;
    case 0x307C: *cons = YK_P_B; *vowel = YK_P_O; return 1;
    /* ぱ行 */
    case 0x3071: *cons = YK_P_P; *vowel = YK_P_A; return 1;
    case 0x3074: *cons = YK_P_P; *vowel = YK_P_I; return 1;
    case 0x3077: *cons = YK_P_P; *vowel = YK_P_U; return 1;
    case 0x307A: *cons = YK_P_P; *vowel = YK_P_E; return 1;
    case 0x307D: *cons = YK_P_P; *vowel = YK_P_O; return 1;
    /* ま行 */
    case 0x307E: *cons = YK_P_M; *vowel = YK_P_A; return 1;
    case 0x307F: *cons = YK_P_M; *vowel = YK_P_I; return 1;
    case 0x3080: *cons = YK_P_M; *vowel = YK_P_U; return 1;
    case 0x3081: *cons = YK_P_M; *vowel = YK_P_E; return 1;
    case 0x3082: *cons = YK_P_M; *vowel = YK_P_O; return 1;
    /* や行 */
    case 0x3084: *cons = YK_P_Y; *vowel = YK_P_A; return 1;
    case 0x3086: *cons = YK_P_Y; *vowel = YK_P_U; return 1;
    case 0x3088: *cons = YK_P_Y; *vowel = YK_P_O; return 1;
    /* ら行 */
    case 0x3089: *cons = YK_P_R; *vowel = YK_P_A; return 1;
    case 0x308A: *cons = YK_P_R; *vowel = YK_P_I; return 1;
    case 0x308B: *cons = YK_P_R; *vowel = YK_P_U; return 1;
    case 0x308C: *cons = YK_P_R; *vowel = YK_P_E; return 1;
    case 0x308D: *cons = YK_P_R; *vowel = YK_P_O; return 1;
    /* わ行 */
    case 0x308F: *cons = YK_P_W; *vowel = YK_P_A; return 1;
    case 0x3090: *cons = YK_P_W; *vowel = YK_P_I; return 1;
    case 0x3091: *cons = YK_P_W; *vowel = YK_P_E; return 1;
    case 0x3092: *cons = YK_P_W; *vowel = YK_P_O; return 1;
    /* ん・っ・長音・小書き */
    case 0x3093: *cons = YK_P_N; *vowel = -1; return 1;     /* ん */
    case 0x3063: *cons = YK_P_CL; *vowel = -1; return 1;    /* っ */
    case 0x30FC: case 0x301C: *cons = -2; *vowel = -1; return 1;  /* ー */
    case 0x3041: *cons = -1; *vowel = YK_P_A; return 2;     /* 小書き */
    case 0x3043: *cons = -1; *vowel = YK_P_I; return 2;
    case 0x3045: *cons = -1; *vowel = YK_P_U; return 2;
    case 0x3047: *cons = -1; *vowel = YK_P_E; return 2;
    case 0x3049: *cons = -1; *vowel = YK_P_O; return 2;
    case 0x3083: *cons = YK_P_Y; *vowel = YK_P_A; return 2;
    case 0x3085: *cons = YK_P_Y; *vowel = YK_P_U; return 2;
    case 0x3087: *cons = YK_P_Y; *vowel = YK_P_O; return 2;
    case 0x308E: *cons = YK_P_W; *vowel = YK_P_A; return 2;
    default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* ローマ字の解析                                                       */
/* ------------------------------------------------------------------ */

/* 長い順に並べる (先に一致したものを採用する) */
typedef struct { const char *rom; int cons; int vowel; int palatal; } RomDef;

static const RomDef ROM[] = {
    /* 拗音。
       日本語の きゃ きゅ きょ は、子音 + /i/ + 母音 ではなく
       「口蓋化した子音 + 母音」の 1 モーラ。だから母音は 1 つだけ積み、
       子音を YK_P_Y で口蓋化する (Y を独立の音素として入れない)。
       以前は "kyu" を k + y + u と解釈して 3 モーラになっていた。 */
    { "kya", YK_P_Y, YK_P_A, 1 }, { "kyu", YK_P_Y, YK_P_U, 1 }, { "kyo", YK_P_Y, YK_P_O, 1 },
    { "gya", YK_P_Y, YK_P_A, 1 }, { "gyu", YK_P_Y, YK_P_U, 1 }, { "gyo", YK_P_Y, YK_P_O, 1 },
    { "sha", YK_P_Y, YK_P_A, 1 }, { "shu", YK_P_Y, YK_P_U, 1 }, { "sho", YK_P_Y, YK_P_O, 1 },
    { "sya", YK_P_Y, YK_P_A, 1 }, { "syu", YK_P_Y, YK_P_U, 1 }, { "syo", YK_P_Y, YK_P_O, 1 },
    { "she", YK_P_Y, YK_P_E, 1 },
    { "cha", YK_P_Y, YK_P_A, 1 }, { "chu", YK_P_Y, YK_P_U, 1 }, { "cho", YK_P_Y, YK_P_O, 1 },
    { "che", YK_P_Y, YK_P_E, 1 },
    { "tya", YK_P_Y, YK_P_A, 1 }, { "tyu", YK_P_Y, YK_P_U, 1 }, { "tyo", YK_P_Y, YK_P_O, 1 },
    { "nya", YK_P_Y, YK_P_A, 1 }, { "nyu", YK_P_Y, YK_P_U, 1 }, { "nyo", YK_P_Y, YK_P_O, 1 },
    { "hya", YK_P_Y, YK_P_A, 1 }, { "hyu", YK_P_Y, YK_P_U, 1 }, { "hyo", YK_P_Y, YK_P_O, 1 },
    { "bya", YK_P_Y, YK_P_A, 1 }, { "byu", YK_P_Y, YK_P_U, 1 }, { "byo", YK_P_Y, YK_P_O, 1 },
    { "pya", YK_P_Y, YK_P_A, 1 }, { "pyu", YK_P_Y, YK_P_U, 1 }, { "pyo", YK_P_Y, YK_P_O, 1 },
    { "mya", YK_P_Y, YK_P_A, 1 }, { "myu", YK_P_Y, YK_P_U, 1 }, { "myo", YK_P_Y, YK_P_O, 1 },
    { "rya", YK_P_Y, YK_P_A, 1 }, { "ryu", YK_P_Y, YK_P_U, 1 }, { "ryo", YK_P_Y, YK_P_O, 1 },
    { "jya", YK_P_Y, YK_P_A, 1 }, { "jyu", YK_P_Y, YK_P_U, 1 }, { "jyo", YK_P_Y, YK_P_O, 1 },
    /* じゃ じゅ じょ も口蓋化 */
    { "ja", YK_P_Y, YK_P_A, 1 }, { "ju", YK_P_Y, YK_P_U, 1 }, { "jo", YK_P_Y, YK_P_O, 1 },
    { "je", YK_P_Y, YK_P_E, 1 },
    /* い段の特殊な子音 (日本語の「し」「ち」「じ」) */
    { "shi", YK_P_SH, YK_P_I, 0 }, { "chi", YK_P_CH, YK_P_I, 0 },
    { "ji", YK_P_J, YK_P_I, 0 }, { "dji", YK_P_J, YK_P_I, 0 },
    /* 子音 + 母音 (すべての組み合わせ) */
    { "ka", YK_P_K, YK_P_A, 0 },
    { "ki", YK_P_K, YK_P_I, 0 },
    { "ku", YK_P_K, YK_P_U, 0 },
    { "ke", YK_P_K, YK_P_E, 0 },
    { "ko", YK_P_K, YK_P_O, 0 },
    { "ga", YK_P_G, YK_P_A, 0 },
    { "gi", YK_P_G, YK_P_I, 0 },
    { "gu", YK_P_G, YK_P_U, 0 },
    { "ge", YK_P_G, YK_P_E, 0 },
    { "go", YK_P_G, YK_P_O, 0 },
    { "za", YK_P_Z, YK_P_A, 0 },
    { "zi", YK_P_Z, YK_P_I, 0 },
    { "zu", YK_P_Z, YK_P_U, 0 },
    { "ze", YK_P_Z, YK_P_E, 0 },
    { "zo", YK_P_Z, YK_P_O, 0 },
    { "ta", YK_P_T, YK_P_A, 0 },
    { "ti", YK_P_T, YK_P_I, 0 },
    { "tu", YK_P_T, YK_P_U, 0 },
    { "te", YK_P_T, YK_P_E, 0 },
    { "to", YK_P_T, YK_P_O, 0 },
    { "da", YK_P_D, YK_P_A, 0 },
    { "di", YK_P_D, YK_P_I, 0 },
    { "du", YK_P_D, YK_P_U, 0 },
    { "de", YK_P_D, YK_P_E, 0 },
    { "do", YK_P_D, YK_P_O, 0 },
    { "na", YK_P_NA, YK_P_A, 0 },
    { "ni", YK_P_NA, YK_P_I, 0 },
    { "nu", YK_P_NA, YK_P_U, 0 },
    { "ne", YK_P_NA, YK_P_E, 0 },
    { "no", YK_P_NA, YK_P_O, 0 },
    { "ha", YK_P_H, YK_P_A, 0 },
    { "hi", YK_P_H, YK_P_I, 0 },
    { "hu", YK_P_H, YK_P_U, 0 },
    { "he", YK_P_H, YK_P_E, 0 },
    { "ho", YK_P_H, YK_P_O, 0 },
    { "ba", YK_P_B, YK_P_A, 0 },
    { "bi", YK_P_B, YK_P_I, 0 },
    { "bu", YK_P_B, YK_P_U, 0 },
    { "be", YK_P_B, YK_P_E, 0 },
    { "bo", YK_P_B, YK_P_O, 0 },
    { "pa", YK_P_P, YK_P_A, 0 },
    { "pi", YK_P_P, YK_P_I, 0 },
    { "pu", YK_P_P, YK_P_U, 0 },
    { "pe", YK_P_P, YK_P_E, 0 },
    { "po", YK_P_P, YK_P_O, 0 },
    { "ma", YK_P_M, YK_P_A, 0 },
    { "mi", YK_P_M, YK_P_I, 0 },
    { "mu", YK_P_M, YK_P_U, 0 },
    { "me", YK_P_M, YK_P_E, 0 },
    { "mo", YK_P_M, YK_P_O, 0 },
    { "ra", YK_P_R, YK_P_A, 0 },
    { "ri", YK_P_R, YK_P_I, 0 },
    { "ru", YK_P_R, YK_P_U, 0 },
    { "re", YK_P_R, YK_P_E, 0 },
    { "ro", YK_P_R, YK_P_O, 0 },
    { "fa", YK_P_F, YK_P_A, 0 },
    { "fi", YK_P_F, YK_P_I, 0 },
    { "fu", YK_P_F, YK_P_U, 0 },
    { "fe", YK_P_F, YK_P_E, 0 },
    { "fo", YK_P_F, YK_P_O, 0 },
    { "va", YK_P_B, YK_P_A, 0 },
    { "vi", YK_P_B, YK_P_I, 0 },
    { "vu", YK_P_B, YK_P_U, 0 },
    { "ve", YK_P_B, YK_P_E, 0 },
    { "vo", YK_P_B, YK_P_O, 0 },
    { "si", YK_P_SH, YK_P_I, 0 }, { "ti", YK_P_CH, YK_P_I, 0 }, { "tu", YK_P_TS, YK_P_U, 0 },
    { "di", YK_P_J, YK_P_I, 0 }, { "du", YK_P_DZ, YK_P_U, 0 }, { "hu", YK_P_F, YK_P_U, 0 },
    { "ya", YK_P_Y, YK_P_A, 0 },
    { "yu", YK_P_Y, YK_P_U, 0 },
    { "yo", YK_P_Y, YK_P_O, 0 },
    { "wa", YK_P_W, YK_P_A, 0 },
    { "wi", YK_P_W, YK_P_I, 0 },
    { "we", YK_P_W, YK_P_E, 0 },
    { "wo", YK_P_W, YK_P_O, 0 },
    /* 破擦音 */
    { "tsu", YK_P_TS, YK_P_U, 0 }, { "tsa", YK_P_TS, YK_P_A, 0 },
    { "tsi", YK_P_TS, YK_P_I, 0 }, { "tse", YK_P_TS, YK_P_E, 0 }, { "tso", YK_P_TS, YK_P_O, 0 },
    /* 1 文字の規則は最後に置く (yu, wa のような 2 文字を先に拾うため) */
    /* 母音 (1 文字) */
    { "a", -1, YK_P_A, 0 }, { "i", -1, YK_P_I, 0 }, { "u", -1, YK_P_U, 0 },
    { "e", -1, YK_P_E, 0 }, { "o", -1, YK_P_O, 0 },
    /* 子音のみ */
    { "sh", YK_P_SH, -1, 0 }, { "ch", YK_P_CH, -1, 0 }, { "ts", YK_P_TS, -1, 0 },
    { "dz", YK_P_DZ, -1, 0 }, { "ng", YK_P_NG, -1, 0 },
    { "b", YK_P_B, -1, 0 }, { "d", YK_P_D, -1, 0 }, { "g", YK_P_G, -1, 0 },
    { "p", YK_P_P, -1, 0 }, { "t", YK_P_T, -1, 0 }, { "k", YK_P_K, -1, 0 },
    { "m", YK_P_M, -1, 0 }, { "s", YK_P_S, -1, 0 }, { "h", YK_P_H, -1, 0 },
    { "f", YK_P_F, -1, 0 }, { "z", YK_P_Z, -1, 0 }, { "j", YK_P_J, -1, 0 },
    { "r", YK_P_R, -1, 0 }, { "y", YK_P_Y, -1, 0 }, { "w", YK_P_W, -1, 0 },
};
static const int NROM = (int)(sizeof(ROM) / sizeof(ROM[0]));

/* ------------------------------------------------------------------ */
/* プラン構築                                                           */
/* ------------------------------------------------------------------ */

/* 1 音素ぶんを、長さとアクセントを決めて積む */
static void push(YkPlan *out, const YkPreset *ps, YkPhone ph, float amp)
{
    float dur = yk_phone_info(ph)->dur;
    /* 母音は長めに取る (子音の直後から共鳴を立ち上げるため) */
    if (ph <= YK_P_O) dur *= 1.45f;
    yk_plan_push(out, ph, dur, ps->pitch, amp);
}

/* 文末のアクセント記号 (0 なし, 1 上げ, -1 下げ) を、その時点の母音に反映する */
static void apply_accent(YkPlan *out, int acc)
{
    if (acc == 0 || out->count == 0) return;
    YkPhoneme *x = &out->items[out->count - 1];
    if (x->phone > YK_P_O) return;
    x->amp = acc > 0 ? 1.15f : 0.85f;
}

/* 直前の母音を 1 モーラ伸ばす (長音) */
static int lengthen(YkPlan *out)
{
    for (int i = out->count - 1; i >= 0; i--) {
        YkPhone p = (YkPhone)out->items[i].phone;
        if (p == YK_P_PAUSE) return 0;
        if (p <= YK_P_O) {
            /* 語中は 0.85 モーラ、文末は 1.5 モーラ (plan.c と同じ方針) */
            float f = 0.85f;
            out->items[i].dur = yk_phone_info(p)->dur * 1.45f * f;
            return 1;
        }
    }
    return 0;
}

/* 直前の子音に促音の閉鎖を足す (っ) */
static int geminate(YkPlan *out)
{
    /* 「っ」は次の子音の閉鎖を伸ばすので、ここでは印だけ積む。
       synth 側が YK_P_CL を見て closureLeft に足す。 */
    return yk_plan_push(out, YK_P_CL, 50.0f, 0, 0.0f);
}

/* ------------------------------------------------------------------ */
/* 日本語の異音規則                                                     */
/* ------------------------------------------------------------------ */

/* ん (YK_P_N) の後続音による同化。
   実際の日本語では ん は後続子音の調音点に同化する:
     ん + ま行/ば行/ぱ行 -> [m]   (「さんぽ」= sampo)
     ん + た行/だ行/な行/ら行 -> [n]
     ん + か行/が行       -> [ŋ]   (「まんが」= maŋga)
   これをやると「ん」の聞こえ方が大きく変わる。 */
/* 日本語の異音規則をまとめて当てる。かな入力でも記号入力でも同じ規則を
   使うことで、「ん」の同化・母音の無声化・ら行の弾き音が自動で入る。 */
static void nasal_assimilation(YkPlan *out)
{
    for (int i = 0; i + 1 < out->count; i++) {
        if (out->items[i].phone != YK_P_N) continue;
        YkPhone nx = (YkPhone)out->items[i + 1].phone;
        YkPhone tgt = YK_P_N;
        switch (nx) {
        case YK_P_M: case YK_P_B: case YK_P_P: tgt = YK_P_M; break;
        case YK_P_NG: case YK_P_G: case YK_P_K: tgt = YK_P_NG; break;
        case YK_P_NA: case YK_P_D: case YK_P_T: case YK_P_R:
        case YK_P_TS: case YK_P_CH: case YK_P_DZ: case YK_P_J:
        case YK_P_S: case YK_P_SH: case YK_P_Z: tgt = YK_P_NA; break;
        default: tgt = YK_P_N; break;
        }
        if (tgt != YK_P_N) out->items[i].phone = (uint8_t)tgt;
    }
}

/* 後ろで定義する規則 */
static void flap_r(YkPlan *out);
static void devoice_vowels(YkPlan *out);
static void final_weak(YkPlan *out);

void yk_apply_allophones(YkPlan *out)
{
    nasal_assimilation(out);
    flap_r(out);
    devoice_vowels(out);
    final_weak(out);
}

/* 母音の無声化。
   次の 2 条件で、無声子音に挟まれた い/う を無声化する (日本語の実際の発音):
     1) 無声子音 + i/u + 無声子音   (「きって」「すき」「した」)
     2) 文末の 無声子音 + i/u        (「です」「ます」の す)
   有声子音が挟まる場合は無声化しない (「くつした」の く は有声化しない等)。 */
static int is_voiceless_cons(YkPhone p)
{
    switch (p) {
    case YK_P_K: case YK_P_T: case YK_P_P:
    case YK_P_S: case YK_P_SH: case YK_P_H: case YK_P_F:
    case YK_P_TS: case YK_P_CH: case YK_P_CL:
        return 1;
    default: return 0;
    }
}

static void devoice_vowels(YkPlan *out)
{
    for (int i = 0; i < out->count; i++) {
        YkPhone v = (YkPhone)out->items[i].phone;
        if (v != YK_P_I && v != YK_P_U) continue;
        /* 直前の無声子音 */
        int prev_voiceless = 0;
        int j = i - 1;
        /* 直前は子音 1 つ (子音が無ければ母音連続なので無声化しない) */
        if (j >= 0 && is_voiceless_cons((YkPhone)out->items[j].phone)) prev_voiceless = 1;
        if (!prev_voiceless) continue;

        /* 直後: 無声子音 or 文末/休止。
           促音 (っ) は次の子音の閉鎖なので、飛ばしてその後ろを見る
           (「きって」の き は t に挟まれているのと同じ)。 */
        int j2 = i + 1;
        while (j2 < out->count && out->items[j2].phone == YK_P_CL) j2++;
        YkPhone nx = (j2 < out->count) ? (YkPhone)out->items[j2].phone : YK_P_PAUSE;
        /* 文末の です/ます の す は無声化するが、単独の「す」「し」は
           無声化しない (単語を 1 つだけ発音するときは母音が残る)。
           ここでは「後ろに無声子音が続く」場合だけ無声化する。
           末尾の弱化は final_weak() が担当する。 */
        int next_voiceless = (nx != YK_P_PAUSE) && is_voiceless_cons(nx);
        if (!next_voiceless) continue;

        /* 無声化。母音そのものは YK_P_DEV (息だけの母音) に置き換え、
           フォルマントは元の母音のものを残す (合成側で直前の母音から引く)。 */
        out->items[i].base = (uint8_t)v;     /* 元の母音 (フォルマント用) */
        out->items[i].phone = (uint8_t)YK_P_DEV;
        out->items[i].dur *= 0.55f;
        out->items[i].amp = 0.35f;
    }
}

/* ら行の弾き音 (語中の ら行 を短い [ɾ] にする) と、
   語頭の ら行を通常の [r] にする。実際の日本語では語中の ら行は
   1 タップの弾き音で、長さが母音よりずっと短い。 */
static void flap_r(YkPlan *out)
{
    for (int i = 0; i < out->count; i++) {
        if (out->items[i].phone != YK_P_R) continue;
        int prev_is_vowel = (i > 0) && out->items[i - 1].phone <= YK_P_O;
        if (prev_is_vowel) {
            /* 母音に挟まれた ら行 = 弾き音。短く、閉鎖は無し。 */
            out->items[i].dur *= 0.55f;
            out->items[i].amp = 0.85f * out->items[i].amp;
        }
    }
}

/* 語末の「です」「ます」などの す を弱く (devocalized + 短く) */
static void final_weak(YkPlan *out)
{
    if (out->count < 2) return;
    int n = out->count;
    if (out->items[n - 1].phone == YK_P_PAUSE) n--;
    if (n < 2) return;
    /* 語末が す (s + u) なら u を弱める */
    if (out->items[n - 1].phone == YK_P_U && out->items[n - 2].phone == YK_P_S) {
        out->items[n - 1].dur *= 0.8f;
        out->items[n - 1].amp *= 0.8f;
    }
}

/* ------------------------------------------------------------------ */
/* メイン                                                               */
/* ------------------------------------------------------------------ */

static void append_reading_str(char *dst, int cap, int *pos, const char *str, int maxn)
{
    for (int i = 0; str[i] && i < maxn; i++) {
        if (*pos + 1 >= cap) return;
        dst[(*pos)++] = str[i];
        dst[*pos] = '\0';
    }
}

static void append_reading(char *dst, int cap, int *pos, char c)
{
    if (*pos + 1 >= cap) return;
    dst[(*pos)++] = c;
    dst[*pos] = '\0';
}

int yk_plan_from_symbols(const char *text, const YkPreset *preset, YkPlan *out)
{
    if (!text || !out) return -2;
    memset(out, 0, sizeof(*out));
    memset(out->reading, 0, sizeof(out->reading));
    int rpos = 0;

    const char *s = text;
    int acc = 0;

    while (*s) {
        uint32_t cp = 0;
        const char *nx = next_char_utf8(s, &cp);
        unsigned char c0 = (unsigned char)*s;

        /* 空白と記号 */
        if (c0 == ' ' || c0 == '\t' || c0 == '\n' || c0 == '\r') { s = nx; continue; }
        if (c0 == ',' || c0 == '.' || c0 == '-' || cp == 0x3001 || cp == 0x3002) {
            yk_plan_push(out, YK_P_PAUSE, 320.0f, preset->pitch, 1.0f);
            s = nx; continue;
        }
        if (c0 == ':' || cp == 0x30FC || cp == 0x301C) {
            lengthen(out);
            s = nx; continue;
        }
        if (c0 == '\'') { acc = 1; s = nx; continue; }
        if (c0 == '`') { acc = -1; s = nx; continue; }
        if (c0 == '\'' ) { s = nx; continue; }
        if (c0 == '/') {
            /* 区切り記号としての / は休止 (「a/k/a」のような記法は想定しない) */
            yk_plan_push(out, YK_P_PAUSE, 200.0f, preset->pitch, 1.0f);
            s = nx; continue;
        }
        if (cp == 0x309B || cp == 0x309C) { s = nx; continue; }  /* 濁点単体 */

        /* かな */
        {
            int cons = 0, vowel = 0;
            int k = kana_to_cv(cp, &cons, &vowel);
            if (k > 0) {
                s = nx;
                /* 拗音 (小書き) が続くなら子音と結合する。ここでは
                   かな 1 文字ずつ積み、合成側の遷移で拗音化させる。 */
                if (cons == YK_P_CL) { geminate(out); append_reading(out->reading, (int)sizeof(out->reading), &rpos, 'Q'); }
                else if (cons == YK_P_N && vowel < 0) {
                    push(out, preset, YK_P_N, 1.0f);
                    append_reading(out->reading, (int)sizeof(out->reading), &rpos, 'N');
                } else if (cons == -2) {
                    lengthen(out);
                    append_reading(out->reading, (int)sizeof(out->reading), &rpos, ':');
                } else {
                    if (cons >= 0) {
                        push(out, preset, (YkPhone)cons, 0.70f);
                        /* 読みは ASCII の音素記号で持つ (UI 表示用) */
                        append_reading_str(out->reading, (int)sizeof(out->reading), &rpos,
                                           yk_phone_symbol((YkPhone)cons), 1);
                    }
                    if (vowel >= 0) {
                        push(out, preset, (YkPhone)vowel, 1.0f);
                        if (k == 2) out->items[out->count - 1].dur *= 0.55f;  /* 小書き母音 */
                        append_reading(out->reading, (int)sizeof(out->reading), &rpos,
                                       "aiueo"[vowel]);
                    }
                }
                apply_accent(out, acc); acc = 0;
                continue;
            }
        }

        /* 撥音 N / 促音 Q。ローマ字表より先に見る (q は表に無い) */
        if (c0 == 'N' || c0 == 'Q') {
            if (c0 == 'N') {
                push(out, preset, YK_P_N, 1.0f);
                append_reading(out->reading, (int)sizeof(out->reading), &rpos, 'N');
            } else {
                geminate(out);
                append_reading(out->reading, (int)sizeof(out->reading), &rpos, 'Q');
            }
            s = nx; continue;
        }

        /* ローマ字 */
        {
            int matched = 0;
            for (int i = 0; i < NROM; i++) {
                size_t L = strlen(ROM[i].rom);
                if (cmp_ci_n(s, ROM[i].rom, L) != 0) continue;
                /* 撥音 N / 促音 Q は大文字だけ特別扱い */
                matched = 1;
                s += L;
                if (ROM[i].cons >= 0 && ROM[i].palatal) {
                    /* 拗音: 子音を口蓋化する。YK_P_Y を独立に積まず、
                       直前に積んだ子音のフォルマントを /i/ 寄りにして
                       1 モーラとして発音させる (合成側の遷移で実現)。 */
                    push(out, preset, YK_P_Y, 0.55f);
                    append_reading_str(out->reading, (int)sizeof(out->reading), &rpos, "y", 1);
                } else if (ROM[i].cons >= 0) {
                    push(out, preset, (YkPhone)ROM[i].cons, 0.70f);
                    append_reading_str(out->reading, (int)sizeof(out->reading), &rpos,
                                       yk_phone_symbol((YkPhone)ROM[i].cons), 1);
                }
                if (ROM[i].vowel >= 0) {
                    push(out, preset, (YkPhone)ROM[i].vowel, 1.0f);
                    append_reading(out->reading, (int)sizeof(out->reading), &rpos,
                                   "aiueo"[ROM[i].vowel]);
                }
                apply_accent(out, acc); acc = 0;
                break;
            }
            if (matched) continue;
        }

        /* 知らない文字は読み飛ばす */
        s = nx;
    }

    if (out->count == 0) return -2;

    /* 異音規則 */
    yk_apply_allophones(out);

    return 0;
}

/* ------------------------------------------------------------------ */
/* 表示用: かな -> 音素記号列                                           */
/* ------------------------------------------------------------------ */
int yk_kana_to_symbols(const char *kana, char *out, int out_cap)
{
    if (!out || out_cap <= 0) return -1;
    out[0] = '\0';
    YkPlan tmp;
    YkPreset ps;
    memset(&ps, 0, sizeof(ps));
    ps.pitch = 280.0f;
    ps.formantScale = 1.09f;
    /* かなを音素列にしてから記号へ */
    if (yk_plan_from_kana(kana, &ps, &tmp) != 0) return -1;
    int pos = 0;
    for (int i = 0; i < tmp.count; i++) {
        const char *sy = yk_phone_symbol((YkPhone)tmp.items[i].phone);
        size_t L = strlen(sy);
        if (pos + (int)L + 2 >= out_cap) break;
        if (pos > 0) out[pos++] = ' ';
        memcpy(out + pos, sy, L);
        pos += (int)L;
        out[pos] = '\0';
    }
    return pos;
}
