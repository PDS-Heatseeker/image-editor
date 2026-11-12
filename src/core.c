/* Image Shrinker - portable image pipeline: decode -> resize -> compress
 *
 * JPEG in/out : mozjpeg (progressive, trellis-quantised, optimised Huffman)
 * WebP in/out : libwebp
 * PNG/BMP/GIF : stb_image (decode); lodepng (PNG encode, deflate for TIFF)
 * Resampling  : stb_image_resize2 (sRGB-correct, Catmull-Rom)
 *
 * Memory use is kept low by decoding large JPEGs at a reduced DCT scale
 * (1/8..7/8) when the target size allows it, and by holding one image at a time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>

#include "jpeglib.h"
#include "webp/decode.h"
#include "webp/encode.h"
#include "webp/mux.h"
#include "lodepng.h"

#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_MAX_DIMENSIONS 40000
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include "core.h"

#ifndef PNG_WINDOW
#define PNG_WINDOW 8192
#endif

#define MAX_PIXELS 300000000LL   /* refuse anything that would need > ~1 GB decoded */

enum { SRC_JPEG, SRC_WEBP, SRC_PNG, SRC_BMP, SRC_GIF, SRC_TIFF, SRC_OTHER };

typedef struct {
    unsigned char *px;
    int w, h, comp;        /* comp: 3 = RGB, 4 = RGBA */
    int full_w, full_h;    /* original stored dimensions before any decode scaling */
    int gray;              /* source had no colour */
    int orient;            /* EXIF orientation 1..8 */
    unsigned char *icc;    /* colour profile worth keeping, or NULL */
    unsigned int icc_len;
    int src;
    int src_lossless;      /* WebP source was lossless */
} Image;

static void seterr(char *err, size_t n, const char *msg)
{
    if (err && n) { strncpy(err, msg, n - 1); err[n - 1] = 0; }
}

/* ---------- target size ---------- */

static void fit_box(int w, int h, int max_w, int max_h, int *ow, int *oh)
{
    double s = 1.0;
    if (max_w > 0 && (double)max_w / w < s) s = (double)max_w / w;
    if (max_h > 0 && (double)max_h / h < s) s = (double)max_h / h;
    *ow = (int)(w * s + 0.5); if (*ow < 1) *ow = 1;
    *oh = (int)(h * s + 0.5); if (*oh < 1) *oh = 1;
}

/* Target size in *stored* (pre-rotation) coordinates. */
static void stored_target(const Image *im, const IsOptions *o, int *tw, int *th)
{
    if (o->stretch && o->max_w > 0 && o->max_h > 0) {
        if (im->orient >= 5) { *tw = o->max_h; *th = o->max_w; }
        else                 { *tw = o->max_w; *th = o->max_h; }
        return;
    }
    if (im->orient >= 5) fit_box(im->full_h, im->full_w, o->max_w, o->max_h, th, tw);
    else                 fit_box(im->full_w, im->full_h, o->max_w, o->max_h, tw, th);
}

/* ---------- EXIF orientation ---------- */

static int exif_orientation(const unsigned char *d, size_t n)
{
    int le; unsigned off, cnt, i;
    if (n < 8) return 1;
    if (d[0] == 'I' && d[1] == 'I') le = 1;
    else if (d[0] == 'M' && d[1] == 'M') le = 0;
    else return 1;
#define RD16(p) (le ? (unsigned)((p)[0] | ((p)[1] << 8)) : (unsigned)(((p)[0] << 8) | (p)[1]))
#define RD32(p) (le ? (RD16(p) | (RD16((p) + 2) << 16)) : ((RD16(p) << 16) | RD16((p) + 2)))
    off = RD32(d + 4);
    if (off > n || n - off < 2) return 1;
    cnt = RD16(d + off);
    for (i = 0; i < cnt; i++) {
        size_t e = (size_t)off + 2 + (size_t)i * 12;
        if (e + 12 > n) break;
        if (RD16(d + e) == 0x0112) {
            unsigned v = RD16(d + e + 8);
            return (v >= 1 && v <= 8) ? (int)v : 1;
        }
    }
    return 1;
#undef RD16
#undef RD32
}

/* Bake the EXIF orientation into the pixels (the tag itself gets stripped). */
static int apply_orientation(Image *im)
{
    int w = im->w, h = im->h, c = im->comp, o = im->orient;
    int dw = (o >= 5) ? h : w, dh = (o >= 5) ? w : h, x, y;
    unsigned char *dst;
    if (o <= 1 || o > 8) return 1;
    dst = (unsigned char *)malloc((size_t)dw * dh * c);
    if (!dst) return 0;
    for (y = 0; y < dh; y++) {
        unsigned char *d = dst + (size_t)y * dw * c;
        for (x = 0; x < dw; x++, d += c) {
            int sx, sy;
            switch (o) {
            case 2: sx = w - 1 - x; sy = y;         break;
            case 3: sx = w - 1 - x; sy = h - 1 - y; break;
            case 4: sx = x;         sy = h - 1 - y; break;
            case 5: sx = y;         sy = x;         break;
            case 6: sx = y;         sy = h - 1 - x; break;
            case 7: sx = w - 1 - y; sy = h - 1 - x; break;
            default: sx = w - 1 - y; sy = x;        break; /* 8 */
            }
            memcpy(d, im->px + ((size_t)sy * w + sx) * c, (size_t)c);
        }
    }
    free(im->px);
    im->px = dst; im->w = dw; im->h = dh; im->orient = 1;
    return 1;
}

/* ---------- colour profile ---------- */

static int has_bytes(const unsigned char *h, size_t hn, const char *nd, size_t nn)
{
    size_t i;
    if (hn < nn) return 0;
    for (i = 0; i + nn <= hn; i++)
        if (h[i] == (unsigned char)nd[0] && memcmp(h + i, nd, nn) == 0) return 1;
    return 0;
}

/* Keep a profile only if dropping it would change how the picture looks:
 * i.e. it is an RGB profile that is not plain sRGB (Display P3, Adobe RGB...). */
static int icc_worth_keeping(const unsigned char *icc, unsigned len)
{
    if (!icc || len < 132 || len > (1u << 20)) return 0;
    if (memcmp(icc + 16, "RGB ", 4) != 0) return 0;
    if (has_bytes(icc, len, "sRGB", 4)) return 0;
    if (has_bytes(icc, len, "s\0R\0G\0B", 7)) return 0;
    return 1;
}

