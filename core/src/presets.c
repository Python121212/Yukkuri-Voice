#include "internal.h"
#include <string.h>

/* 「ゆっくり」らしさの核心は 3 つ:
 *   1) 高めの F0 とほぼ平坦なピッチ (抑扬は小さめ)
 *   2) フォルマントを上へずらした軽い声質
 *   3) 遅い話速と長めの母音
 * ここではその 3 つをパラメータとして外に出し、キャラは組み合わせで作る。 */
static const YkPreset P[] = {
    { "yukkuri",   "ゆっくり（標準）",      235, 1.09f, 0.80f, 0.55f, 0.06f, 0.30f, 0.90f },
    { "reimu",     "れいむ",                252, 1.12f, 0.80f, 0.60f, 0.05f, 0.35f, 0.90f },
    { "marisa",    "まりさ",                208, 1.05f, 0.84f, 0.70f, 0.07f, 0.30f, 0.92f },
    { "yukkuri_lo","ゆっくり（低め・落ち着き）", 190, 1.02f, 0.72f, 0.45f, 0.05f, 0.20f, 0.95f },
    { "yukkuri_hi","ゆっくり（高め・元気）",  285, 1.16f, 0.88f, 0.85f, 0.08f, 0.35f, 0.88f },
    { "slow",      "ゆっくり（超低速）",    235, 1.09f, 0.55f, 0.45f, 0.06f, 0.25f, 0.90f },
};
static const int NP = (int)(sizeof(P) / sizeof(P[0]));

int yk_preset_count(void) { return NP; }

const YkPreset *yk_preset(int i)
{
    if (i < 0 || i >= NP) return &P[0];
    return &P[i];
}

int yk_preset_index_of(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < NP; i++)
        if (strcmp(name, P[i].name) == 0) return i;
    return -1;
}
