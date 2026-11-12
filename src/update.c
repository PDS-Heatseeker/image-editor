/* Image Shrinker - self-update from GitHub Releases.
 *
 * 1. GET <repo>/releases/latest (GitHub REST API, no login needed for public repos)
 * 2. If tag_name is newer than this build, download the .exe asset
 * 3. Verify it: SHA-256 against the asset "digest" GitHub publishes, and a PE header check
 * 4. Save as "<this exe>.new". On close (or "Restart now") the running exe is renamed to
 *    "<exe>.old" (Windows allows renaming a running exe, not overwriting it) and the new
 *    file takes its place. The .old file is removed on the next start.
 */
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "update.h"

#define MAX_JSON  (2u << 20)
#define MAX_EXE   (64u << 20)

static HWND g_hwnd;
static UINT g_msg;
static wchar_t g_cur[32];
static wchar_t g_url[512];
static wchar_t g_exe[MAX_PATH], g_new[MAX_PATH + 8], g_old[MAX_PATH + 8];
static volatile LONG g_ready;

/* ---------- SHA-256 ---------- */

typedef struct { unsigned int h[8]; unsigned char buf[64]; unsigned long long len; unsigned int n; } Sha;

static const unsigned int K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(Sha *s, const unsigned char *p)
{
    unsigned int w[64], a, b, c, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++) w[i] = ((unsigned)p[i*4] << 24) | (p[i*4+1] << 16) | (p[i*4+2] << 8) | p[i*4+3];
    for (i = 16; i < 64; i++)
        w[i] = w[i-16] + (ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3)) + w[i-7] +
               (ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10));
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256(const unsigned char *p, size_t n, unsigned char out[32])
{
    static const unsigned int H0[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    Sha s;
    size_t i;
    unsigned long long bits = (unsigned long long)n * 8;
    unsigned char tail[128];
    size_t rem = n % 64, tl;
    memcpy(s.h, H0, sizeof H0);
    for (i = 0; i + 64 <= n; i += 64) sha_block(&s, p + i);
    memset(tail, 0, sizeof tail);
    memcpy(tail, p + n - rem, rem);
    tail[rem] = 0x80;
    tl = rem < 56 ? 64 : 128;
    for (i = 0; i < 8; i++) tail[tl - 1 - i] = (unsigned char)(bits >> (8 * i));
    sha_block(&s, tail);
    if (tl == 128) sha_block(&s, tail + 64);
    for (i = 0; i < 8; i++) {
        out[i*4] = (unsigned char)(s.h[i] >> 24); out[i*4+1] = (unsigned char)(s.h[i] >> 16);
        out[i*4+2] = (unsigned char)(s.h[i] >> 8); out[i*4+3] = (unsigned char)s.h[i];
    }
}

/* ---------- HTTP ---------- */

static unsigned char *http_get(const wchar_t *url, size_t limit, size_t *outn)
{
    URL_COMPONENTS uc;
    wchar_t host[256], path[2048];
    HINTERNET ses = NULL, con = NULL, req = NULL;
    unsigned char *buf = NULL;
    size_t n = 0, cap = 0;
    DWORD status = 0, sl = sizeof status, avail;
    wchar_t ua[64];
    int ok = 0;

    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return NULL;

    swprintf(ua, 64, L"ImageShrinker/%ls", g_cur);
    ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return NULL;
    WinHttpSetTimeouts(ses, 15000, 15000, 30000, 60000);
    con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con) goto done;
    req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req) goto done;
    if (!WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL))
        goto done;
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sl, NULL) ||
        status != 200)
        goto done;
    for (;;) {
        DWORD got = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) goto done;
        if (avail == 0) break;
        if (n + avail > limit) goto done;
        if (n + avail + 1 > cap) {
            size_t nc = cap ? cap * 2 : 65536;
            unsigned char *nb;
            while (nc < n + avail + 1) nc *= 2;
            nb = (unsigned char *)realloc(buf, nc);
            if (!nb) goto done;
            buf = nb; cap = nc;
        }
        if (!WinHttpReadData(req, buf + n, avail, &got)) goto done;
        n += got;
    }
    if (!buf) buf = (unsigned char *)malloc(1);
    if (buf) { buf[n] = 0; ok = 1; }
done:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    if (!ok) { free(buf); return NULL; }
    *outn = n;
    return buf;
}

/* ---------- tiny JSON helpers (GitHub's release format only) ---------- */

