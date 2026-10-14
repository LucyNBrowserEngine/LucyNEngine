# third_party/

Third-party source we vendor (QuickJS, stb, Mongoose, and similar components)
goes here as plain folders. Gumbo is a system dependency rather than vendored
source. curl-impersonate is a separately supplied binary dependency.

License summary and the distribution implications of Mongoose are in the repository root's
`THIRD_PARTY_NOTICES.md`. Each vendored component's own license files remain authoritative.

## curl-impersonate  (put it in `third_party/curl-impersonate/`)

Gives LucyN real Chrome / Safari / Firefox TLS + HTTP/2 handshakes. CMake finds it by itself when it is here.

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