/* ---------- JPEG decode (mozjpeg / libjpeg API) ---------- */

struct jerr {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
    char msg[JMSG_LENGTH_MAX];
};

static void jerr_exit(j_common_ptr c)
{
    struct jerr *e = (struct jerr *)c->err;
    (*c->err->format_message)(c, e->msg);
    longjmp(e->jb, 1);
}
static void jerr_emit(j_common_ptr c, int lvl) { (void)c; (void)lvl; }

static int decode_jpeg(const unsigned char *in, size_t n, const IsOptions *o,
                       Image *im, char *err, size_t errn)
{
    struct jpeg_decompress_struct ci;
    struct jerr je;
    unsigned char * volatile px = NULL;
    unsigned char * volatile icc = NULL;
    unsigned char * volatile row4 = NULL;
    jpeg_saved_marker_ptr m;
    int cmyk, tw, th, num;
    unsigned int icclen = 0;
    JOCTET *iccp = NULL;

    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jerr_exit;
    je.pub.emit_message = jerr_emit;
    if (setjmp(je.jb)) {
        jpeg_destroy_decompress(&ci);
        free(px); free(icc); free(row4);
        seterr(err, errn, "damaged or incomplete JPEG");
        return 0;
    }
    jpeg_create_decompress(&ci);
    jpeg_mem_src(&ci, (unsigned char *)in, (unsigned long)n);
    jpeg_save_markers(&ci, JPEG_APP0 + 1, 0xFFFF);
    jpeg_save_markers(&ci, JPEG_APP0 + 2, 0xFFFF);
    jpeg_read_header(&ci, TRUE);

    im->orient = 1;
    for (m = ci.marker_list; m; m = m->next)
        if (m->marker == JPEG_APP0 + 1 && m->data_length > 6 && memcmp(m->data, "Exif\0\0", 6) == 0) {
            im->orient = exif_orientation(m->data + 6, m->data_length - 6);
            break;
        }

    cmyk = (ci.jpeg_color_space == JCS_CMYK || ci.jpeg_color_space == JCS_YCCK);
    im->gray = (ci.jpeg_color_space == JCS_GRAYSCALE);
    if (!cmyk && jpeg_read_icc_profile(&ci, &iccp, &icclen)) {
        icc = iccp;
        if (!icc_worth_keeping(icc, icclen)) { free(icc); icc = NULL; icclen = 0; }
    }

    im->full_w = (int)ci.image_width;
    im->full_h = (int)ci.image_height;
    stored_target(im, o, &tw, &th);

    /* Decode at the smallest DCT scale that is still >= 2x the target size, so the
     * final (sharper) resampling step does the visible part of the reduction. */
    for (num = 1; num < 8; num++) {
        long sw = ((long)im->full_w * num + 7) / 8, sh = ((long)im->full_h * num + 7) / 8;
        if (sw >= 2L * tw && sh >= 2L * th) break;
    }
    ci.scale_num = (unsigned)num;
    ci.scale_denom = 8;
    ci.out_color_space = cmyk ? JCS_CMYK : JCS_RGB;
    jpeg_start_decompress(&ci);

    if ((long long)ci.output_width * ci.output_height > MAX_PIXELS) {
        jpeg_destroy_decompress(&ci);
        free(icc);
        seterr(err, errn, "image is too large");
        return 0;
    }
    px = (unsigned char *)malloc((size_t)ci.output_width * ci.output_height * 3);
    if (cmyk) row4 = (unsigned char *)malloc((size_t)ci.output_width * 4);
    if (!px || (cmyk && !row4)) {
        jpeg_destroy_decompress(&ci);
        free(px); free(icc); free(row4);
        seterr(err, errn, "not enough memory");
        return 0;
    }
    while (ci.output_scanline < ci.output_height) {
        unsigned char *dst = px + (size_t)ci.output_scanline * ci.output_width * 3;
        if (!cmyk) {
            JSAMPROW r = dst;
            jpeg_read_scanlines(&ci, &r, 1);
        } else {
            JSAMPROW r = row4;
            unsigned x;
            int inv = ci.saw_Adobe_marker;   /* Adobe CMYK JPEGs are stored inverted */
            jpeg_read_scanlines(&ci, &r, 1);
            for (x = 0; x < ci.output_width; x++) {
                int c = row4[x * 4], mm = row4[x * 4 + 1], yy = row4[x * 4 + 2], k = row4[x * 4 + 3];
                if (!inv) { c = 255 - c; mm = 255 - mm; yy = 255 - yy; k = 255 - k; }
                dst[x * 3]     = (unsigned char)((c * k + 127) / 255);
                dst[x * 3 + 1] = (unsigned char)((mm * k + 127) / 255);
                dst[x * 3 + 2] = (unsigned char)((yy * k + 127) / 255);
            }
        }
    }
    im->w = (int)ci.output_width;
    im->h = (int)ci.output_height;
    im->comp = 3;
    im->px = px;
    im->icc = icc;
    im->icc_len = icclen;
    im->src = SRC_JPEG;
    jpeg_finish_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    free(row4);
    return 1;
}

/* ---------- WebP decode ---------- */

static int decode_webp(const unsigned char *in, size_t n, Image *im, char *err, size_t errn)
{
    WebPBitstreamFeatures f;
    size_t sz;
    if (WebPGetFeatures(in, n, &f) != VP8_STATUS_OK) { seterr(err, errn, "damaged WebP"); return 0; }
    if (f.has_animation) { seterr(err, errn, "animated WebP is not supported"); return 0; }
    if ((long long)f.width * f.height > MAX_PIXELS) { seterr(err, errn, "image is too large"); return 0; }
    im->comp = f.has_alpha ? 4 : 3;
    sz = (size_t)f.width * f.height * im->comp;
    im->px = (unsigned char *)malloc(sz);
    if (!im->px) { seterr(err, errn, "not enough memory"); return 0; }
    if (!(f.has_alpha ? WebPDecodeRGBAInto(in, n, im->px, sz, f.width * 4)
                      : WebPDecodeRGBInto(in, n, im->px, sz, f.width * 3))) {
        free(im->px); im->px = NULL;
        seterr(err, errn, "damaged WebP");
        return 0;
    }
    im->w = im->full_w = f.width;
    im->h = im->full_h = f.height;
    im->orient = 1;
    im->src = SRC_WEBP;
    im->src_lossless = (f.format == 2);
    return 1;
}