/* Copies the string value of "key" found in [p, end) into out. Returns pointer after it, or NULL. */
static const char *json_str(const char *p, const char *end, const char *key, char *out, size_t outn)
{
    size_t kl = strlen(key);
    while (p && p < end) {
        const char *q, *k = strstr(p, key);
        size_t i = 0;
        if (!k || k >= end) return NULL;
        q = k + kl;
        if (k > p && k[-1] == '"' && *q == '"') {
            q++;
            while (*q == ' ' || *q == ':' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            if (*q == '"') {
                q++;
                while (*q && *q != '"' && q < end) {
                    if (*q == '\\' && q[1]) q++;
                    if (i + 1 < outn) out[i++] = *q;
                    q++;
                }
                out[i] = 0;
                return q;
            }
        }
        p = k + kl;
    }
    return NULL;
}

static void parse_ver(const char *s, int v[3])
{
    int i = 0;
    v[0] = v[1] = v[2] = 0;
    while (*s && (*s < '0' || *s > '9')) s++;
    while (*s && i < 3) {
        if (*s >= '0' && *s <= '9') v[i] = v[i] * 10 + (*s - '0');
        else if (*s == '.') i++;
        else break;
        s++;
    }
}

static int newer(const char *tag, const wchar_t *cur)
{
    char c[32];
    int a[3], b[3], i;
    WideCharToMultiByte(CP_UTF8, 0, cur, -1, c, sizeof c, NULL, NULL);
    parse_ver(tag, a);
    parse_ver(c, b);
    for (i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
    return 0;
}

static int ends_with_exe(const char *s)
{
    size_t l = strlen(s);
    return l > 4 && _stricmp(s + l - 4, ".exe") == 0;
}

/* ---------- worker ---------- */

static void post(int installable, const char *tag)
{
    wchar_t *v = (wchar_t *)malloc(64 * sizeof(wchar_t));
    if (!v) return;
    MultiByteToWideChar(CP_UTF8, 0, tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag, -1, v, 64);
    v[63] = 0;
    if (!PostMessageW(g_hwnd, g_msg, (WPARAM)installable, (LPARAM)v)) free(v);
}

static DWORD WINAPI worker(LPVOID p)
{
    size_t jn = 0, en = 0;
    char *json, tag[64], url[1024], digest[160];
    int found = 0;
    const char *q, *assets, *end, *prev;
    unsigned char *exe = NULL, h[32];
    HANDLE f;
    DWORD wrote = 0;
    wchar_t wurl[1024];
    (void)p;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    Sleep(1500);                                   /* let the window settle first */
    json = (char *)http_get(g_url, MAX_JSON, &jn);
    if (!json) return 0;
    end = json + jn;
    if (!json_str(json, end, "tag_name", tag, sizeof tag) || !newer(tag, g_cur)) { free(json); return 0; }

    /* find the .exe asset, and the digest inside the same asset object
     * (GitHub lists "digest" before "browser_download_url" in each asset) */
    assets = strstr(json, "\"assets\"");
    url[0] = digest[0] = 0;
    prev = assets ? assets : json;
    q = prev;
    while ((q = json_str(q, end, "browser_download_url", url, sizeof url)) != NULL) {
        if (ends_with_exe(url)) {
            const char *d = prev;
            char tmp[160];
            while ((d = json_str(d, q, "digest", tmp, sizeof tmp)) != NULL) strcpy(digest, tmp);
            found = 1;
            break;
        }
        prev = q;
    }
    free(json);
    if (!found) return 0;

    /* can we write next to the exe? */
    f = CreateFileW(g_new, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { post(0, tag); return 0; }

    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024);
    exe = http_get(wurl, MAX_EXE, &en);
    if (!exe || en < 4096 || exe[0] != 'M' || exe[1] != 'Z') goto fail;
    if (strncmp(digest, "sha256:", 7) == 0) {
        char hex[65];
        int i;
        sha256(exe, en, h);
        for (i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", h[i]);
        if (_stricmp(hex, digest + 7) != 0) goto fail;
    }
    if (!WriteFile(f, exe, (DWORD)en, &wrote, NULL) || wrote != en) goto fail;
    CloseHandle(f);
    free(exe);
    InterlockedExchange(&g_ready, 1);
    post(1, tag);
    return 0;
fail:
    CloseHandle(f);
    DeleteFileW(g_new);
    free(exe);
    return 0;
}

/* ---------- public ---------- */

static void paths(void)
{
    if (g_exe[0]) return;
    GetModuleFileNameW(NULL, g_exe, MAX_PATH);
    swprintf(g_new, MAX_PATH + 8, L"%ls.new", g_exe);
    swprintf(g_old, MAX_PATH + 8, L"%ls.old", g_exe);
}

static DWORD WINAPI cleanup_worker(LPVOID p)
{
    int i;
    (void)p;
    for (i = 0; i < 20; i++) {                     /* the old process may still be closing */
        if (GetFileAttributesW(g_old) == INVALID_FILE_ATTRIBUTES || DeleteFileW(g_old)) break;
        Sleep(500);
    }
    return 0;
}

void upd_cleanup(void)
{
    HANDLE t;
    paths();
    DeleteFileW(g_new);                            /* half-finished download from last time */
    if (GetFileAttributesW(g_old) == INVALID_FILE_ATTRIBUTES) return;
    t = CreateThread(NULL, 0, cleanup_worker, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

void upd_start(HWND hwnd, UINT msg, const wchar_t *current_version, const wchar_t *url)
{
    HANDLE t;
    paths();
    g_hwnd = hwnd;
    g_msg = msg;
    wcsncpy(g_cur, current_version, 31);
    wcsncpy(g_url, url && url[0] ? url : UPD_DEFAULT_URL, 511);
    t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

int upd_apply(int relaunch)
{
    if (!g_ready) return 0;
    paths();
    DeleteFileW(g_old);
    if (!MoveFileExW(g_exe, g_old, MOVEFILE_REPLACE_EXISTING)) return 0;
    if (!MoveFileExW(g_new, g_exe, MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(g_old, g_exe, MOVEFILE_REPLACE_EXISTING);   /* put the old one back */
        return 0;
    }
    InterlockedExchange(&g_ready, 0);
    if (relaunch) {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        if (CreateProcessW(g_exe, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    return 1;
}
