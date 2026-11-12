/* Image Shrinker - Windows interface.
 *
 * Threads:
 *   UI thread          window, list, options
 *   estimator thread   lowest priority; decodes each image once into a small cached
 *                      sample and estimates the output size for the current settings
 *   job thread         below-normal priority; does the real work into the zip
 *   update thread      one quick check of GitHub Releases at start-up
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
#include "core.h"
#include "zipw.h"
#include "update.h"
#include "preview.h"

#ifndef APP_VERSION
#define APP_VERSION "1.3.0"
#endif
#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)
#define APP_NAME L"Image Shrinker"
#define REPO_URL L"https://github.com/PDS-Heatseeker/image-editor"

enum {
    ID_LIST = 100, ID_ADD, ID_ADDFOLDER, ID_REMOVE, ID_CLEAR, ID_COUNT, ID_TOTALS,
    ID_PRESET, ID_CUSTOM, ID_LOCK, ID_LINK, ID_W, ID_H, ID_HINT,
    ID_FORMAT, ID_QUALITY, ID_QLABEL, ID_PROGRESS, ID_STATUS, ID_GO, ID_RESTART,
    ID_PREVIEW, ID_SAVE, ID_SAVE_FOLDER, ID_SAVE_ZIP, ID_SAVEINFO, ID_SAVEHINT
};
#define WM_JOB_PROGRESS (WM_APP + 1)   /* wParam = images finished, lParam = heap wchar_t* status */
#define WM_JOB_DONE     (WM_APP + 2)   /* lParam = JobResult* */
#define WM_JOB_ITEM     (WM_APP + 3)   /* lParam = ItemRes* (real result) */
#define WM_EST_RESULT   (WM_APP + 4)   /* lParam = ItemRes* (estimate) */
#define WM_UPDATE       (WM_APP + 5)   /* wParam = installable, lParam = heap wchar_t* version */
#define TIMER_EST 1
#define TMP_PREFIX L"ImageShrinker-"

#define COL_TEXT   RGB(28, 32, 38)
#define COL_DIM    RGB(118, 124, 132)
#define COL_STALE  RGB(170, 174, 180)
#define COL_GREEN  RGB(16, 124, 16)
#define COL_RED    RGB(196, 43, 28)
#define COL_SEL    RGB(204, 228, 247)
#define COL_ALT    RGB(246, 248, 250)
#define COL_ACCENT RGB(0, 103, 192)
#define EST_CACHE_BYTES ((size_t)64 << 20)

static const struct { const wchar_t *label; int px; } PRESETS[] = {
    { L"1920 px  \x2013  Full HD (default)", 1920 },
    { L"3840 px  \x2013  4K", 3840 },
    { L"2560 px", 2560 },
    { L"1600 px", 1600 },
    { L"1280 px  \x2013  HD", 1280 },
    { L"1024 px", 1024 },
    { L"800 px  \x2013  web / email", 800 },
    { L"640 px", 640 },
    { L"320 px  \x2013  thumbnail", 320 },
    { L"Keep original size (compress only)", 0 },
};
#define NPRESETS ((int)(sizeof PRESETS / sizeof PRESETS[0]))

static const struct { const wchar_t *label; int fmt; int long_ext; int uses_quality; } FORMATS[] = {
    { L"Same as original (default)", IS_FMT_SAME, 0, 1 },
    { L"JPG", IS_FMT_JPEG, 0, 1 },
    { L"JPEG  (.jpeg extension)", IS_FMT_JPEG, 1, 1 },
    { L"PNG  \x2013  lossless", IS_FMT_PNG, 0, 0 },
    { L"WebP  \x2013  smallest files", IS_FMT_WEBP, 0, 1 },
    { L"WebP lossless", IS_FMT_WEBP_LOSSLESS, 0, 0 },
    { L"TIFF  \x2013  lossless", IS_FMT_TIFF, 0, 0 },
    { L"BMP  \x2013  uncompressed", IS_FMT_BMP, 0, 0 },
};
#define NFORMATS ((int)(sizeof FORMATS / sizeof FORMATS[0]))
#define Q_DEFAULT 90

/* ---------- data ---------- */

typedef struct {
    wchar_t *path;
    unsigned hash;
    int id;
    unsigned long long bytes;      /* size on disk now */
    int src_w, src_h;              /* 0 = not known yet */
    int res_gen;                   /* settings generation the result belongs to (0 = none) */
    int res_ok, res_actual, res_kept;
    long long res_bytes;
    int res_w, res_h;
    wchar_t res_err[80];
    int checked;                   /* included in Resize & compress / Save */
    wchar_t *out_path;             /* latest compressed result (temp folder), or NULL */
    int out_gen;                   /* settings generation it was made with */
    unsigned long long out_bytes;
    int out_w, out_h;
    wchar_t out_ext[16];           /* extension to use when saving */
} Item;

typedef struct {                   /* estimate or real result, posted to the UI thread */
    int gen, id, ok, kept, actual;
    long long bytes;
    int src_w, src_h, out_w, out_h;
    wchar_t err[80];
    wchar_t out_path[MAX_PATH + 64];   /* real results: where the file was written */
    wchar_t out_ext[16];
} ItemRes;

typedef struct { int valid, max_w, max_h, stretch, format, quality; } EstKey;

static HINSTANCE g_inst;
static HWND g_wnd, g_list, g_count, g_totals, g_preset, g_custom, g_lock, g_link, g_w, g_h,
            g_wl, g_hl, g_wpx, g_hpx, g_hint, g_format, g_quality, g_qlabel, g_ql1, g_ql2, g_ql3,
            g_qnote, g_progress, g_status, g_go, g_add, g_addfolder, g_remove, g_clear, g_restart,
            g_preview, g_save, g_save_folder, g_save_zip, g_saveinfo, g_savehint, g_gohint;
static HFONT g_font, g_bold;
static int g_dpi = 96;
static Item **g_items;
static int g_nitems, g_icap, g_next_id = 1;
static int g_gen = 1;
static EstKey g_key;
static int g_driver;               /* 0 = width was typed last, 1 = height */
static int g_linking;              /* set while we fill a field ourselves */
static int g_running;
static volatile LONG g_cancel, g_quit;
static HANDLE g_thread;
static wchar_t g_update_ver[64];
static wchar_t g_tmpdir[MAX_PATH];
static int g_auto_ok, g_auto_failed, g_auto_conflict;   /* 0 = keep both */
static int g_update_ready;

/* estimator */
typedef struct { int gen, drop_cache, n; IsOptions opt; int *ids; wchar_t **paths; } EstReq;
static CRITICAL_SECTION g_est_cs;
static EstReq *g_est_req;
static HANDLE g_est_ev, g_est_thread;
static volatile LONG g_est_pause;

/* test hooks */
static int g_auto;
static wchar_t g_autozip[1024], g_autoest[1024];

static int S(int v) { return MulDiv(v, g_dpi, 96); }
static void update_save_panel(void);

static void fmt_bytes(unsigned long long b, wchar_t *out, int n)
{
    if (b >= 1024ULL * 1024 * 1024) swprintf(out, n, L"%.2f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024 * 1024)      swprintf(out, n, L"%.1f MB", b / (1024.0 * 1024));
    else if (b >= 1024)             swprintf(out, n, L"%.0f KB", b / 1024.0);
    else                            swprintf(out, n, L"%llu B", b);
}

static const wchar_t *base_name(const wchar_t *p)
{
    const wchar_t *a = wcsrchr(p, L'\\'), *b = wcsrchr(p, L'/');
    if (b > a) a = b;
    return a ? a + 1 : p;
}

static unsigned path_hash(const wchar_t *p)
{
    unsigned h = 2166136261u;
    for (; *p; p++) { h ^= (unsigned)towlower(*p); h *= 16777619u; }
    return h;
}

static unsigned char *read_file(const wchar_t *path, size_t *n, const wchar_t **why)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    LARGE_INTEGER sz;
    unsigned char *buf;
    DWORD got = 0, total = 0;
    if (h == INVALID_HANDLE_VALUE) { *why = L"could not open file"; return NULL; }
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); *why = L"could not read file"; return NULL; }
    if (sz.QuadPart == 0) { CloseHandle(h); *why = L"file is empty"; return NULL; }
    if (sz.QuadPart > 1024LL * 1024 * 1024) { CloseHandle(h); *why = L"file is larger than 1 GB"; return NULL; }
    buf = (unsigned char *)malloc((size_t)sz.QuadPart);
    if (!buf) { CloseHandle(h); *why = L"not enough memory"; return NULL; }
    while (total < (DWORD)sz.QuadPart) {
        if (!ReadFile(h, buf + total, (DWORD)sz.QuadPart - total, &got, NULL) || got == 0) {
            free(buf); CloseHandle(h); *why = L"could not read file"; return NULL;
        }
        total += got;
    }
    CloseHandle(h);
    *n = (size_t)sz.QuadPart;
    return buf;
}

/* Reads just the header to learn an image's size (used for the aspect-ratio link). */
static int probe_item(Item *it)
{
    HANDLE h;
    unsigned char *buf;
    DWORD got = 0;
    int w = 0, hh = 0, ok;
    if (it->src_w > 0) return 1;
    h = CreateFileW(it->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    buf = (unsigned char *)malloc(512 * 1024);
    if (!buf) { CloseHandle(h); return 0; }
    ok = ReadFile(h, buf, 512 * 1024, &got, NULL) && is_probe(buf, got, &w, &hh);
    CloseHandle(h);
    free(buf);
    if (!ok) return 0;
    it->src_w = w; it->src_h = hh;
    return 1;
}

/* ---------- options ---------- */

static int cur_format(void)
{
    int i = (int)SendMessageW(g_format, CB_GETCURSEL, 0, 0);
    return (i < 0 || i >= NFORMATS) ? 0 : i;
}
static int is_checked(HWND b) { return SendMessageW(b, BM_GETCHECK, 0, 0) == BST_CHECKED; }
static int edit_int(HWND e) { wchar_t b[16]; GetWindowTextW(e, b, 16); return _wtoi(b); }

/* Fills o from the controls. Returns 0 if the size settings are incomplete. */
static int read_options(IsOptions *o)
{
    int w, h, i;
    memset(o, 0, sizeof *o);
    o->quality = (int)SendMessageW(g_quality, TBM_GETPOS, 0, 0);
    o->format = FORMATS[cur_format()].fmt;
    o->jpeg_long_ext = FORMATS[cur_format()].long_ext;
    if (!is_checked(g_custom)) {
        i = (int)SendMessageW(g_preset, CB_GETCURSEL, 0, 0);
        if (i < 0 || i >= NPRESETS) i = 0;
        o->max_w = o->max_h = PRESETS[i].px;
        return 1;
    }
    w = edit_int(g_w);
    h = edit_int(g_h);
    if (is_checked(g_lock)) {          /* the field typed last decides; each image keeps its shape */
        if (g_driver == 0 && w > 0) o->max_w = w;
        else if (g_driver == 1 && h > 0) o->max_h = h;
        else if (w > 0) o->max_w = w;
        else if (h > 0) o->max_h = h;
        else return 0;
        return 1;
    }
    if (w > 0 && h > 0) { o->max_w = w; o->max_h = h; o->stretch = 1; return 1; }
    if (w > 0) { o->max_w = w; return 1; }
    if (h > 0) { o->max_h = h; return 1; }
    return 0;
}

static void make_key(EstKey *k)
{
    IsOptions o;
    memset(k, 0, sizeof *k);
    k->valid = read_options(&o);
    if (!k->valid) return;
    k->max_w = o.max_w; k->max_h = o.max_h; k->stretch = o.stretch; k->format = o.format;
    k->quality = FORMATS[cur_format()].uses_quality ? o.quality : 0;
}

static void update_totals(void) { InvalidateRect(g_totals, NULL, TRUE); }

/* Called whenever an option changes; re-estimates only if the output would change. */
static void settings_changed(void)
{
    EstKey k;
    make_key(&k);
    if (memcmp(&k, &g_key, sizeof k) == 0) return;
    g_key = k;
    g_gen++;
    InvalidateRect(g_list, NULL, FALSE);
    update_totals();
    update_save_panel();
    SetTimer(g_wnd, TIMER_EST, 300, NULL);
}

/* ---------- estimator thread ---------- */

static void est_req_free(EstReq *r)
{
    int i;
    if (!r) return;
    for (i = 0; i < r->n; i++) free(r->paths[i]);
    free(r->paths);
    free(r->ids);
    free(r);
}

static int est_newer(void)
{
    int x;
    EnterCriticalSection(&g_est_cs);
    x = g_est_req != NULL;
    LeaveCriticalSection(&g_est_cs);
    return x;
}

typedef struct { int id; IsSample *s; size_t mem; unsigned long long used; } CacheEnt;

static DWORD WINAPI est_worker(LPVOID param)
{
    CacheEnt *c = NULL;
    int nc = 0, cap = 0, i, j;
    size_t total = 0;
    unsigned long long tick = 0;
    (void)param;

    if (!SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN))
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    while (!g_quit) {
        EstReq *r;
        WaitForSingleObject(g_est_ev, INFINITE);
        for (;;) {
            if (g_quit) break;
            EnterCriticalSection(&g_est_cs);
            r = g_est_req;
            g_est_req = NULL;
            LeaveCriticalSection(&g_est_cs);
            if (!r) break;
            if (r->drop_cache) {
                for (j = 0; j < nc; j++) is_sample_free(c[j].s);
                nc = 0; total = 0;
            }
            for (i = 0; i < r->n; i++) {
                ItemRes *res;
                CacheEnt *e = NULL;
                IsSample *s = NULL;
                char err[160];

                while (g_est_pause && !g_quit && !est_newer()) Sleep(150);
                if (g_quit || est_newer()) break;
                res = (ItemRes *)calloc(1, sizeof *res);
                if (!res) break;
                res->gen = r->gen;
                res->id = r->ids[i];
                for (j = 0; j < nc; j++) if (c[j].id == res->id) { e = &c[j]; break; }
                if (e && is_sample_fits(e->s, &r->opt)) s = e->s;
                if (!s) {
                    size_t n = 0;
                    const wchar_t *why = L"";
                    unsigned char *buf = read_file(r->paths[i], &n, &why);
                    if (!buf) { wcsncpy(res->err, why, 79); goto post; }
                    s = is_sample_create(buf, n, &r->opt, err, sizeof err);
                    free(buf);
                    if (!s) { MultiByteToWideChar(CP_UTF8, 0, err, -1, res->err, 80); res->err[79] = 0; goto post; }
                    if (e) {
                        total -= e->mem;
                        is_sample_free(e->s);
                    } else {
                        if (nc == cap) {
                            int ncap = cap ? cap * 2 : 128;
                            CacheEnt *nn = (CacheEnt *)realloc(c, (size_t)ncap * sizeof *nn);
                            if (!nn) { is_sample_free(s); wcscpy(res->err, L"not enough memory"); goto post; }
                            c = nn; cap = ncap;
                        }
                        e = &c[nc++];
                        e->id = res->id;
                    }
                    e->s = s;
                    e->mem = is_sample_mem(s);
                    total += e->mem;
                }
                e->used = ++tick;
                is_sample_info(s, &res->src_w, &res->src_h, &res->out_w, &res->out_h);
                if (is_sample_estimate(s, &r->opt, &res->bytes, &res->kept, err, sizeof err)) res->ok = 1;
                else { MultiByteToWideChar(CP_UTF8, 0, err, -1, res->err, 80); res->err[79] = 0; }
                while (total > EST_CACHE_BYTES && nc > 1) {      /* forget the least recently used */
                    int lru = -1;
                    for (j = 0; j < nc; j++)
                        if (c[j].id != res->id && (lru < 0 || c[j].used < c[lru].used)) lru = j;
                    if (lru < 0) break;
                    total -= c[lru].mem;
                    is_sample_free(c[lru].s);
                    c[lru] = c[--nc];
                }
            post:
                if (!PostMessageW(g_wnd, WM_EST_RESULT, 0, (LPARAM)res)) free(res);
            }
            est_req_free(r);
        }
    }
    for (j = 0; j < nc; j++) is_sample_free(c[j].s);
    free(c);
    return 0;
}

