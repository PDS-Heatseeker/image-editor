/* Image Shrinker - preview window with before/after comparison. */
#ifndef IS_PREVIEW_H
#define IS_PREVIEW_H
#include <windows.h>

typedef struct {
    wchar_t path[1024];            /* original file */
    wchar_t out_path[1024];        /* compressed result, or empty */
    unsigned long long orig_bytes, out_bytes;
    int src_w, src_h, out_w, out_h;
    int stale;                     /* result was made with earlier settings */
} PreviewInfo;

/* Fills *pi for an item id; returns 0 if the item no longer exists. */
typedef int (*PreviewGet)(int id, PreviewInfo *pi);

void preview_open(HWND owner, HINSTANCE inst, HFONT font, HFONT bold, int dpi,
                  const int *ids, int n, int start, PreviewGet get);
void preview_refresh(int id);      /* call when an item's result changed */
int  preview_translate(MSG *m);    /* keyboard shortcuts; returns 1 if handled */
void preview_close(void);

#endif
