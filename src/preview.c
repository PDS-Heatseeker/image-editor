/* Image Shrinker - preview window with before/after comparison.
 *
 * Shows the original; once an image has been compressed, shows original (left of the
 * divider) and result (right) at the same size so they can be compared. Drag the divider,
 * scroll to zoom, drag to pan when zoomed, arrows to step through the images.
 */
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "core.h"
#include "preview.h"

#define COL_BG     RGB(36, 38, 42)
#define COL_TEXT   RGB(28, 32, 38)
#define COL_DIM    RGB(118, 124, 132)
#define COL_GREEN  RGB(16, 124, 16)
#define COL_RED    RGB(196, 43, 28)
#define MAX_SIDE   4096            /* decode cap for display */

enum { PB_PREV = 1, PB_NEXT, PB_FIT, PB_100, PB_200 };

static HWND pw, b_prev, b_next, b_fit, b_100, b_200;
static HINSTANCE p_inst;
static HFONT p_font, p_bold;
static int p_dpi = 96;
static int *p_ids, p_n, p_pos;
static PreviewGet p_get;

static PreviewInfo cur;
static int cur_ok;
static unsigned char *bpx, *apx;   /* before / after, BGRA */
static int iw, ih;                 /* both images share this size */
static int has_after;
static wchar_t p_err[200];

static int fit = 1;
static double zoom = 1, cx, cy, split = 0.5;
static int drag, lastx, lasty;

static HBITMAP cache_b, cache_a;   /* pre-scaled copies for smooth dragging when zoomed out */
static int cache_w, cache_h;

static int PS(int v) { return MulDiv(v, p_dpi, 96); }
#define TB PS(48)
#define IB PS(40)

static void free_images(void)
{
    free(bpx); free(apx);
    bpx = apx = NULL;
    if (cache_b) DeleteObject(cache_b);
    if (cache_a) DeleteObject(cache_a);
    cache_b = cache_a = NULL;
    cache_w = cache_h = 0;
    has_after = 0;
    iw = ih = 0;
}

static unsigned char *slurp(const wchar_t *path, size_t *n)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER sz;
    unsigned char *b;
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 1024LL * 1024 * 1024) { CloseHandle(h); return NULL; }
    b = (unsigned char *)malloc((size_t)sz.QuadPart);
    if (b && (!ReadFile(h, b, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart)) { free(b); b = NULL; }
    CloseHandle(h);
    *n = (size_t)sz.QuadPart;
    return b;
}

static const wchar_t *base_name(const wchar_t *p)
{
    const wchar_t *a = wcsrchr(p, L'\\'), *b = wcsrchr(p, L'/');
    if (b > a) a = b;
    return a ? a + 1 : p;
}

static void load(void)
{
    size_t n = 0, an = 0;
    unsigned char *in, *out = NULL;
    char err[160];
    wchar_t title[400];

    free_images();
    p_err[0] = 0;
    cur_ok = p_n > 0 && p_get(p_ids[p_pos], &cur);
    fit = 1; split = 0.5;
    EnableWindow(b_prev, p_pos > 0);
    EnableWindow(b_next, p_pos < p_n - 1);
    if (!cur_ok) { wcscpy(p_err, L"This image is no longer in the list."); goto done; }
    swprintf(title, 400, L"Preview \x2013 %ls", base_name(cur.path));
    SetWindowTextW(pw, title);

    SetCursor(LoadCursorW(NULL, IDC_WAIT));
    in = slurp(cur.path, &n);
    if (!in) { wcscpy(p_err, L"The original file can't be opened."); goto done; }
    if (cur.out_path[0]) out = slurp(cur.out_path, &an);
    if (out) {
        int w = 0, h = 0;
        if (is_preview_decode(out, an, MAX_SIDE, MAX_SIDE, 0, &apx, &w, &h, err, sizeof err) &&
            is_preview_decode(in, n, w, h, 1, &bpx, &iw, &ih, err, sizeof err)) {
            has_after = 1;
        } else {
            free(apx); apx = NULL;
        }
        free(out);
    }
    if (!has_after) {
        free(bpx); bpx = NULL;
        if (!is_preview_decode(in, n, MAX_SIDE, MAX_SIDE, 0, &bpx, &iw, &ih, err, sizeof err)) {
            MultiByteToWideChar(CP_UTF8, 0, err, -1, p_err, 200);
            if (p_err[0]) p_err[0] = towupper(p_err[0]);
        }
    }
    free(in);
    cx = iw / 2.0; cy = ih / 2.0;
    SetCursor(LoadCursorW(NULL, IDC_ARROW));
done:
    InvalidateRect(pw, NULL, FALSE);
}