/* Sends the estimator the list of images that need a (new) estimate, visible rows first. */
static void est_schedule(int drop_cache)
{
    EstReq *r, *old;
    int k, n = 0, top;
    KillTimer(g_wnd, TIMER_EST);
    r = (EstReq *)calloc(1, sizeof *r);
    if (!r) return;
    r->gen = g_gen;
    r->drop_cache = drop_cache;
    if (g_key.valid && g_nitems > 0 && !g_running) {
        read_options(&r->opt);
        r->ids = (int *)malloc((size_t)g_nitems * sizeof *r->ids);
        r->paths = (wchar_t **)malloc((size_t)g_nitems * sizeof *r->paths);
        if (!r->ids || !r->paths) { est_req_free(r); return; }
        top = (int)SendMessageW(g_list, LVM_GETTOPINDEX, 0, 0);
        if (top < 0 || top >= g_nitems) top = 0;
        for (k = 0; k < g_nitems; k++) {
            Item *it = g_items[(top + k) % g_nitems];
            if (it->res_gen == g_gen) continue;
            r->ids[n] = it->id;
            r->paths[n] = _wcsdup(it->path);
            if (r->paths[n]) n++;
        }
    }
    r->n = n;
    EnterCriticalSection(&g_est_cs);
    old = g_est_req;
    g_est_req = r;
    LeaveCriticalSection(&g_est_cs);
    est_req_free(old);
    SetEvent(g_est_ev);
}

static int find_item(int id)
{
    static int hint;
    int i;
    if (hint < g_nitems && g_items[hint]->id == id) return hint;
    for (i = 0; i < g_nitems; i++) if (g_items[i]->id == id) { hint = i; return i; }
    return -1;
}

/* ---------- file list ---------- */

static int is_image_name(const wchar_t *p)
{
    static const wchar_t *exts[] = { L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".bmp", L".gif", L".webp" };
    const wchar_t *dot = wcsrchr(p, L'.');
    int i;
    if (!dot) return 0;
    for (i = 0; i < (int)(sizeof exts / sizeof exts[0]); i++)
        if (_wcsicmp(dot, exts[i]) == 0) return 1;
    return 0;
}

static void add_file(const wchar_t *path, unsigned long long size, int have_size)
{
    Item *it;
    unsigned h;
    int i;
    if (!is_image_name(path)) return;
    h = path_hash(path);
    for (i = 0; i < g_nitems; i++)
        if (g_items[i]->hash == h && _wcsicmp(g_items[i]->path, path) == 0) return;
    if (!have_size) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return;
        size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    }
    if (g_nitems == g_icap) {
        int nc = g_icap ? g_icap * 2 : 256;
        Item **ni = (Item **)realloc(g_items, (size_t)nc * sizeof *ni);
        if (!ni) return;
        g_items = ni; g_icap = nc;
    }
    it = (Item *)calloc(1, sizeof *it);
    if (!it) return;
    it->path = _wcsdup(path);
    if (!it->path) { free(it); return; }
    it->hash = h;
    it->id = g_next_id++;
    it->bytes = size;
    it->checked = 1;
    g_items[g_nitems++] = it;
}

