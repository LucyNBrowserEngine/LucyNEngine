# third_party/

Everything LucyN carries with it. Source-vendored libraries (quickjs, gumbo, stb, mongoose, imgui)
go here as plain folders. The one prebuilt binary dependency is below.

## curl-impersonate  (put it in `third_party/curl-impersonate/`)

Gives LucyN real Chrome / Safari / Firefox TLS + HTTP/2 handshakes. **This folder is the only place LucyN looks.** CMake does not
search `/usr`, does not take a `-D` path and has no stock-libcurl fallback: if the files below are missing, `cmake` stops with an
error that prints these steps. (A silent fallback to stock libcurl gave the engine curl's own TLS fingerprint, and sites answered
it with bot challenges.) The build also bakes this folder into the binaries' rpath, so the loader uses this copy at run time.

```bash
cd third_party && mkdir -p curl-impersonate && cd curl-impersonate
# pick the file for your CPU (`uname -m`): x86_64-linux-gnu | aarch64-linux-gnu | riscv64-linux-gnu | arm64-macos | x86_64-macos | x86_64-win32
curl -LO https://github.com/lexiforest/curl-impersonate/releases/download/v2.2.3/libcurl-impersonate-v2.2.3.x86_64-linux-gnu.tar.gz
tar xzf libcurl-impersonate-v2.2.3.x86_64-linux-gnu.tar.gz && rm libcurl-impersonate-v2.2.3.x86_64-linux-gnu.tar.gz
```

Expected layout (flat, exactly as the tarball unpacks): `libcurl-impersonate.so`, `libcurl-impersonate.a`,
`include/curl/*.h`, and the LICENSE_* files. Keep the license files: they cover the bundled BoringSSL, nghttp2, etc.

Pinned version: **v2.2.3**. Upstream: https://github.com/lexiforest/curl-impersonate

## quickjs  (vendored in `third_party/quickjs/`, nothing to download)

QuickJS-ng **v0.17.0**, MIT. Only the files needed for the static library are kept; see `third_party/quickjs/VENDORED.txt`.
It is plain C: the `js` module turns the C language on for it. To upgrade, download the new tag from
`https://codeload.github.com/quickjs-ng/quickjs/tar.gz/refs/tags/<tag>` (github.com is reachable from the sandbox, `bellard.org` is not),
copy the same files, bump the version in `VENDORED.txt`, and run the `js` suite under ASan/UBSan (its teardown asserts on leaked JS objects).
Do not edit the vendored sources.

## mongoose  (vendored in `third_party/mongoose/`, nothing to download)

Mongoose **7.23**, used by `modules/server` (TinyElysia). **License: GPL-2.0-only OR a paid commercial license** from Cesanta.
Linking it makes the combined binary GPL-2.0 unless you buy the commercial license: decide before distributing binaries.
Built with `MG_ENABLE_LOG=0` (its default logger prints to stdout, which is LucyN's protocol channel). Do not edit the
vendored files; upgrade steps are in `third_party/mongoose/VENDORED.txt`.