/* ---------- geometry ---------- */

static void area_rect(RECT *a)
{
    GetClientRect(pw, a);
    a->top += TB;
    a->bottom -= IB;
    if (a->bottom < a->top) a->bottom = a->top;
}

static double fit_scale(const RECT *a)
{
    double sx, sy;
    if (iw <= 0 || ih <= 0) return 1;
    sx = (double)(a->right - a->left - PS(24)) / iw;
    sy = (double)(a->bottom - a->top - PS(24)) / ih;
    return sx < sy ? sx : sy;
}

static double cur_scale(const RECT *a) { return fit ? fit_scale(a) : zoom; }

static void clamp_center(const RECT *a, double s)
{
    double hw = (a->right - a->left) / (2 * s), hh = (a->bottom - a->top) / (2 * s);
    if (iw * s <= a->right - a->left) cx = iw / 2.0;
    else { if (cx < hw) cx = hw; if (cx > iw - hw) cx = iw - hw; }
    if (ih * s <= a->bottom - a->top) cy = ih / 2.0;
    else { if (cy < hh) cy = hh; if (cy > ih - hh) cy = ih - hh; }
}

/* image placement (x0,y0,dw,dh) and its visible part */
static void layout(RECT *a, double *s, int *x0, int *y0, int *dw, int *dh, RECT *vis)
{
    area_rect(a);
    *s = cur_scale(a);
    clamp_center(a, *s);
    *dw = (int)(iw * *s + 0.5);
    *dh = (int)(ih * *s + 0.5);
    *x0 = (int)((a->left + a->right) / 2.0 - cx * *s + 0.5);
    *y0 = (int)((a->top + a->bottom) / 2.0 - cy * *s + 0.5);
    vis->left = *x0 > a->left ? *x0 : a->left;
    vis->top = *y0 > a->top ? *y0 : a->top;
    vis->right = *x0 + *dw < a->right ? *x0 + *dw : a->right;
    vis->bottom = *y0 + *dh < a->bottom ? *y0 + *dh : a->bottom;
}

static int divider_x(const RECT *vis) { return vis->left + (int)(split * (vis->right - vis->left) + 0.5); }

/* ---------- drawing ---------- */

static HBITMAP scaled_copy(HDC ref, const unsigned char *px, int w, int h)
{
    BITMAPINFO bi;
    HDC m = CreateCompatibleDC(ref);
    HBITMAP b = CreateCompatibleBitmap(ref, w, h), ob;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = iw;
    bi.bmiHeader.biHeight = -ih;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    ob = (HBITMAP)SelectObject(m, b);
    SetStretchBltMode(m, HALFTONE);
    SetBrushOrgEx(m, 0, 0, NULL);
    StretchDIBits(m, 0, 0, w, h, 0, 0, iw, ih, px, &bi, DIB_RGB_COLORS, SRCCOPY);
    SelectObject(m, ob);
    DeleteDC(m);
    return b;
}

