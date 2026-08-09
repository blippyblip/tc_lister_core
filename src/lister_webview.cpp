#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <strsafe.h>
#include <wrl.h>
#include <cctype>
#include <set>
#include <string>
#include "WebView2.h"
#include "lister_webview.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

#define lcp_darkmode 128

static HINSTANCE g_inst;
static std::set<HWND> g_live;

struct view {
    ComPtr<ICoreWebView2Controller> controller;
    int focus_tries = 0;
};

static std::wstring dir_of(const std::wstring &path) {
    size_t i = path.find_last_of(L"\\/");
    return i == std::wstring::npos ? std::wstring() : path.substr(0, i);
}

static bool starts_with(const std::wstring &s, const std::wstring &prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

static const std::wstring &asset_origin() {
    static std::wstring s = L"https://" + std::wstring(lister_config.asset_host) + L"/";
    return s;
}

static const std::wstring &file_origin() {
    static std::wstring s = L"https://" + std::wstring(lister_config.file_host) + L"/";
    return s;
}

static std::wstring plugin_dir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(g_inst, buf.data(), (DWORD)buf.size());
        if (n == 0) return std::wstring();
        if (n < buf.size()) { buf.resize(n); break; }
        buf.resize(buf.size() * 2);
    }
    return dir_of(buf);
}

static std::wstring user_data_dir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring dir = std::wstring(buf) + L"\\" + lister_config.data_dir;
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static HANDLE open_log(const std::wstring &dir) {
    std::wstring path = dir + L"\\log.txt";
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return f;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 256 * 1024) return f;

    CloseHandle(f);
    return CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

static void write_log(HANDLE f, const wchar_t *line) {
    char utf8[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof utf8 - 2, nullptr, nullptr);
    if (n <= 1) return;
    utf8[n - 1] = '\r';
    utf8[n] = '\n';
    DWORD written;
    WriteFile(f, utf8, n + 1, &written, nullptr);
}

static void log_line(const wchar_t *fmt, ...) {
    wchar_t line[1024];
    va_list ap;
    va_start(ap, fmt);
    HRESULT hr = StringCchVPrintfW(line, ARRAYSIZE(line), fmt, ap);
    va_end(ap);
    if (FAILED(hr) && hr != STRSAFE_E_INSUFFICIENT_BUFFER) return;

    std::wstring dir = user_data_dir();
    if (dir.empty()) return;

    HANDLE f = open_log(dir);
    if (f == INVALID_HANDLE_VALUE) return;
    write_log(f, line);
    CloseHandle(f);
}

static std::wstring local_app_data() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return (n == 0 || n >= MAX_PATH) ? std::wstring() : buf;
}

static int setting(const std::wstring &dir, const wchar_t *key, int fallback) {
    if (dir.empty()) return fallback;
    return GetPrivateProfileIntW(L"lister", key, fallback, (dir + L"\\config.ini").c_str());
}

// Written once so the file is there to be found and edited; GetPrivateProfileInt
// would fall back to the same defaults without it.
static void seed_config() {
    std::wstring dir = user_data_dir();
    if (dir.empty()) return;

    std::wstring path = dir + L"\\config.ini";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;

    static const char text[] =
        "; Settings for this plugin. Delete a line to go back to its default.\r\n"
        "; A file at %LOCALAPPDATA%\\tc_lister\\config.ini sets the default for\r\n"
        "; every plugin in the family; this one overrides it for this plugin.\r\n"
        "\r\n"
        "[lister]\r\n"
        "; Take the keyboard focus when the pane opens, so the first key works\r\n"
        "; without clicking into it. Never applies to quick view.\r\n"
        "TakeFocus=1\r\n";
    DWORD written;
    WriteFile(f, text, sizeof text - 1, &written, nullptr);
    CloseHandle(f);
}

static bool take_focus_setting() {
    int shared = setting(local_app_data() + L"\\tc_lister", L"TakeFocus", 1);
    return setting(user_data_dir(), L"TakeFocus", shared) != 0;
}

static std::string to_utf8(const std::wstring &s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

static std::wstring url_encode(const std::wstring &s) {
    std::wstring out;
    for (unsigned char c : to_utf8(s)) {
        if (isalnum(c) || strchr("-_.~", c)) {
            out += (wchar_t)c;
        } else {
            wchar_t hex[4];
            StringCchPrintfW(hex, ARRAYSIZE(hex), L"%%%02X", c);
            out += hex;
        }
    }
    return out;
}

static std::wstring commander_ini() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"COMMANDER_INI", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;
    n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::wstring(buf) + L"\\GHISLER\\wincmd.ini";
    return std::wstring();
}

