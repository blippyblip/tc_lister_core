# tc-lister-core

The parts shared by my Total Commander Lister plugins that preview a file by
hosting a WebView2 control and pointing it at a local page:

- [src/lister_webview.cpp](src/lister_webview.cpp) — the whole `.wlx64`: Lister's
  exports, WebView2 hosting, the two virtual hosts, Total Commander's dark-mode
  setting, network containment, and the log. Format-agnostic.
- [src/lister_webview.h](src/lister_webview.h) — the six values a plugin supplies.
- [build.cmd](build.cmd) — builds any plugin in the family.
- [test/host.cpp](test/host.cpp) — a ~70-line stand-in for Lister.
- [devserver.js](devserver.js) — serves the staged viewer to a normal browser.

Used as a submodule at `core/` by
[gltf-lister](https://github.com/blippyblip/gltf_viewer) and
[md-lister](https://github.com/blippyblip/md_viewer).

## Writing a plugin against it

`src/plugin.cpp` is the entire C++ side:

```cpp
#include "lister_webview.h"

const lister_plugin lister_config = {
    L"MdListerWnd",       // window class, unique per DLL
    L"md_wlx",            // %LOCALAPPDATA% folder for the WebView2 profile and log
    L"md-assets.invalid", // virtual host for the plugin's web\ folder
    L"md-file.invalid",   // virtual host for the previewed file's folder
    L"viewer.html",
    "EXT=\"MD\" | EXT=\"MARKDOWN\"",
};
```

The rest of the plugin is `res/` (the page), `src/<name>_wlx.rc`,
`src/pluginst.inf`, and a `fetch-deps.ps1` that calls `core\fetch-wv2.ps1` and
then downloads whatever the page needs into `vendor\<lib>\`. Every `vendor\`
folder except `wv2` is copied into the installed plugin's `web\`, so the page can
reach them as siblings.

The page is loaded as
`https://<assetHost>/<page>?theme=dark|light&src=<url of the file>` and reports
its result by setting `document.title` to `ok: …` or `err: …`, which the host
mirrors onto its window for `test/host.cpp` to read.

## Contract with the page

- Nothing may leave the two virtual hosts. The host cancels cross-origin
  navigations, popups and downloads; the page is expected to carry a CSP that
  confines fetches the same way.
- The page must never inject file-derived text as markup, and must cap what it
  puts in the title (it lands in a fixed buffer on the C++ side, which truncates,
  but the cap keeps the log readable).
- <kbd>Esc</kbd> is handed back to Lister by the host, so the page must not rely
  on it.

## License

MIT — see [LICENSE.md](LICENSE.md).