static void blit(HDC dc, HDC ref, const unsigned char *px, HBITMAP cached, int x0, int y0, int dw, int dh, double s)
{
    if (cached) {
        HDC m = CreateCompatibleDC(ref);
        HBITMAP ob = (HBITMAP)SelectObject(m, cached);
        BitBlt(dc, x0, y0, dw, dh, m, 0, 0, SRCCOPY);
        SelectObject(m, ob);
        DeleteDC(m);
    } else {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = iw;
        bi.bmiHeader.biHeight = -ih;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        SetStretchBltMode(dc, s < 1 ? HALFTONE : COLORONCOLOR);   /* crisp pixels when zoomed in */
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchDIBits(dc, x0, y0, dw, dh, 0, 0, iw, ih, px, &bi, DIB_RGB_COLORS, SRCCOPY);
    }
}

static void badge(HDC dc, int x, int y, const wchar_t *t, int right_align)
{
    SIZE sz;
    RECT r;
    HBRUSH br = CreateSolidBrush(RGB(20, 22, 26)), ob;
    HPEN op = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
    HFONT of = (HFONT)SelectObject(dc, p_bold);
    GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
    if (right_align) x -= sz.cx + PS(20);
    ob = (HBRUSH)SelectObject(dc, br);
    RoundRect(dc, x, y, x + sz.cx + PS(20), y + sz.cy + PS(10), PS(12), PS(12));
    SelectObject(dc, ob);
    DeleteObject(br);
    SelectObject(dc, op);
    SetTextColor(dc, RGB(255, 255, 255));
    r.left = x; r.top = y; r.right = x + sz.cx + PS(20); r.bottom = y + sz.cy + PS(10);
    DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
}

