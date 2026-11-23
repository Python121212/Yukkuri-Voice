#ifndef YK_INTERNAL_H
#define YK_INTERNAL_H

#include "yukkuri.h"

/* 母音 5 種のフォルマント (男性基準, 成人男性の平均的な値から調整) */
static const float YK_VOWEL_F[5][5] = {
    /*  F1    F2    F3    F4    F5  */
    { 780, 1200, 2550, 3400, 4300 }, /* a */
    { 300, 2150, 2950, 3700, 4400 }, /* i */
    { 340, 1350, 2400, 3300, 4300 }, /* u */
    { 480, 1900, 2650, 3500, 4400 }, /* e */
    { 450,  850, 2400, 3300, 4300 }, /* o */
};

int yk_plan_push(YkPlan *plan, YkPhone phone, float dur, float f0, float amp);

#endif