static void add_dir(const wchar_t *dir, int depth)
{
    wchar_t pat[1024];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (depth > 12 || wcslen(dir) > 900) return;
    swprintf(pat, 1024, L"%ls\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t full[1024];
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (wcslen(dir) + wcslen(fd.cFileName) + 2 > 1024) continue;
        swprintf(full, 1024, L"%ls\\%ls", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) add_dir(full, depth + 1);
        } else {
            add_file(full, ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, 1);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void add_path(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (a & FILE_ATTRIBUTE_DIRECTORY) add_dir(p, 0);
    else add_file(p, 0, 0);
}

static void set_status(const wchar_t *s) { SetWindowTextW(g_status, s); }

static void apply_link(void);
static void update_hint(void);

static int count_checked(void)
{
    int i, n = 0;
    for (i = 0; i < g_nitems; i++) if (g_items[i]->checked) n++;
    return n;
}

static void update_save_panel(void);

static void update_go(void)
{
    wchar_t b[64];
    int n = count_checked();
    if (g_running) return;
    if (n == 0) wcscpy(b, L"Resize && compress");
    else swprintf(b, 64, n == 1 ? L"Resize && compress 1 image" : L"Resize && compress %d images", n);
    SetWindowTextW(g_go, b);
    EnableWindow(g_go, n > 0);
}

static void checks_changed(void)
{
    wchar_t b[64];
    int n = count_checked();
    HWND hdr = (HWND)SendMessageW(g_list, LVM_GETHEADER, 0, 0);
    if (hdr) InvalidateRect(hdr, NULL, FALSE);
    if (n == g_nitems) swprintf(b, 64, g_nitems == 1 ? L"%d image" : L"%d images", g_nitems);
    else swprintf(b, 64, L"%d of %d checked", n, g_nitems);
    SetWindowTextW(g_count, b);
    update_go();
    update_totals();
    update_save_panel();
}

static void list_changed(void)
{
    SendMessageW(g_list, LVM_SETITEMCOUNT, (WPARAM)g_nitems, LVSICF_NOSCROLL);
    InvalidateRect(g_list, NULL, TRUE);
    EnableWindow(g_preview, g_nitems > 0);
    checks_changed();
    apply_link();
    update_hint();
}

static void item_free(Item *it)
{
    if (it->out_path) { DeleteFileW(it->out_path); free(it->out_path); }
    free(it->path);
    free(it);
}

static void begin_bulk(void) { SetCursor(LoadCursorW(NULL, IDC_WAIT)); }
static void end_bulk(int before)
{
    list_changed();
    SetCursor(LoadCursorW(NULL, IDC_ARROW));
    if (g_nitems != before) {
        SetTimer(g_wnd, TIMER_EST, 150, NULL);
        if (!g_running) set_status(L"Ready.");
    }
}

static void clear_files(void)
{
    int i;
    for (i = 0; i < g_nitems; i++) item_free(g_items[i]);
    g_nitems = 0;
    SendMessageW(g_list, LVM_SETITEMSTATE, (WPARAM)-1, 0);
    list_changed();
    est_schedule(1);
    set_status(L"Add some images to get started.");
}

static void remove_selected(void)
{
    int i, k = 0, idx = -1;
    char *del;
    if (g_nitems == 0) return;
    del = (char *)calloc((size_t)g_nitems, 1);
    if (!del) return;
    while ((idx = (int)SendMessageW(g_list, LVM_GETNEXTITEM, (WPARAM)idx, LVNI_SELECTED)) >= 0) del[idx] = 1;
    for (i = 0; i < g_nitems; i++) {
        if (del[i]) item_free(g_items[i]);
        else g_items[k++] = g_items[i];
    }
    free(del);
    if (k == g_nitems) return;
    g_nitems = k;
    {
        LVITEMW lv;
        memset(&lv, 0, sizeof lv);
        lv.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(g_list, LVM_SETITEMSTATE, (WPARAM)-1, (LPARAM)&lv);
    }
    list_changed();
}

static void browse_files(void)
{
    OPENFILENAMEW ofn;
    DWORD cap = 1 << 20;
    wchar_t *buf = (wchar_t *)calloc(cap, sizeof(wchar_t));
    if (!buf) return;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"Images (JPG, PNG, WebP, BMP, GIF)\0*.jpg;*.jpeg;*.jpe;*.jfif;*.png;*.webp;*.bmp;*.gif\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = cap;
    ofn.lpstrTitle = L"Choose images";
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) {
        wchar_t *p = buf + wcslen(buf) + 1;
        int before = g_nitems;
        begin_bulk();
        if (!*p) {
            add_file(buf, 0, 0);
        } else {
            for (; *p; p += wcslen(p) + 1) {
                wchar_t full[1024];
                size_t dl = wcslen(buf);
                if (dl + wcslen(p) + 2 > 1024) continue;
                swprintf(full, 1024, (dl && buf[dl - 1] == L'\\') ? L"%ls%ls" : L"%ls\\%ls", buf, p);
                add_file(full, 0, 0);
            }
        }
        end_bulk(before);
    } else if (CommDlgExtendedError() == FNERR_BUFFERTOOSMALL) {
        MessageBoxW(g_wnd, L"That is too many files to pick at once.\nUse \"Add folder\" or drag the folder onto the window instead.",
                    APP_NAME, MB_ICONINFORMATION);
    }
    free(buf);
}

static void browse_folder(void)
{
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;
    wchar_t path[MAX_PATH];
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = g_wnd;
    bi.lpszTitle = L"Choose a folder of images (subfolders are included)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;
    pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    if (SHGetPathFromIDListW(pidl, path)) {
        int before = g_nitems;
        begin_bulk();
        add_dir(path, 0);
        end_bulk(before);
        if (g_nitems == before)
            MessageBoxW(g_wnd, L"No new images were found in that folder.", APP_NAME, MB_ICONINFORMATION);
    }
    CoTaskMemFree(pidl);
}

/* ---------- aspect-ratio link ---------- */

static Item *ref_item(void)
{
    int idx;
    if (g_nitems == 0) return NULL;
    idx = (int)SendMessageW(g_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (idx < 0 || idx >= g_nitems) idx = 0;
    return g_items[idx];
}

/* With the ratio locked, fill the other field from the one typed last, using the
 * shape of the selected image (or the first one). */
static void apply_link(void)
{
    Item *r;
    HWND from, to;
    int v;
    wchar_t b[16] = L"";
    if (!is_checked(g_custom) || !is_checked(g_lock)) return;
    from = g_driver == 0 ? g_w : g_h;
    to = g_driver == 0 ? g_h : g_w;
    v = edit_int(from);
    r = ref_item();
    if (v > 0 && r && probe_item(r)) {
        double o = g_driver == 0 ? (double)v * r->src_h / r->src_w : (double)v * r->src_w / r->src_h;
        long lo = (long)(o + 0.5);
        if (lo < 1) lo = 1;
        if (lo > 99999) lo = 99999;
        swprintf(b, 16, L"%ld", lo);
    }
    g_linking = 1;
    SetWindowTextW(to, b);
    g_linking = 0;
}

static void update_hint(void)
{
    wchar_t b[300];
    int w = edit_int(g_w), h = edit_int(g_h);
    Item *r = ref_item();
    if (!is_checked(g_custom)) {
        wcscpy(b, L"The longest side of each image is reduced to this size. Images that are already smaller keep their size.");
    } else if (is_checked(g_lock)) {
        int v = g_driver == 0 ? w : h;
        if (v <= 0) v = g_driver == 0 ? h : w;
        if (v <= 0)
            wcscpy(b, L"Type a width or a height. The other fills in to keep the shape of the selected image.");
        else if (r)
            swprintf(b, 300, L"Each image becomes %d px %ls and keeps its own shape. The %ls shown is for %.28ls.",
                     g_driver == 0 && w > 0 ? w : h, g_driver == 0 && w > 0 ? L"wide" : L"tall",
                     g_driver == 0 && w > 0 ? L"height" : L"width", base_name(r->path));
        else
            swprintf(b, 300, L"Each image becomes %d px %ls and keeps its own shape.",
                     g_driver == 0 && w > 0 ? w : h, g_driver == 0 && w > 0 ? L"wide" : L"tall");
    } else if (w > 0 && h > 0) {
        swprintf(b, 300, L"Every image is resized to exactly %d \x00D7 %d px. Images with a different shape will look stretched.", w, h);
    } else if (w > 0 || h > 0) {
        swprintf(b, 300, L"Each image becomes %d px %ls and keeps its own shape.", w > 0 ? w : h, w > 0 ? L"wide" : L"tall");
    } else {
        wcscpy(b, L"Enter a width and/or a height in pixels.");
    }
    SetWindowTextW(g_hint, b);
}

/* ---------- job thread ---------- */

typedef struct {
    HWND hwnd;
    int n, gen, kind;              /* kind: 0 = compress, 1 = save to folder, 2 = save to zip */
    int *ids;
    wchar_t **files;               /* compress: originals; save: results to copy */
    wchar_t **names;               /* save: file names to use */
    IsOptions opt;
    wchar_t dest[1024];            /* save: folder or zip file */
    wchar_t tmpdir[MAX_PATH];
    int conflict;                  /* folder save, name already taken: CONFLICT_* */
} Job;

#define CONFLICT_KEEP_BOTH 0       /* save as "name (2).ext" */
#define CONFLICT_REPLACE   1       /* overwrite the existing file */
#define CONFLICT_SKIP      2       /* leave the existing file, don't save this one */

typedef struct {
    int kind, ok, failed, cancelled, fatal, replaced, skipped;
    unsigned long long in_bytes, out_bytes;
    wchar_t fatalmsg[400];
    wchar_t errors[2400];
    wchar_t dest[1024];
} JobResult;

static void job_free(Job *job)
{
    int i;
    if (!job) return;
    for (i = 0; i < job->n; i++) {
        if (job->files) free(job->files[i]);
        if (job->names) free(job->names[i]);
    }
    free(job->files);
    free(job->names);
    free(job->ids);
    free(job);
}

static void post_status(HWND hwnd, int done, const wchar_t *fmt, ...)
{
    wchar_t *s = (wchar_t *)malloc(600 * sizeof(wchar_t));
    va_list ap;
    if (!s) return;
    va_start(ap, fmt);
    vswprintf(s, 600, fmt, ap);
    va_end(ap);
    if (!PostMessageW(hwnd, WM_JOB_PROGRESS, (WPARAM)done, (LPARAM)s)) free(s);
}

static void post_item(HWND hwnd, int gen, int id, const IsResult *r, const wchar_t *err,
                      const wchar_t *out_path, const wchar_t *out_ext)
{
    ItemRes *x = (ItemRes *)calloc(1, sizeof *x);
    if (!x) return;
    x->gen = gen; x->id = id; x->actual = 1;
    if (r) {
        x->ok = 1; x->bytes = (long long)r->size; x->kept = r->kept_original;
        x->src_w = r->src_w; x->src_h = r->src_h; x->out_w = r->w; x->out_h = r->h;
        wcsncpy(x->out_path, out_path, MAX_PATH + 63);
        wcsncpy(x->out_ext, out_ext, 15);
    } else {
        wcsncpy(x->err, err, 79);
    }
    if (!PostMessageW(hwnd, WM_JOB_ITEM, 0, (LPARAM)x)) free(x);
}

static void note_error(JobResult *r, const wchar_t *name, const wchar_t *why)
{
    r->failed++;
    if (r->failed <= 8) {
        size_t l = wcslen(r->errors);
        swprintf(r->errors + l, 2400 - l, L"   \x2022 %.90ls  \x2013  %.120ls\n", name, why);
    }
}

static int ext_fits(int fmt, const wchar_t *ext)
{
    switch (fmt) {
    case IS_FMT_JPEG: return !_wcsicmp(ext, L"jpg") || !_wcsicmp(ext, L"jpeg") || !_wcsicmp(ext, L"jpe") || !_wcsicmp(ext, L"jfif");
    case IS_FMT_PNG:  return !_wcsicmp(ext, L"png");
    case IS_FMT_WEBP: case IS_FMT_WEBP_LOSSLESS: return !_wcsicmp(ext, L"webp");
    case IS_FMT_BMP:  return !_wcsicmp(ext, L"bmp");
    case IS_FMT_TIFF: return !_wcsicmp(ext, L"tif") || !_wcsicmp(ext, L"tiff");
    }
    return 0;
}

static int write_whole(const wchar_t *path, const void *d, size_t n)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    DWORD w = 0;
    int ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(h, d, (DWORD)n, &w, NULL) && w == (DWORD)n;
    if (!CloseHandle(h)) ok = 0;
    if (!ok) DeleteFileW(path);
    return ok;
}

/* Step 1: every run starts again from the original files; results go to the temp folder. */
static DWORD WINAPI compress_worker(LPVOID param)
{
    Job *job = (Job *)param;
    JobResult *res = (JobResult *)calloc(1, sizeof *res);
    int i;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    if (!res) goto done;
    for (i = 0; i < job->n && !g_cancel; i++) {
        const wchar_t *name = base_name(job->files[i]);
        const wchar_t *why = L"";
        unsigned char *in;
        size_t n = 0;
        IsResult r;
        char err[200];
        wchar_t wext[16], outp[MAX_PATH + 64];
        const wchar_t *dot;

        post_status(job->hwnd, i, L"Compressing %d of %d  \x2013  %.200ls", i + 1, job->n, name);
        in = read_file(job->files[i], &n, &why);
        if (!in) { note_error(res, name, why); post_item(job->hwnd, job->gen, job->ids[i], NULL, why, NULL, NULL); continue; }
        if (!is_process(in, n, &job->opt, &r, err, sizeof err)) {
            wchar_t werr[200];
            MultiByteToWideChar(CP_UTF8, 0, err, -1, werr, 200);
            note_error(res, name, werr);
            post_item(job->hwnd, job->gen, job->ids[i], NULL, werr, NULL, NULL);
            free(in);
            continue;
        }
        MultiByteToWideChar(CP_UTF8, 0, r.ext, -1, wext, 16);
        dot = wcsrchr(name, L'.');
        if (job->opt.format == IS_FMT_SAME && dot && dot != name && ext_fits(r.out_fmt, dot + 1)) {
            wcsncpy(wext, dot, 15);                 /* keep the original spelling (.jpeg, .JPG, ...) */
            wext[15] = 0;
        }
        swprintf(outp, MAX_PATH + 64, L"%ls\\%d%ls", job->tmpdir, job->ids[i], wext);
        if (!write_whole(outp, r.data, r.size)) {
            note_error(res, name, L"could not write a temporary file (is the disk full?)");
            post_item(job->hwnd, job->gen, job->ids[i], NULL, L"could not write a temporary file", NULL, NULL);
        } else {
            res->ok++;
            res->in_bytes += n;
            res->out_bytes += r.size;
            post_item(job->hwnd, job->gen, job->ids[i], &r, NULL, outp, wext);
        }
        is_free(&r);
        free(in);
    }
    res->cancelled = g_cancel ? 1 : 0;
done:
    PostMessageW(job->hwnd, WM_JOB_DONE, 0, (LPARAM)res);
    job_free(job);
    return 0;
}

/* Step 2: copy the results into a folder (never overwriting anything) or pack them into a zip. */
static DWORD WINAPI save_worker(LPVOID param)
{
    Job *job = (Job *)param;
    JobResult *res = (JobResult *)calloc(1, sizeof *res);
    int i, zerr = ZIP_OK;
    FILE *zf = NULL;
    Zip z;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    memset(&z, 0, sizeof z);
    if (!res) goto done;
    res->kind = job->kind;
    wcsncpy(res->dest, job->dest, 1023);

    if (job->kind == 2) {
        zf = _wfopen(job->dest, L"wb");
        if (!zf) {
            res->fatal = 1;
            swprintf(res->fatalmsg, 400, L"Could not create the zip file. Check that the folder exists and is writable.");
            goto done;
        }
        setvbuf(zf, NULL, _IOFBF, 1 << 16);
        zip_begin(&z, zf);
    }
    for (i = 0; i < job->n && !g_cancel; i++) {
        post_status(job->hwnd, i, L"Saving %d of %d  \x2013  %.200ls", i + 1, job->n, job->names[i]);
        if (job->kind == 2) {
            size_t n = 0;
            const wchar_t *why = L"";
            char utf8[1100];
            unsigned char *buf = read_file(job->files[i], &n, &why);
            if (!buf) { note_error(res, job->names[i], why); continue; }
            WideCharToMultiByte(CP_UTF8, 0, job->names[i], -1, utf8, sizeof utf8, NULL, NULL);
            zerr = zip_add(&z, utf8, buf, n);
            free(buf);
            if (zerr != ZIP_OK) break;
            res->ok++;
            res->out_bytes += n;
        } else {
            wchar_t target[1400], stem[400];
            const wchar_t *ext = wcsrchr(job->names[i], L'.');
            int k;
            WIN32_FILE_ATTRIBUTE_DATA fa;
            wcsncpy(stem, job->names[i], 399);
            stem[399] = 0;
            if (ext) stem[ext - job->names[i]] = 0; else ext = L"";
            swprintf(target, 1400, L"%ls\\%ls", job->dest, job->names[i]);
            if (job->conflict != CONFLICT_KEEP_BOTH && GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES) {
                if (job->conflict == CONFLICT_SKIP) { res->skipped++; continue; }
                /* replace: write next to it first, then swap, so a failure never leaves a half-written file */
                {
                    wchar_t tmp[1450];
                    swprintf(tmp, 1450, L"%ls.imageshrinker-tmp", target);
                    if (!CopyFileW(job->files[i], tmp, FALSE) ||
                        !MoveFileExW(tmp, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                        DWORD e = GetLastError();
                        DeleteFileW(tmp);
                        note_error(res, job->names[i], e == ERROR_ACCESS_DENIED ? L"the existing file is read-only or in use"
                                                                               : L"could not replace the existing file");
                        continue;
                    }
                    res->replaced++;
                }
            } else {
                for (k = 1; k < 10000; k++) {
                    if (k == 1) swprintf(target, 1400, L"%ls\\%ls", job->dest, job->names[i]);
                    else swprintf(target, 1400, L"%ls\\%ls (%d)%ls", job->dest, stem, k, ext);
                    if (CopyFileW(job->files[i], target, TRUE)) break;
                    if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) { k = 10000; break; }
                }
                if (k >= 10000) { note_error(res, job->names[i], L"could not write the file"); continue; }
            }
            res->ok++;
            if (GetFileAttributesExW(target, GetFileExInfoStandard, &fa))
                res->out_bytes += ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        }
    }
    if (job->kind == 2) {
        if (zerr == ZIP_ELIMIT && res->ok > 0 && !g_cancel) {
            zerr = zip_finish(&z);
            if (zerr == ZIP_OK)
                swprintf(res->fatalmsg, 400, L"The zip reached the 4 GB / 65,535-file limit, so it contains the first %d images only.", res->ok);
        } else if (zerr == ZIP_OK && !g_cancel && res->ok > 0) {
            zerr = zip_finish(&z);
        }
        if (fclose(zf) != 0 && zerr == ZIP_OK) zerr = ZIP_EWRITE;
        zf = NULL;
        if (zerr != ZIP_OK) {
            res->fatal = 1;
            swprintf(res->fatalmsg, 400, zerr == ZIP_EWRITE ? L"Could not write the zip file. The disk may be full."
                                       : zerr == ZIP_ELIMIT ? L"The zip would be larger than 4 GB. Save to a folder instead."
                                       : L"Not enough memory to finish the zip file.");
        }
        if (res->fatal || g_cancel || res->ok == 0) DeleteFileW(job->dest);
    }
    res->cancelled = g_cancel ? 1 : 0;
done:
    zip_free(&z);
    PostMessageW(job->hwnd, WM_JOB_DONE, 0, (LPARAM)res);
    job_free(job);
    return 0;
}

/* ---------- temp folder ---------- */

static void remove_dir_files(const wchar_t *dir)
{
    wchar_t pat[MAX_PATH + 8], f[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf(pat, MAX_PATH + 8, L"%ls\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            swprintf(f, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName);
            DeleteFileW(f);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static int ensure_tmpdir(void)
{
    wchar_t t[MAX_PATH];
    if (g_tmpdir[0]) return 1;
    if (!GetTempPathW(MAX_PATH - 40, t)) return 0;
    swprintf(g_tmpdir, MAX_PATH, L"%ls" TMP_PREFIX L"%lu", t, GetCurrentProcessId());
    if (!CreateDirectoryW(g_tmpdir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) { g_tmpdir[0] = 0; return 0; }
    return 1;
}

/* results left behind by a copy of the app that crashed or was killed */
static void cleanup_old_tmp(void)
{
    wchar_t t[MAX_PATH], pat[MAX_PATH + 40], d[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!GetTempPathW(MAX_PATH, t)) return;
    swprintf(pat, MAX_PATH + 40, L"%ls" TMP_PREFIX L"*", t);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        DWORD pid = (DWORD)wcstoul(fd.cFileName + wcslen(TMP_PREFIX), NULL, 10);
        HANDLE ph;
        int alive = 0;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || pid == GetCurrentProcessId()) continue;
        ph = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (ph) { alive = WaitForSingleObject(ph, 0) == WAIT_TIMEOUT; CloseHandle(ph); }
        if (alive) continue;
        swprintf(d, MAX_PATH * 2, L"%ls%ls", t, fd.cFileName);
        remove_dir_files(d);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* ---------- UI state ---------- */

static void update_qlabel(void)
{
    wchar_t b[32];
    BOOL used = FORMATS[cur_format()].uses_quality;
    if (used) swprintf(b, 32, L"%d", (int)SendMessageW(g_quality, TBM_GETPOS, 0, 0));
    else      wcscpy(b, L"lossless");
    SetWindowTextW(g_qlabel, b);
    EnableWindow(g_quality, used && !g_running);
    EnableWindow(g_ql1, used); EnableWindow(g_ql2, used); EnableWindow(g_ql3, used);
}

static void sync_custom(void)
{
    BOOL custom = is_checked(g_custom), on = !g_running;
    EnableWindow(g_preset, on && !custom);
    EnableWindow(g_lock, on && custom);
    EnableWindow(g_link, on && custom);
    EnableWindow(g_w, on && custom);
    EnableWindow(g_h, on && custom);
    EnableWindow(g_wl, custom); EnableWindow(g_hl, custom);
    EnableWindow(g_wpx, custom); EnableWindow(g_hpx, custom);
    InvalidateRect(g_link, NULL, TRUE);
}

static void set_running(int run)
{
    BOOL on = !run;
    g_running = run;
    EnableWindow(g_add, on); EnableWindow(g_addfolder, on);
    EnableWindow(g_remove, on); EnableWindow(g_clear, on); EnableWindow(g_custom, on);
    EnableWindow(g_format, on);
    EnableWindow(g_restart, on);
    sync_custom();
    update_qlabel();
    if (run) { SetWindowTextW(g_go, L"Cancel"); EnableWindow(g_go, TRUE); }
    else update_go();
    update_save_panel();
    DragAcceptFiles(g_wnd, on);
    DragAcceptFiles(g_list, on);
}

static void update_save_panel(void)
{
    int i, n = 0, stale = 0, missing = 0, show;
    unsigned long long tot = 0;
    wchar_t b[300], sz[32];
    for (i = 0; i < g_nitems; i++) {
        Item *it = g_items[i];
        if (!it->checked) continue;
        if (it->out_path) { n++; tot += it->out_bytes; if (it->out_gen != g_gen) stale++; }
        else if (!(it->res_gen == g_gen && !it->res_ok)) missing++;   /* unreadable files don't count */
    }
    show = n > 0;
    ShowWindow(g_save_folder, show ? SW_SHOW : SW_HIDE);
    ShowWindow(g_save_zip, show ? SW_SHOW : SW_HIDE);
    ShowWindow(g_save, show ? SW_SHOW : SW_HIDE);
    ShowWindow(g_saveinfo, show ? SW_SHOW : SW_HIDE);
    ShowWindow(g_savehint, show ? SW_HIDE : SW_SHOW);
    EnableWindow(g_save, show && !g_running);
    EnableWindow(g_save_folder, !g_running);
    EnableWindow(g_save_zip, !g_running);
    if (!show) return;
    fmt_bytes(tot, sz, 32);
    if (stale)
        swprintf(b, 300, L"%d ready (%ls). Settings changed \x2013 compress again to apply them.", n, sz);
    else if (missing)
        swprintf(b, 300, L"%d ready (%ls). %d checked not compressed yet.", n, sz, missing);
    else
        swprintf(b, 300, n == 1 ? L"%d image ready  \x00B7  %ls" : L"%d images ready  \x00B7  %ls", n, sz);
    SetWindowTextW(g_saveinfo, b);
}

static int get_options(IsOptions *o)
{
    if (read_options(o)) return 1;
    MessageBoxW(g_wnd, L"Enter a custom width and/or height in pixels,\nor untick \"Custom size\".", APP_NAME, MB_ICONINFORMATION);
    SetFocus(g_w);
    return 0;
}

static int run_job(Job *job, LPTHREAD_START_ROUTINE fn, const wchar_t *what)
{
    DWORD tid;
    g_cancel = 0;
    InterlockedExchange(&g_est_pause, 1);
    SendMessageW(g_progress, PBM_SETRANGE32, 0, job->n);
    SendMessageW(g_progress, PBM_SETPOS, 0, 0);
    set_running(1);
    set_status(what);
    g_thread = CreateThread(NULL, 0, fn, job, 0, &tid);
    if (!g_thread) {
        job_free(job);
        set_running(0);
        InterlockedExchange(&g_est_pause, 0);
        set_status(L"Could not start.");
        return 0;
    }
    return 1;
}

static void start_save(void);

static void start_compress(void)
{
    Job *job;
    IsOptions opt;
    int i, n = 0;

    if (g_running || count_checked() == 0) return;
    if (!get_options(&opt)) return;
    if (!ensure_tmpdir()) {
        MessageBoxW(g_wnd, L"Couldn't create a temporary folder for the results.", APP_NAME, MB_ICONERROR);
        return;
    }
    job = (Job *)calloc(1, sizeof *job);
    if (!job) return;
    job->files = (wchar_t **)calloc((size_t)g_nitems, sizeof *job->files);
    job->ids = (int *)calloc((size_t)g_nitems, sizeof *job->ids);
    if (!job->files || !job->ids) { job_free(job); return; }
    for (i = 0; i < g_nitems; i++) {
        Item *it = g_items[i];
        if (!it->checked) continue;
        if (it->out_path && it->out_gen == g_gen) continue;   /* already done with these settings */
        job->files[n] = _wcsdup(it->path);
        job->ids[n] = it->id;
        n++;
    }
    job->n = n;
    if (n == 0) {
        job_free(job);
        set_status(L"The checked images are already compressed with these settings.");
        update_save_panel();
        if (g_auto) start_save();
        return;
    }
    job->kind = 0;
    job->gen = g_gen;
    job->opt = opt;
    job->hwnd = g_wnd;
    wcsncpy(job->tmpdir, g_tmpdir, MAX_PATH - 1);
    run_job(job, compress_worker, L"Starting\x2026");
}

/* modern folder picker, starting in the folder of the first image */
static int pick_folder(wchar_t *out, int outn, const wchar_t *start)
{
    IFileOpenDialog *d = NULL;
    int ok = 0;
    if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d))) {
        FILEOPENDIALOGOPTIONS o = 0;
        IShellItem *it = NULL;
        IFileOpenDialog_GetOptions(d, &o);
        IFileOpenDialog_SetOptions(d, o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        IFileOpenDialog_SetTitle(d, L"Choose the folder to save the images in");
        IFileOpenDialog_SetOkButtonLabel(d, L"Save here");
        if (start && start[0] && SUCCEEDED(SHCreateItemFromParsingName(start, NULL, &IID_IShellItem, (void **)&it))) {
            IFileOpenDialog_SetFolder(d, it);
            IShellItem_Release(it);
            it = NULL;
        }
        if (SUCCEEDED(IFileOpenDialog_Show(d, g_wnd)) && SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
            PWSTR p = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p)) && p) {
                wcsncpy(out, p, outn - 1);
                out[outn - 1] = 0;
                CoTaskMemFree(p);
                ok = 1;
            }
            IShellItem_Release(it);
        }
        IFileOpenDialog_Release(d);
        return ok;
    } else {
        BROWSEINFOW bi;
        LPITEMIDLIST pidl;
        wchar_t path[MAX_PATH];
        memset(&bi, 0, sizeof bi);
        bi.hwndOwner = g_wnd;
        bi.lpszTitle = L"Choose the folder to save the images in";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        pidl = SHBrowseForFolderW(&bi);
        if (!pidl) return 0;
        if (SHGetPathFromIDListW(pidl, path)) { wcsncpy(out, path, outn - 1); out[outn - 1] = 0; ok = 1; }
        CoTaskMemFree(pidl);
        return ok;
    }
}

