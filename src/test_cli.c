/* Native test harness for the pipeline: test_cli out.zip maxw maxh quality jpeg|webp files... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"
#include "zipw.h"

int main(int argc, char **argv)
{
    IsOptions o;
    Zip z;
    FILE *zf;
    int i, fails = 0;
    if (argc < 7) { fprintf(stderr, "usage\n"); return 2; }
    memset(&o, 0, sizeof o);
    o.max_w = atoi(argv[2]); o.max_h = atoi(argv[3]); o.quality = atoi(argv[4]);
    { const char *names[] = { "same", "jpeg", "png", "webp", "webpll", "tiff", "bmp" }; int k; o.format = IS_FMT_JPEG; o.jpeg_long_ext = 0;
      for (k = 0; k < 7; k++) if (strcmp(argv[5], names[k]) == 0) o.format = k; }
    zf = fopen(argv[1], "wb");
    zip_begin(&z, zf);
    for (i = 6; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        unsigned char *buf; long n; IsResult r; char err[200], name[512];
        const char *base = strrchr(argv[i], '/'); char *dot;
        if (!f) { printf("%s: cannot open\n", argv[i]); fails++; continue; }
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        buf = malloc(n ? n : 1); if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); fails++; continue; }
        fclose(f);
        if (!is_process(buf, n, &o, &r, err, sizeof err)) {
            printf("FAIL %s: %s\n", argv[i], err); fails++; free(buf); continue;
        }
        base = base ? base + 1 : argv[i];
        snprintf(name, sizeof name - 8, "%s", base);
        dot = strrchr(name, '.'); if (dot) *dot = 0;
        strcat(name, r.ext);
        printf("OK   %-28s %5dx%-5d -> %5dx%-5d %9ld -> %8zu bytes%s\n", base, r.src_w, r.src_h, r.w, r.h, n, r.size,
               r.kept_original ? "  (kept original)" : "");
        if (zip_add(&z, name, r.data, r.size) != ZIP_OK) { printf("zip error\n"); return 1; }
        is_free(&r); free(buf);
    }
    if (zip_finish(&z) != ZIP_OK) { printf("zip finish error\n"); return 1; }
    fclose(zf); zip_free(&z);
    return fails ? 3 : 0;
}
