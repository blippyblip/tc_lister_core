// Shared host for Total Commander Lister plugins that preview a file by pointing
// a WebView2 control at a local page. Everything in lister_webview.cpp is
// format-agnostic; a plugin supplies this struct and nothing else.
//
// The page is served from https://<assetHost>/<page>, and the folder the previewed
// file lives in from https://<fileHost>/ (mapping the folder, not the file, is what
// lets a document resolve its siblings - a .gltf its .bin, a .md its images).
// It is navigated to as:
//
//   https://<assetHost>/<page>?theme=dark|light&src=https%3A%2F%2F<fileHost>%2F<name>
#pragma once

struct ListerPlugin {
    const wchar_t *windowClass; // must be unique per plugin DLL
    const wchar_t *dataDir;     // folder under %LOCALAPPDATA% for the WebView2 profile and log
    const wchar_t *assetHost;   // virtual host for the plugin's own web\ folder
    const wchar_t *fileHost;    // virtual host for the previewed file's folder
    const wchar_t *page;        // page to navigate to, relative to assetHost
    const char *detect;         // Lister detect string, e.g. "EXT=\"MD\""
};

// Defined by the plugin, consumed by lister_webview.cpp.
extern const ListerPlugin ListerConfig;