static bool system_prefers_dark() {
    DWORD light = 1, size = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

static bool use_dark_theme(int show_flags) {
    static bool logged = false;
    if (!logged) {
        log_line(L"ShowFlags 0x%08X", show_flags);
        logged = true;
    }
    if (show_flags & lcp_darkmode) return true;

    std::wstring ini = commander_ini();
    UINT mode = ini.empty() ? 0 : GetPrivateProfileIntW(L"Configuration", L"DarkMode", 0, ini.c_str());
    return mode == 1 || (mode == 2 && system_prefers_dark());
}

// Quick view returns the focus to the file list itself - that is Total
// Commander's own behaviour, not something the plugin has to arrange - so the
// focus is simply taken and TC decides whether to keep it. The host window class
// is logged because it is the first thing worth knowing if that stops holding.
static void log_host_window(HWND parent) {
    static bool logged = false;
    if (logged) return;
    logged = true;

    wchar_t cls[64] = {0};
    GetClassNameW(GetAncestor(parent, GA_ROOT), cls, ARRAYSIZE(cls));
    log_line(L"host window class %s", cls);
}

static std::wstring window_class(HWND hwnd) {
    wchar_t cls[64] = {0};
    if (hwnd) GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    return hwnd ? cls : L"(none)";
}

// Whether the focus has landed anywhere inside the WebView2, which parents its
// own child windows under ours.
static bool focus_is_ours(HWND hwnd) {
    HWND focus = GetFocus();
    return focus && (focus == hwnd || IsChild(hwnd, focus));
}

static void take_focus(ICoreWebView2Controller *ctrl, HWND hwnd) {
    std::wstring before = window_class(GetFocus());
    SetFocus(hwnd);
    ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);

    static int logged = 0;
    if (logged < 6) {
        logged++;
        log_line(L"focus %s -> %s (ours %d)", before.c_str(), window_class(GetFocus()).c_str(),
                 (int)focus_is_ours(hwnd));
    }
}

// Lister decides where the focus goes on its own schedule, and whatever it does
// after the pane is set up would undo a single attempt. Rather than guess at its
// order, the focus is retaken a few times over the first half second and the
// timer stops as soon as it has stuck.
#define FOCUS_TIMER 2
#define FOCUS_TRIES 5

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    view *v = (view *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_SETFOCUS:
        // Whoever hands this window the focus means the browser in it, not the
        // bare child window, which would swallow every key.
        if (v && v->controller) {
            v->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
        return 0;
    case WM_TIMER:
        if (wp == FOCUS_TIMER) {
            if (!v || !v->controller || ++v->focus_tries > FOCUS_TRIES || focus_is_ours(hwnd)) {
                KillTimer(hwnd, FOCUS_TIMER);
            } else {
                take_focus(v->controller.Get(), hwnd);
            }
            return 0;
        }
        break;
    case WM_SIZE:
        if (v && v->controller) {
            RECT r;
            GetClientRect(hwnd, &r);
            v->controller->put_Bounds(r);
        }
        return 0;
    case WM_DESTROY:
        g_live.erase(hwnd);
        if (v) {
            if (v->controller) v->controller->Close();
            delete v;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void contain_navigation(ICoreWebView2 *web) {
    EventRegistrationToken tok;
    web->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *a) -> HRESULT {
                LPWSTR uri = nullptr;
                if (FAILED(a->get_Uri(&uri)) || !uri) return S_OK;

                std::wstring u = uri;
                CoTaskMemFree(uri);
                bool web_scheme = starts_with(u, L"http://") || starts_with(u, L"https://");
                if (web_scheme && !starts_with(u, asset_origin()) && !starts_with(u, file_origin())) {
                    a->put_Cancel(TRUE);
                    log_line(L"blocked navigation to %s", u.c_str());
                }
                return S_OK;
            })
            .Get(),
        &tok);

    web->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *a) -> HRESULT {
                a->put_Handled(TRUE);
                return S_OK;
            })
            .Get(),
        &tok);

    ComPtr<ICoreWebView2_4> web4;
    if (FAILED(web->QueryInterface(IID_PPV_ARGS(&web4)))) return;

    web4->add_DownloadStarting(
        Callback<ICoreWebView2DownloadStartingEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2DownloadStartingEventArgs *a) -> HRESULT {
                a->put_Cancel(TRUE);
                return S_OK;
            })
            .Get(),
        &tok);
}

