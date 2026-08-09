#pragma once

// The six values that differ between plugins in this family. The page is served
// from https://<asset_host>/<page>, and the folder the previewed file lives in
// from https://<file_host>/ - mapping the folder rather than the file is what
// lets a document resolve its siblings. The page is navigated to as
//
//   https://<asset_host>/<page>?theme=dark|light&src=https%3A%2F%2F<file_host>%2F<name>
//
// and reports its result by setting document.title to "ok: …" or "err: …".
struct lister_plugin {
    const wchar_t *window_class;
    const wchar_t *data_dir;
    const wchar_t *asset_host;
    const wchar_t *file_host;
    const wchar_t *page;
    const char *detect;
};

extern const lister_plugin lister_config;
