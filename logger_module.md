# Lucy N. Logger Module

---

## Overview

The Lucy N. engine uses a two-layer logging system — one for the C++ engine side, one for the QuickJS JS side. Both layers mirror the style, color, and emoji conventions of **ernest-logger** so that dev terminal output is unified and readable regardless of which layer produced the message.

Ernest-logger is an Ernest Tech House npm library. Its output style is the reference for all Lucy N. terminal output.

---

## Layer 1 — Engine Logger (C++ Side)

Handles all output produced by the engine itself: LNM compilation, asset loading, event wiring, build errors, runtime warnings.

### Output Format

Mirrors ernest-logger exactly — colored output with emoji prefixes, ANSI codes for terminal color.

### Log Levels

| Level | Emoji | Color | When Used |
|---|---|---|---|
| `INFO` | ℹ️ | Cyan | General engine messages |
| `SUCCESS` | ✅ | Green | Successful operations |
| `WARN` | ⚠️ | Yellow | Non-fatal issues (prod build unregistered functions etc.) |
| `ERROR` | ❌ | Red | Recoverable errors |
| `FATAL` | 💀 | Bright Red | Fatal condition; the caller controls shutdown |
| `BUILD` | 🔧 | Blue | Compile-time messages from `lnmc` |
| `LNM` | 🧩 | Magenta | LNM-specific events and wiring messages |

### Example Output

```
[LNM] 🧩 Compiling settings.lnm...
[LNM] ✅ settings.lnm compiled successfully.
[LNM] ❌ Build failed.
      → runjs: setTheme() — not registered in settings.js
      → Register it with LNM.register({ setTheme }) and rebuild.
[LNM] ⚠️  logout() not registered — #logout-btn will not respond as expected.
[BUILD] 🔧 Minifying settings.html...
[BUILD] 🔧 Running uglify-js on settings.js...
[BUILD] ✅ Assets embedded.
```

---

## Layer 2 — JS Logger (QuickJS Side)

The engine injects a logger into every QuickJS context automatically. Every `.js` file loaded via `grab.js()` has access to it without importing anything.

### API

Available on the `LNM.log` global:

```js
LNM.log.info("Login handler called")
LNM.log.success("User authenticated")
LNM.log.warn("Token expiring soon")
LNM.log.error("Auth request failed")
LNM.log.debug("Payload: " + JSON.stringify(data))
```

### Log Levels (JS Side)

| Method | Emoji | When Used |
|---|---|---|
| `LNM.log.info()` | ℹ️ | General messages |
| `LNM.log.success()` | ✅ | Successful JS operations |
| `LNM.log.warn()` | ⚠️ | Non-critical issues |
| `LNM.log.error()` | ❌ | Errors from JS logic |
| `LNM.log.debug()` | 🐞 | Debug output (dev build only) |

Debug output is suppressed in production builds automatically.

---

## Dev vs Prod Behavior

| Behavior | Dev Build | Prod Build |
|---|---|---|
| `LNM.log.debug()` | Visible | Suppressed |
| Unregistered `runjs:` | Fatal — app does not launch | Warn — element does nothing |
| Engine INFO messages | Visible | Suppressed |
| WARN / ERROR | Visible | Visible |
| FATAL | Visible | Visible |

---

## Source

The C++ engine logger implementation lives in `modules/logger/` (`include/lucyn/logger/logger.h` and `src/logger.cpp`) and is built by the root CMake project as `lucyn::logger`. It is a standalone module; engine call sites can link it without adding third-party dependencies.

The QuickJS `LNM.log` layer described above is still a planned integration: the current runtime does not implement LNM, and its existing page console remains a separate API.

ernest-logger: [github.com/Ernest12287/ernest-logger](https://github.com/Ernest12287/ernest-logger)

When LNM is implemented, its JS-side logger can be a lite port of ernest-logger without Node.js dependencies (file logging, `require`), injected into the LNM QuickJS context at startup.

---

*Lucy N. Logger Module — Ernest Tech House*
