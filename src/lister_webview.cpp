// Total Commander Lister plugin host: previews a file by hosting WebView2 and
// pointing it at a local page. Shared by every plugin in this family; see
// lister_webview.h for the per-plugin configuration.
//
// Threat model: the file being previewed is untrusted. It is parsed entirely
// inside the WebView2 sandbox, it may not reach the network (see the page's CSP
// and the navigation/download handlers below), and nothing it produces is
// formatted into a fixed-size buffer on this side.
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

static HINSTANCE g_inst;

// The two origins, built once from the plugin's host names so the navigation
// filter below can stay captureless.
static const std::wstring &AssetOrigin() {
    static std::wstring s = L"https://" + std::wstring(ListerConfig.assetHost) + L"/";
    return s;
}
static const std::wstring &FileOrigin() {
    static std::wstring s = L"https://" + std::wstring(ListerConfig.fileHost) + L"/";
    return s;
}

// HWND values are recycled, so an async WebView2 callback cannot trust that the
// handle it captured is still one of ours. Only the UI thread touches this.
static std::set<HWND> g_live;

struct View {
    ComPtr<ICoreWebView2Controller> controller;
};

static std::wstring DirOf(const std::wstring &path) {
    size_t i = path.find_last_of(L"\\/");
    return i == std::wstring::npos ? std::wstring() : path.substr(0, i);
}

static std::wstring PluginDir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(g_inst, buf.data(), (DWORD)buf.size());
        if (n == 0) return std::wstring();
        if (n < buf.size()) { buf.resize(n); break; }
        buf.resize(buf.size() * 2); // path longer than MAX_PATH
    }
    return DirOf(buf);
}

static std::wstring UserDataDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring d = std::wstring(buf) + L"\\" + ListerConfig.dataDir;
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

// There is no console inside Lister, so failures go to a log next to the user
// data. StringCch* truncates rather than overflowing: some of what lands here
// (page titles, error text) originates in the file being previewed.
static void Log(const wchar_t *fmt, ...) {
    wchar_t line[1024];
    va_list ap;
    va_start(ap, fmt);
    HRESULT hr = StringCchVPrintfW(line, ARRAYSIZE(line), fmt, ap);
    va_end(ap);
    if (FAILED(hr) && hr != STRSAFE_E_INSUFFICIENT_BUFFER) return;

    std::wstring dir = UserDataDir();
    if (dir.empty()) return;
    HANDLE f = CreateFileW((dir + L"\\log.txt").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;

    // Keep the log from growing without bound across sessions.
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(f, &size) && size.QuadPart > 256 * 1024) {
        CloseHandle(f);
        f = CreateFileW((dir + L"\\log.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
    }

    char utf8[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof utf8 - 2, nullptr, nullptr);
    if (n > 1) {
        utf8[n - 1] = '\r';
        utf8[n] = '\n';
        DWORD written;
        WriteFile(f, utf8, n + 1, &written, nullptr);
    }
    CloseHandle(f);
}

// Percent-encode a file name as UTF-8 so it survives inside a URL query.
static std::wstring UrlEncode(const std::wstring &s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::wstring();
    std::string utf8(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), utf8.data(), n, nullptr, nullptr);

    std::wstring out;
    out.reserve(utf8.size());
    for (unsigned char c : utf8) {
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

static bool StartsWith(const std::wstring &s, const std::wstring &prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

static std::wstring CommanderIni() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"COMMANDER_INI", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;
    n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::wstring(buf) + L"\\GHISLER\\wincmd.ini";
    return std::wstring();
}

static bool SystemPrefersDark() {
    DWORD light = 1, size = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

// Lister's ShowFlags, per listplug.h. A real TC 11 session in dark mode sends
// 0x1C4 = lcp_ansi | lcp_center | lcp_darkmode | lcp_darkmodenative.
#define lcp_darkmode 128

// Match whatever Total Commander is showing next to us. The flag is the sanctioned
// route; the ini is only consulted when it is clear, so an older TC that never sets
// the bit still gets the right theme.
static bool UseDarkTheme(int showFlags) {
    static bool logged = false;
    if (!logged) { Log(L"ShowFlags 0x%08X", showFlags); logged = true; }
    if (showFlags & lcp_darkmode) return true;

    std::wstring ini = CommanderIni();
    UINT mode = ini.empty() ? 0 : GetPrivateProfileIntW(L"Configuration", L"DarkMode", 0, ini.c_str());
    return mode == 1 || (mode == 2 && SystemPrefersDark());
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    View *v = (View *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
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

// Nothing in these viewers legitimately leaves the two virtual hosts. A crafted
// file can reference external URIs (glTF image/buffer uri, FBX texture paths,
// Markdown image and link targets); without this it would phone home the moment
// the file is previewed.
static void ContainNavigation(ICoreWebView2 *web) {
    EventRegistrationToken tok;
    web->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *a) -> HRESULT {
                LPWSTR uri = nullptr;
                if (SUCCEEDED(a->get_Uri(&uri)) && uri) {
                    std::wstring u = uri;
                    CoTaskMemFree(uri);
                    if ((StartsWith(u, L"http://") || StartsWith(u, L"https://")) &&
                        !StartsWith(u, AssetOrigin()) && !StartsWith(u, FileOrigin())) {
                        a->put_Cancel(TRUE);
                        Log(L"blocked navigation to %s", u.c_str());
                    }
                }
                return S_OK;
            })
            .Get(),
        &tok);

    web->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *a) -> HRESULT {
                a->put_Handled(TRUE); // swallow popups instead of opening a window
                return S_OK;
            })
            .Get(),
        &tok);

    ComPtr<ICoreWebView2_4> web4;
    if (SUCCEEDED(web->QueryInterface(IID_PPV_ARGS(&web4)))) {
        web4->add_DownloadStarting(
            Callback<ICoreWebView2DownloadStartingEventHandler>(
                [](ICoreWebView2 *, ICoreWebView2DownloadStartingEventArgs *a) -> HRESULT {
                    a->put_Cancel(TRUE);
                    return S_OK;
                })
                .Get(),
            &tok);
    }
}

