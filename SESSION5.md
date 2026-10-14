# Session 5: M4 — JSON command loop + `nothing-browser` JS client

## What was built

**M4 is the "front door"**: the `lucyn` binary that accepts newline-delimited JSON
commands over stdio, and the `nothing-browser` Node.js client that spawns it.

### New files
```
modules/controller/
  include/lucyn/controller/controller.h   — public API (ControllerOptions, Controller)
  src/controller.cpp                      — command loop, DOM helpers, JS host wiring
  tests/test_controller.cpp               — 15 unit tests (all in-memory, no network)
  CMakeLists.txt

apps/lucyn/
  main.cpp                                — CLI entry point (--profile, --settle, --console, --version)
  CMakeLists.txt

client/
  package.json                            — "nothing-browser" npm package (ESM, Node >=18)
  src/index.js                            — LucynSession: spawn, protocol, EventEmitter API
  README.md
```

### Root CMake changes
- Added `LUCYN_MODULE_CONTROLLER` option (ON by default when all deps present).
- `add_subdirectory(modules/controller)` and `add_subdirectory(apps/lucyn)` wired in.
- `lucyn` binary lands in `build/bin/` alongside the other tools.

## Verified (Ubuntu 24, gcc 13, zero warnings)
- **9/9 ctest suites pass** (core, profile, identity, net, dom, dom_mutate, shell, js, controller).
- `lucyn --version` → `lucyn 0.4.0-m4`
- `lucyn --help` → usage printed
- Manual smoke test: `info`, `profile`, `html` (no page), `quit` all reply correctly.
- Profile switch to `safari184` fails cleanly with stock libcurl (correct: impersonation needs curl-impersonate).

## Protocol (M4 commands)

| Command | Args | Reply |
|---|---|---|
| `visit` | `url`, optional `profile` | `{ok, status, url, title}` |
| `html` | — | `{ok, value: html}` |
| `extract` | `selector` | `{ok, value: text}` |
| `extractAll` | `selector` | `{ok, values: [text...]}` |
| `eval` | `js` | `{ok:false, error: "not in M4"}` (M5+) |
| `profile` | `name` | `{ok, value: resolvedName}` |
| `info` | — | `{ok, profile, url, impersonating}` |
| `quit` | — | `{ok}` |

Events (unprompted):
- `{"event":"pageError","message":"..."}` — uncaught JS errors from the page
- `{"event":"note","message":"..."}` — things LucyN couldn't do (e.g. `<script src>` blocked)

## `nothing-browser` JS client

```js
import Lucyn from "nothing-browser";

const a = new Lucyn({ profile: "chrome", settleTimeoutMs: 5000 });
const { status, title } = await a.visit("https://example.com");
const h1   = await a.extract("h1");
const links = await a.extractAll("a");
const html  = await a.html();
await a.profile("safari184");
const info = await a.info();
await a.close();
```

Binary resolution order:
1. `LUCYN_BIN` env var
2. `../../build/bin/lucyn` (monorepo default)
3. `lucyn` on PATH

## NOT verified
- `visit` on a real URL (sandbox blocks external HTTP; run on your machine).
- The `nothing-browser` JS client end-to-end (requires Node ≥ 18 and a built `lucyn` binary).
- Windows build (controller and app are standard C++17; should work, untested).

## Known M4 limitations / M5 work
- **`eval` not implemented**: the QuickJS context is torn down after `settle()`. M5+ can keep
  it alive between commands or re-enter via a second hydrate of a `javascript:` URI.
- **`waitFor` / `waitIdle`**: not in M4. Settle happens automatically inside `visit()`.
- **Cookies**: not persisted between visits (M8 per the roadmap).
- **Moonbug**: not hooked yet. M5 adds capture hooks to `net` and the TinyElysia dashboard.

## Suggested next steps
1. Author: run `lucyn` on their machine, do a real `visit`, confirm `extract`/`html` work.
2. M5: add Moonbug capture hooks in `modules/net/`, then `modules/server/` (TinyElysia on Mongoose).
3. Continue M3 compatibility work (POST/XHR, more selectors) as real sites reveal gaps.