/* ---------- PNG / BMP / GIF decode ---------- */

static int decode_other(const unsigned char *in, size_t n, Image *im, char *err, size_t errn)
{
    int w, h, c;
    if (n > 0x7fffffff || !stbi_info_from_memory(in, (int)n, &w, &h, &c)) {
        seterr(err, errn, "not a supported image format");
        return 0;
    }
    if ((long long)w * h > MAX_PIXELS) { seterr(err, errn, "image is too large"); return 0; }
    im->comp = (c == 2 || c == 4) ? 4 : 3;
    im->px = stbi_load_from_memory(in, (int)n, &w, &h, &c, im->comp);
    if (!im->px) {
        const char *r = stbi_failure_reason();
        seterr(err, errn, r ? r : "could not read image");
        return 0;
    }
    im->w = im->full_w = w;
    im->h = im->full_h = h;
    im->gray = (c <= 2);
    im->orient = 1;
    im->src = (n >= 4 && memcmp(in, "\x89PNG", 4) == 0) ? SRC_PNG
            : (n >= 2 && in[0] == 'B' && in[1] == 'M') ? SRC_BMP
            : (n >= 4 && memcmp(in, "GIF8", 4) == 0) ? SRC_GIF : SRC_OTHER;
    return 1;
}

/* ---------- TIFF decode (8-bit chunky gray/RGB/RGBA, none or deflate, as this app writes) ---------- */

static unsigned tget(const unsigned char *d, size_t n, size_t off, int sz)
{
    if (off + (size_t)sz > n) return 0;
    return sz == 2 ? (unsigned)(d[off] | (d[off + 1] << 8))
                   : (unsigned)(d[off] | (d[off + 1] << 8) | (d[off + 2] << 16) | ((unsigned)d[off + 3] << 24));
}

/* value i of a SHORT/LONG tag (inline or via offset) */
static unsigned tval(const unsigned char *d, size_t n, size_t e, unsigned i)
{
    unsigned type = tget(d, n, e + 2, 2), cnt = tget(d, n, e + 4, 4);
    int sz = type == 3 ? 2 : 4;
    size_t base = (size_t)cnt * sz <= 4 ? e + 8 : tget(d, n, e + 8, 4);
    if (i >= cnt) return 0;
    return tget(d, n, base + (size_t)i * sz, sz);
}

static int decode_tiff(const unsigned char *d, size_t n, Image *im, char *err, size_t errn)
{
    size_t ifd = tget(d, n, 4, 4), e, rowb, need, got = 0;
    unsigned cnt, i, w = 0, h = 0, comp = 1, spp = 1, bps = 8, pred = 1, photo = 2;
    size_t soff_e = 0, scnt_e = 0;
    unsigned nstrips = 0, s, x, y, c;
    unsigned char *raw;
    if (ifd + 2 > n) { seterr(err, errn, "damaged TIFF"); return 0; }
    cnt = tget(d, n, ifd, 2);
    for (i = 0; i < cnt; i++) {
        e = ifd + 2 + (size_t)i * 12;
        switch (tget(d, n, e, 2)) {
        case 256: w = tval(d, n, e, 0); break;
        case 257: h = tval(d, n, e, 0); break;
        case 258: bps = tval(d, n, e, 0); break;
        case 259: comp = tval(d, n, e, 0); break;
        case 262: photo = tval(d, n, e, 0); break;
        case 273: soff_e = e; nstrips = tget(d, n, e + 4, 4); break;
        case 277: spp = tval(d, n, e, 0); break;
        case 279: scnt_e = e; break;
        case 317: pred = tval(d, n, e, 0); break;
        }
    }
    if (!w || !h || bps != 8 || (spp != 1 && spp != 3 && spp != 4) || (photo != 1 && photo != 2) ||
        (comp != 1 && comp != 8 && comp != 32946) || !soff_e || !scnt_e || !nstrips) {
        seterr(err, errn, "unsupported TIFF");
        return 0;
    }
    if ((long long)w * h > MAX_PIXELS) { seterr(err, errn, "image is too large"); return 0; }
    rowb = (size_t)w * spp;
    need = rowb * h;
    raw = (unsigned char *)malloc(need);
    if (!raw) { seterr(err, errn, "not enough memory"); return 0; }
    for (s = 0; s < nstrips && got < need; s++) {
        size_t off = tval(d, n, soff_e, s), len = tval(d, n, scnt_e, s);
        if (off > n || len > n - off) break;
        if (comp == 1) {
            size_t k = len < need - got ? len : need - got;
            memcpy(raw + got, d + off, k);
            got += k;
        } else {
            unsigned char *z = NULL;
            size_t zn = 0;
            if (lodepng_zlib_decompress(&z, &zn, d + off, len, &lodepng_default_decompress_settings)) { free(z); break; }
            if (zn > need - got) zn = need - got;
            memcpy(raw + got, z, zn);
            got += zn;
            free(z);
        }
    }
    if (got < need) { free(raw); seterr(err, errn, "damaged TIFF"); return 0; }
    if (pred == 2)
        for (y = 0; y < h; y++) {
            unsigned char *r = raw + (size_t)y * rowb;
            for (x = 1; x < w; x++)
                for (c = 0; c < spp; c++) r[x * spp + c] = (unsigned char)(r[x * spp + c] + r[(x - 1) * spp + c]);
        }
    im->comp = spp == 4 ? 4 : 3;
    if (spp == 1) {
        size_t k, np = (size_t)w * h;
        unsigned char *rgb = (unsigned char *)malloc(np * 3);
        if (!rgb) { free(raw); seterr(err, errn, "not enough memory"); return 0; }
        for (k = 0; k < np; k++) rgb[k * 3] = rgb[k * 3 + 1] = rgb[k * 3 + 2] = photo == 0 ? (unsigned char)(255 - raw[k]) : raw[k];
        free(raw);
        raw = rgb;
        im->gray = 1;
    }
    im->px = raw;
    im->w = im->full_w = (int)w;
    im->h = im->full_h = (int)h;
    im->orient = 1;
    im->src = SRC_TIFF;
    return 1;
}

/* ---------- encoders ---------- */