static bool on_our_hosts(const std::wstring &uri) {
    if (!starts_with(uri, L"http://") && !starts_with(uri, L"https://")) return true;
    return starts_with(uri, asset_origin()) || starts_with(uri, file_origin());
}

// The page's own CSP cannot reach inside a document the page frames, and a
// framed file is exactly what the HTML and PDF plugins hand to the browser. This
// stops every request that leaves the two virtual hosts at the host instead, so
// containment does not depend on the previewed file cooperating.
static void contain_requests(ICoreWebView2 *web, ICoreWebView2Environment *env) {
    // The original filter only covers the top-level document, which would leave
    // everything a framed file asks for unchecked - exactly the case that needs
    // this most. ICoreWebView2_22 is what widens it to every frame.
    ComPtr<ICoreWebView2_22> web22;
    if (SUCCEEDED(web->QueryInterface(IID_PPV_ARGS(&web22)))) {
        web22->AddWebResourceRequestedFilterWithRequestSourceKinds(
            L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL,
            COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL);
    } else {
        log_line(L"WebView2 runtime is too old to filter framed requests");
        web->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    }

    ComPtr<ICoreWebView2Environment> owned = env;
    EventRegistrationToken tok;
    web->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [owned](ICoreWebView2 *, ICoreWebView2WebResourceRequestedEventArgs *a) -> HRESULT {
                ComPtr<ICoreWebView2WebResourceRequest> request;
                if (FAILED(a->get_Request(&request)) || !request) return S_OK;

                LPWSTR uri = nullptr;
                if (FAILED(request->get_Uri(&uri)) || !uri) return S_OK;
                std::wstring u = uri;
                CoTaskMemFree(uri);
                if (on_our_hosts(u)) return S_OK;

                ComPtr<ICoreWebView2WebResourceResponse> blocked;
                if (SUCCEEDED(owned->CreateWebResourceResponse(nullptr, 403, L"Blocked", L"", &blocked))) {
                    a->put_Response(blocked.Get());
                }
                log_line(L"blocked request to %s", u.c_str());
                return S_OK;
            })
            .Get(),
        &tok);
}

static void harden_settings(ICoreWebView2 *web) {
    ComPtr<ICoreWebView2Settings> s;
    if (FAILED(web->get_Settings(&s)) || !s) return;

    s->put_AreDefaultContextMenusEnabled(FALSE);
    s->put_IsStatusBarEnabled(FALSE);
    s->put_AreDevToolsEnabled(FALSE);
    s->put_IsWebMessageEnabled(FALSE);
    s->put_AreHostObjectsAllowed(FALSE);
    s->put_IsZoomControlEnabled(FALSE);

    ComPtr<ICoreWebView2Settings3> s3;
    if (SUCCEEDED(s.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);

    ComPtr<ICoreWebView2Settings4> s4;
    if (FAILED(s.As(&s4))) return;
    s4->put_IsGeneralAutofillEnabled(FALSE);
    s4->put_IsPasswordAutosaveEnabled(FALSE);
}

static void map_virtual_hosts(ICoreWebView2 *web, const std::wstring &assets, const std::wstring &folder) {
    ComPtr<ICoreWebView2_3> web3;
    if (FAILED(web->QueryInterface(IID_PPV_ARGS(&web3)))) return;

    web3->SetVirtualHostNameToFolderMapping(lister_config.asset_host, assets.c_str(),
                                            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    web3->SetVirtualHostNameToFolderMapping(lister_config.file_host, folder.c_str(),
                                            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
}

static bool is_key_down(COREWEBVIEW2_KEY_EVENT_KIND kind) {
    return kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
           kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN;
}

static void forward_escape(ICoreWebView2Controller *ctrl, HWND hwnd) {
    EventRegistrationToken tok;
    ctrl->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [hwnd](ICoreWebView2Controller *, ICoreWebView2AcceleratorKeyPressedEventArgs *a) -> HRESULT {
                COREWEBVIEW2_KEY_EVENT_KIND kind;
                UINT key = 0;
                a->get_KeyEventKind(&kind);
                a->get_VirtualKey(&key);
                if (is_key_down(kind) && key == VK_ESCAPE && g_live.count(hwnd)) {
                    a->put_Handled(TRUE);
                    PostMessageW(GetParent(hwnd), WM_KEYDOWN, VK_ESCAPE, 0);
                }
                return S_OK;
            })
            .Get(),
        &tok);
}

static void mirror_title(ICoreWebView2 *web, HWND hwnd) {
    if (!lister_config.page) return;   // the file's own title is not a status

    EventRegistrationToken tok;
    web->add_DocumentTitleChanged(
        Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
            [hwnd](ICoreWebView2 *sender, IUnknown *) -> HRESULT {
                LPWSTR title = nullptr;
                if (FAILED(sender->get_DocumentTitle(&title)) || !title) return S_OK;

                if (g_live.count(hwnd)) SetWindowTextW(hwnd, title);
                if (wcsncmp(title, L"err:", 4) == 0) log_line(L"%s", title);
                CoTaskMemFree(title);
                return S_OK;
            })
            .Get(),
        &tok);
}