static void HardenSettings(ICoreWebView2 *web) {
    ComPtr<ICoreWebView2Settings> s;
    if (FAILED(web->get_Settings(&s)) || !s) return;

    s->put_AreDefaultContextMenusEnabled(FALSE);
    s->put_IsStatusBarEnabled(FALSE);
    s->put_AreDevToolsEnabled(FALSE);
    s->put_IsWebMessageEnabled(FALSE);   // the page never posts to the host
    s->put_AreHostObjectsAllowed(FALSE); // and the host exposes no objects
    s->put_IsZoomControlEnabled(FALSE);  // the page decides what Ctrl+wheel means

    ComPtr<ICoreWebView2Settings3> s3;
    if (SUCCEEDED(s.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);

    ComPtr<ICoreWebView2Settings4> s4;
    if (SUCCEEDED(s.As(&s4))) {
        s4->put_IsGeneralAutofillEnabled(FALSE);
        s4->put_IsPasswordAutosaveEnabled(FALSE);
    }
}

// Returns false when the WebView2 runtime cannot start at all, so ListLoadW can
// decline the file and let Lister fall through to another viewer.
static bool StartWebView(HWND hwnd, const std::wstring &file, bool dark) {
    std::wstring folder = DirOf(file);
    std::wstring name = folder.empty() ? file : file.substr(folder.size() + 1);
    if (folder.empty() || name.empty()) {
        Log(L"cannot split path %s", file.c_str());
        return false;
    }
    std::wstring assets = PluginDir() + L"\\web";
    std::wstring url = AssetOrigin() + ListerConfig.page + L"?theme=" +
                       (dark ? L"dark" : L"light") + L"&src=" +
                       UrlEncode(FileOrigin() + name);

    HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, UserDataDir().c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd, folder, assets, url](HRESULT hr, ICoreWebView2Environment *env) -> HRESULT {
                if (FAILED(hr) || !env) { Log(L"environment creation 0x%08X", hr); return S_OK; }
                if (!g_live.count(hwnd)) return S_OK;

                env->CreateCoreWebView2Controller(
                    hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd, folder, assets, url](HRESULT hr, ICoreWebView2Controller *ctrl) -> HRESULT {
                            if (FAILED(hr) || !ctrl) { Log(L"controller creation 0x%08X", hr); return S_OK; }
                            if (!g_live.count(hwnd)) { ctrl->Close(); return S_OK; }

                            View *v = (View *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
                            if (!v) { ctrl->Close(); return S_OK; }
                            v->controller = ctrl;

                            RECT r;
                            GetClientRect(hwnd, &r);
                            ctrl->put_Bounds(r);

                            ComPtr<ICoreWebView2> web;
                            ctrl->get_CoreWebView2(&web);
                            if (!web) return S_OK;

                            // Serve the viewer and the file's folder as two virtual hosts. The
                            // file host allows cross-origin so the viewer page may fetch it;
                            // mapping the folder is what lets a document find its siblings.
                            ComPtr<ICoreWebView2_3> web3;
                            if (SUCCEEDED(web.As(&web3))) {
                                web3->SetVirtualHostNameToFolderMapping(
                                    ListerConfig.assetHost, assets.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                                web3->SetVirtualHostNameToFolderMapping(
                                    ListerConfig.fileHost, folder.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            }

                            HardenSettings(web.Get());
                            ContainNavigation(web.Get());

                            // WebView2 eats keystrokes once the page has focus, so Escape has
                            // to be handed back to Lister explicitly or the window never closes.
                            EventRegistrationToken keyTok;
                            ctrl->add_AcceleratorKeyPressed(
                                Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                                    [hwnd](ICoreWebView2Controller *, ICoreWebView2AcceleratorKeyPressedEventArgs *a) -> HRESULT {
                                        COREWEBVIEW2_KEY_EVENT_KIND kind;
                                        UINT key = 0;
                                        a->get_KeyEventKind(&kind);
                                        a->get_VirtualKey(&key);
                                        bool down = kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
                                                    kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN;
                                        if (down && key == VK_ESCAPE && g_live.count(hwnd)) {
                                            a->put_Handled(TRUE);
                                            PostMessageW(GetParent(hwnd), WM_KEYDOWN, VK_ESCAPE, 0);
                                        }
                                        return S_OK;
                                    })
                                    .Get(),
                                &keyTok);

                            // The page reports its load result via the document title; mirror it
                            // onto our window so a test host can read it, and log failures.
                            EventRegistrationToken titleTok;
                            web->add_DocumentTitleChanged(
                                Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                                    [hwnd](ICoreWebView2 *sender, IUnknown *) -> HRESULT {
                                        LPWSTR title = nullptr;
                                        if (SUCCEEDED(sender->get_DocumentTitle(&title)) && title) {
                                            if (g_live.count(hwnd)) SetWindowTextW(hwnd, title);
                                            if (wcsncmp(title, L"err:", 4) == 0) Log(L"%s", title);
                                            CoTaskMemFree(title);
                                        }
                                        return S_OK;
                                    })
                                    .Get(),
                                &titleTok);

                            // A malformed file can take the renderer or GPU process down;
                            // without this the pane just goes blank with no explanation.
                            EventRegistrationToken failTok;
                            web->add_ProcessFailed(
                                Callback<ICoreWebView2ProcessFailedEventHandler>(
                                    [](ICoreWebView2 *, ICoreWebView2ProcessFailedEventArgs *a) -> HRESULT {
                                        COREWEBVIEW2_PROCESS_FAILED_KIND kind;
                                        a->get_ProcessFailedKind(&kind);
                                        Log(L"WebView2 process failed, kind %d", (int)kind);
                                        return S_OK;
                                    })
                                    .Get(),
                                &failTok);

                            EventRegistrationToken navTok;
                            web->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *a) -> HRESULT {
                                        BOOL ok = FALSE;
                                        COREWEBVIEW2_WEB_ERROR_STATUS st = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                        a->get_IsSuccess(&ok);
                                        a->get_WebErrorStatus(&st);
                                        if (!ok) Log(L"navigation failed, status %d", (int)st);
                                        return S_OK;
                                    })
                                    .Get(),
                                &navTok);

                            HRESULT nav = web->Navigate(url.c_str());
                            if (FAILED(nav)) Log(L"Navigate 0x%08X", nav);
                            return S_OK;
                        })
                        .Get());
                return S_OK;
            })
            .Get());

    if (FAILED(started)) {
        Log(L"CreateCoreWebView2Environment 0x%08X (runtime missing?)", started);
        return false;
    }
    return true;
}

extern "C" {

void __stdcall ListGetDetectString(char *DetectString, int maxlen) {
    lstrcpynA(DetectString, ListerConfig.detect, maxlen);
}

// Exceptions must not cross back into Total Commander, so the body is wrapped;
// returning NULL just makes Lister fall through to the next viewer.
HWND __stdcall ListLoadW(HWND ParentWin, WCHAR *FileToLoad, int ShowFlags) try {
    if (!ParentWin || !FileToLoad || !*FileToLoad) return nullptr;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof wc};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = ListerConfig.windowClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
        registered = true;
    }

    RECT r;
    GetClientRect(ParentWin, &r);
    HWND hwnd = CreateWindowExW(0, ListerConfig.windowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                0, 0, r.right, r.bottom, ParentWin, nullptr, g_inst, nullptr);
    if (!hwnd) return nullptr;

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) new View());
    g_live.insert(hwnd);
    if (!StartWebView(hwnd, FileToLoad, UseDarkTheme(ShowFlags))) {
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
        // Unloaded while the process lives on: the class would otherwise point at
        // a WndProc that is no longer mapped.
        UnregisterClassW(ListerConfig.windowClass, inst);
    }
    return TRUE;
}
