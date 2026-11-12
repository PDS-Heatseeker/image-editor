/* Compares size estimates with real encoded sizes: test_est maxw maxh files... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "core.h"
int main(int argc, char **argv)
{
    static const struct { const char *name; int fmt, q; } F[] = {
        { "jpg50", IS_FMT_JPEG, 50 }, { "jpg90", IS_FMT_JPEG, 90 }, { "jpg100", IS_FMT_JPEG, 100 },
        { "webp60", IS_FMT_WEBP, 60 }, { "webp90", IS_FMT_WEBP, 90 }, { "png", IS_FMT_PNG, 90 },
        { "webpll", IS_FMT_WEBP_LOSSLESS, 90 }, { "tiff", IS_FMT_TIFF, 90 }, { "bmp", IS_FMT_BMP, 90 },
        { "same", IS_FMT_SAME, 90 } };
    int i, k, nf = (int)(sizeof F / sizeof F[0]);
    double sumabs[16] = {0}, worst[16] = {0}; int cnt[16] = {0};
    IsOptions o; memset(&o, 0, sizeof o);
    o.max_w = atoi(argv[1]); o.max_h = atoi(argv[2]);
    for (i = 3; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb"); long n; unsigned char *b; IsSample *s; char err[160];
        if (!f) continue;
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); b = malloc(n ? n : 1);
        if (fread(b, 1, n, f) != (size_t)n) { fclose(f); continue; } fclose(f);
        s = is_sample_create(b, n, &o, err, sizeof err);
        if (!s) { printf("%-30s skip: %s\n", argv[i], err); free(b); continue; }
        printf("%-30s", strrchr(argv[i], '/') + 1);
        for (k = 0; k < nf; k++) {
            IsResult r; long long est; int kept; double e;
            o.format = F[k].fmt; o.quality = F[k].q;
            if (!is_sample_estimate(s, &o, &est, &kept, err, sizeof err) || !is_process(b, n, &o, &r, err, sizeof err)) { printf(" %6s:ERR", F[k].name); continue; }
            e = 100.0 * (est - (double)r.size) / r.size;
            printf(" %s:%+.0f%%", F[k].name, e);
            sumabs[k] += fabs(e); cnt[k]++; if (fabs(e) > fabs(worst[k])) worst[k] = e;
            is_free(&r);
        }
        printf("\n"); is_sample_free(s); free(b);
    }
    printf("\nmean |error| / worst:");
    for (k = 0; k < nf; k++) printf("  %s %.1f%%/%+.0f%%", F[k].name, cnt[k] ? sumabs[k] / cnt[k] : 0, worst[k]);
    printf("\n");
    return 0;
}
