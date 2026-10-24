# third_party/

What LucyN carries with it. Source-vendored libraries (mongoose) live here as plain folders; the one
prebuilt binary dependency is CEF.

## cef  (put the CEF binary distribution in `third_party/cef/`)

LucyN's browser engine **is** Chromium, driven through the Chromium Embedded Framework: CEF owns HTTP, TLS,
page JS (V8), cookies and rendering. **This folder is the only place CMake looks.** `cmake/FindCEF.cmake` does
not search `/usr` and takes no `-D` path; if the distribution is missing it stops with an error that says exactly
this.

Download a *Standard Distribution* (linux64) from https://cef-builds.spotifyopen.com/index.html and unpack it so
the folder looks like this — the first two paths are what `FindCEF.cmake` probes for:

```
third_party/cef/
  cmake/cef_variables.cmake        libcef_dll/CMakeLists.txt        include/
  Release/libcef.so                libcef_dll/                      Resources/{icudtl.dat,*.pak,locales/}
```

The build links `libcef_dll_wrapper`, sets `-Wl,-rpath,$ORIGIN`, and `lucyn_cef_copy_runtime()` copies `Release/`
and `Resources/` next to the built binaries, so `build/bin/` is self-contained afterwards. `initCef()` also embeds
the distro's absolute `Resources/` path as a fallback: CEF aborts with `Invalid file descriptor to ICU data` if it
cannot find `icudtl.dat` from the loaded `libcef.so`, so never move a binary away from those files.

CEF is **not** committed to git — it is ~1.5 GB unpacked. Its own licence (BSD-3-Clause) covers the Chromium code
in the distribution; keep the `LICENSE.txt` that ships with it if you redistribute the folder.

## mongoose  (vendored in `third_party/mongoose/`, nothing to download)

Mongoose **7.23**, used by `modules/server` (TinyElysia). **License: GPL-2.0-only OR a paid commercial license** from Cesanta.
Linking it makes the combined binary GPL-2.0 unless you buy the commercial license: decide before distributing binaries.
Built with `MG_ENABLE_LOG=0` (its default logger prints to stdout, which is LucyN's protocol channel). Do not edit the
vendored files; upgrade steps are in `third_party/mongoose/VENDORED.txt`.

---

**Removed in Session 19** (with the CEF migration — do not re-add without a reason):

- `curl-impersonate/` — Chromium's network stack is the HTTP client now, so the impersonating libcurl build went away.
- `quickjs/` — page JS runs in Chromium's V8.
- `litehtml/` (with its bundled Gumbo), `stb/`, `fonts/` — Chromium lays out, paints, shapes text and renders; the
  hand-written layout/paint pipeline and its vendored helpers are gone.