static int *g_repl_ids, g_nrepl;      /* images whose original file a save may replace */

typedef HRESULT (WINAPI *TaskDialogIndirectFn)(const TASKDIALOGCONFIG *, int *, int *, BOOL *);

/* Asks what to do when files with the same names are already in the folder.
 * Returns CONFLICT_* or -1 to cancel. */
static int ask_conflict(int nconf, int norig, const wchar_t *dest)
{
    wchar_t head[200], body[600];
    const wchar_t *folder = wcsrchr(dest, L'\\');
    HMODULE cc = GetModuleHandleW(L"comctl32.dll");
    TaskDialogIndirectFn tdi = cc ? (TaskDialogIndirectFn)(void *)GetProcAddress(cc, "TaskDialogIndirect") : NULL;
    folder = folder && folder[1] ? folder + 1 : dest;
    if (nconf == 1) swprintf(head, 200, L"A file with the same name is already in \x201C%.80ls\x201D", folder);
    else swprintf(head, 200, L"%d files with the same names are already in \x201C%.80ls\x201D", nconf, folder);
    if (norig == nconf)
        swprintf(body, 600, norig == 1 ? L"It\x2019s the original image you added. Replacing it can\x2019t be undone."
                                       : L"They\x2019re the original images you added. Replacing them can\x2019t be undone.");
    else if (norig > 0)
        swprintf(body, 600, L"%d of them %ls the original image%ls you added. Replacing can\x2019t be undone.",
                 norig, norig == 1 ? L"is" : L"are", norig == 1 ? L"" : L"s");
    else
        wcscpy(body, L"Replacing overwrites the files that are there now.");

    if (tdi) {
        TASKDIALOGCONFIG c;
        TASKDIALOG_BUTTON b[3];
        int pressed = 0;
        b[0].nButtonID = 100 + CONFLICT_REPLACE;
        b[0].pszButtonText = nconf == 1 ? L"Replace the file\nOverwrite it with the new version."
                                        : L"Replace the files\nOverwrite them with the new versions.";
        b[1].nButtonID = 100 + CONFLICT_KEEP_BOTH;
        b[1].pszButtonText = L"Keep both\nSave the new versions with \u201C(2)\u201D added to the name.";
        b[2].nButtonID = 100 + CONFLICT_SKIP;
        b[2].pszButtonText = nconf == 1 ? L"Skip this file\nOnly save the images that aren\x2019t already there."
                                        : L"Skip these files\nOnly save the images that aren\x2019t already there.";
        memset(&c, 0, sizeof c);
        c.cbSize = sizeof c;
        c.hwndParent = g_wnd;
        c.hInstance = g_inst;
        c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        c.pszWindowTitle = APP_NAME;
        c.pszMainIcon = norig > 0 ? TD_WARNING_ICON : NULL;
        c.pszMainInstruction = head;
        c.pszContent = body;
        c.cButtons = 3;
        c.pButtons = b;
        c.nDefaultButton = 100 + CONFLICT_KEEP_BOTH;   /* Enter never overwrites by accident */
        if (SUCCEEDED(tdi(&c, &pressed, NULL, NULL)) && pressed >= 100 && pressed <= 102) return pressed - 100;
        return -1;
    } else {
        wchar_t m[1000];
        int r;
        swprintf(m, 1000, L"%ls.\n%ls\n\nReplace %ls?\n\nYes = replace    No = keep both (adds \u201C(2)\u201D)    Cancel = don\x2019t save",
                 head, body, nconf == 1 ? L"it" : L"them");
        r = MessageBoxW(g_wnd, m, APP_NAME, MB_YESNOCANCEL | MB_DEFBUTTON2 | (norig ? MB_ICONWARNING : MB_ICONQUESTION));
        return r == IDYES ? CONFLICT_REPLACE : r == IDNO ? CONFLICT_KEEP_BOTH : -1;
    }
}

static int same_path(const wchar_t *a, const wchar_t *b)
{
    wchar_t fa[1400], fb[1400];
    if (!GetFullPathNameW(a, 1400, fa, NULL) || !GetFullPathNameW(b, 1400, fb, NULL)) return _wcsicmp(a, b) == 0;
    return _wcsicmp(fa, fb) == 0;
}