// With no viewer page there is nothing to set a status title, so the host says
// what happened instead - the test host reads the same window text either way.
static void report_result(ICoreWebView2 *web, HWND hwnd, const std::wstring &name) {
    EventRegistrationToken tok;
    web->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [hwnd, name](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *a) -> HRESULT {
                if (!g_live.count(hwnd)) return S_OK;

                BOOL ok = FALSE;
                a->get_IsSuccess(&ok);

                wchar_t text[512];
                StringCchPrintfW(text, ARRAYSIZE(text), ok ? L"ok: %s" : L"err: could not open %s",
                                 name.c_str());
                SetWindowTextW(hwnd, text);
                return S_OK;
            })
            .Get(),
        &tok);
}

static void log_failures(ICoreWebView2 *web) {
    EventRegistrationToken tok;
    web->add_ProcessFailed(
        Callback<ICoreWebView2ProcessFailedEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2ProcessFailedEventArgs *a) -> HRESULT {
                COREWEBVIEW2_PROCESS_FAILED_KIND kind;
                a->get_ProcessFailedKind(&kind);
                log_line(L"WebView2 process failed, kind %d", (int)kind);
                return S_OK;
            })
            .Get(),
        &tok);

    web->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *a) -> HRESULT {
                BOOL ok = FALSE;
                COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                a->get_IsSuccess(&ok);
                a->get_WebErrorStatus(&status);
                if (!ok) log_line(L"navigation failed, status %d", (int)status);
                return S_OK;
            })
            .Get(),
        &tok);
}

static void attach_webview(HWND hwnd, ICoreWebView2Controller *ctrl, ICoreWebView2Environment *env,
                           const std::wstring &assets, const std::wstring &folder,
                           const std::wstring &url, const std::wstring &name) {
    view *v = (view *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!v) {
        ctrl->Close();
        return;
    }
    v->controller = ctrl;

    RECT r;
    GetClientRect(hwnd, &r);
    ctrl->put_Bounds(r);

    ComPtr<ICoreWebView2> web;
    ctrl->get_CoreWebView2(&web);
    if (!web) return;

    map_virtual_hosts(web.Get(), assets, folder);
    harden_settings(web.Get());
    contain_navigation(web.Get());
    contain_requests(web.Get(), env);
    forward_escape(ctrl, hwnd);
    mirror_title(web.Get(), hwnd);
    if (!lister_config.page) report_result(web.Get(), hwnd, name);
    log_failures(web.Get());

    HRESULT nav = web->Navigate(url.c_str());
    if (FAILED(nav)) log_line(L"Navigate 0x%08X", nav);

    // Lister hands the window no focus of its own, so without this the first
    // keystroke goes nowhere and the pane needs a click before it responds.
    // Taken twice: once now, and again once the page has loaded, since anything
    // the host does with the focus in between would otherwise win.
    log_host_window(GetParent(hwnd));
    if (!take_focus_setting()) return;

    take_focus(ctrl, hwnd);
    SetTimer(hwnd, FOCUS_TIMER, 100, nullptr);

    ComPtr<ICoreWebView2Controller> keep = ctrl;
    EventRegistrationToken tok;
    web->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [keep, hwnd](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *) -> HRESULT {
                if (g_live.count(hwnd)) take_focus(keep.Get(), hwnd);
                return S_OK;
            })
            .Get(),
        &tok);
}

