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
| [`modules/net`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/net) | libcurl-based HTTP client, cookies, request metadata, and Moonbug capture hooks. |
| [`modules/adblock`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/adblock) | C++ network filter parser and blocker, connected to app requests. |
| [`modules/dom`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/dom) | HTML parsing into a mutable DOM, selectors, and before/after comparison. |
| [`modules/shell`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/shell) | Page hydration, injected user scripts, lifecycle hooks, and snapshots. |
| [`modules/js`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/js) | QuickJS runtime and DOM bridge for page and user scripts. |
| [`modules/layout`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/layout) | Block and inline layout that produces display-list commands. |
| [`modules/paint`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/paint) | Software painter, framebuffer, text, image, and PNG output. |
| [`modules/ui`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/ui) | Optional SDL2 window support, including hidden-window Ghostful Mode. |
| [`modules/security`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/security) | NULLBYTE HMAC-SHA256 integrity seals for captured data. |
| [`modules/server`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/server) | Loopback-only capture streams, local server APIs, and the embedded dashboard. |
| [`modules/logger`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/modules/logger) | Dependency-free C++ engine logging. The planned LNM JavaScript logger is not part of the current runtime. |
| [`client`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/client) | Node.js 18+ client that starts LucyN and communicates with its JSON command protocol. |
| [`dashboard`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/dashboard) | Local UI for viewing Moonbug network captures. |
| [`lnminterpreter`](https://github.com/LucyNBrowserEngine/LucyNEngine/tree/main/lnminterpreter) | Reserved for the proposed LNM interpreter; currently not implemented. |

## How the engine fits together

```text
apps/lucyn or client
        ↓
controller → profile + net/adblock → Moonbug capture
        ↓
shell → QuickJS + DOM → layout → paint → optional SDL window
                         security/server expose signed local captures
```

## Build

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Some modules need system dependencies: Gumbo for HTML parsing, libcurl for networking, and optionally SDL2 for windows. See the root [README](https://github.com/LucyNBrowserEngine/LucyNEngine/blob/main/README.md) and each module's README for setup and limitations.

## Contributing

LucyN welcomes focused contributions that explain the user problem and the module responsible for solving it. See [CONTRIBUTING.md](https://github.com/LucyNBrowserEngine/LucyNEngine/blob/main/CONTRIBUTING.md), the [AI contribution policy](https://github.com/LucyNBrowserEngine/LucyNEngine/blob/main/LLM.md), and the issue templates. Feature requests ask which module should change so cross-module work can be planned clearly.

## License

LucyN-authored code is MIT-licensed. Some dependencies have separate terms; in particular, the default full app includes Mongoose, which is GPL-2.0-only or commercially licensed. Read [the license](https://github.com/LucyNBrowserEngine/LucyNEngine/blob/main/LICENSE) and [third-party notices](https://github.com/LucyNBrowserEngine/LucyNEngine/blob/main/THIRD_PARTY_NOTICES.md) before distributing an app built with LucyN.
