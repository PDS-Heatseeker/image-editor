/* Image Shrinker - copy files into a folder with Windows' own copy engine (IFileOperation).
 *
 * This is the documented Shell API that File Explorer itself uses. Windows shows its normal
 * copy progress window and, when names are already taken, its "Replace or Skip Files"
 * dialog (Replace / Skip / Let me decide for each file). A progress sink records what
 * actually happened to each file so the app can report it.
 */
#define COBJMACROS
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "shellcopy.h"

typedef struct {
    IFileOperationProgressSink iface;   /* must be first */
    LONG ref;
    wchar_t *const *src;
    int n, hint;
    ShellCopyResult *res;
} Sink;

static Sink *impl(IFileOperationProgressSink *p) { return (Sink *)p; }

static HRESULT STDMETHODCALLTYPE qi(IFileOperationProgressSink *This, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IFileOperationProgressSink)) {
        *out = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE addref(IFileOperationProgressSink *This) { return (ULONG)InterlockedIncrement(&impl(This)->ref); }
static ULONG STDMETHODCALLTYPE release(IFileOperationProgressSink *This) { return (ULONG)InterlockedDecrement(&impl(This)->ref); }

/* which of our files is this? (the engine reports them in order, so start where we left off) */
static int index_of(Sink *s, IShellItem *item)
{
    PWSTR p = NULL;
    int k, i = -1;
    if (!item || FAILED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &p)) || !p) return -1;
    for (k = 0; k < s->n; k++) {
        int j = (s->hint + k) % s->n;
        if (_wcsicmp(s->src[j], p) == 0) { i = j; s->hint = j + 1; break; }
    }
    CoTaskMemFree(p);
    return i;
}

