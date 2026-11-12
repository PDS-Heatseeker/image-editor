/* Minimal streaming ZIP writer (stored entries - the images are already compressed). */
#ifndef IS_ZIPW_H
#define IS_ZIPW_H
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *name;
    uint32_t crc, size, offset;
} ZipEntry;

typedef struct {
    FILE *f;
    uint64_t off;
    ZipEntry *ents;
    int n, cap;
    uint16_t dtime, ddate;
} Zip;

#define ZIP_OK      0
#define ZIP_EWRITE -1   /* disk write failed */
#define ZIP_ELIMIT -2   /* would exceed classic zip limits (4 GB / 65535 files) */
#define ZIP_EMEM   -3

void zip_begin(Zip *z, FILE *f);
int  zip_add(Zip *z, const char *utf8_name, const void *data, size_t n);
int  zip_finish(Zip *z);       /* writes the directory; does not fclose */
void zip_free(Zip *z);

#endif