static void start_save(void)
{
    Job *job;
    int i, n = 0, zip, k, j, clash;
    wchar_t dest[1024] = L"", start[1024] = L"";

    if (g_running) return;
    for (i = 0; i < g_nitems; i++) if (g_items[i]->checked && g_items[i]->out_path) n++;
    if (n == 0) {
        MessageBoxW(g_wnd, L"Compress the checked images first, then save them.", APP_NAME, MB_ICONINFORMATION);
        return;
    }
    zip = g_auto ? (wcslen(g_autozip) > 4 && _wcsicmp(g_autozip + wcslen(g_autozip) - 4, L".zip") == 0)
                 : is_checked(g_save_zip);
    if (g_auto) {
        wcsncpy(dest, g_autozip, 1023);
    } else {
        for (i = 0; i < g_nitems; i++) if (g_items[i]->checked && g_items[i]->out_path) break;
        wcsncpy(start, g_items[i]->path, 1023);
        { wchar_t *sl = wcsrchr(start, L'\\'); if (sl) *sl = 0; }
        if (zip) {
            OPENFILENAMEW ofn;
            wcscpy(dest, L"resized-images.zip");
            memset(&ofn, 0, sizeof ofn);
            ofn.lStructSize = sizeof ofn;
            ofn.hwndOwner = g_wnd;
            ofn.lpstrFilter = L"Zip archive (*.zip)\0*.zip\0";
            ofn.lpstrFile = dest;
            ofn.nMaxFile = 1024;
            ofn.lpstrInitialDir = start;
            ofn.lpstrDefExt = L"zip";
            ofn.lpstrTitle = L"Save the images as a zip";
            ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
            if (!GetSaveFileNameW(&ofn)) return;
        } else if (!pick_folder(dest, 1024, start)) {
            return;
        }
    }
    if (!zip) CreateDirectoryW(dest, NULL);

    job = (Job *)calloc(1, sizeof *job);
    if (!job) return;
    job->files = (wchar_t **)calloc((size_t)n, sizeof *job->files);
    job->names = (wchar_t **)calloc((size_t)n, sizeof *job->names);
    job->ids = (int *)calloc((size_t)n, sizeof *job->ids);
    if (!job->files || !job->names || !job->ids) { job_free(job); return; }
    for (i = 0, k = 0; i < g_nitems && k < n; i++) {
        Item *it = g_items[i];
        wchar_t stem[300], name[340], *dot;
        int m = 1;
        if (!it->checked || !it->out_path) continue;
        wcsncpy(stem, base_name(it->path), 259);
        stem[259] = 0;
        dot = wcsrchr(stem, L'.');
        if (dot && dot != stem) *dot = 0;
        for (;;) {                                   /* unique within this batch */
            if (m == 1) swprintf(name, 340, L"%ls%ls", stem, it->out_ext);
            else        swprintf(name, 340, L"%ls (%d)%ls", stem, m, it->out_ext);
            clash = 0;
            for (j = 0; j < k; j++) if (_wcsicmp(job->names[j], name) == 0) { clash = 1; break; }
            if (!clash) break;
            m++;
        }
        job->files[k] = _wcsdup(it->out_path);
        job->names[k] = _wcsdup(name);
        job->ids[k] = it->id;
        k++;
    }
    job->n = k;
    job->kind = zip ? 2 : 1;
    job->hwnd = g_wnd;
    wcsncpy(job->dest, dest, 1023);

    free(g_repl_ids);
    g_repl_ids = NULL;
    g_nrepl = 0;
    if (!zip) {                                  /* names already taken in that folder? */
        int nconf = 0, norig = 0;
        g_repl_ids = (int *)malloc((size_t)(k ? k : 1) * sizeof *g_repl_ids);
        for (i = 0; i < k; i++) {
            wchar_t target[1400];
            int fi;
            swprintf(target, 1400, L"%ls\\%ls", dest, job->names[i]);
            if (GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES) continue;
            nconf++;
            fi = find_item(job->ids[i]);
            if (fi >= 0 && same_path(g_items[fi]->path, target)) {
                norig++;
                if (g_repl_ids) g_repl_ids[g_nrepl++] = job->ids[i];
            }
        }
        if (nconf > 0) {
            int c = g_auto ? g_auto_conflict : ask_conflict(nconf, norig, dest);
            if (c < 0) { job_free(job); g_nrepl = 0; set_status(L"Not saved."); return; }
            job->conflict = c;
            if (c != CONFLICT_REPLACE) g_nrepl = 0;
        }
    }
    run_job(job, save_worker, L"Saving\x2026");
}

static void write_auto_estimates(void)
{
    FILE *f = _wfopen(g_autoest, L"w");
    int i;
    char p[1100], e[200], hb[400];
    wchar_t hint[300], wb[16], hbw[16];
    if (!f) return;
    GetWindowTextW(g_w, wb, 16); GetWindowTextW(g_h, hbw, 16); GetWindowTextW(g_hint, hint, 300);
    WideCharToMultiByte(CP_UTF8, 0, hint, -1, hb, sizeof hb, NULL, NULL);
    fprintf(f, "W=%ls H=%ls\nHINT=%s\n", wb, hbw, hb);
    for (i = 0; i < g_nitems; i++) {
        Item *it = g_items[i];
        WideCharToMultiByte(CP_UTF8, 0, base_name(it->path), -1, p, sizeof p, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, it->res_err, -1, e, sizeof e, NULL, NULL);
        fprintf(f, "%s\t%llu\t%lld\t%d\t%d\t%d\t%d\t%d\t%s\n", p, it->bytes, it->res_ok ? it->res_bytes : -1LL,
                it->src_w, it->src_h, it->res_w, it->res_h, it->res_actual, e);
    }
    fclose(f);
}

static void check_auto_est(void)
{
    int i;
    if (!g_autoest[0] || g_running) return;
    for (i = 0; i < g_nitems; i++) if (g_items[i]->res_gen != g_gen) return;
    write_auto_estimates();
    PostQuitMessage(0);
}

/* After replacing originals, those images now ARE the saved versions: re-read them so the list,
 * estimates and any later compress start from what's on disk now. */
static void refresh_replaced(void)
{
    int i;
    for (i = 0; i < g_nrepl; i++) {
        int idx = find_item(g_repl_ids[i]);
        Item *it;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (idx < 0) continue;
        it = g_items[idx];
        if (GetFileAttributesExW(it->path, GetFileExInfoStandard, &fa))
            it->bytes = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        if (it->out_path) { DeleteFileW(it->out_path); free(it->out_path); it->out_path = NULL; }
        it->id = g_next_id++;                        /* forget cached estimates for the old file */
        it->src_w = it->src_h = 0;
        it->res_gen = 0;
        it->res_actual = 0;
    }
    if (g_nrepl > 0) {
        InvalidateRect(g_list, NULL, FALSE);
        update_totals();
        update_save_panel();
        SetTimer(g_wnd, TIMER_EST, 150, NULL);
    }
    g_nrepl = 0;
}