static void fmt_bytes(unsigned long long b, wchar_t *out, int n)
{
    if (b >= 1024ULL * 1024 * 1024) swprintf(out, n, L"%.2f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024 * 1024)      swprintf(out, n, L"%.1f MB", b / (1024.0 * 1024));
    else if (b >= 1024)             swprintf(out, n, L"%.0f KB", b / 1024.0);
    else                            swprintf(out, n, L"%llu B", b);
}

static int text_out(HDC dc, int x, int y, int h, const wchar_t *t, COLORREF c)
{
    SIZE sz;
    RECT r;
    GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
    r.left = x; r.right = x + sz.cx + 2; r.top = y; r.bottom = y + h;
    SetTextColor(dc, c);
    DrawTextW(dc, t, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    return sz.cx;
}

static void paint(HDC wdc)
{
    RECT cr, a, vis, r;
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bm, obm;
    HBRUSH br;
    HFONT of;
    double s;
    int x0, y0, dw, dh, w, x;
    wchar_t t[300], t2[64], sz[32];

    GetClientRect(pw, &cr);
    bm = CreateCompatibleBitmap(wdc, cr.right > 0 ? cr.right : 1, cr.bottom > 0 ? cr.bottom : 1);
    obm = (HBITMAP)SelectObject(dc, bm);
    SetBkMode(dc, TRANSPARENT);
    of = (HFONT)SelectObject(dc, p_font);

    /* toolbar */
    r = cr; r.bottom = TB;
    FillRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE));
    if (cur_ok) {
        SelectObject(dc, p_bold);
        x = PS(212);
        x += text_out(dc, x, 0, TB, base_name(cur.path), COL_TEXT);
        SelectObject(dc, p_font);
        swprintf(t, 300, L"     %d of %d", p_pos + 1, p_n);
        text_out(dc, x, 0, TB, t, COL_DIM);
    }

    /* image area */
    layout(&a, &s, &x0, &y0, &dw, &dh, &vis);
    br = CreateSolidBrush(COL_BG);
    FillRect(dc, &a, br);
    DeleteObject(br);
    if (bpx) {
        int use_cache = s < 1 && dw > 0 && dh > 0;
        if (use_cache && (cache_w != dw || cache_h != dh || !cache_b)) {
            if (cache_b) DeleteObject(cache_b);
            if (cache_a) DeleteObject(cache_a);
            cache_b = scaled_copy(wdc, bpx, dw, dh);
            cache_a = has_after ? scaled_copy(wdc, apx, dw, dh) : NULL;
            cache_w = dw; cache_h = dh;
        }
        SaveDC(dc);
        IntersectClipRect(dc, a.left, a.top, a.right, a.bottom);
        if (has_after) {
            int dx = divider_x(&vis);
            SaveDC(dc);
            IntersectClipRect(dc, a.left, a.top, dx, a.bottom);
            blit(dc, wdc, bpx, use_cache ? cache_b : NULL, x0, y0, dw, dh, s);
            RestoreDC(dc, -1);
            SaveDC(dc);
            IntersectClipRect(dc, dx, a.top, a.right, a.bottom);
            blit(dc, wdc, apx, use_cache ? cache_a : NULL, x0, y0, dw, dh, s);
            RestoreDC(dc, -1);
            {   /* divider with a round handle */
                HPEN pen = CreatePen(PS_SOLID, PS(2), RGB(255, 255, 255)), op = (HPEN)SelectObject(dc, pen);
                HBRUSH hb = CreateSolidBrush(RGB(255, 255, 255)), ob;
                int my = (vis.top + vis.bottom) / 2, rr = PS(15);
                POINT tl[3], tr[3];
                MoveToEx(dc, dx, vis.top, NULL);
                LineTo(dc, dx, vis.bottom);
                ob = (HBRUSH)SelectObject(dc, hb);
                Ellipse(dc, dx - rr, my - rr, dx + rr + 1, my + rr + 1);
                SelectObject(dc, op);
                DeleteObject(pen);
                DeleteObject(hb);
                hb = CreateSolidBrush(RGB(60, 64, 72));
                SelectObject(dc, hb);
                SelectObject(dc, GetStockObject(NULL_PEN));
                tl[0].x = dx - PS(4); tl[0].y = my - PS(5); tl[1].x = dx - PS(4); tl[1].y = my + PS(5); tl[2].x = dx - PS(10); tl[2].y = my;
                tr[0].x = dx + PS(4); tr[0].y = my - PS(5); tr[1].x = dx + PS(4); tr[1].y = my + PS(5); tr[2].x = dx + PS(10); tr[2].y = my;
                Polygon(dc, tl, 3);
                Polygon(dc, tr, 3);
                SelectObject(dc, ob);
                DeleteObject(hb);
            }
            badge(dc, vis.left + PS(10), vis.top + PS(10), L"BEFORE", 0);
            badge(dc, vis.right - PS(10), vis.top + PS(10), L"AFTER", 1);
        } else {
            blit(dc, wdc, bpx, use_cache ? cache_b : NULL, x0, y0, dw, dh, s);
            badge(dc, vis.left + PS(10), vis.top + PS(10), L"ORIGINAL", 0);
        }
        RestoreDC(dc, -1);
        if (!fit || s != 1) {
            swprintf(t, 300, L"%d%%", (int)(s * 100 + 0.5));
            SetTextColor(dc, RGB(200, 204, 210));
            r = a; r.right -= PS(12); r.bottom -= PS(8);
            DrawTextW(dc, t, -1, &r, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);
        }
    } else {
        SetTextColor(dc, RGB(230, 232, 236));
        DrawTextW(dc, p_err[0] ? p_err : L"Loading\x2026", -1, &a, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    /* info bar */
    r = cr; r.top = cr.bottom - IB;
    FillRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE));
    if (cur_ok) {
        int y = r.top;
        x = PS(14);
        SelectObject(dc, p_bold);
        x += text_out(dc, x, y, IB, has_after ? L"Before  " : L"Original  ", COL_TEXT);
        SelectObject(dc, p_font);
        fmt_bytes(cur.orig_bytes, sz, 32);
        if (cur.src_w > 0) swprintf(t, 300, L"%d \x00D7 %d  \x00B7  %ls", cur.src_w, cur.src_h, sz);
        else swprintf(t, 300, L"%ls", sz);
        text_out(dc, x, y, IB, t, COL_TEXT);
        if (has_after) {
            double p = cur.orig_bytes ? 100.0 * ((double)cur.out_bytes - (double)cur.orig_bytes) / (double)cur.orig_bytes : 0;
            int ip = (int)(p < 0 ? p - 0.5 : p + 0.5), wa, wb, wc, wd;
            COLORREF pc = cur.out_bytes == cur.orig_bytes ? COL_DIM : p < 0 ? COL_GREEN : COL_RED;
            fmt_bytes(cur.out_bytes, sz, 32);
            swprintf(t, 300, L"%d \x00D7 %d  \x00B7  %ls  ", cur.out_w, cur.out_h, sz);
            if (cur.out_bytes == cur.orig_bytes) wcscpy(t2, L"(unchanged)");
            else if (ip < 0) swprintf(t2, 64, L"(\x2212%d%%)", -ip);
            else swprintf(t2, 64, L"(+%d%%)", ip);
            SelectObject(dc, p_bold);
            { SIZE q; GetTextExtentPoint32W(dc, L"After  ", 7, &q); wa = q.cx; }
            SelectObject(dc, p_font);
            { SIZE q; GetTextExtentPoint32W(dc, t, (int)wcslen(t), &q); wb = q.cx; }
            { SIZE q; GetTextExtentPoint32W(dc, t2, (int)wcslen(t2), &q); wc = q.cx; }
            wd = 0;
            if (cur.stale) { SIZE q; GetTextExtentPoint32W(dc, L"   earlier settings", 19, &q); wd = q.cx; }
            x = cr.right - PS(14) - wa - wb - wc - wd;
            SelectObject(dc, p_bold);
            x += text_out(dc, x, y, IB, L"After  ", COL_TEXT);
            SelectObject(dc, p_font);
            x += text_out(dc, x, y, IB, t, COL_TEXT);
            x += text_out(dc, x, y, IB, t2, pc);
            if (cur.stale) text_out(dc, x, y, IB, L"   earlier settings", COL_DIM);
            w = cr.right / 2;
            r.left = w - PS(170); r.right = w + PS(170);
            SetTextColor(dc, COL_DIM);
            DrawTextW(dc, L"Drag the line to compare  \x00B7  scroll to zoom", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            r.left = cr.right / 2 - PS(220); r.right = cr.right - PS(14);
            SetTextColor(dc, COL_DIM);
            DrawTextW(dc, L"Not compressed yet \x2013 click Resize & compress to compare before and after.", -1, &r,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }

    BitBlt(wdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, of);
    SelectObject(dc, obm);
    DeleteObject(bm);
    DeleteDC(dc);
}

/* ---------- interaction ---------- */

static void set_zoom(double nz, int mx, int my)
{
    RECT a, vis;
    double s;
    int x0, y0, dw, dh;
    double u, v, fs;
    layout(&a, &s, &x0, &y0, &dw, &dh, &vis);
    fs = fit_scale(&a);
    if (nz < fs * 0.5) nz = fs * 0.5;
    if (nz > 16) nz = 16;
    u = (mx - x0) / s;                     /* image point under the cursor stays put */
    v = (my - y0) / s;
    if (u < 0) u = 0; if (u > iw) u = iw;
    if (v < 0) v = 0; if (v > ih) v = ih;
    fit = 0;
    zoom = nz;
    cx = u - (mx - (a.left + a.right) / 2.0) / nz;
    cy = v - (my - (a.top + a.bottom) / 2.0) / nz;
    InvalidateRect(pw, NULL, FALSE);
}

static void go(int d)
{
    int np = p_pos + d;
    if (np < 0 || np >= p_n) return;
    p_pos = np;
    load();
}

static int pannable(void)
{
    RECT a, vis;
    double s;
    int x0, y0, dw, dh;
    layout(&a, &s, &x0, &y0, &dw, &dh, &vis);
    return dw > a.right - a.left || dh > a.bottom - a.top;
}

static int near_divider(int x, int y)
{
    RECT a, vis;
    double s;
    int x0, y0, dw, dh;
    if (!has_after) return 0;
    layout(&a, &s, &x0, &y0, &dw, &dh, &vis);
    return y >= vis.top && y < vis.bottom && abs(x - divider_x(&vis)) <= PS(16);
}

static void key(WPARAM k)
{
    RECT a;
    switch (k) {
    case VK_LEFT:  case VK_PRIOR: go(-1); break;
    case VK_RIGHT: case VK_NEXT:  go(1); break;
    case VK_ESCAPE: DestroyWindow(pw); break;
    case 'F': case '0': fit = 1; InvalidateRect(pw, NULL, FALSE); break;
    case '1': area_rect(&a); set_zoom(1, (a.left + a.right) / 2, (a.top + a.bottom) / 2); break;
    case '2': area_rect(&a); set_zoom(2, (a.left + a.right) / 2, (a.top + a.bottom) / 2); break;
    }
}

static void place_buttons(void)
{
    RECT cr;
    int y = (TB - PS(28)) / 2;
    GetClientRect(pw, &cr);
    MoveWindow(b_prev, PS(10), y, PS(92), PS(28), TRUE);
    MoveWindow(b_next, PS(108), y, PS(92), PS(28), TRUE);
    MoveWindow(b_fit, cr.right - PS(10) - 3 * PS(62) - 2 * PS(6), y, PS(62), PS(28), TRUE);
    MoveWindow(b_100, cr.right - PS(10) - 2 * PS(62) - PS(6), y, PS(62), PS(28), TRUE);
    MoveWindow(b_200, cr.right - PS(10) - PS(62), y, PS(62), PS(28), TRUE);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE:
        place_buttons();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = PS(560);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = PS(380);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case PB_PREV: go(-1); break;
        case PB_NEXT: go(1); break;
        case PB_FIT: fit = 1; InvalidateRect(h, NULL, FALSE); break;
        case PB_100: case PB_200: {
            RECT a;
            area_rect(&a);
            set_zoom(LOWORD(wp) == PB_100 ? 1 : 2, (a.left + a.right) / 2, (a.top + a.bottom) / 2);
            break;
        }
        }
        SetFocus(h);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (HWND)wp == h) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            if (drag == 1 || near_divider(p.x, p.y)) { SetCursor(LoadCursorW(NULL, IDC_SIZEWE)); return TRUE; }
            if (drag == 2 || (bpx && p.y > TB && pannable())) { SetCursor(LoadCursorW(NULL, IDC_SIZEALL)); return TRUE; }
        }
        break;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        RECT a, vis;
        double s;
        int x0, y0, dw, dh;
        SetFocus(h);
        if (!bpx) return 0;
        layout(&a, &s, &x0, &y0, &dw, &dh, &vis);
        if (y < a.top || y >= a.bottom) return 0;
        if (near_divider(x, y)) drag = 1;
        else if (pannable()) drag = 2;
        else if (has_after && vis.right > vis.left) {
            split = (double)(x - vis.left) / (vis.right - vis.left);
            if (split < 0) split = 0; if (split > 1) split = 1;
            drag = 1;
            InvalidateRect(h, NULL, FALSE);
        }
        if (drag) { SetCapture(h); lastx = x; lasty = y; }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (drag == 1) {
            RECT a, vis;
            double s;
            int x0, y0, dw, dh;
            layout(&a, &s, &x0, &y0, &dw, &dh, &vis);
            if (vis.right > vis.left) {
                split = (double)(x - vis.left) / (vis.right - vis.left);
                if (split < 0) split = 0; if (split > 1) split = 1;
            }
            InvalidateRect(h, NULL, FALSE);
        } else if (drag == 2) {
            RECT a;
            double s;
            area_rect(&a);
            s = cur_scale(&a);
            if (fit) { fit = 0; zoom = s; }
            cx -= (x - lastx) / s;
            cy -= (y - lasty) / s;
            InvalidateRect(h, NULL, FALSE);
        }
        lastx = x; lasty = y;
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (drag) { drag = 0; if (GetCapture() == h) ReleaseCapture(); }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p;
        RECT a;
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        double s, f = 1;
        if (!bpx) return 0;
        p.x = GET_X_LPARAM(lp); p.y = GET_Y_LPARAM(lp);
        ScreenToClient(h, &p);
        area_rect(&a);
        s = cur_scale(&a);
        while (d >= 120) { f *= 1.25; d -= 120; }
        while (d <= -120) { f /= 1.25; d += 120; }
        if (s * f <= fit_scale(&a) && f < 1) { fit = 1; InvalidateRect(h, NULL, FALSE); }
        else set_zoom(s * f, p.x, p.y);
        return 0;
    }
    case WM_KEYDOWN:
        key(wp);
        return 0;
    case WM_DESTROY:
        free_images();
        free(p_ids);
        p_ids = NULL;
        p_n = 0;
        pw = NULL;
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static HWND button(const wchar_t *t, int id)
{
    HWND b = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, pw,
                             (HMENU)(INT_PTR)id, p_inst, NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)p_font, TRUE);
    return b;
}

