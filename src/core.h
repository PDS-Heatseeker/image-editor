/* Image Shrinker - portable image pipeline: decode -> resize -> compress */
#ifndef IS_CORE_H
#define IS_CORE_H
#include <stddef.h>

#define IS_FMT_SAME          0   /* keep each image's own format */
#define IS_FMT_JPEG          1
#define IS_FMT_PNG           2   /* lossless */
#define IS_FMT_WEBP          3
#define IS_FMT_WEBP_LOSSLESS 4
#define IS_FMT_TIFF          5   /* lossless (deflate) */
#define IS_FMT_BMP           6   /* uncompressed */

typedef struct {
    int max_w;     /* 0 = no limit */
    int max_h;     /* 0 = no limit */
    int quality;   /* 1..100, used by JPEG and lossy WebP */
    int format;    /* IS_FMT_* */
    int jpeg_long_ext;  /* 1 = name JPEG files ".jpeg" instead of ".jpg" */
    int stretch;        /* 1 = make every image exactly max_w x max_h (may distort or enlarge) */
} IsOptions;

typedef struct {
    unsigned char *data;   /* encoded output, free with is_free() */
    size_t size;
    int w, h;              /* output dimensions */
    int src_w, src_h;      /* source dimensions (as displayed) */
    int kept_original;     /* 1 = original bytes passed through (re-encoding would not have been smaller) */
    const char *ext;       /* ".jpg", ".webp", ... */
    int out_fmt;           /* IS_FMT_* actually written (never IS_FMT_SAME) */
} IsResult;

/* Returns 1 on success, 0 on failure (err filled with a short reason). */
int  is_process(const unsigned char *in, size_t n, const IsOptions *opt,
                IsResult *res, char *err, size_t errn);
void is_free(IsResult *res);

/* Displayed size (after EXIF rotation) read from the file header; the first ~512 KB is enough. */
int  is_probe(const unsigned char *in, size_t n, int *w, int *h);

/* Size estimation: decode + resize once into a small sample (~60k pixels), then
 * estimate the encoded size for any format/quality cheaply. */
typedef struct IsSample IsSample;
IsSample *is_sample_create(const unsigned char *in, size_t n, const IsOptions *opt, char *err, size_t errn);
int    is_sample_fits(const IsSample *s, const IsOptions *opt);   /* still valid for these size options? */
int    is_sample_estimate(const IsSample *s, const IsOptions *opt, long long *bytes, int *kept,
                          char *err, size_t errn);
void   is_sample_info(const IsSample *s, int *src_w, int *src_h, int *out_w, int *out_h);
size_t is_sample_mem(const IsSample *s);
void   is_sample_free(IsSample *s);

/* Decode any supported file (including this app's TIFF output) for display:
 * fitted inside w x h (0 = no limit), or exactly w x h if stretch. Returns 32-bit BGRA, top-down,
 * transparency drawn over a checkerboard. free() the pixels. */
int    is_preview_decode(const unsigned char *in, size_t n, int w, int h, int stretch,
                         unsigned char **bgra, int *ow, int *oh, char *err, size_t errn);

#endif
