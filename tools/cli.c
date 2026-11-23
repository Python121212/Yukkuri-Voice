/* 開発用 CLI: かな列 -> WAV (16kHz/48kHz) とプラン情報のダンプ */
#include "yukkuri.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float buf[YK_SR * 8];

static void write_wav(const char *path, const float *x, int n, int sr)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); exit(1); }
    int dataLen = n * 2;
    unsigned char h[44];
    memcpy(h, "RIFF", 4); *(unsigned*)(h+4) = 36 + dataLen; memcpy(h+8, "WAVEfmt ", 8);
    *(unsigned*)(h+16) = 16; *(unsigned short*)(h+20) = 1; *(unsigned short*)(h+22) = 1;
    *(unsigned*)(h+24) = sr; *(unsigned*)(h+28) = sr * 2; *(unsigned short*)(h+32) = 2;
    *(unsigned short*)(h+34) = 16; memcpy(h+36, "data", 4); *(unsigned*)(h+40) = dataLen;
    fwrite(h, 1, 44, f);
    for (int i = 0; i < n; i++) {
        float v = x[i]; if (v > 1) v = 1; if (v < -1) v = -1;
        short s = (short)(v * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <kana> <out.wav> [preset] [--dump]\n", argv[0]);
        return 2;
    }
    const char *kana = argv[1];
    const char *outp = argv[2];
    const char *pname = argc > 3 ? argv[3] : "yukkuri";
    int dump = 0;
    for (int i = 3; i < argc; i++) if (strcmp(argv[i], "--dump") == 0) dump = 1;

    int pi = yk_preset_index_of(pname);
    if (pi < 0) { fprintf(stderr, "unknown preset %s\n", pname); return 2; }
    const YkPreset *ps = yk_preset(pi);

    char reading[1024];
    int n = yk_speak_kana(kana, ps, buf, (int)(sizeof(buf)/sizeof(buf[0])), reading, sizeof(reading));
    if (n <= 0) { fprintf(stderr, "synth failed: %d\n", n); return 1; }
    write_wav(outp, buf, n, YK_SR);
    fprintf(stderr, "preset=%s reading=%s samples=%d (%.2fs)\n",
            ps->name, reading, n, (double)n / YK_SR);
    if (dump) {
        YkPlan plan;
        yk_plan_from_kana(kana, ps, &plan);
        for (int i = 0; i < plan.count; i++)
            printf("%3d %-8s dur=%6.1f f0=%6.1f amp=%.2f\n", i,
                   yk_phone_info((YkPhone)plan.items[i].phone)->sym,
                   plan.items[i].dur, plan.items[i].f0, plan.items[i].amp);
        /* RMS を 50ms ごとに */
        int win = YK_SR / 20;
        for (int i = 0; i + win <= n; i += win) {
            double acc = 0; for (int j = 0; j < win; j++) acc += buf[i+j]*buf[i+j];
            printf("rms %.3f\n", __builtin_sqrt(acc / win));
        }
    }
    return 0;
}
