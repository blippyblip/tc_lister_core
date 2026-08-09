// Minimal Lister host: loads a .wlx64 the way Total Commander does, so a plugin
// can be exercised without TC.
// Usage: host.exe <plugin> <file> [ms] [expected status prefix, default "ok:"]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef HWND(__stdcall *ListLoadW_t)(HWND, WCHAR *, int);
typedef void(__stdcall *ListCloseWindow_t)(HWND);
typedef void(__stdcall *ListGetDetectString_t)(char *, int);

static HWND g_child;

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SIZE && g_child) {
        MoveWindow(g_child, 0, 0, LOWORD(l), HIWORD(l), TRUE);
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 3) { printf("usage: host <plugin.wlx64> <file> [ms] [expect]\n"); return 2; }
    UINT lifetime = argc > 3 ? _wtoi(argv[3]) : 6000;
    const wchar_t *expect = argc > 4 ? argv[4] : L"ok:";

    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) { printf("FAIL: LoadLibrary %lu\n", GetLastError()); return 1; }

    auto detect = (ListGetDetectString_t)GetProcAddress(dll, "ListGetDetectString");
    auto load = (ListLoadW_t)GetProcAddress(dll, "ListLoadW");
    auto close = (ListCloseWindow_t)GetProcAddress(dll, "ListCloseWindow");
    if (!detect || !load || !close) { printf("FAIL: missing exports\n"); return 1; }

    char ds[256] = {0};
    detect(ds, sizeof ds);
    printf("detect: %s\n", ds);

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WlxHost";
    RegisterClassExW(&wc);
    HWND top = CreateWindowExW(0, L"WlxHost", L"wlx host", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, 900, 650, nullptr, nullptr, wc.hInstance, nullptr);

    SetWindowPos(top, HWND_TOPMOST, 0, 0, 900, 650, SWP_SHOWWINDOW);

    g_child = load(top, argv[2], 0);
    if (!g_child) { printf("FAIL: ListLoadW returned NULL\n"); return 1; }
    printf("ListLoadW ok\n");

    // Poll the status the plugin mirrors onto its window (see DocumentTitleChanged).
    SetTimer(top, 1, 500, nullptr);
    MSG msg;
    UINT elapsed = 0;
    wchar_t last[256] = {0};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_TIMER) {
            elapsed += 500;
            wchar_t t[256] = {0};
            GetWindowTextW(g_child, t, 256);
            if (wcscmp(t, last) != 0) { wcscpy_s(last, t); wprintf(L"[%4ums] %s\n", elapsed, t); }
            if (wcsncmp(last, L"ok:", 3) == 0 || wcsncmp(last, L"err:", 4) == 0 || elapsed >= lifetime) break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // A WebView2 child means the plugin got as far as hosting a browser; the
    // document title (mirrored onto a child window) reports whether it loaded.
    HWND wv = FindWindowExW(g_child, nullptr, nullptr, nullptr);
    printf(wv ? "webview child present\n" : "FAIL: no webview child\n");
    if (wcsncmp(last, expect, wcslen(expect)) != 0) {
        wprintf(L"FAIL: expected \"%s...\", got \"%s\"\n", expect, last);
        return 1;
    }

    close(g_child);
    DestroyWindow(top);
    return wv ? 0 : 1;
}