static int encode_jpeg(const Image *im, int quality, unsigned char **out, size_t *outn,
                       char *err, size_t errn)
{
    struct jpeg_compress_struct ci;
    struct jerr je;
    unsigned char * volatile mem = NULL;
    unsigned char * volatile row = NULL;
    unsigned long memn = 0;
    int nc = im->gray ? 1 : 3, x;

    if (im->w > 65500 || im->h > 65500) {
        seterr(err, errn, "too large for JPEG (max 65,500 px) - choose a smaller size");
        return 0;
    }

    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jerr_exit;
    je.pub.emit_message = jerr_emit;
    if (setjmp(je.jb)) {
        jpeg_destroy_compress(&ci);
        free(mem); free(row);
        seterr(err, errn, je.msg[0] ? je.msg : "JPEG encoding failed");
        return 0;
    }
    jpeg_create_compress(&ci);
    jpeg_mem_dest(&ci, (unsigned char **)&mem, &memn);
    ci.image_width = (JDIMENSION)im->w;
    ci.image_height = (JDIMENSION)im->h;
    ci.input_components = nc;
    ci.in_color_space = im->gray ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&ci);            /* mozjpeg default profile = max compression */
    jpeg_set_quality(&ci, quality, TRUE);
    if (!im->gray) {                   /* keep full colour detail at high quality */
        /* >= 85: full colour resolution (4:4:4); 70-84: 4:2:2; below: 4:2:0 */
        ci.comp_info[0].h_samp_factor = quality >= 85 ? 1 : 2;
        ci.comp_info[0].v_samp_factor = quality >= 70 ? 1 : 2;
    }
    jpeg_start_compress(&ci, TRUE);
    if (im->icc && !im->gray) jpeg_write_icc_profile(&ci, im->icc, im->icc_len);

    row = (unsigned char *)malloc((size_t)im->w * nc);
    if (!row) {
        jpeg_destroy_compress(&ci);
        free(mem);
        seterr(err, errn, "not enough memory");
        return 0;
    }
    while (ci.next_scanline < ci.image_height) {
        const unsigned char *s = im->px + (size_t)ci.next_scanline * im->w * im->comp;
        JSAMPROW r = row;
        if (im->comp == 3 && nc == 3) {
            r = (JSAMPROW)s;
        } else {
            for (x = 0; x < im->w; x++, s += im->comp) {
                int rr = s[0], gg = s[1], bb = s[2];
                if (im->comp == 4) {              /* flatten transparency onto white */
                    int a = s[3];
                    rr = (rr * a + 255 * (255 - a) + 127) / 255;
                    gg = (gg * a + 255 * (255 - a) + 127) / 255;
                    bb = (bb * a + 255 * (255 - a) + 127) / 255;
                }
                if (nc == 1) row[x] = (unsigned char)rr;
                else { row[x * 3] = (unsigned char)rr; row[x * 3 + 1] = (unsigned char)gg; row[x * 3 + 2] = (unsigned char)bb; }
            }
        }
        jpeg_write_scanlines(&ci, &r, 1);
    }
    jpeg_finish_compress(&ci);
    jpeg_destroy_compress(&ci);
    free(row);
    *out = mem;
    *outn = (size_t)memn;
    return 1;
}

static int encode_webp(const Image *im, int quality, int lossless, unsigned char **out, size_t *outn,
                       char *err, size_t errn)
{
    WebPConfig cfg;
    WebPPicture pic;
    WebPMemoryWriter wr;
    int ok;

    if (im->w > 16383 || im->h > 16383) {
        seterr(err, errn, "too large for WebP (max 16383 px) - choose a smaller size or JPEG");
        return 0;
    }
    if (!WebPConfigPreset(&cfg, WEBP_PRESET_DEFAULT, (float)quality) || !WebPPictureInit(&pic)) {
        seterr(err, errn, "WebP encoder error");
        return 0;
    }
    if (lossless) {
        if (!WebPConfigLosslessPreset(&cfg, 6)) { seterr(err, errn, "WebP encoder error"); return 0; }
        pic.use_argb = 1;
    } else {
        cfg.method = 6;                /* slowest / smallest */
        cfg.use_sharp_yuv = 1;         /* crisper colour edges */
    }
    pic.width = im->w;
    pic.height = im->h;
    ok = (im->comp == 4) ? WebPPictureImportRGBA(&pic, im->px, im->w * 4)
                         : WebPPictureImportRGB(&pic, im->px, im->w * 3);
    if (!ok) { WebPPictureFree(&pic); seterr(err, errn, "not enough memory"); return 0; }
    WebPMemoryWriterInit(&wr);
    pic.writer = WebPMemoryWrite;
    pic.custom_ptr = &wr;
    ok = WebPEncode(&cfg, &pic);
    WebPPictureFree(&pic);
    if (!ok) { WebPMemoryWriterClear(&wr); seterr(err, errn, "WebP encoding failed"); return 0; }

    *out = NULL;
    if (im->icc) {                     /* wrap with the colour profile */
        WebPMux *mux = WebPMuxNew();
        WebPData img, prof, res;
        img.bytes = wr.mem;  img.size = wr.size;
        prof.bytes = im->icc; prof.size = im->icc_len;
        memset(&res, 0, sizeof res);
        if (mux && WebPMuxSetImage(mux, &img, 0) == WEBP_MUX_OK &&
            WebPMuxSetChunk(mux, "ICCP", &prof, 0) == WEBP_MUX_OK &&
            WebPMuxAssemble(mux, &res) == WEBP_MUX_OK) {
            *out = (unsigned char *)malloc(res.size);
            if (*out) { memcpy(*out, res.bytes, res.size); *outn = res.size; }
        }
        WebPDataClear(&res);
        if (mux) WebPMuxDelete(mux);
    }
    if (!*out) {
        *out = (unsigned char *)malloc(wr.size ? wr.size : 1);
        if (*out) { memcpy(*out, wr.mem, wr.size); *outn = wr.size; }
    }
    WebPMemoryWriterClear(&wr);
    if (!*out) { seterr(err, errn, "not enough memory"); return 0; }
    return 1;
}

/* Flattened copy for formats without transparency: RGB (or 1-channel gray). */
static unsigned char *flat_pixels(const Image *im, int nc)
{
    size_t np = (size_t)im->w * im->h, i;
    unsigned char *d = (unsigned char *)malloc(np ? np * (size_t)nc : 1);
    const unsigned char *s = im->px;
    if (!d) return NULL;
    for (i = 0; i < np; i++, s += im->comp) {
        int rr = s[0], gg = s[1], bb = s[2];
        if (im->comp == 4) {
            int a = s[3];
            rr = (rr * a + 255 * (255 - a) + 127) / 255;
            gg = (gg * a + 255 * (255 - a) + 127) / 255;
            bb = (bb * a + 255 * (255 - a) + 127) / 255;
        }
        if (nc == 1) d[i] = (unsigned char)rr;
        else { d[i * 3] = (unsigned char)rr; d[i * 3 + 1] = (unsigned char)gg; d[i * 3 + 2] = (unsigned char)bb; }
    }
    return d;
}

