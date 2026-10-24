# Building LucyN

## Prerequisites

| Need | Notes |
|---|---|
| C++17 compiler | gcc 12+, clang 14+, MSVC 2022. The `lucyn` binary itself is compiled as **C++20** (CEF 156 headers require it); the static modules stay on C++17 |
| CMake ≥ 3.16 | developed against 3.31 |
| C and C enabled at configure | CMake probes C flags when CEF's own `cef_variables.cmake` is included |
| **CEF binary distribution** | unpacked at `third_party/cef/` — Chromium supplies HTTP, TLS, JS and rendering. ~1.5 GB unpacked, not in git |
| python3 | reads the CEF build index (scripts) and runs the loopback test servers |
| Node ≥ 18 | only for the `client/` package |

No libcurl, no libgumbo, no QuickJS, no SDL, no display server — nothing that opens a window.

## 1. Get CEF

```bash
./scripts/install.sh        # detects your OS/arch, asks, downloads + unpacks into third_party/cef/
```

Already have a distro there? Skip this. Want to check for a newer one without touching anything:

```bash
./scripts/update_cef.sh --check
```

See [install.sh](../scripts/install.sh) / [update_cef.sh](../scripts/update_cef.sh) for
`--platform`, `--version`, `--yes` and `--check`. The build looks **only** at `third_party/cef/` —
`cmake/FindCEF.cmake` does not search `/usr` and takes no `-D` path.

## 2. Configure and build

```bash
cmake -S . -B build
cmake --build build -j
```

Everything lands in `build/bin/`, where the CEF runtime files (`libcef.so`, `icudtl.dat`, `*.pak`,
`locales/`) are copied next to the binaries, so `build/bin/` is self-contained and portable as a
folder.

## 3. Test

```bash
ctest --test-dir build --output-on-failure
```

Eight suites: `core, search, security, moonbug, server, cef_host, controller, cli_download`.
`cli_download` copies the real `lucyn` binary into a temp directory and drives it against a
loopback server, so it also proves the binary still runs when moved.

## CMake options

| Option | Default | Effect |
|---|---|---|
| `LUCYN_WITH_CEF` | ON | Find/link CEF. **OFF** builds a binary that still speaks the JSON protocol but answers every engine command with `built without CEF` |
| `LUCYN_MODULE_CEF_HOST` | ON | the CEF host module (no-op unless `LUCYN_WITH_CEF`) |
| `LUCYN_MODULE_CONTROLLER` | ON | JSON command loop + the `lucyn` binary (needs core, net, security, server) |
| `LUCYN_MODULE_CORE` | ON | `nothing-data` paths — everything the engine stores lives there |
| `LUCYN_MODULE_NET` | ON | Moonbug, the capture wiretap (self-contained) |
| `LUCYN_MODULE_SECURITY` | ON | NULLBYTE (HMAC-SHA256 seals) |
| `LUCYN_MODULE_SERVER` | ON | TinyElysia on vendored Mongoose + the capture dashboard |
| `LUCYN_MODULE_SEARCH` | ON | search provider registry |
| `LUCYN_MODULE_LOGGER` | ON | engine diagnostics logging |
| `LUCYN_BUILD_TESTS` | ON | build the test executables and register them with CTest |
| `LUCYN_BUILD_TOOLS` | ON | build the small CLI tools (`lucyn-search`, `lucyn-cef-smoke`, …) |

`modules/adblock` is wired unconditionally (its filter rules are copied to
`build/bin/adblock-filters/`).

```bash
# a smaller build, no CEF, tests off
cmake -S . -B build-min -DLUCYN_WITH_CEF=OFF -DLUCYN_BUILD_TESTS=OFF
```

## One module at a time

Every module still configures on its own and pulls in the siblings it needs:

```bash
cmake -S modules/net -B build-net && cmake --build build-net -j && ctest --test-dir build-net
cmake -S modules/security -B build-security && cmake --build build-security -j
```

The root only wires modules; each `modules/<name>/CMakeLists.txt` works standalone and exports the
alias `lucyn::<name>`.

## Scripts

| Script | What it does |
|---|---|
| `scripts/install.sh` | detect OS/arch → confirm or pick a platform → download the CEF *standard* distro from `https://cef-builds.spotifycdn.com/` → sha1-verify → unpack into `third_party/cef/` → print the installed version |
| `scripts/update_cef.sh` | read the installed version from `third_party/cef/include/cef_version.h`, fetch the newest stable build for your platform, compare, and install it when newer (`--check` never downloads) |

Both print what they found, what is installed and what they did.

## Troubleshooting

**`Invalid file descriptor to ICU data`** — CEF could not find `icudtl.dat` next to the loaded
`libcef.so`. Never move a binary away from the runtime files in `build/bin/`; re-run the build so
`lucyn_cef_copy_runtime()` puts them back. `initCef()` also embeds the distro's absolute
`Resources/` path as a fallback, so re-run CMake after moving `third_party/cef/`.

**`CEF binary distribution not found at: ...`** — `third_party/cef/` is missing or incomplete;
run `./scripts/install.sh`.

**C++20 errors from CEF headers** — the `lucyn` target sets `CXX_STANDARD 20` itself; a custom
build overriding that globally to C++17 will fail.

**A stale binary in `build/bin/`** — CMake never deletes outputs when you remove a target. If a
tool disappears from the build, delete its old binary from `build/bin/` (or start a fresh build
directory).
