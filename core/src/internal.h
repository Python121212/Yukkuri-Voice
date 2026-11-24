#ifndef YK_INTERNAL_H
#define YK_INTERNAL_H

#include "yukkuri.h"

/* 母音 5 種のフォルマント (男性基準, 成人男性の平均的な値から調整) */
static const float YK_VOWEL_F[5][5] = {
    /*  F1    F2    F3    F4    F5
       日本語の成人男性の代表値。F1-F2 の距離が母音の弁別を決めるので、
       /o/ は F2 を低め (800Hz 前後) に取る。 */
    { 780, 1200, 2550, 3400, 4300 }, /* a */
    { 300, 2250, 3000, 3700, 4400 }, /* i */
    { 350, 1450, 2500, 3400, 4300 }, /* u */
    { 500, 1950, 2700, 3500, 4400 }, /* e */
    { 480,  800, 2400, 3300, 4300 }, /* o */
};

int yk_plan_push(YkPlan *plan, YkPhone phone, float dur, float f0, float amp);

/* 日本語の異音規則 (ん の同化 / 母音の無声化 / ら行の弾き音 / 語末の弱化) を
   まとめて当てる。かな入力と記号入力の両方から呼ぶ。 */
void yk_apply_allophones(YkPlan *plan);

/* UTF-8 デコード (記号列とかな列の両方で使う最小実装) */
const char *next_char_utf8(const char *s, uint32_t *cp);

#endif