static int encode_png(const Image *im, unsigned char **out, size_t *outn, char *err, size_t errn)
{
    LodePNGState st;
    unsigned e;
    lodepng_state_init(&st);
    st.info_raw.colortype = im->comp == 4 ? LCT_RGBA : LCT_RGB;
    st.info_raw.bitdepth = 8;
    st.encoder.auto_convert = 1;                 /* picks palette / gray / RGB automatically */
    st.encoder.zlibsettings.windowsize = PNG_WINDOW;
    if (im->icc) lodepng_set_icc(&st.info_png, "ICC profile", im->icc, im->icc_len);
    *out = NULL; *outn = 0;
    e = lodepng_encode(out, outn, im->px, (unsigned)im->w, (unsigned)im->h, &st);
    lodepng_state_cleanup(&st);
    if (e) { free(*out); *out = NULL; seterr(err, errn, e == 83 ? "not enough memory" : "PNG encoding failed"); return 0; }
    return 1;
}

static void w16(unsigned char *b, unsigned v) { b[0] = (unsigned char)v; b[1] = (unsigned char)(v >> 8); }
static void w32(unsigned char *b, unsigned long v) { w16(b, (unsigned)(v & 0xFFFF)); w16(b + 2, (unsigned)(v >> 16)); }

/* Lossless TIFF: one deflate-compressed strip with horizontal differencing. */
static int encode_tiff(const Image *im, unsigned char **out, size_t *outn, char *err, size_t errn)
{
    int gray = im->gray && im->comp == 3, alpha = im->comp == 4, keep_icc = im->icc && !gray;
    int spp = gray ? 1 : im->comp, nent = 11 + (alpha ? 1 : 0) + (keep_icc ? 1 : 0), x, y, c;
    size_t rowb = (size_t)im->w * spp, rawn = rowb * im->h, zn = 0, zpad, ifd, extra, total;
    unsigned char *raw, *z = NULL, *o, *e;
    LodePNGCompressSettings zs;

    if (gray) raw = flat_pixels(im, 1);
    else { raw = (unsigned char *)malloc(rawn ? rawn : 1); if (raw) memcpy(raw, im->px, rawn); }
    if (!raw) { seterr(err, errn, "not enough memory"); return 0; }
    for (y = 0; y < im->h; y++) {
        unsigned char *r = raw + (size_t)y * rowb;
        for (x = im->w - 1; x >= 1; x--)
            for (c = 0; c < spp; c++) r[x * spp + c] = (unsigned char)(r[x * spp + c] - r[(x - 1) * spp + c]);
    }
    lodepng_compress_settings_init(&zs);
    zs.windowsize = 8192;
    if (lodepng_zlib_compress(&z, &zn, raw, rawn, &zs) || zn > 0xF0000000u) {
        free(raw); free(z);
        seterr(err, errn, "TIFF encoding failed");
        return 0;
    }
    free(raw);
    zpad = zn + (zn & 1);
    ifd = 8 + zpad;
    extra = ifd + 2 + (size_t)nent * 12 + 4;        /* BitsPerSample array, then ICC */
    total = extra + (spp > 2 ? (size_t)spp * 2 : 0) + (keep_icc ? im->icc_len : 0);
    o = (unsigned char *)calloc(total, 1);
    if (!o) { free(z); seterr(err, errn, "not enough memory"); return 0; }
    memcpy(o, "II*\0", 4);
    w32(o + 4, (unsigned long)ifd);
    memcpy(o + 8, z, zn);
    free(z);
    e = o + ifd;
    w16(e, (unsigned)nent); e += 2;
#define ENT(tag, type, count, val) do { w16(e, tag); w16(e + 2, type); w32(e + 4, count); w32(e + 8, val); e += 12; } while (0)
    ENT(256, 4, 1, (unsigned long)im->w);
    ENT(257, 4, 1, (unsigned long)im->h);
    if (spp > 2) ENT(258, 3, (unsigned long)spp, (unsigned long)extra); else ENT(258, 3, 1, 8);
    ENT(259, 3, 1, 8);                               /* Adobe deflate */
    ENT(262, 3, 1, gray ? 1 : 2);
    ENT(273, 4, 1, 8);
    ENT(277, 3, 1, (unsigned long)spp);
    ENT(278, 4, 1, (unsigned long)im->h);
    ENT(279, 4, 1, (unsigned long)zn);
    ENT(284, 3, 1, 1);
    ENT(317, 3, 1, 2);                               /* horizontal predictor */
    if (alpha) ENT(338, 3, 1, 2);                    /* unassociated alpha */
    if (keep_icc) ENT(34675u, 7, im->icc_len, (unsigned long)(extra + (spp > 2 ? (size_t)spp * 2 : 0)));
#undef ENT
    if (spp > 2) for (c = 0; c < spp; c++) w16(o + extra + c * 2, 8);
    if (keep_icc) memcpy(o + extra + (spp > 2 ? (size_t)spp * 2 : 0), im->icc, im->icc_len);
    *out = o; *outn = total;
    return 1;
}

/* Plain 24-bit BMP (transparency flattened onto white). */
static int encode_bmp(const Image *im, unsigned char **out, size_t *outn, char *err, size_t errn)
{
    size_t stride = ((size_t)im->w * 3 + 3) & ~(size_t)3, total = 54 + stride * im->h;
    unsigned char *px = flat_pixels(im, 3), *o;
    int x, y;
    if (!px || total > 0xF0000000u) { free(px); seterr(err, errn, px ? "too large for BMP" : "not enough memory"); return 0; }
    o = (unsigned char *)calloc(total, 1);
    if (!o) { free(px); seterr(err, errn, "not enough memory"); return 0; }
    o[0] = 'B'; o[1] = 'M';
    w32(o + 2, (unsigned long)total); w32(o + 10, 54);
    w32(o + 14, 40); w32(o + 18, (unsigned long)im->w); w32(o + 22, (unsigned long)im->h);
    w16(o + 26, 1); w16(o + 28, 24);
    w32(o + 34, (unsigned long)(stride * im->h)); w32(o + 38, 2835); w32(o + 42, 2835);
    for (y = 0; y < im->h; y++) {
        const unsigned char *s = px + (size_t)(im->h - 1 - y) * im->w * 3;
        unsigned char *d = o + 54 + (size_t)y * stride;
        for (x = 0; x < im->w; x++, s += 3, d += 3) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; }
    }
    free(px);
    *out = o; *outn = total;
    return 1;
}

