#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef HWND(__stdcall *list_load_w_fn)(HWND, WCHAR *, int);
typedef void(__stdcall *list_close_window_fn)(HWND);
typedef void(__stdcall *list_get_detect_string_fn)(char *, int);

static HWND g_child;

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SIZE && g_child) {
        MoveWindow(g_child, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND create_top_window() {
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WlxHost";
    RegisterClassExW(&wc);

    HWND top = CreateWindowExW(0, L"WlxHost", L"wlx host", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, 900, 650, nullptr, nullptr,
                               wc.hInstance, nullptr);
    SetWindowPos(top, HWND_TOPMOST, 0, 0, 900, 650, SWP_SHOWWINDOW);
    return top;
}

static bool settled(const wchar_t *status) {
    return wcsncmp(status, L"ok:", 3) == 0 || wcsncmp(status, L"err:", 4) == 0;
}

static void await_status(HWND top, UINT lifetime, wchar_t *status, size_t count) {
    SetTimer(top, 1, 500, nullptr);

    MSG msg;
    UINT elapsed = 0;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_TIMER) {
            elapsed += 500;

            wchar_t now[256] = {0};
            GetWindowTextW(g_child, now, 256);
            if (wcscmp(now, status) != 0) {
                wcscpy_s(status, count, now);
                wprintf(L"[%4ums] %s\n", elapsed, status);
            }
            if (settled(status) || elapsed >= lifetime) break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 3) {
        printf("usage: host <plugin.wlx64> <file> [ms] [expect]\n");
        return 2;
    }
    UINT lifetime = argc > 3 ? _wtoi(argv[3]) : 6000;
    const wchar_t *expect = argc > 4 ? argv[4] : L"ok:";

    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) {
        printf("FAIL: LoadLibrary %lu\n", GetLastError());
        return 1;
    }

    auto detect = (list_get_detect_string_fn)GetProcAddress(dll, "ListGetDetectString");
    auto load = (list_load_w_fn)GetProcAddress(dll, "ListLoadW");
    auto close = (list_close_window_fn)GetProcAddress(dll, "ListCloseWindow");
    if (!detect || !load || !close) {
        printf("FAIL: missing exports\n");
        return 1;
    }

    char detect_string[256] = {0};
    detect(detect_string, sizeof detect_string);
    printf("detect: %s\n", detect_string);

    HWND top = create_top_window();
    g_child = load(top, argv[2], 0);
    if (!g_child) {
        printf("FAIL: ListLoadW returned NULL\n");
        return 1;
    }
    printf("ListLoadW ok\n");

    wchar_t status[256] = {0};
    await_status(top, lifetime, status, ARRAYSIZE(status));

    HWND webview = FindWindowExW(g_child, nullptr, nullptr, nullptr);
    printf(webview ? "webview child present\n" : "FAIL: no webview child\n");

    if (wcsncmp(status, expect, wcslen(expect)) != 0) {
        wprintf(L"FAIL: expected \"%s...\", got \"%s\"\n", expect, status);
        return 1;
    }

    close(g_child);
    DestroyWindow(top);
    return webview ? 0 : 1;
}
