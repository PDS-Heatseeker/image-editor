/* Image Shrinker - copy files into a folder with Windows' own copy engine (IFileOperation),
 * so the user gets Explorer's progress window and "Replace or Skip Files" dialog. */
#ifndef IS_SHELLCOPY_H
#define IS_SHELLCOPY_H
#include <windows.h>

typedef struct {
    int copied, skipped, failed, aborted;
    unsigned long long bytes;
    wchar_t **written;     /* per file: full path it was saved as (may differ if "keep both"), or NULL */
} ShellCopyResult;

/* Copies src[i] to dest\names[i]. Must be called on the UI thread (COM, single-threaded apartment).
 * Returns 1 if Windows did the copy (check res for what happened), 0 if the copy engine
 * isn't available here, so the caller can copy another way. */
int  shell_copy(HWND owner, wchar_t *const *src, wchar_t *const *names, int n, const wchar_t *dest,
                ShellCopyResult *res);
void shell_copy_free(ShellCopyResult *res, int n);

#endif