/* ---------- pipeline ---------- */

typedef struct {
    int resized;
    int src_w, src_h;     /* as displayed (after EXIF rotation) */
    int full_w, full_h;   /* as stored */
    int orient;           /* EXIF orientation of the source */
    int tw, th;           /* target, stored orientation */
} PrepInfo;

/* decode -> resize -> drop opaque alpha -> bake in rotation */
static int prepare(const unsigned char *in, size_t n, const IsOptions *opt, Image *im, PrepInfo *pi,
                   char *err, size_t errn)
{
    int ok;
    memset(im, 0, sizeof *im);
    memset(pi, 0, sizeof *pi);
    if (n >= 3 && in[0] == 0xFF && in[1] == 0xD8 && in[2] == 0xFF)
        ok = decode_jpeg(in, n, opt, im, err, errn);
    else if (n >= 12 && memcmp(in, "RIFF", 4) == 0 && memcmp(in + 8, "WEBP", 4) == 0)
        ok = decode_webp(in, n, im, err, errn);
    else if (n >= 8 && memcmp(in, "II*\0", 4) == 0)
        ok = decode_tiff(in, n, im, err, errn);
    else
        ok = decode_other(in, n, im, err, errn);
    if (!ok) return 0;

    pi->full_w = im->full_w;
    pi->full_h = im->full_h;
    pi->orient = im->orient;
    pi->src_w = im->orient >= 5 ? im->full_h : im->full_w;
    pi->src_h = im->orient >= 5 ? im->full_w : im->full_h;
    stored_target(im, opt, &pi->tw, &pi->th);
    pi->resized = (pi->tw != im->full_w || pi->th != im->full_h);
    if ((long long)pi->tw * pi->th > MAX_PIXELS) {
        free(im->px); free(im->icc); im->px = NULL; im->icc = NULL;
        seterr(err, errn, "the chosen size is too large");
        return 0;
    }

    if (im->w != pi->tw || im->h != pi->th) {
        unsigned char *dst = (unsigned char *)malloc((size_t)pi->tw * pi->th * im->comp);
        if (!dst || !stbir_resize(im->px, im->w, im->h, 0, dst, pi->tw, pi->th, 0,
                                  im->comp == 4 ? STBIR_RGBA : STBIR_RGB, STBIR_TYPE_UINT8_SRGB,
                                  STBIR_EDGE_CLAMP, STBIR_FILTER_CATMULLROM)) {
            free(dst); free(im->px); free(im->icc); im->px = NULL; im->icc = NULL;
            seterr(err, errn, "not enough memory");
            return 0;
        }
        free(im->px);
        im->px = dst; im->w = pi->tw; im->h = pi->th;
    }

    if (im->comp == 4) {               /* drop an alpha channel that is fully opaque */
        size_t i, np = (size_t)im->w * im->h;
        int opaque = 1;
        for (i = 0; i < np; i++) if (im->px[i * 4 + 3] != 255) { opaque = 0; break; }
        if (opaque) {
            for (i = 0; i < np; i++) memmove(im->px + i * 3, im->px + i * 4, 3);
            im->comp = 3;
        }
    }

    if (!apply_orientation(im)) {
        free(im->px); free(im->icc); im->px = NULL; im->icc = NULL;
        seterr(err, errn, "not enough memory");
        return 0;
    }
    return 1;
}

static int resolve_format(int src, int src_lossless, int fmt)
{
    if (fmt != IS_FMT_SAME) return fmt;
    return src == SRC_JPEG ? IS_FMT_JPEG
         : src == SRC_WEBP ? (src_lossless ? IS_FMT_WEBP_LOSSLESS : IS_FMT_WEBP)
         : src == SRC_BMP  ? IS_FMT_BMP
         : src == SRC_TIFF ? IS_FMT_TIFF
         : IS_FMT_PNG;                          /* PNG, and GIF (first frame) */
}

static int same_family(int src, int fmt)
{
    return (src == SRC_JPEG && fmt == IS_FMT_JPEG) || (src == SRC_PNG && fmt == IS_FMT_PNG) ||
           (src == SRC_BMP && fmt == IS_FMT_BMP) ||
           (src == SRC_WEBP && (fmt == IS_FMT_WEBP || fmt == IS_FMT_WEBP_LOSSLESS));
}

static int clamp_q(int q) { return q < 1 ? 1 : (q > 100 ? 100 : q); }

static int encode_any(const Image *im, int fmt, int q, unsigned char **out, size_t *outn,
                      char *err, size_t errn)
{
    switch (fmt) {
    case IS_FMT_PNG:           return encode_png(im, out, outn, err, errn);
    case IS_FMT_WEBP:          return encode_webp(im, q, 0, out, outn, err, errn);
    case IS_FMT_WEBP_LOSSLESS: return encode_webp(im, q, 1, out, outn, err, errn);
    case IS_FMT_TIFF:          return encode_tiff(im, out, outn, err, errn);
    case IS_FMT_BMP:           return encode_bmp(im, out, outn, err, errn);
    default:                   return encode_jpeg(im, q, out, outn, err, errn);
    }
}

int is_process(const unsigned char *in, size_t n, const IsOptions *opt,
               IsResult *res, char *err, size_t errn)
{
    Image im;
    PrepInfo pi;
    int ok, fmt;

    memset(res, 0, sizeof *res);
    if (err && errn) err[0] = 0;
    if (!prepare(in, n, opt, &im, &pi, err, errn)) return 0;

    fmt = resolve_format(im.src, im.src_lossless, opt->format);
    if (fmt < IS_FMT_JPEG || fmt > IS_FMT_BMP) fmt = IS_FMT_JPEG;
    ok = encode_any(&im, fmt, clamp_q(opt->quality), &res->data, &res->size, err, errn);
    res->ext = fmt == IS_FMT_PNG ? ".png" : fmt == IS_FMT_TIFF ? ".tif" : fmt == IS_FMT_BMP ? ".bmp"
             : (fmt == IS_FMT_WEBP || fmt == IS_FMT_WEBP_LOSSLESS) ? ".webp"
             : opt->jpeg_long_ext ? ".jpeg" : ".jpg";
    res->out_fmt = fmt;
    res->w = im.w;
    res->h = im.h;
    res->src_w = pi.src_w;
    res->src_h = pi.src_h;
    free(im.px);
    free(im.icc);
    if (!ok) { res->data = NULL; return 0; }

    /* Never make a file bigger: if it was already this format, needed no resize,
     * and re-encoding did not help, pass the original through untouched. */
    if (!pi.resized && same_family(im.src, fmt) && res->size >= n) {
        unsigned char *copy = (unsigned char *)malloc(n);
        if (copy) {
            memcpy(copy, in, n);
            free(res->data);
            res->data = copy;
            res->size = n;
            res->kept_original = 1;
        }
    }
    return 1;
}

