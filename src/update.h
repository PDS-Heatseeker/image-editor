/* Image Shrinker - self-update from GitHub Releases. */
#ifndef IS_UPDATE_H
#define IS_UPDATE_H
#include <windows.h>

#define UPD_DEFAULT_URL L"https://api.github.com/repos/PDS-Heatseeker/image-editor/releases/latest"

/* Remove the previous version left behind by an update (call at start-up). */
void upd_cleanup(void);

/* Check for a newer release in the background. If one is found it is downloaded
 * and verified next to the running exe, then `msg` is posted to `hwnd` with
 * lParam = malloc'd wchar_t* version text (free it) and wParam = 1.
 * If a newer version exists but cannot be installed here (folder not writable),
 * the message is posted with wParam = 0. Nothing is posted otherwise. */
void upd_start(HWND hwnd, UINT msg, const wchar_t *current_version, const wchar_t *url);

/* Swap the downloaded exe in. Returns 1 on success. If relaunch, starts the new exe. */
int upd_apply(int relaunch);

#endif
