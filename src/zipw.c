/* Minimal streaming ZIP writer (stored entries - the images are already compressed). */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zipw.h"

static uint32_t crc_tab[256];
static int crc_ready;

static uint32_t crc32_buf(const unsigned char *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    if (!crc_ready) {
        uint32_t k, v; int j;
        for (k = 0; k < 256; k++) {
            v = k;
            for (j = 0; j < 8; j++) v = (v & 1) ? (0xEDB88320u ^ (v >> 1)) : (v >> 1);
            crc_tab[k] = v;
        }
        crc_ready = 1;
    }
    for (i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void p16(unsigned char *b, unsigned v) { b[0] = (unsigned char)v; b[1] = (unsigned char)(v >> 8); }
static void p32(unsigned char *b, uint32_t v) { p16(b, v & 0xFFFF); p16(b + 2, v >> 16); }

static int put(Zip *z, const void *p, size_t n)
{
    if (n && fwrite(p, 1, n, z->f) != n) return 0;
    z->off += n;
    return 1;
}

void zip_begin(Zip *z, FILE *f)
{
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    memset(z, 0, sizeof *z);
    z->f = f;
    if (lt && lt->tm_year >= 80) {
        z->dtime = (uint16_t)((lt->tm_hour << 11) | (lt->tm_min << 5) | (lt->tm_sec / 2));
        z->ddate = (uint16_t)(((lt->tm_year - 80) << 9) | ((lt->tm_mon + 1) << 5) | lt->tm_mday);
    } else {
        z->ddate = (1 << 5) | 1;
    }
}

int zip_add(Zip *z, const char *name, const void *data, size_t n)
{
    unsigned char h[30];
    size_t nl = strlen(name);
    ZipEntry *e;

    /* stay inside classic (non-zip64) limits, leaving room for the directory */
    if (z->n >= 65535 || nl > 0xFFFF || n > 0xFFFFFFFFu ||
        z->off + 30 + nl + n + ((uint64_t)z->n + 1) * 400 > 0xFFFFFF00u)
        return ZIP_ELIMIT;

    if (z->n == z->cap) {
        int nc = z->cap ? z->cap * 2 : 64;
        ZipEntry *ne = (ZipEntry *)realloc(z->ents, (size_t)nc * sizeof *ne);
        if (!ne) return ZIP_EMEM;
        z->ents = ne; z->cap = nc;
    }
    e = &z->ents[z->n];
    e->name = (char *)malloc(nl + 1);
    if (!e->name) return ZIP_EMEM;
    memcpy(e->name, name, nl + 1);
    e->crc = crc32_buf((const unsigned char *)data, n);
    e->size = (uint32_t)n;
    e->offset = (uint32_t)z->off;
    z->n++;

    p32(h, 0x04034b50u);
    p16(h + 4, 20);            /* version needed */
    p16(h + 6, 0x0800);        /* UTF-8 names */
    p16(h + 8, 0);             /* stored */
    p16(h + 10, z->dtime);
    p16(h + 12, z->ddate);
    p32(h + 14, e->crc);
    p32(h + 18, e->size);
    p32(h + 22, e->size);
    p16(h + 26, (unsigned)nl);
    p16(h + 28, 0);
    if (!put(z, h, 30) || !put(z, name, nl) || !put(z, data, n)) return ZIP_EWRITE;
    return ZIP_OK;
}

int zip_finish(Zip *z)
{
    unsigned char h[46];
    uint64_t cd_start = z->off;
    int i;
    for (i = 0; i < z->n; i++) {
        ZipEntry *e = &z->ents[i];
        size_t nl = strlen(e->name);
        memset(h, 0, sizeof h);
        p32(h, 0x02014b50u);
        p16(h + 4, 20);        /* made by */
        p16(h + 6, 20);        /* needed */
        p16(h + 8, 0x0800);
        p16(h + 10, 0);
        p16(h + 12, z->dtime);
        p16(h + 14, z->ddate);
        p32(h + 16, e->crc);
        p32(h + 20, e->size);
        p32(h + 24, e->size);
        p16(h + 28, (unsigned)nl);
        p32(h + 42, e->offset);
        if (!put(z, h, 46) || !put(z, e->name, nl)) return ZIP_EWRITE;
    }
    if (z->off > 0xFFFFFFFFu) return ZIP_ELIMIT;
    memset(h, 0, 22);
    p32(h, 0x06054b50u);
    p16(h + 8, (unsigned)z->n);
    p16(h + 10, (unsigned)z->n);
    p32(h + 12, (uint32_t)(z->off - cd_start));
    p32(h + 16, (uint32_t)cd_start);
    if (!put(z, h, 22)) return ZIP_EWRITE;
    if (fflush(z->f) != 0) return ZIP_EWRITE;
    return ZIP_OK;
}

void zip_free(Zip *z)
{
    int i;
    for (i = 0; i < z->n; i++) free(z->ents[i].name);
    free(z->ents);
    z->ents = NULL; z->n = z->cap = 0;
}
