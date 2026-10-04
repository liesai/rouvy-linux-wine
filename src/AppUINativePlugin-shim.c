/*
 * Wine compatibility shim for Unity App UI on Windows.
 *
 * Unity's original AppUINativePlugin initializes Windows.UI.ViewManagement
 * through WinRT.  Wine 11 does not currently provide that activation factory,
 * which makes ROUVY 4.7.2 terminate during startup.  This replacement keeps
 * the documented native ABI and implements the small subset ROUVY needs with
 * ordinary Win32 calls.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define EXPORT __declspec(dllexport)

typedef struct AppUIColor {
    float r;
    float g;
    float b;
    float a;
} AppUIColor;

static AppUIColor color_from_syscolor(int index)
{
    COLORREF color = GetSysColor(index);
    AppUIColor result = {
        (float)GetRValue(color) / 255.0f,
        (float)GetGValue(color) / 255.0f,
        (float)GetBValue(color) / 255.0f,
        1.0f
    };
    return result;
}

EXPORT uint8_t NativeAppUI_Initialize(void *config_data)
{
    (void)config_data;
    return 1;
}

EXPORT uint8_t NativeAppUI_EnsureUnityWindowFound(void)
{
    return 1;
}

EXPORT void NativeAppUI_Uninitialize(void)
{
}

EXPORT float NativeAppUI_ScaleFactor(void)
{
    return 1.0f;
}

EXPORT uint8_t NativeAppUI_DarkMode(void)
{
    return 0;
}

EXPORT uint8_t NativeAppUI_HighContrast(void)
{
    HIGHCONTRASTW contrast = { sizeof(contrast), 0, NULL };
    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast),
                               &contrast, 0))
        return 0;
    return (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

EXPORT uint8_t NativeAppUI_ReduceMotion(void)
{
    BOOL enabled = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0))
        return 0;
    return !enabled;
}

EXPORT float NativeAppUI_TextScaleFactor(void)
{
    return 1.0f;
}

EXPORT int NativeAppUI_LayoutDirection(void)
{
    DWORD layout = 0;
    return GetProcessDefaultLayout(&layout) && (layout & LAYOUT_RTL) ? 1 : 0;
}

EXPORT AppUIColor NativeAppUI_GetSystemColor(int type)
{
    switch (type) {
    case 0: return color_from_syscolor(COLOR_WINDOWTEXT);
    case 1: return color_from_syscolor(COLOR_HOTLIGHT);
    case 2: return color_from_syscolor(COLOR_GRAYTEXT);
    case 3: return color_from_syscolor(COLOR_HIGHLIGHTTEXT);
    case 4: return color_from_syscolor(COLOR_HIGHLIGHT);
    case 5: return color_from_syscolor(COLOR_BTNTEXT);
    case 6: return color_from_syscolor(COLOR_BTNFACE);
    case 7: return color_from_syscolor(COLOR_WINDOW);
    case 8: return color_from_syscolor(COLOR_HIGHLIGHT);
    default: {
        AppUIColor clear = { 0.0f, 0.0f, 0.0f, 0.0f };
        return clear;
    }
    }
}

static SIZE_T clipboard_utf8_length(void)
{
    HANDLE handle;
    const WCHAR *wide;
    int bytes;

    if (!OpenClipboard(NULL))
        return 0;
    handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) {
        CloseClipboard();
        return 0;
    }
    wide = (const WCHAR *)GlobalLock(handle);
    if (!wide) {
        CloseClipboard();
        return 0;
    }
    bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    GlobalUnlock(handle);
    CloseClipboard();
    return bytes > 0 ? (SIZE_T)(bytes - 1) : 0;
}

EXPORT SIZE_T NativeAppUI_GetPasteBoardDataLength(int type)
{
    return type == 0 ? clipboard_utf8_length() : 0;
}

EXPORT void NativeAppUI_GetPasteBoardData(int type, SIZE_T size, void *data)
{
    HANDLE handle;
    const WCHAR *wide;
    int bytes;

    if (type != 0 || !size || !data || !OpenClipboard(NULL))
        return;
    handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) {
        CloseClipboard();
        return;
    }
    wide = (const WCHAR *)GlobalLock(handle);
    if (!wide) {
        CloseClipboard();
        return;
    }
    bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (bytes > 0) {
        char *temporary = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)bytes);
        if (temporary) {
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, temporary, bytes,
                                NULL, NULL);
            memcpy(data, temporary, size < (SIZE_T)(bytes - 1)
                                      ? size : (SIZE_T)(bytes - 1));
            HeapFree(GetProcessHeap(), 0, temporary);
        }
    }
    GlobalUnlock(handle);
    CloseClipboard();
}

EXPORT void NativeAppUI_SetPasteBoardData(int type, SIZE_T size,
                                           const void *data)
{
    int wide_chars;
    HGLOBAL handle;
    WCHAR *wide;

    if (type != 0 || !size || !data)
        return;
    wide_chars = MultiByteToWideChar(CP_UTF8, 0, (const char *)data,
                                     (int)size, NULL, 0);
    if (wide_chars <= 0)
        return;
    handle = GlobalAlloc(GMEM_MOVEABLE,
                         ((SIZE_T)wide_chars + 1) * sizeof(WCHAR));
    if (!handle)
        return;
    wide = (WCHAR *)GlobalLock(handle);
    if (!wide) {
        GlobalFree(handle);
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, (const char *)data, (int)size,
                        wide, wide_chars);
    wide[wide_chars] = L'\0';
    GlobalUnlock(handle);
    if (!OpenClipboard(NULL)) {
        GlobalFree(handle);
        return;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, handle))
        GlobalFree(handle);
    CloseClipboard();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