void is_free(IsResult *res)
{
    if (res) { free(res->data); res->data = NULL; res->size = 0; }
}

/* ---------- header probe ---------- */

int is_probe(const unsigned char *d, size_t n, int *w, int *h)
{
    *w = *h = 0;
    if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
        size_t i = 2;
        int orient = 1;
        while (i + 4 <= n) {
            unsigned m, len;
            if (d[i] != 0xFF) { i++; continue; }
            m = d[i + 1];
            if (m == 0xFF) { i++; continue; }
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
            if (m == 0xD9 || m == 0xDA) break;
            len = ((unsigned)d[i + 2] << 8) | d[i + 3];
            if (len < 2) break;
            if (m == 0xE1 && len >= 8 && i + 2 + len <= n && memcmp(d + i + 4, "Exif\0\0", 6) == 0)
                orient = exif_orientation(d + i + 10, len - 8);
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                if (i + 9 > n) break;
                *h = (d[i + 5] << 8) | d[i + 6];
                *w = (d[i + 7] << 8) | d[i + 8];
                if (orient >= 5) { int t = *w; *w = *h; *h = t; }
                return *w > 0 && *h > 0;
            }
            i += 2 + len;
        }
        return 0;
    }
    if (n >= 24 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) {
        *w = (int)(((unsigned)d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19]);
        *h = (int)(((unsigned)d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23]);
        return *w > 0 && *h > 0;
    }
    if (n >= 10 && memcmp(d, "GIF8", 4) == 0) {
        *w = d[6] | (d[7] << 8);
        *h = d[8] | (d[9] << 8);
        return *w > 0 && *h > 0;
    }
    if (n >= 26 && d[0] == 'B' && d[1] == 'M') {
        unsigned hs = d[14] | (d[15] << 8) | (d[16] << 16) | ((unsigned)d[17] << 24);
        if (hs == 12) { *w = d[18] | (d[19] << 8); *h = d[20] | (d[21] << 8); }
        else {
            *w = (int)(d[18] | (d[19] << 8) | (d[20] << 16) | ((unsigned)d[21] << 24));
            *h = (int)(d[22] | (d[23] << 8) | (d[24] << 16) | ((unsigned)d[25] << 24));
            if (*h < 0) *h = -*h;
        }
        return *w > 0 && *h > 0;
    }
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0) {
        WebPBitstreamFeatures f;
        if (WebPGetFeatures(d, n, &f) != VP8_STATUS_OK) return 0;
        *w = f.width; *h = f.height;
        return *w > 0 && *h > 0;
    }
    return 0;
}

/* ---------- size estimation ---------- */

#ifndef SAMPLE_T
#define SAMPLE_T 48     /* tile edge (multiple of 16 so JPEG/WebP blocks never straddle tiles) */
#endif
#ifndef SAMPLE_G
#define SAMPLE_G 6      /* G x G tiles, one from the centre of each cell of a G x G grid */
#endif

struct IsSample {
    Image tile, tiny;             /* sample mosaic, and a 16x16 corner of it */
    unsigned char *icc;
    unsigned icc_len;
    long long total_px, tile_px, tiny_px;
    int exact;                    /* sample is the whole picture -> estimate is exact */
    int full_w, full_h, orient;   /* to check whether new size options still match */
    int tw, th;
    int src_w, src_h, out_w, out_h;
    int src, resized;
    size_t in_len;
};

static void copy_rect(const Image *s, int x0, int y0, int w, int h, unsigned char *d, int dstride)
{
    int y;
    for (y = 0; y < h; y++)
        memcpy(d + (size_t)y * dstride, s->px + ((size_t)(y0 + y) * s->w + x0) * s->comp, (size_t)w * s->comp);
}

IsSample *is_sample_create(const unsigned char *in, size_t n, const IsOptions *opt, char *err, size_t errn)
{
    Image im;
    PrepInfo pi;
    IsSample *s;
    int c, tw;

    if (err && errn) err[0] = 0;
    if (!prepare(in, n, opt, &im, &pi, err, errn)) return NULL;
    s = (IsSample *)calloc(1, sizeof *s);
    if (!s) { free(im.px); free(im.icc); seterr(err, errn, "not enough memory"); return NULL; }
    c = im.comp;
    s->icc = im.icc; s->icc_len = im.icc_len;
    s->full_w = pi.full_w; s->full_h = pi.full_h; s->orient = pi.orient;
    s->tw = pi.tw; s->th = pi.th;
    s->src_w = pi.src_w; s->src_h = pi.src_h;
    s->out_w = im.w; s->out_h = im.h;
    s->src = im.src; s->resized = pi.resized; s->in_len = n;
    s->total_px = (long long)im.w * im.h;

    s->tile = im;                                 /* copies w/h/comp/gray/icc/src fields */
    tw = SAMPLE_T * SAMPLE_G;
#ifndef EXACT_PX
#define EXACT_PX 262144
#endif
    if (s->total_px <= EXACT_PX || s->total_px <= (long long)tw * tw ||
        im.w < 2 * SAMPLE_T || im.h < 2 * SAMPLE_T) {
        s->exact = 1;                             /* keep the whole (small) picture */
    } else {
        int gi, gj;
        unsigned char *m = (unsigned char *)malloc((size_t)tw * tw * c);
        if (!m) { free(im.px); free(im.icc); free(s); seterr(err, errn, "not enough memory"); return NULL; }
        for (gj = 0; gj < SAMPLE_G; gj++)
            for (gi = 0; gi < SAMPLE_G; gi++) {
                int x0 = (int)((long long)im.w * (2 * gi + 1) / (2 * SAMPLE_G)) - SAMPLE_T / 2;
                int y0 = (int)((long long)im.h * (2 * gj + 1) / (2 * SAMPLE_G)) - SAMPLE_T / 2;
                if (x0 < 0) x0 = 0;
                if (y0 < 0) y0 = 0;
                if (x0 > im.w - SAMPLE_T) x0 = im.w - SAMPLE_T;
                if (y0 > im.h - SAMPLE_T) y0 = im.h - SAMPLE_T;
                copy_rect(&im, x0, y0, SAMPLE_T, SAMPLE_T,
                          m + ((size_t)gj * SAMPLE_T * tw + (size_t)gi * SAMPLE_T) * c, tw * c);
            }
        free(im.px);
        s->tile.px = m; s->tile.w = tw; s->tile.h = tw;
    }
    s->tile_px = (long long)s->tile.w * s->tile.h;

    /* a 16x16 corner of the sample gives the second point of the size model */
    s->tiny = s->tile;
    s->tiny.w = s->tile.w < 16 ? s->tile.w : 16;
    s->tiny.h = s->tile.h < 16 ? s->tile.h : 16;
    s->tiny.px = (unsigned char *)malloc((size_t)s->tiny.w * s->tiny.h * c);
    if (!s->tiny.px) { is_sample_free(s); seterr(err, errn, "not enough memory"); return NULL; }
    copy_rect(&s->tile, 0, 0, s->tiny.w, s->tiny.h, s->tiny.px, s->tiny.w * c);
    s->tiny_px = (long long)s->tiny.w * s->tiny.h;
    return s;
}