static void job_done(JobResult *r)
{
    wchar_t msg[4000], a[32], b[32];
    int kind = r ? r->kind : 0;
    if (g_thread) { WaitForSingleObject(g_thread, INFINITE); CloseHandle(g_thread); g_thread = NULL; }
    set_running(0);
    InterlockedExchange(&g_est_pause, 0);
    SendMessageW(g_progress, PBM_SETPOS, 0, 0);
    InvalidateRect(g_list, NULL, FALSE);
    update_totals();
    update_save_panel();

    if (g_auto) {                               /* test hook: compress, save, write a log, quit */
        if (r && kind == 0 && !r->cancelled) {
            g_auto_ok = r->ok; g_auto_failed = r->failed;
            free(r);
            start_save();
            if (g_running) return;
            r = NULL;
        }
        {
            wchar_t logp[1040];
            FILE *lf;
            swprintf(logp, 1040, L"%ls.log", g_autozip);
            lf = _wfopen(logp, L"w");
            if (lf) {
                if (r) fprintf(lf, "compressed=%d failed=%d saved=%d save_failed=%d fatal=%d out=%llu replaced=%d skipped=%d\n",
                               g_auto_ok, g_auto_failed, r->ok, r->failed, r->fatal, r->out_bytes, r->replaced, r->skipped);
                else fprintf(lf, "compressed=%d failed=%d saved=0\n", g_auto_ok, g_auto_failed);
                fclose(lf);
            }
        }
        if (g_autoest[0]) write_auto_estimates();
        free(r);
        PostQuitMessage(0);
        return;
    }
    est_schedule(0);
    if (!r) { set_status(L"Not enough memory."); return; }

    if (kind == 0) {
        if (r->cancelled) {
            set_status(L"Stopped. The images finished so far are kept and can be saved.");
        } else if (r->ok == 0 && r->failed > 0) {
            set_status(L"No images could be compressed.");
            swprintf(msg, 4000, L"None of the checked files could be read as images.\n\n%ls", r->errors);
            MessageBoxW(g_wnd, msg, APP_NAME, MB_ICONWARNING);
        } else {
            int pct = r->in_bytes ? (int)(100.0 * ((double)r->out_bytes - (double)r->in_bytes) / (double)r->in_bytes + (r->out_bytes < r->in_bytes ? -0.5 : 0.5)) : 0;
            fmt_bytes(r->in_bytes, a, 32);
            fmt_bytes(r->out_bytes, b, 32);
            swprintf(msg, 4000, L"Compressed %d image%ls: %ls \x2192 %ls (%ls%d%%). Preview or save them below.",
                     r->ok, r->ok == 1 ? L"" : L"s", a, b, pct < 0 ? L"\x2212" : L"+", pct < 0 ? -pct : pct);
            set_status(msg);
            if (r->failed) {
                swprintf(msg, 4000, L"%d file%ls couldn't be compressed:\n%ls%ls", r->failed, r->failed == 1 ? L"" : L"s",
                         r->errors, r->failed > 8 ? L"   \x2026 and more\n" : L"");
                MessageBoxW(g_wnd, msg, APP_NAME, MB_ICONWARNING);
            }
        }
    } else {
        refresh_replaced();
        if (r->cancelled) {
            set_status(r->kind == 2 ? L"Cancelled. No zip was saved." : L"Cancelled. Some images may already be in the folder.");
        } else if (r->fatal) {
            set_status(L"Saving failed.");
            MessageBoxW(g_wnd, r->fatalmsg, APP_NAME, MB_ICONERROR);
        } else {
            size_t l;
            fmt_bytes(r->out_bytes, b, 32);
            swprintf(msg, 4000, L"Saved %d image%ls (%ls) to %ls", r->ok, r->ok == 1 ? L"" : L"s", b, r->dest);
            set_status(msg);
            swprintf(msg, 4000, L"Saved %d image%ls (%ls) to:\n%ls\n", r->ok, r->ok == 1 ? L"" : L"s", b, r->dest);
            if (r->replaced) {
                l = wcslen(msg);
                swprintf(msg + l, 4000 - l, L"\n%d existing file%ls replaced.\n", r->replaced, r->replaced == 1 ? L" was" : L"s were");
            }
            if (r->skipped) {
                l = wcslen(msg);
                swprintf(msg + l, 4000 - l, L"\n%d image%ls skipped because %ls already there.\n", r->skipped,
                         r->skipped == 1 ? L" was" : L"s were", r->skipped == 1 ? L"its name was" : L"their names were");
            }
            if (r->fatalmsg[0]) { l = wcslen(msg); swprintf(msg + l, 4000 - l, L"\n%ls\n", r->fatalmsg); }
            if (r->failed) {
                l = wcslen(msg);
                swprintf(msg + l, 4000 - l, L"\n%d file%ls skipped:\n%ls", r->failed, r->failed == 1 ? L" was" : L"s were", r->errors);
            }
            l = wcslen(msg);
            swprintf(msg + l, 4000 - l, r->kind == 2 ? L"\nShow the zip in its folder?" : L"\nOpen the folder?");
            if (MessageBoxW(g_wnd, msg, APP_NAME, MB_YESNO | (r->failed ? MB_ICONWARNING : MB_ICONINFORMATION)) == IDYES) {
                if (r->kind == 2) {
                    wchar_t args[1100];
                    swprintf(args, 1100, L"/select,\"%ls\"", r->dest);
                    ShellExecuteW(g_wnd, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
                } else {
                    ShellExecuteW(g_wnd, L"open", r->dest, NULL, NULL, SW_SHOWNORMAL);
                }
            }
        }
    }
    free(r);
    if (g_update_ready) {
        wchar_t s2[200];
        swprintf(s2, 200, L"Version %ls is ready. It installs when you close Image Shrinker.", g_update_ver);
        set_status(s2);
    }
}

static void on_result(ItemRes *x)
{
    int idx = find_item(x->id), had_dims;
    Item *it;
    if (idx < 0 || x->gen != g_gen) {
        if (x->out_path[0]) DeleteFileW(x->out_path);
        free(x);
        return;
    }
    it = g_items[idx];
    if (x->out_path[0]) {                           /* a real result: keep the file for saving/preview */
        if (it->out_path && wcscmp(it->out_path, x->out_path) != 0) DeleteFileW(it->out_path);
        free(it->out_path);
        it->out_path = _wcsdup(x->out_path);
        it->out_gen = x->gen;
        it->out_bytes = (unsigned long long)x->bytes;
        it->out_w = x->out_w;
        it->out_h = x->out_h;
        wcsncpy(it->out_ext, x->out_ext, 15);
    }
    if (!x->actual && it->res_gen == g_gen && it->res_actual) { free(x); return; }
    had_dims = it->src_w > 0;
    if (x->src_w > 0) { it->src_w = x->src_w; it->src_h = x->src_h; }
    it->res_gen = x->gen;
    it->res_ok = x->ok;
    it->res_actual = x->actual;
    it->res_kept = x->kept;
    it->res_bytes = x->bytes;
    it->res_w = x->out_w;
    it->res_h = x->out_h;
    wcsncpy(it->res_err, x->err, 79);
    it->res_err[79] = 0;
    if (it->res_err[0]) it->res_err[0] = towupper(it->res_err[0]);
    if (x->actual) { update_save_panel(); preview_refresh(it->id); }
    free(x);
    SendMessageW(g_list, LVM_REDRAWITEMS, (WPARAM)idx, (LPARAM)idx);
    update_totals();
    if (!had_dims && it == ref_item()) { apply_link(); update_hint(); }
    check_auto_est();
}

/* ---------- drawing ---------- */

static int text_w(HDC dc, const wchar_t *t)
{
    SIZE sz;
    GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
    return sz.cx;
}

static void draw_text(HDC dc, const wchar_t *t, int x0, int x1, int top, int bottom, COLORREF c, UINT extra)
{
    RECT r;
    r.left = x0; r.right = x1; r.top = top; r.bottom = bottom;
    SetTextColor(dc, c);
    DrawTextW(dc, t, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | extra);
}

/* "(−73%)" etc. Returns the colour to use. */
static COLORREF change_text(long long now, unsigned long long before, int kept, wchar_t *out, int n)
{
    double p;
    int ip;
    if (kept || (unsigned long long)now == before || before == 0) { swprintf(out, n, L"(unchanged)"); return COL_DIM; }
    p = 100.0 * ((double)now - (double)before) / (double)before;
    ip = (int)(p < 0 ? p - 0.5 : p + 0.5);
    if (ip == 0) swprintf(out, n, p < 0 ? L"(\x2212<1%%)" : L"(+<1%%)");
    else if (ip < 0) swprintf(out, n, L"(\x2212%d%%)", -ip);
    else swprintf(out, n, L"(+%d%%)", ip);
    return p < 0 ? COL_GREEN : COL_RED;
}

static void draw_check(HDC dc, int x, int y, int sz, int on)
{
    HPEN pen = CreatePen(PS_SOLID, 1, on ? COL_ACCENT : RGB(138, 144, 152)), op;
    HBRUSH br = CreateSolidBrush(on ? COL_ACCENT : RGB(255, 255, 255)), ob;
    op = (HPEN)SelectObject(dc, pen);
    ob = (HBRUSH)SelectObject(dc, br);
    RoundRect(dc, x, y, x + sz, y + sz, S(5), S(5));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
    if (on) {
        pen = CreatePen(PS_SOLID, S(2), RGB(255, 255, 255));
        op = (HPEN)SelectObject(dc, pen);
        MoveToEx(dc, x + sz * 24 / 100, y + sz * 52 / 100, NULL);
        LineTo(dc, x + sz * 43 / 100, y + sz * 71 / 100);
        LineTo(dc, x + sz * 77 / 100, y + sz * 31 / 100);
        SelectObject(dc, op);
        DeleteObject(pen);
    }
}

static void draw_row(const DRAWITEMSTRUCT *d)
{
    HDC dc = d->hDC;
    RECT rc = d->rcItem;
    Item *it;
    int w[5], i, x, pad = S(8), fresh, stale, on, csz = S(15);
    wchar_t t[160], t2[48];
    COLORREF bg, ctext, cdim;
    HFONT of;
    HBRUSH br;

    if ((int)d->itemID < 0 || (int)d->itemID >= g_nitems) return;
    it = g_items[d->itemID];
    on = it->checked;
    bg = (d->itemState & ODS_SELECTED) ? COL_SEL : (d->itemID & 1) ? COL_ALT : RGB(255, 255, 255);
    br = CreateSolidBrush(bg);
    FillRect(dc, &rc, br);
    DeleteObject(br);
    SetBkMode(dc, TRANSPARENT);
    of = (HFONT)SelectObject(dc, g_font);
    for (i = 0; i < 5; i++) w[i] = (int)SendMessageW(g_list, LVM_GETCOLUMNWIDTH, (WPARAM)i, 0);
    fresh = it->res_gen == g_gen;
    stale = !fresh && it->res_gen != 0 && it->res_ok;
    ctext = on ? COL_TEXT : COL_STALE;          /* unchecked rows are greyed out */
    cdim = on ? COL_DIM : COL_STALE;

    /* checkbox */
    x = rc.left;
    draw_check(dc, x + (w[0] - csz) / 2, (rc.top + rc.bottom - csz) / 2, csz, on);
    x += w[0];

    /* path */
    draw_text(dc, it->path, x + S(4), x + w[1] - pad / 2, rc.top, rc.bottom, ctext, DT_PATH_ELLIPSIS);
    x += w[1];

    /* dimensions, or the reason it can't be used */
    if (fresh && !it->res_ok) {
        draw_text(dc, it->res_err, x + pad, x + w[2] - pad / 2, rc.top, rc.bottom, on ? COL_RED : COL_STALE, DT_END_ELLIPSIS);
    } else if ((fresh || stale) && it->src_w > 0) {
        if (it->res_w == it->src_w && it->res_h == it->src_h)
            swprintf(t, 160, L"%d \x00D7 %d  (same)", it->src_w, it->src_h);
        else
            swprintf(t, 160, L"%d \x00D7 %d  \x2192  %d \x00D7 %d", it->src_w, it->src_h, it->res_w, it->res_h);
        draw_text(dc, t, x + pad, x + w[2] - pad / 2, rc.top, rc.bottom, fresh ? ctext : COL_STALE, DT_END_ELLIPSIS);
    } else if (it->src_w > 0) {
        swprintf(t, 160, L"%d \x00D7 %d", it->src_w, it->src_h);
        draw_text(dc, t, x + pad, x + w[2] - pad / 2, rc.top, rc.bottom, cdim, DT_END_ELLIPSIS);
    } else {
        draw_text(dc, L"\x2026", x + pad, x + w[2] - pad / 2, rc.top, rc.bottom, COL_STALE, 0);
    }
    x += w[2];

    /* original size */
    fmt_bytes(it->bytes, t, 160);
    draw_text(dc, t, x, x + w[3] - pad, rc.top, rc.bottom, ctext, DT_RIGHT);
    x += w[3];

    /* new size + change: real size once compressed with these settings, otherwise an estimate */
    if (!g_key.valid || (fresh && !it->res_ok)) {
        draw_text(dc, L"\x2014", x, x + w[4] - pad, rc.top, rc.bottom, COL_DIM, DT_RIGHT);
    } else if (!fresh && !stale) {
        draw_text(dc, L"\x2026", x, x + w[4] - pad, rc.top, rc.bottom, COL_STALE, DT_RIGHT);
    } else {
        wchar_t sz[32];
        COLORREF pc = change_text(it->res_bytes, it->bytes, it->res_kept, t2, 48);
        int right = x + w[4] - pad, pw;
        fmt_bytes((unsigned long long)it->res_bytes, sz, 32);
        swprintf(t, 160, L"%ls%ls", it->res_actual && fresh ? L"" : L"\x2248 ", sz);
        pw = text_w(dc, t2);
        draw_text(dc, t2, right - pw, right, rc.top, rc.bottom, stale || !on ? COL_STALE : pc, 0);
        draw_text(dc, t, x, right - pw - S(6), rc.top, rc.bottom, stale ? COL_STALE : ctext, DT_RIGHT);
    }
    SelectObject(dc, of);
}

static void draw_totals(const DRAWITEMSTRUCT *d)
{
    HDC dc = d->hDC;
    RECT rc = d->rcItem;
    unsigned long long orig = 0, newb = 0;
    int i, fresh = 0, errs = 0, pending = 0, actual = 1, x;
    wchar_t a[32], b[32], t[160];
    HFONT of;

    FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
    if (g_nitems == 0) return;
    if (count_checked() == 0) {
        SetBkMode(dc, TRANSPARENT);
        of = (HFONT)SelectObject(dc, g_font);
        draw_text(dc, L"No images are checked. Tick the boxes on the left to include images.", rc.left + S(2), rc.right,
                  rc.top, rc.bottom, COL_DIM, 0);
        SelectObject(dc, of);
        return;
    }
    for (i = 0; i < g_nitems; i++) {
        Item *it = g_items[i];
        if (!it->checked) continue;
        if (it->res_gen == g_gen && !it->res_ok) { errs++; continue; }
        orig += it->bytes;
        if (it->res_gen == g_gen) { newb += (unsigned long long)it->res_bytes; fresh++; if (!it->res_actual) actual = 0; }
        else pending++;
    }
    SetBkMode(dc, TRANSPARENT);
    of = (HFONT)SelectObject(dc, g_bold);
    x = rc.left + S(2);
    draw_text(dc, L"Total", x, rc.right, rc.top, rc.bottom, COL_TEXT, 0);
    x += text_w(dc, L"Total") + S(12);
    SelectObject(dc, g_font);
    fmt_bytes(orig, a, 32);
    swprintf(t, 160, L"%ls  \x2192  ", a);
    draw_text(dc, t, x, rc.right, rc.top, rc.bottom, COL_TEXT, 0);
    x += text_w(dc, t);
    if (!g_key.valid) {
        draw_text(dc, L"enter a size to see estimates", x, rc.right, rc.top, rc.bottom, COL_DIM, 0);
        x += text_w(dc, L"enter a size to see estimates");
    } else if (pending > 0) {
        swprintf(t, 160, L"estimating\x2026  (%d of %d)", fresh, fresh + pending);
        draw_text(dc, t, x, rc.right, rc.top, rc.bottom, COL_DIM, 0);
        x += text_w(dc, t);
    } else {
        COLORREF pc = change_text((long long)newb, orig, 0, b, 32);
        fmt_bytes(newb, a, 32);
        swprintf(t, 160, L"%ls%ls  ", actual ? L"" : L"\x2248 ", a);
        SelectObject(dc, g_bold);
        draw_text(dc, t, x, rc.right, rc.top, rc.bottom, COL_TEXT, 0);
        x += text_w(dc, t);
        draw_text(dc, b, x, rc.right, rc.top, rc.bottom, pc, 0);
        x += text_w(dc, b);
        SelectObject(dc, g_font);
        if (!actual) {
            draw_text(dc, L"   estimated", x, rc.right, rc.top, rc.bottom, COL_DIM, 0);
            x += text_w(dc, L"   estimated");
        }
    }
    if (errs) {
        swprintf(t, 160, L"   \x00B7   %d can\x2019t be read", errs);
        draw_text(dc, t, x, rc.right, rc.top, rc.bottom, COL_RED, 0);
    }
    SelectObject(dc, of);
}

/* chain-link toggle between the width and height boxes */
static void draw_link(const DRAWITEMSTRUCT *d)
{
    HDC dc = d->hDC;
    RECT rc = d->rcItem;
    int locked = is_checked(g_lock), enabled = IsWindowEnabled(g_link);
    int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2, lw = S(9), lh = S(8), gap = locked ? -S(3) : S(2);
    COLORREF c = !enabled ? RGB(190, 194, 200) : locked ? COL_ACCENT : COL_DIM;
    HPEN pen = CreatePen(PS_SOLID, S(2), c), op;
    HBRUSH ob;
    FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
    op = (HPEN)SelectObject(dc, pen);
    ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, cx - lw - gap / 2, cy - lh / 2, cx - gap / 2 + S(1), cy + lh / 2 + 1, S(7), S(7));
    RoundRect(dc, cx + gap / 2 - S(1), cy - lh / 2, cx + lw + gap / 2, cy + lh / 2 + 1, S(7), S(7));
    if (!locked) {                       /* little break marks */
        MoveToEx(dc, cx - S(1), cy - lh / 2 - S(4), NULL); LineTo(dc, cx - S(1), cy - lh / 2 - S(1));
        MoveToEx(dc, cx + S(1), cy + lh / 2 + S(2), NULL); LineTo(dc, cx + S(1), cy + lh / 2 + S(5));
    }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
    if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) {
        RECT f = rc;
        InflateRect(&f, -1, -1);
        DrawFocusRect(dc, &f);
    }
}

/* ---------- window ---------- */

static HWND mk(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             g_wnd, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static LRESULT CALLBACK list_proc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    LRESULT r;
    (void)id; (void)ref;
    if (m == WM_DROPFILES) return SendMessageW(g_wnd, m, wp, lp);
    if (m == WM_DRAWITEM && ((DRAWITEMSTRUCT *)lp)->CtlType == ODT_HEADER) {
        DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)lp;
        int n = count_checked(), csz = S(15);
        int cx = (d->rcItem.left + d->rcItem.right - csz) / 2, cy = (d->rcItem.top + d->rcItem.bottom - csz) / 2;
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(229, 229, 229)), op;
        FillRect(d->hDC, &d->rcItem, GetSysColorBrush(COLOR_WINDOW));
        op = (HPEN)SelectObject(d->hDC, pen);
        MoveToEx(d->hDC, d->rcItem.right - 1, d->rcItem.top + S(3), NULL);
        LineTo(d->hDC, d->rcItem.right - 1, d->rcItem.bottom - S(3));
        SelectObject(d->hDC, op);
        DeleteObject(pen);
        draw_check(d->hDC, cx, cy, csz, g_nitems > 0 && n == g_nitems);
        if (n > 0 && n < g_nitems) {               /* some checked: a dash */
            RECT r;
            r.left = cx + S(4); r.right = cx + csz - S(4); r.top = cy + csz / 2 - S(1); r.bottom = r.top + S(2);
            FillRect(d->hDC, &r, GetSysColorBrush(COLOR_HIGHLIGHT));
        }
        return TRUE;
    }
    r = DefSubclassProc(h, m, wp, lp);
    if (m == WM_PAINT && g_nitems == 0) {         /* empty-list hint */
        HDC dc = GetDC(h);
        RECT rc, hr;
        HWND hdr = (HWND)SendMessageW(h, LVM_GETHEADER, 0, 0);
        HFONT of = (HFONT)SelectObject(dc, g_font);
        GetClientRect(h, &rc);
        if (hdr && GetWindowRect(hdr, &hr)) rc.top += hr.bottom - hr.top;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, COL_DIM);
        rc.bottom = (rc.top + rc.bottom) / 2 + S(2);
        DrawTextW(dc, L"Drag and drop images or folders here", -1, &rc, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
        rc.top = rc.bottom + S(4); rc.bottom = rc.top + S(20);
        SetTextColor(dc, COL_STALE);
        DrawTextW(dc, L"or use Add images\x2026 / Add folder\x2026 below", -1, &rc, DT_CENTER | DT_TOP | DT_SINGLELINE);
        SelectObject(dc, of);
        ReleaseDC(h, dc);
    }
    return r;
}

#define CLIENT_W 820
#define CLIENT_H 686