void preview_open(HWND owner, HINSTANCE inst, HFONT font, HFONT bold, int dpi,
                  const int *ids, int n, int start, PreviewGet get)
{
    static int registered;
    if (n <= 0) return;
    p_inst = inst; p_font = font; p_bold = bold; p_dpi = dpi; p_get = get;
    free(p_ids);
    p_ids = (int *)malloc((size_t)n * sizeof *p_ids);
    if (!p_ids) return;
    memcpy(p_ids, ids, (size_t)n * sizeof *p_ids);
    p_n = n;
    p_pos = start < 0 || start >= n ? 0 : start;
    if (!registered) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"ImageShrinkerPreview";
        RegisterClassExW(&wc);
        registered = 1;
    }
    if (!pw) {
        RECT orc;
        int w = PS(1000), h = PS(720), x, y;
        GetWindowRect(owner, &orc);
        x = (orc.left + orc.right - w) / 2;
        y = (orc.top + orc.bottom - h) / 2;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        pw = CreateWindowExW(0, L"ImageShrinkerPreview", L"Preview", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             x, y, w, h, owner, NULL, inst, NULL);
        if (!pw) return;
        b_prev = button(L"\x2039  Previous", PB_PREV);
        b_next = button(L"Next  \x203A", PB_NEXT);
        b_fit = button(L"Fit", PB_FIT);
        b_100 = button(L"100%", PB_100);
        b_200 = button(L"200%", PB_200);
        place_buttons();
        ShowWindow(pw, SW_SHOW);
    } else {
        ShowWindow(pw, SW_SHOWNORMAL);
        SetForegroundWindow(pw);
    }
    load();
    UpdateWindow(pw);
    SetFocus(pw);
}

void preview_refresh(int id)
{
    if (pw && p_n > 0 && p_ids[p_pos] == id) {
        int f = fit;
        double z = zoom, x = cx, y = cy, sp = split;
        load();
        fit = f; zoom = z; cx = x; cy = y; split = sp;   /* keep the view while comparing */
        InvalidateRect(pw, NULL, FALSE);
    }
}

int preview_translate(MSG *m)
{
    if (!pw || m->message != WM_KEYDOWN) return 0;
    if (m->hwnd != pw && GetParent(m->hwnd) != pw) return 0;
    if (m->wParam == VK_LEFT || m->wParam == VK_RIGHT || m->wParam == VK_ESCAPE || m->wParam == VK_PRIOR ||
        m->wParam == VK_NEXT || m->wParam == 'F' || m->wParam == '0' || m->wParam == '1' || m->wParam == '2') {
        key(m->wParam);
        return 1;
    }
    return 0;
}

void preview_close(void)
{
    if (pw) DestroyWindow(pw);
}