// With no page of its own a plugin hands the file straight to the browser, which
// is what the PDF viewer needs: as a framed document it never takes the keyboard
// focus without being clicked, and as the top-level one it always does.
static std::wstring page_url(const std::wstring &name, bool dark) {
    if (!lister_config.page) return file_origin() + url_encode(name);

    return asset_origin() + lister_config.page + L"?theme=" + (dark ? L"dark" : L"light") +
           L"&src=" + url_encode(file_origin() + name);
}

static bool start_webview(HWND hwnd, const std::wstring &file, bool dark) {
    std::wstring folder = dir_of(file);
    std::wstring name = folder.empty() ? file : file.substr(folder.size() + 1);
    if (folder.empty() || name.empty()) {
        log_line(L"cannot split path %s", file.c_str());
        return false;
    }

    std::wstring assets = plugin_dir() + L"\\web";
    std::wstring url = page_url(name, dark);

    auto on_environment = [hwnd, assets, folder, url, name](HRESULT hr, ICoreWebView2Environment *env) -> HRESULT {
        if (FAILED(hr) || !env) {
            log_line(L"environment creation 0x%08X", hr);
            return S_OK;
        }
        if (!g_live.count(hwnd)) return S_OK;

        ComPtr<ICoreWebView2Environment> owned = env;
        auto on_controller = [hwnd, owned, assets, folder, url, name](HRESULT hr,
                                                                      ICoreWebView2Controller *ctrl) -> HRESULT {
            if (FAILED(hr) || !ctrl) {
                log_line(L"controller creation 0x%08X", hr);
                return S_OK;
            }
            if (!g_live.count(hwnd)) {
                ctrl->Close();
                return S_OK;
            }
            attach_webview(hwnd, ctrl, owned.Get(), assets, folder, url, name);
            return S_OK;
        };

        env->CreateCoreWebView2Controller(
            hwnd,
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(on_controller).Get());
        return S_OK;
    };

    HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data_dir().c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(on_environment).Get());
    if (FAILED(started)) {
        log_line(L"CreateCoreWebView2Environment 0x%08X (runtime missing?)", started);
        return false;
    }
    return true;
}

static bool register_window_class() {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = lister_config.window_class;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    registered = true;
    return true;
}

static HWND create_view_window(HWND parent) {
    RECT r;
    GetClientRect(parent, &r);

    HWND hwnd = CreateWindowExW(0, lister_config.window_class, L"",
                                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, r.right, r.bottom,
                                parent, nullptr, g_inst, nullptr);
    if (!hwnd) return nullptr;

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) new view());
    g_live.insert(hwnd);
    return hwnd;
}

extern "C" {

void __stdcall ListGetDetectString(char *DetectString, int maxlen) {
    lstrcpynA(DetectString, lister_config.detect, maxlen);
}

HWND __stdcall ListLoadW(HWND ParentWin, WCHAR *FileToLoad, int ShowFlags) try {
    if (!ParentWin || !FileToLoad || !*FileToLoad) return nullptr;
    if (!register_window_class()) return nullptr;

    static bool seeded = false;
    if (!seeded) {
        seed_config();
        seeded = true;
    }

    HWND hwnd = create_view_window(ParentWin);
    if (!hwnd) return nullptr;

    if (!start_webview(hwnd, FileToLoad, use_dark_theme(ShowFlags))) {
        DestroyWindow(hwnd);
        return nullptr;
    }
    return hwnd;
} catch (...) {
    return nullptr;
}

HWND __stdcall ListLoad(HWND ParentWin, char *FileToLoad, int ShowFlags) {
    if (!FileToLoad) return nullptr;

    int n = MultiByteToWideChar(CP_ACP, 0, FileToLoad, -1, nullptr, 0);
    if (n <= 0) return nullptr;

    std::wstring wide(n, L'\0');
    if (!MultiByteToWideChar(CP_ACP, 0, FileToLoad, -1, wide.data(), n)) return nullptr;
    return ListLoadW(ParentWin, wide.data(), ShowFlags);
}

void __stdcall ListCloseWindow(HWND ListWin) {
    if (ListWin) DestroyWindow(ListWin);
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
    } else if (reason == DLL_PROCESS_DETACH && !reserved) {
        UnregisterClassW(lister_config.window_class, inst);
    }
    return TRUE;
}