static void build_ui(void)
{
    int i, g;
    HWND t;
    LVCOLUMNW col;
    RECT lr;
    int cw[5];

    t = mk(L"STATIC", L"Images", 0, 0, 16, 12, 200, 18, -1);
    SendMessageW(t, WM_SETFONT, (WPARAM)g_bold, TRUE);
    g_count = mk(L"STATIC", L"0 images", SS_RIGHT, 0, 604, 12, 200, 18, ID_COUNT);

    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_OWNERDRAWFIXED | LVS_SHOWSELALWAYS,
                             S(16), S(34), S(788), S(232), g_wnd, (HMENU)(INT_PTR)ID_LIST, g_inst, NULL);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowSubclass(g_list, list_proc, 1, 0);
    GetClientRect(g_list, &lr);
    cw[0] = S(32); cw[2] = S(196); cw[3] = S(82); cw[4] = S(150);
    cw[1] = (lr.right - lr.left) - cw[0] - cw[2] - cw[3] - cw[4] - GetSystemMetrics(SM_CXVSCROLL);
    {
        static const wchar_t *names[5] = { L"", L"File", L"Dimensions", L"Original", L"New size" };
        for (i = 0; i < 5; i++) {
            memset(&col, 0, sizeof col);
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            col.fmt = i >= 3 ? LVCFMT_RIGHT : i == 0 ? LVCFMT_CENTER : LVCFMT_LEFT;
            col.cx = cw[i];
            col.pszText = (LPWSTR)names[i];
            SendMessageW(g_list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
        }
        {   /* the first header cell draws a "check all" box */
            HWND hdr = (HWND)SendMessageW(g_list, LVM_GETHEADER, 0, 0);
            HDITEMW hi;
            memset(&hi, 0, sizeof hi);
            hi.mask = HDI_FORMAT;
            SendMessageW(hdr, HDM_GETITEMW, 0, (LPARAM)&hi);
            hi.fmt |= HDF_OWNERDRAW;
            SendMessageW(hdr, HDM_SETITEMW, 0, (LPARAM)&hi);
        }
    }
    g_totals = mk(L"STATIC", NULL, SS_OWNERDRAW, 0, 18, 272, 786, 22, ID_TOTALS);

    g_add       = mk(L"BUTTON", L"Add images\x2026", WS_TABSTOP, 0, 16, 302, 124, 28, ID_ADD);
    g_addfolder = mk(L"BUTTON", L"Add folder\x2026", WS_TABSTOP, 0, 148, 302, 124, 28, ID_ADDFOLDER);
    g_preview   = mk(L"BUTTON", L"Preview", WS_TABSTOP, 0, 280, 302, 124, 28, ID_PREVIEW);
    g_remove    = mk(L"BUTTON", L"Remove selected", WS_TABSTOP, 0, 548, 302, 132, 28, ID_REMOVE);
    g_clear     = mk(L"BUTTON", L"Clear list", WS_TABSTOP, 0, 688, 302, 116, 28, ID_CLEAR);

    /* image size */
    g = 344;
    mk(L"BUTTON", L"Image size", BS_GROUPBOX, 0, 16, g, 388, 166, -1);
    mk(L"STATIC", L"Longest side:", 0, 0, 32, g + 30, 90, 18, -1);
    g_preset = mk(L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 126, g + 26, 262, 300, ID_PRESET);
    for (i = 0; i < NPRESETS; i++) SendMessageW(g_preset, CB_ADDSTRING, 0, (LPARAM)PRESETS[i].label);
    SendMessageW(g_preset, CB_SETCURSEL, 0, 0);
    g_custom = mk(L"BUTTON", L"Custom size", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, g + 62, 140, 22, ID_CUSTOM);
    g_lock   = mk(L"BUTTON", L"Maintain aspect ratio", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 226, g + 62, 162, 22, ID_LOCK);
    SendMessageW(g_lock, BM_SETCHECK, BST_CHECKED, 0);
    g_wl  = mk(L"STATIC", L"Width", 0, 0, 52, g + 98, 42, 18, -1);
    g_w   = mk(L"EDIT", L"", ES_NUMBER | WS_TABSTOP | ES_AUTOHSCROLL | ES_RIGHT, WS_EX_CLIENTEDGE, 96, g + 94, 64, 24, ID_W);
    g_wpx = mk(L"STATIC", L"px", 0, 0, 164, g + 98, 18, 18, -1);
    g_link = mk(L"BUTTON", NULL, BS_OWNERDRAW | WS_TABSTOP, 0, 186, g + 94, 30, 24, ID_LINK);
    g_hl  = mk(L"STATIC", L"Height", 0, 0, 222, g + 98, 44, 18, -1);
    g_h   = mk(L"EDIT", L"", ES_NUMBER | WS_TABSTOP | ES_AUTOHSCROLL | ES_RIGHT, WS_EX_CLIENTEDGE, 268, g + 94, 64, 24, ID_H);
    g_hpx = mk(L"STATIC", L"px", 0, 0, 336, g + 98, 18, 18, -1);
    SendMessageW(g_w, EM_SETLIMITTEXT, 5, 0);
    SendMessageW(g_h, EM_SETLIMITTEXT, 5, 0);
    g_hint = mk(L"STATIC", L"", SS_NOPREFIX, 0, 32, g + 126, 360, 34, ID_HINT);

    /* format and quality */
    mk(L"BUTTON", L"Format and quality", BS_GROUPBOX, 0, 416, g, 388, 166, -1);
    mk(L"STATIC", L"Save as:", 0, 0, 432, g + 30, 70, 18, -1);
    g_format = mk(L"COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 506, g + 26, 282, 300, ID_FORMAT);
    for (i = 0; i < NFORMATS; i++) SendMessageW(g_format, CB_ADDSTRING, 0, (LPARAM)FORMATS[i].label);
    SendMessageW(g_format, CB_SETCURSEL, 0, 0);
    g_ql1 = mk(L"STATIC", L"Quality:", 0, 0, 432, g + 64, 70, 18, -1);
    g_quality = mk(TRACKBAR_CLASSW, NULL, TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 0, 500, g + 58, 226, 30, ID_QUALITY);
    SendMessageW(g_quality, TBM_SETRANGE, TRUE, MAKELPARAM(30, 100));
    SendMessageW(g_quality, TBM_SETTICFREQ, 5, 0);
    SendMessageW(g_quality, TBM_SETPAGESIZE, 0, 5);
    SendMessageW(g_quality, TBM_SETPOS, TRUE, Q_DEFAULT);
    g_qlabel = mk(L"STATIC", L"", 0, 0, 732, g + 64, 60, 18, ID_QLABEL);
    g_ql2 = mk(L"STATIC", L"smaller file", 0, 0, 508, g + 90, 100, 16, -1);
    g_ql3 = mk(L"STATIC", L"best quality", SS_RIGHT, 0, 618, g + 90, 100, 16, -1);
    g_qnote = mk(L"STATIC", L"Quality applies to JPG and WebP. PNG, WebP lossless, TIFF and BMP keep every pixel; PNG, WebP and TIFF also keep transparency.",
                 SS_NOPREFIX, 0, 432, g + 118, 360, 40, -1);

    /* step 1 and step 2 */
    g = 518;
    mk(L"BUTTON", L"Step 1  \x00B7  Resize and compress", BS_GROUPBOX, 0, 16, g, 388, 104, -1);
    g_go = mk(L"BUTTON", L"Resize && compress", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 32, g + 24, 356, 44, ID_GO);
    SendMessageW(g_go, WM_SETFONT, (WPARAM)g_bold, TRUE);
    g_gohint = mk(L"STATIC", L"Always starts from your original files, so you can try different settings.",
                  SS_NOPREFIX | SS_CENTER, 0, 32, g + 76, 356, 18, -1);
    mk(L"BUTTON", L"Step 2  \x00B7  Save", BS_GROUPBOX, 0, 416, g, 388, 104, -1);
    g_save_folder = mk(L"BUTTON", L"To a folder", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 0, 432, g + 26, 150, 22, ID_SAVE_FOLDER);
    g_save_zip = mk(L"BUTTON", L"As a zip file", BS_AUTORADIOBUTTON, 0, 432, g + 50, 150, 22, ID_SAVE_ZIP);
    SendMessageW(g_save_folder, BM_SETCHECK, BST_CHECKED, 0);
    g_save = mk(L"BUTTON", L"Save\x2026", WS_TABSTOP | WS_GROUP, 0, 640, g + 24, 148, 44, ID_SAVE);
    SendMessageW(g_save, WM_SETFONT, (WPARAM)g_bold, TRUE);
    g_saveinfo = mk(L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS, 0, 432, g + 78, 360, 18, ID_SAVEINFO);
    g_savehint = mk(L"STATIC", L"Save options appear here after you compress.", SS_NOPREFIX | SS_CENTER, 0, 432, g + 44, 356, 18, ID_SAVEHINT);

    /* progress and status */
    g_progress = mk(PROGRESS_CLASSW, NULL, PBS_SMOOTH, 0, 16, 636, 788, 12, ID_PROGRESS);
    g_status = mk(L"STATIC", L"Add some images to get started.", SS_NOPREFIX | SS_ENDELLIPSIS, 0, 16, 656, 660, 18, ID_STATUS);
    g_restart = mk(L"BUTTON", L"Restart now", WS_TABSTOP, 0, 690, 652, 114, 24, ID_RESTART);
    ShowWindow(g_restart, SW_HIDE);

    update_qlabel();
    sync_custom();
    update_hint();
    list_changed();
    make_key(&g_key);
}

static void on_custom_toggled(void)
{
    sync_custom();
    if (is_checked(g_custom) && edit_int(g_w) <= 0 && edit_int(g_h) <= 0) {
        int i = (int)SendMessageW(g_preset, CB_GETCURSEL, 0, 0), px = (i >= 0 && i < NPRESETS) ? PRESETS[i].px : 0;
        Item *r = ref_item();
        if (px > 0) {                     /* start from the preset: longest side of the reference image */
            wchar_t b[16];
            g_driver = (r && probe_item(r) && r->src_h > r->src_w) ? 1 : 0;
            swprintf(b, 16, L"%d", px);
            g_linking = 1;
            SetWindowTextW(g_driver == 0 ? g_w : g_h, b);
            g_linking = 0;
            apply_link();
        }
    }
    if (is_checked(g_custom)) { SetFocus(g_driver == 0 ? g_w : g_h); SendMessageW(g_driver == 0 ? g_w : g_h, EM_SETSEL, 0, -1); }
    update_hint();
    settings_changed();
}

static void on_lock_toggled(void)
{
    if (is_checked(g_lock)) {
        if (edit_int(g_driver == 0 ? g_w : g_h) <= 0 && edit_int(g_driver == 0 ? g_h : g_w) > 0) g_driver = !g_driver;
        apply_link();
    }
    InvalidateRect(g_link, NULL, TRUE);
    update_hint();
    settings_changed();
}

static int preview_get(int id, PreviewInfo *pi)
{
    int i = find_item(id);
    Item *it;
    if (i < 0) return 0;
    it = g_items[i];
    memset(pi, 0, sizeof *pi);
    wcsncpy(pi->path, it->path, 1023);
    if (it->out_path) wcsncpy(pi->out_path, it->out_path, 1023);
    probe_item(it);
    pi->orig_bytes = it->bytes;
    pi->out_bytes = it->out_bytes;
    pi->src_w = it->src_w; pi->src_h = it->src_h;
    pi->out_w = it->out_w; pi->out_h = it->out_h;
    pi->stale = it->out_path && it->out_gen != g_gen;
    return 1;
}

/* Preview the selected images (or all of them, starting at the clicked one). */
static void open_preview(int clicked)
{
    int *ids, n = 0, start = 0, idx = -1, nsel = 0;
    if (g_nitems == 0) return;
    while ((idx = (int)SendMessageW(g_list, LVM_GETNEXTITEM, (WPARAM)idx, LVNI_SELECTED)) >= 0) nsel++;
    ids = (int *)malloc((size_t)g_nitems * sizeof *ids);
    if (!ids) return;
    if (nsel > 1) {
        idx = -1;
        while ((idx = (int)SendMessageW(g_list, LVM_GETNEXTITEM, (WPARAM)idx, LVNI_SELECTED)) >= 0) {
            if (idx == clicked) start = n;
            ids[n++] = g_items[idx]->id;
        }
    } else {
        int i;
        if (clicked < 0) clicked = (int)SendMessageW(g_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
        for (i = 0; i < g_nitems; i++) ids[n++] = g_items[i]->id;
        start = clicked < 0 ? 0 : clicked;
    }
    preview_open(g_wnd, g_inst, g_font, g_bold, g_dpi, ids, n, start, preview_get);
    free(ids);
}

static void set_checks(int from_selection, int value)
{
    int i, idx = -1;
    if (from_selection) {
        while ((idx = (int)SendMessageW(g_list, LVM_GETNEXTITEM, (WPARAM)idx, LVNI_SELECTED)) >= 0) g_items[idx]->checked = value;
    } else {
        for (i = 0; i < g_nitems; i++) g_items[i]->checked = value;
    }
    InvalidateRect(g_list, NULL, FALSE);
    checks_changed();
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *m = (MEASUREITEMSTRUCT *)lp;
        if (m->CtlType == ODT_LISTVIEW) { m->itemHeight = S(24); return TRUE; }
        break;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)lp;
        if (d->CtlID == ID_LIST) draw_row(d);
        else if (d->CtlID == ID_TOTALS) draw_totals(d);
        else if (d->CtlID == ID_LINK) draw_link(d);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
        HWND c = (HWND)lp;
        if (c == g_hint || c == g_qnote || c == g_gohint || c == g_savehint || c == g_saveinfo ||
            ((c == g_ql2 || c == g_ql3) && IsWindowEnabled(c))) {
            SetTextColor((HDC)wp, COL_DIM);
            SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == ID_LIST) {
            if (nh->code == LVN_GETDISPINFOW) {
                NMLVDISPINFOW *di = (NMLVDISPINFOW *)lp;
                if ((di->item.mask & LVIF_TEXT) && di->item.iItem >= 0 && di->item.iItem < g_nitems && di->item.cchTextMax > 0) {
                    wcsncpy(di->item.pszText, di->item.iSubItem == 0 ? base_name(g_items[di->item.iItem]->path) : L"",
                            (size_t)di->item.cchTextMax - 1);
                    di->item.pszText[di->item.cchTextMax - 1] = 0;
                }
            } else if (nh->code == LVN_ITEMCHANGED || nh->code == LVN_ODSTATECHANGED) {
                if (nh->code == LVN_ODSTATECHANGED || (((NMLISTVIEW *)lp)->uChanged & LVIF_STATE)) {
                    EnableWindow(g_preview, g_nitems > 0);
                    apply_link();
                    update_hint();
                }
            } else if (nh->code == NM_CLICK || nh->code == NM_DBLCLK) {
                NMITEMACTIVATE *ia = (NMITEMACTIVATE *)lp;
                if (ia->iItem >= 0 && ia->iItem < g_nitems) {
                    int c0 = (int)SendMessageW(g_list, LVM_GETCOLUMNWIDTH, 0, 0);
                    if (ia->ptAction.x < c0) {
                        if (!g_running && nh->code == NM_CLICK) {
                            g_items[ia->iItem]->checked = !g_items[ia->iItem]->checked;
                            SendMessageW(g_list, LVM_REDRAWITEMS, (WPARAM)ia->iItem, (LPARAM)ia->iItem);
                            checks_changed();
                        }
                    } else if (nh->code == NM_DBLCLK) {
                        open_preview(ia->iItem);
                    }
                }
            } else if (nh->code == LVN_COLUMNCLICK) {
                if (((NMLISTVIEW *)lp)->iSubItem == 0 && !g_running && g_nitems > 0)
                    set_checks(0, count_checked() != g_nitems);
            } else if (nh->code == LVN_KEYDOWN && ((NMLVKEYDOWN *)lp)->wVKey == VK_RETURN) {
                open_preview(-1);
            } else if (nh->code == LVN_KEYDOWN && !g_running) {
                NMLVKEYDOWN *k = (NMLVKEYDOWN *)lp;
                if (k->wVKey == VK_SPACE) {
                    int f = (int)SendMessageW(g_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
                    if (f >= 0) set_checks(1, !g_items[f]->checked);
                }
                else if (k->wVKey == VK_DELETE) remove_selected();
                else if (k->wVKey == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                    LVITEMW lv;
                    memset(&lv, 0, sizeof lv);
                    lv.stateMask = lv.state = LVIS_SELECTED;
                    SendMessageW(g_list, LVM_SETITEMSTATE, (WPARAM)-1, (LPARAM)&lv);
                }
            }
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ADD:       browse_files(); break;
        case ID_ADDFOLDER: browse_folder(); break;
        case ID_REMOVE:    remove_selected(); break;
        case ID_CLEAR:     clear_files(); break;
        case ID_CUSTOM:    on_custom_toggled(); break;
        case ID_LOCK:      on_lock_toggled(); break;
        case ID_LINK:
            SendMessageW(g_lock, BM_SETCHECK, is_checked(g_lock) ? BST_UNCHECKED : BST_CHECKED, 0);
            on_lock_toggled();
            break;
        case ID_W:
        case ID_H:
            if (HIWORD(wp) == EN_CHANGE && !g_linking) {
                g_driver = LOWORD(wp) == ID_W ? 0 : 1;
                apply_link();
                update_hint();
                settings_changed();
            }
            break;
        case ID_PRESET:
            if (HIWORD(wp) == CBN_SELCHANGE) settings_changed();
            break;
        case ID_FORMAT:
            if (HIWORD(wp) == CBN_SELCHANGE) { update_qlabel(); settings_changed(); }
            break;
        case ID_RESTART:
            if (!g_running && upd_apply(1)) { g_update_ready = 0; DestroyWindow(hwnd); }
            break;
        case ID_GO:
            if (g_running) {
                g_cancel = 1;
                EnableWindow(g_go, FALSE);
                set_status(L"Cancelling\x2026");
            } else start_compress();
            break;
        case ID_SAVE:    start_save(); break;
        case IDOK:                                  /* Enter: preview from the list */
            if (GetFocus() == g_list) open_preview(-1);
            break;
        case ID_PREVIEW: open_preview(-1); break;
        }
        return 0;
    case WM_HSCROLL:
        if ((HWND)lp == g_quality) { update_qlabel(); settings_changed(); }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_EST) est_schedule(0);
        return 0;
    case WM_DROPFILES: {
        HDROP d = (HDROP)wp;
        UINT n = DragQueryFileW(d, 0xFFFFFFFF, NULL, 0), i;
        if (!g_running) {
            int before = g_nitems;
            begin_bulk();
            for (i = 0; i < n; i++) {
                wchar_t p[1024];
                if (DragQueryFileW(d, i, p, 1024)) add_path(p);
            }
            end_bulk(before);
        }
        DragFinish(d);
        return 0;
    }
    case WM_JOB_PROGRESS:
        SendMessageW(g_progress, PBM_SETPOS, wp, 0);
        if ((int)wp < g_nitems) SendMessageW(g_list, LVM_ENSUREVISIBLE, wp, FALSE);
        if (lp) { if (!g_cancel) set_status((wchar_t *)lp); free((void *)lp); }
        return 0;
    case WM_JOB_ITEM:
    case WM_EST_RESULT:
        on_result((ItemRes *)lp);
        return 0;
    case WM_JOB_DONE:
        job_done((JobResult *)lp);
        return 0;
    case WM_UPDATE: {
        wchar_t *v = (wchar_t *)lp, s[300];
        if (!v) return 0;
        wcsncpy(g_update_ver, v, 63);
        free(v);
        if (wp) {
            g_update_ready = 1;
            swprintf(s, 300, L"Version %ls is ready. It installs when you close Image Shrinker.", g_update_ver);
            ShowWindow(g_restart, SW_SHOW);
            EnableWindow(g_restart, !g_running);
        } else {
            swprintf(s, 300, L"Version %ls is available at " REPO_URL L"/releases", g_update_ver);
        }
        if (!g_running) set_status(s);
        return 0;
    }
    case WM_CLOSE:
        if (g_running) {
            if (MessageBoxW(hwnd, L"Images are still being processed.\nStop and close?", APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            g_cancel = 1;
            if (g_thread) WaitForSingleObject(g_thread, 15000);
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        preview_close();
        {
            int i;
            for (i = 0; i < g_nitems; i++) item_free(g_items[i]);
            g_nitems = 0;
            if (g_tmpdir[0]) remove_dir_files(g_tmpdir);
        }
        if (g_update_ready) upd_apply(0);         /* swap in the new version for next time */
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    INITCOMMONCONTROLSEX icc;
    WNDCLASSEXW wc;
    NONCLIENTMETRICSW ncm;
    RECT rc;
    MSG msg;
    HDC dc;
    int argc = 0, i, want_format = -1, want_quality = -1, want_max = -1, want_w = -1, want_h = -1, want_lock = -1,
        want_sel = -1, no_update = 0, before, want_preview = -1;
    wchar_t **argv, title[64], upd_url[512] = L"";
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    (void)prev; (void)cmd;

    g_inst = inst;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    upd_cleanup();
    cleanup_old_tmp();

    dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    if (g_dpi < 96) g_dpi = 96;

    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) {
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
        g_bold = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (!g_bold) g_bold = g_font;

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ImageShrinkerWnd";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    if (!RegisterClassExW(&wc)) return 1;

    rc.left = 0; rc.top = 0; rc.right = S(CLIENT_W); rc.bottom = S(CLIENT_H);
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_CONTROLPARENT | WS_EX_ACCEPTFILES);
    swprintf(title, 64, L"%ls %ls", APP_NAME, WIDEN(APP_VERSION));
    g_wnd = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_ACCEPTFILES, wc.lpszClassName, title, style,
                            CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                            NULL, NULL, inst, NULL);
    if (!g_wnd) return 1;

    InitializeCriticalSection(&g_est_cs);
    g_est_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    build_ui();
    DragAcceptFiles(g_list, TRUE);
    g_est_thread = CreateThread(NULL, 0, est_worker, NULL, 0, NULL);

    /* Files/folders dropped onto the exe icon arrive as arguments (plus hidden test flags). */
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    before = g_nitems;
    if (argv) {
        begin_bulk();
        for (i = 1; i < argc; i++) {
            if (wcscmp(argv[i], L"--run") == 0 && i + 1 < argc) { g_auto = 1; wcsncpy(g_autozip, argv[++i], 1023); }
            else if (wcscmp(argv[i], L"--estimate") == 0 && i + 1 < argc) wcsncpy(g_autoest, argv[++i], 1023);
            else if (wcscmp(argv[i], L"--format") == 0 && i + 1 < argc) want_format = _wtoi(argv[++i]);
            else if (wcscmp(argv[i], L"--quality") == 0 && i + 1 < argc) want_quality = _wtoi(argv[++i]);
            else if (wcscmp(argv[i], L"--preset") == 0 && i + 1 < argc) want_max = _wtoi(argv[++i]);
            else if (wcscmp(argv[i], L"--custom") == 0 && i + 2 < argc) { want_w = _wtoi(argv[i + 1]); want_h = _wtoi(argv[i + 2]); i += 2; }
            else if (wcscmp(argv[i], L"--lock") == 0) want_lock = 1;
            else if (wcscmp(argv[i], L"--nolock") == 0) want_lock = 0;
            else if (wcscmp(argv[i], L"--select") == 0 && i + 1 < argc) want_sel = _wtoi(argv[++i]);
            else if (wcscmp(argv[i], L"--no-update") == 0) no_update = 1;
            else if (wcscmp(argv[i], L"--on-conflict") == 0 && i + 1 < argc) {
                i++;
                g_auto_conflict = !wcscmp(argv[i], L"replace") ? CONFLICT_REPLACE : !wcscmp(argv[i], L"skip") ? CONFLICT_SKIP : CONFLICT_KEEP_BOTH;
            }
            else if (wcscmp(argv[i], L"--uncheck") == 0 && i + 1 < argc) { int k = _wtoi(argv[++i]); if (k >= 0 && k < g_nitems) g_items[k]->checked = 0; }
            else if (wcscmp(argv[i], L"--preview") == 0 && i + 1 < argc) want_preview = _wtoi(argv[++i]);
            else if (wcscmp(argv[i], L"--update-url") == 0 && i + 1 < argc) wcsncpy(upd_url, argv[++i], 511);
            else if (wcscmp(argv[i], L"--drop") == 0 && i + 1 < argc) {
                /* test hook: deliver a real WM_DROPFILES, exactly as Explorer would */
                size_t len = wcslen(argv[++i]);
                HGLOBAL hg = GlobalAlloc(GHND, sizeof(DROPFILES) + (len + 2) * sizeof(wchar_t));
                if (hg) {
                    DROPFILES *df = (DROPFILES *)GlobalLock(hg);
                    df->pFiles = sizeof(DROPFILES);
                    df->fWide = TRUE;
                    memcpy((char *)df + sizeof(DROPFILES), argv[i], len * sizeof(wchar_t));
                    GlobalUnlock(hg);
                    SendMessageW(g_wnd, WM_DROPFILES, (WPARAM)hg, 0);
                }
            }
            else add_path(argv[i]);
        }
        end_bulk(before);
        LocalFree(argv);
    }
    if (want_format >= 0 && want_format < NFORMATS) { SendMessageW(g_format, CB_SETCURSEL, (WPARAM)want_format, 0); update_qlabel(); }
    if (want_quality > 0) { SendMessageW(g_quality, TBM_SETPOS, TRUE, want_quality); update_qlabel(); }
    if (want_max >= 0)
        for (i = 0; i < NPRESETS; i++) if (PRESETS[i].px == want_max) SendMessageW(g_preset, CB_SETCURSEL, (WPARAM)i, 0);
    if (want_sel >= 0 && want_sel < g_nitems) {
        LVITEMW lv;
        memset(&lv, 0, sizeof lv);
        lv.stateMask = lv.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(g_list, LVM_SETITEMSTATE, (WPARAM)want_sel, (LPARAM)&lv);
    }
    if (want_lock >= 0) SendMessageW(g_lock, BM_SETCHECK, want_lock ? BST_CHECKED : BST_UNCHECKED, 0);
    if (want_w >= 0) {
        wchar_t b[16];
        SendMessageW(g_custom, BM_SETCHECK, BST_CHECKED, 0);
        sync_custom();
        if (want_w > 0) { swprintf(b, 16, L"%d", want_w); SetWindowTextW(g_w, b); }   /* EN_CHANGE links as typing would */
        if (want_h > 0) { swprintf(b, 16, L"%d", want_h); SetWindowTextW(g_h, b); }
    }
    update_hint();
    settings_changed();

    ShowWindow(g_wnd, show);
    UpdateWindow(g_wnd);
    if (!no_update && !g_auto && !g_autoest[0]) upd_start(g_wnd, WM_UPDATE, WIDEN(APP_VERSION), upd_url);
    else if (upd_url[0]) upd_start(g_wnd, WM_UPDATE, WIDEN(APP_VERSION), upd_url);
    checks_changed();
    if (g_auto) {
        if (g_nitems > 0) start_compress();
        else job_done(NULL);
    } else {
        est_schedule(0);
        if (want_preview >= 0 && want_preview < g_nitems) open_preview(want_preview);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (preview_translate(&msg)) continue;
        if (!IsDialogMessageW(g_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    g_quit = 1;
    SetEvent(g_est_ev);
    if (g_est_thread) WaitForSingleObject(g_est_thread, 3000);
    CoUninitialize();
    return 0;
}