int is_sample_fits(const IsSample *s, const IsOptions *opt)
{
    Image t;
    int tw, th;
    memset(&t, 0, sizeof t);
    t.full_w = s->full_w; t.full_h = s->full_h; t.orient = s->orient;
    stored_target(&t, opt, &tw, &th);
    return tw == s->tw && th == s->th;
}

int is_sample_estimate(const IsSample *s, const IsOptions *opt, long long *bytes, int *kept,
                       char *err, size_t errn)
{
    int fmt = resolve_format(s->src, s->tile.src_lossless, opt->format), q = clamp_q(opt->quality);
    unsigned char *o = NULL;
    size_t on = 0, tn = 0;
    long long est;

    if (err && errn) err[0] = 0;
    *bytes = 0;
    if (kept) *kept = 0;
    if (fmt < IS_FMT_JPEG || fmt > IS_FMT_BMP) fmt = IS_FMT_JPEG;
    if ((fmt == IS_FMT_WEBP || fmt == IS_FMT_WEBP_LOSSLESS) && (s->out_w > 16383 || s->out_h > 16383)) {
        seterr(err, errn, "too large for WebP (max 16,383 px)");
        return 0;
    }
    if (fmt == IS_FMT_JPEG && (s->out_w > 65500 || s->out_h > 65500)) {
        seterr(err, errn, "too large for JPEG (max 65,500 px)");
        return 0;
    }
    if (fmt == IS_FMT_BMP) {
        est = 54 + (long long)(((long long)s->out_w * 3 + 3) & ~3LL) * s->out_h;
    } else {
        if (!encode_any(&s->tile, fmt, q, &o, &on, err, errn)) return 0;
        free(o); o = NULL;
        if (s->exact) {
            est = (long long)on;
        } else {
            /* two-point model: bytes = header + rate * pixels */
            double rate, head;
            if (!encode_any(&s->tiny, fmt, q, &o, &tn, err, errn)) return 0;
            free(o);
            rate = ((double)on - (double)tn) / (double)(s->tile_px - s->tiny_px);
            if (rate < 0) rate = 0;
            head = (double)tn - rate * (double)s->tiny_px;
            if (head < 0) head = 0;
            est = (long long)(head + rate * (double)s->total_px + 0.5);
        }
    }
    if (!s->resized && same_family(s->src, fmt) && est >= (long long)s->in_len) {
        est = (long long)s->in_len;
        if (kept) *kept = 1;
    }
    *bytes = est;
    return 1;
}

void is_sample_info(const IsSample *s, int *src_w, int *src_h, int *out_w, int *out_h)
{
    *src_w = s->src_w; *src_h = s->src_h; *out_w = s->out_w; *out_h = s->out_h;
}

size_t is_sample_mem(const IsSample *s)
{
    return sizeof *s + (size_t)s->tile_px * s->tile.comp + (size_t)s->tiny_px * s->tiny.comp + s->icc_len;
}

void is_sample_free(IsSample *s)
{
    if (!s) return;
    free(s->tile.px);
    free(s->tiny.px);
    free(s->icc);
    free(s);
}

/* ---------- preview ---------- */

int is_preview_decode(const unsigned char *in, size_t n, int w, int h, int stretch,
                      unsigned char **bgra, int *ow, int *oh, char *err, size_t errn)
{
    Image im;
    PrepInfo pi;
    IsOptions o;
    size_t i, np;
    unsigned char *d;
    int x, y;

    memset(&o, 0, sizeof o);
    o.max_w = w; o.max_h = h; o.stretch = stretch;
    *bgra = NULL;
    if (err && errn) err[0] = 0;
    if (!prepare(in, n, &o, &im, &pi, err, errn)) return 0;
    np = (size_t)im.w * im.h;
    d = (unsigned char *)malloc(np * 4);
    if (!d) { free(im.px); free(im.icc); seterr(err, errn, "not enough memory"); return 0; }
    for (y = 0, i = 0; y < im.h; y++)
        for (x = 0; x < im.w; x++, i++) {
            const unsigned char *s = im.px + i * im.comp;
            unsigned char *t = d + i * 4;
            if (im.comp == 4 && s[3] != 255) {    /* transparency shown over a checkerboard */
                int bg = (((x >> 3) + (y >> 3)) & 1) ? 204 : 255, a = s[3];
                t[2] = (unsigned char)((s[0] * a + bg * (255 - a) + 127) / 255);
                t[1] = (unsigned char)((s[1] * a + bg * (255 - a) + 127) / 255);
                t[0] = (unsigned char)((s[2] * a + bg * (255 - a) + 127) / 255);
            } else {
                t[2] = s[0]; t[1] = s[1]; t[0] = s[2];
            }
            t[3] = 255;
        }
    *bgra = d; *ow = im.w; *oh = im.h;
    free(im.px);
    free(im.icc);
    return 1;
}