static HRESULT STDMETHODCALLTYPE post_copy(IFileOperationProgressSink *This, DWORD flags, IShellItem *item,
                                           IShellItem *dest_folder, LPCWSTR new_name, HRESULT hr, IShellItem *created)
{
    Sink *s = impl(This);
    int i = index_of(s, item);
    (void)flags; (void)dest_folder; (void)new_name;
    if (SUCCEEDED(hr) && created && hr != COPYENGINE_S_USER_IGNORED) {
        PWSTR p = NULL;
        s->res->copied++;
        if (SUCCEEDED(IShellItem_GetDisplayName(created, SIGDN_FILESYSPATH, &p)) && p) {
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (GetFileAttributesExW(p, GetFileExInfoStandard, &fa))
                s->res->bytes += ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            if (i >= 0 && !s->res->written[i]) s->res->written[i] = _wcsdup(p);
            CoTaskMemFree(p);
        }
    } else if (SUCCEEDED(hr)) {
        s->res->skipped++;                        /* "Skip" in the Replace or Skip dialog */
    } else if (hr != COPYENGINE_E_USER_CANCELLED && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        s->res->failed++;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ok0(IFileOperationProgressSink *This) { (void)This; return S_OK; }
static HRESULT STDMETHODCALLTYPE finish_ops(IFileOperationProgressSink *This, HRESULT hr) { (void)This; (void)hr; return S_OK; }
static HRESULT STDMETHODCALLTYPE pre_rename(IFileOperationProgressSink *This, DWORD f, IShellItem *i, LPCWSTR n)
{ (void)This; (void)f; (void)i; (void)n; return S_OK; }
static HRESULT STDMETHODCALLTYPE post_rename(IFileOperationProgressSink *This, DWORD f, IShellItem *i, LPCWSTR n, HRESULT h, IShellItem *c)
{ (void)This; (void)f; (void)i; (void)n; (void)h; (void)c; return S_OK; }
static HRESULT STDMETHODCALLTYPE pre_move(IFileOperationProgressSink *This, DWORD f, IShellItem *i, IShellItem *d, LPCWSTR n)
{ (void)This; (void)f; (void)i; (void)d; (void)n; return S_OK; }
static HRESULT STDMETHODCALLTYPE post_move(IFileOperationProgressSink *This, DWORD f, IShellItem *i, IShellItem *d, LPCWSTR n, HRESULT h, IShellItem *c)
{ (void)This; (void)f; (void)i; (void)d; (void)n; (void)h; (void)c; return S_OK; }
static HRESULT STDMETHODCALLTYPE pre_delete(IFileOperationProgressSink *This, DWORD f, IShellItem *i)
{ (void)This; (void)f; (void)i; return S_OK; }
static HRESULT STDMETHODCALLTYPE post_delete(IFileOperationProgressSink *This, DWORD f, IShellItem *i, HRESULT h, IShellItem *c)
{ (void)This; (void)f; (void)i; (void)h; (void)c; return S_OK; }
static HRESULT STDMETHODCALLTYPE pre_new(IFileOperationProgressSink *This, DWORD f, IShellItem *d, LPCWSTR n)
{ (void)This; (void)f; (void)d; (void)n; return S_OK; }
static HRESULT STDMETHODCALLTYPE post_new(IFileOperationProgressSink *This, DWORD f, IShellItem *d, LPCWSTR n, LPCWSTR t, DWORD a, HRESULT h, IShellItem *c)
{ (void)This; (void)f; (void)d; (void)n; (void)t; (void)a; (void)h; (void)c; return S_OK; }
static HRESULT STDMETHODCALLTYPE update_progress(IFileOperationProgressSink *This, UINT total, UINT sofar)
{ (void)This; (void)total; (void)sofar; return S_OK; }

static IFileOperationProgressSinkVtbl sink_vtbl = {
    qi, addref, release,
    ok0, finish_ops,                 /* StartOperations, FinishOperations */
    pre_rename, post_rename,
    pre_move, post_move,
    pre_move, post_copy,             /* PreCopyItem has the same shape as PreMoveItem */
    pre_delete, post_delete,
    pre_new, post_new,
    update_progress,
    ok0, ok0, ok0                    /* ResetTimer, PauseTimer, ResumeTimer */
};

int shell_copy(HWND owner, wchar_t *const *src, wchar_t *const *names, int n, const wchar_t *dest,
               ShellCopyResult *res)
{
    IFileOperation *op = NULL;
    IShellItem *folder = NULL;
    Sink sink;
    DWORD cookie = 0;
    BOOL aborted = FALSE;
    HRESULT hr;
    int i, queued = 0;

    memset(res, 0, sizeof *res);
    res->written = (wchar_t **)calloc((size_t)(n > 0 ? n : 1), sizeof *res->written);
    if (!res->written) return 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOperation, NULL, CLSCTX_ALL, &IID_IFileOperation, (void **)&op)))
        return 0;
    if (FAILED(SHCreateItemFromParsingName(dest, NULL, &IID_IShellItem, (void **)&folder))) {
        IFileOperation_Release(op);
        return 0;
    }
    IFileOperation_SetOwnerWindow(op, owner);
    IFileOperation_SetOperationFlags(op, FOF_NOCONFIRMMKDIR | FOF_ALLOWUNDO);

    memset(&sink, 0, sizeof sink);
    sink.iface.lpVtbl = &sink_vtbl;
    sink.ref = 1;
    sink.src = src;
    sink.n = n;
    sink.res = res;
    IFileOperation_Advise(op, &sink.iface, &cookie);

    for (i = 0; i < n; i++) {
        IShellItem *item = NULL;
        if (FAILED(SHCreateItemFromParsingName(src[i], NULL, &IID_IShellItem, (void **)&item))) { res->failed++; continue; }
        hr = IFileOperation_CopyItem(op, item, folder, names[i], NULL);
        IShellItem_Release(item);
        if (hr == E_NOTIMPL) break;                   /* copy engine not available here */
        if (SUCCEEDED(hr)) queued++;
        else res->failed++;
    }
    if (i < n) {
        IFileOperation_Unadvise(op, cookie);
        IShellItem_Release(folder);
        IFileOperation_Release(op);
        return 0;
    }

    hr = queued ? IFileOperation_PerformOperations(op) : S_OK;
    IFileOperation_GetAnyOperationsAborted(op, &aborted);
    IFileOperation_Unadvise(op, cookie);
    IShellItem_Release(folder);
    IFileOperation_Release(op);

    if (hr == E_NOTIMPL && res->copied == 0) return 0;   /* engine not available (e.g. not real Windows) */
    res->aborted = aborted || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == COPYENGINE_E_USER_CANCELLED;
    return 1;
}

void shell_copy_free(ShellCopyResult *res, int n)
{
    int i;
    if (!res->written) return;
    for (i = 0; i < n; i++) free(res->written[i]);
    free(res->written);
    res->written = NULL;
}
