/*
 * yukkuri.h - 「ゆっくり」専用フォルマント合成エンジン (公開 API)
 *
 * 設計方針
 *  - 依存ゼロの C99。Android では JNI 経由、PC では CLI 経由で叩く。
 *  - 周波数軸でパラメータを補間し、時間軸サンプルごとに
 *    フォルマントフィルタ係数を再計算する (ソース・フィルタ合成)。
 *  - 声質はすべてプリセット (ピッチ/フォルマント/速さ/抑揚) で表現する。
 */
#ifndef YUKKURI_H
#define YUKKURI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YK_SR 48000            /* 内部サンプリング周波数 */
#define YK_MAX_PHONEMES 2048   /* 1 発話あたりの最大音素数 */
#define YK_MAX_MORAS 4096

/* ------------------------------------------------------------------ *
 * プリセット
 * ------------------------------------------------------------------ */
typedef struct {
    char  name[24];
    char  label[48];      /* UI 表示名 (UTF-8) */
    float pitch;          /* 基準 F0 [Hz] */
    float formantScale;   /* フォルマント周波数の倍率 (>1 で子供/女声寄り) */
    float speed;          /* 話速倍率 (1.0 基準) */
    float intonation;     /* 抑扬の強さ 0..2 */
    float breath;         /* 息成分 0..1 */
    float vibrato;        /* ビブラート深さ (半音) */
    float gain;           /* 出力ゲイン 0..1.5 */
} YkPreset;

int         yk_preset_count(void);
const YkPreset *yk_preset(int index);
int         yk_preset_index_of(const char *name);

/* ------------------------------------------------------------------ *
 * 音素
 * ------------------------------------------------------------------ */
typedef enum {
    YK_P_A = 0, YK_P_I, YK_P_U, YK_P_E, YK_P_O,   /* 母音 */
    YK_P_N,                                        /* 撥音 ん */
    YK_P_CL,                                       /* 促音 (無音) */
    YK_P_B, YK_P_D, YK_P_G, YK_P_P, YK_P_T, YK_P_K,/* 破裂音 */
    YK_P_M, YK_P_NG,                               /* 鼻音 (んが) */
    YK_P_NA,                                       /* 歯茎鼻音 /n/ (な行) */
    YK_P_S, YK_P_SH, YK_P_H, YK_P_F,               /* 摩擦音 */
    YK_P_Z, YK_P_J,                                /* 破擦/摩擦 (有声) */
    YK_P_R, YK_P_Y, YK_P_W,                        /* 半母音・流音 */
    YK_P_GLOTTAL,                                  /* 母音の前の軽い声門破裂 (あ 行・母音連続) */
    /* 以下は音素記号列 (ローマ字/かな/ISO 風) から直接指定したときに使う異音。
       かな入力の自動変換でも選ばれる (日本語の実際の発音に寄せるため)。 */
    YK_P_TS,     /* /ts/  つ の破擦音 (t + s) */
    YK_P_CH,     /* /tʃ/  ち・ちゃ の破擦音 */
    YK_P_DZ,     /* /dz/  ず・づ の破擦音 */
    YK_P_DEV,    /* 無声化した母音 (声帯は弱く、息だけ) */
    YK_P_PAUSE,                                    /* 句読点などの休止 */
    YK_P_COUNT
} YkPhone;

/* 音素の性質 (テーブルは core/src/phones.c を参照) */
typedef struct {
    const char *sym;      /* かな表記の代表 (デバッグ用) */
    int   voiced;         /* 声帯振動の有無 */
    int   formant[5];     /* フォルマント周波数 [Hz] x 5 (母音は必須) */
    float bw;             /* 帯域幅 [Hz] */
    float amp;            /* 振幅 */
    float noise;          /* 摩擦/息ノイズ量 */
    float noiseHp;        /* ノイズの高域寄せ (0..1) */
    float noiseF;         /* ノイズの中心周波数 [Hz] */
    float noiseBw;        /* ノイズの帯域幅 [Hz] */
    float dur;            /* 既定長 [ms] */
    float gain;           /* 音量補正 (共振器の減衰差を吸収, 1.0 基準) */
} YkPhoneInfo;

const YkPhoneInfo *yk_phone_info(YkPhone p);

/* ------------------------------------------------------------------ *
 * 発話プラン (音素列 + 韻律)
 * ------------------------------------------------------------------ */
typedef struct {
    uint8_t phone;
    float   dur;      /* 長さ [ms] (ポーズ・無音を含む) */
    float   f0;       /* その音素の F0 [Hz] */
    float   amp;      /* 0..1 の音量係数 (アクセント) */
    float   glide;    /* 直前のフォルマントからの遷移時間 [ms] (0 なら既定値) */
    /* 無声化した母音 (YK_P_DEV) のときに、どの母音として発音するか。
       それ以外では 255 (未使用)。 */
    uint8_t base;
    /* ---- 韻律 (アクセント) ----
       アクセント句の中でのモーラ位置と、句の長さ。
       「はし」と「箸」のように、同じ音素列で意味が変わるものを
       F0 の段で区別するために持つ。 */
    uint8_t mora;      /* 句の中でのモーラ番号 (0 始まり) */
    uint8_t moras;     /* その句のモーラ数 */
    int8_t  accent;    /* アクセント核のモーラ番号。-1 = 平板 (下がり目なし) */
} YkPhoneme;

typedef struct {
    YkPhoneme items[YK_MAX_PHONEMES];
    int       count;
    /* 最後の読み (UI 表示用, カタカナ) */
    char      reading[1024];
} YkPlan;

/* かな列 -> 発話プラン。成功で 0 を返す。 */
int yk_plan_from_kana(const char *kana, const YkPreset *preset, YkPlan *out);

/* 音素記号列 -> 発話プラン。成功で 0 を返す。
   ローマ字 (ka, sha, tsu, N, Q, :)、かな、記号を混在で受け付け、
   日本語の異音規則 (ん の同化、母音の無声化、ら行の弾き音、
   つ/ち/ず の破擦化) を自動で当てる。
   例: "yuQkuri shite iQte ne" / "ゆっくりしていってね" / "saNpo" */
int yk_plan_from_symbols(const char *text, const YkPreset *preset, YkPlan *out);

/* 音素記号の文字列表現 (表示用)。例: YK_P_SH -> "sh" */
const char *yk_phone_symbol(YkPhone p);

/* 音素記号 -> 音素。未知なら YK_P_COUNT。 */
YkPhone yk_phone_from_symbol(const char *sym);

/* かな列を音素記号列に変換する (UI の「自動変換」表示用)。
   書き込んだ文字数を返す。 */
int yk_kana_to_symbols(const char *kana, char *out, int out_cap);

/* ------------------------------------------------------------------ *
 * 合成
 * ------------------------------------------------------------------ */
/* out は最大 YK_SR*4 サンプル (モノラル, -1..1) を書き込む。
 * 生成サンプル数を返す。失敗時は負値。 */
int yk_synthesize(const YkPlan *plan, const YkPreset *preset, float *out, int out_cap);

/* 簡易 API: かな列 -> サンプル列 */
int yk_speak_kana(const char *kana, const YkPreset *preset,
                  float *out, int out_cap, char *reading, int reading_cap);

/* 簡易 API: 音素記号列 -> サンプル列 */
int yk_speak_symbols(const char *text, const YkPreset *preset,
                     float *out, int out_cap, char *reading, int reading_cap);

const char *yk_version(void);

#ifdef __cplusplus
}
#endif

#endif /* YUKKURI_H */
