<!-- Copy this file to profile/README.md in the public LucyNBrowserEngine/.github repository. -->

<p align="center">
  <img src="https://github.com/LucyNBrowserEngine/LucyNEngine/blob/main/drawing.png?raw=true" alt="LucyN Engine — stealth web scraping and automation" width="100%">
</p>

# LucyN Browser Engine

LucyN is an experimental, modular browser engine written in C++17 for browser automation, web scraping, network capture, and lightweight rendering. Its engine is split into focused libraries so modules can be built, tested, and improved independently.

**Status: pre-alpha and actively changing.** LucyN is not a standards-complete replacement for a mainstream browser. Check module documentation for what is implemented and what remains limited.

## Projects in LucyN

The main repository, [LucyNEngine](https://github.com/LucyNBrowserEngine/LucyNEngine), is a monorepo containing the engine, modules, command-line tools, and JavaScript client.

| Project / module | Role |
|---|---|
| [`apps/lucyn`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/apps/lucyn) | Main executable. Reads JSON commands over stdin/stdout for navigation, extraction, evaluation, downloads, and capture controls. |
| [`modules/controller`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/controller) | Connects app commands to the engine modules and manages a browser session. |
| [`modules/core`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/core) | Shared paths and runtime data under `nothing-data/`. |
| [`modules/profile`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/profile) | Browser persona selection and consistent device identities. |
| [`modules/net`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/net) | Moonbug capture wiretap (the libcurl HTTP client left with the CEF migration). |
| [`modules/cef_host`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/cef_host) | Chromium Embedded Framework host: navigation, page JS, cookies, offscreen rendering. |
| [`modules/adblock`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/adblock) | C++ network filter parser and blocker, connected to app requests. |
| [`modules/security`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/security) | NULLBYTE HMAC-SHA256 integrity seals for captured data. |
| [`modules/server`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/server) | Loopback-only capture streams, local server APIs, and the embedded dashboard. |
| [`modules/logger`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/logger) | Dependency-free C++ engine logging. |
| [`client`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/client) | Node.js 18+ client that starts LucyN and communicates with its JSON command protocol. |
| [`dashboard`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/dashboard) | Local UI for viewing Moonbug network captures. |

## How the engine fits together

```text
apps/lucyn or client
        ↓
controller → profile + net/adblock → Moonbug capture
        ↓
shell → QuickJS + DOM → layout → paint → offscreen framebuffer (PNG screenshots only)
                         security/server expose signed local captures
```

## Build

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Some modules need system dependencies: Gumbo for HTML parsing and libcurl (the curl-impersonate build) for networking. There is no windowing dependency — the engine is headless. See the root [README](https://github.com/LucyNBrowserEngine/LucyNEngine/blob/main/README.md) and each module's README for setup and limitations.

## Contributing

LucyN welcomes focused contributions that explain the user problem and the module responsible for solving it. See [CONTRIBUTING.md](https://github.com/LucyNBrowserEngine/LucyNEngine/blob/main/CONTRIBUTING.md), the [AI contribution policy](https://github.com/LucyNBrowserEngine/LucyNEngine/blob/main/LLM.md), and the issue templates. Feature requests ask which module should change so cross-module work can be planned clearly.

## License

LucyN-authored code is MIT-licensed. Some dependencies have separate terms; in particular, the default full app includes Mongoose, which is GPL-2.0-only or commercially licensed. Read [the license](https://github.com/LucyNBrowserEngine/LucyNEngine/blob/main/LICENSE) and [third-party notices](https://github.com/LucyNBrowserEngine/LucyNEngine/blob/main/THIRD_PARTY_NOTICES.md) before distributing an app built with LucyN.
