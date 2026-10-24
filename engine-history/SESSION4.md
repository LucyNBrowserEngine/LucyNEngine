# Session 4: the `js` module (QuickJS + DOM bridge)

`shell::ScriptHost` now has a real implementation: `lucyn::js::QuickJsHost`. `lucyn-shell` uses it, so scripts run and `before != after`
for the first time. Read `modules/js/README.md` for the design rules and the exact implemented / not-implemented lists.

## What changed
- **New:** `modules/js/` (host, DOM bridge, window bridge, JS prelude, tests), `third_party/quickjs/` (QuickJS-ng v0.17.0, vendored).
- **Root CMake:** option `LUCYN_MODULE_JS`; `js` is added after `shell` + `profile`; `lucyn-shell` is linked to it **at the root** (js depends on
  shell, so shell cannot link js itself) with `LUCYN_SHELL_HAVE_JS`. Without js the tool falls back to `NullHost`.
- **`shell/src/hydrator.cpp`:** `shell` (the Document) is now declared *before* the `Cleanup` guard. Before, on every error/exception path the
  Document was destroyed before `host_.detach()` ran. Regression test added to `test_shell` (verified to fail with the old ordering).
- **`shell/src/assets.cpp`:** the generated `inject.js`, `hydrator.js` and `example.js` no longer say "the JS module doesn't exist".
  They now state what is true (the C++ host already runs your scripts in order; the shell's `<script src>` files are not executed yet).
- **`lucyn-shell`:** `--console` (page console to stderr); prints `note:` and `page error:` lines; the persona comes from `IdentityManager`.

## Decisions taken (all overridable)
QuickJS-ng over Bellard's (maintained, CMake, Windows). `lucyn` is a closure parameter (top-level `var`s in a script do not leak to the page).
A failed hook throws a JS `Error`. Real clock for timers. Idle = no pending one-shot timers (intervals/rAF never block). The mutation
listener is made inert with a shared token instead of being unregistered, because the Document may already be gone.

## Verified (sandbox: Ubuntu 24, gcc 13, QuickJS-ng 0.17.0)
- 8/8 ctest suites, **zero compiler warnings** (`-Wall -Wextra`).
- `test_js`, `test_shell`, `test_dom`, `test_dom_mutate` clean under **ASan + UBSan + leak detection**; QuickJS's debug asserts on leaked JS
  objects at teardown are active in that build and never fired. Covered: Document destroyed before `detach()`, Document that outlives the host and
  keeps mutating, attach twice, infinite loop, runaway microtasks, out-of-memory, stack overflow.
- End to end with the real `lucyn-shell` binary on a local page: site script ran, its timer fired in `settle`, `load` handler ran, a start-script
  `MutationObserver` saw the mount, two user scripts ran, `navigator.userAgent` came from the real `IdentityManager` persona.

## NOT verified (do not claim otherwise)
- **No real website was ever run.** The sandbox blocks everything but github/pypi/npm. Only synthetic pages. Expect real sites to hit the
  not-implemented list first (`fetch`, `<script src>`, selectors, fragments).
- No detection page (CreepJS etc.), no `navigator`/`Function.prototype.toString` comparison against a real Chrome. The native-looking
  masking is tested only against our own expectations of Chrome's format.
- The Chrome-shaped `toString` format `function name() { [native code] }` is from memory. So are the `Event`/`MouseEvent` default fields.
- `innerHeight = availHeight - 85` is invented.
- Not built or run on Windows/macOS (the vendored CMake has Windows branches, unexercised).

## Lessons from the Nothing Browser list (HANDOFF section 6), status
Done: `webdriver === false`; `deviceMemory` capped at 8 and only when the Surface says it exists; `userAgentData` derived from the Surface (not
hardcoded); `navigator`, `languages`, `userAgentData`, `brands` cached (`===` holds); no leaked globals (`lucyn` and the private native object are
not on `window`); the page console is silent unless a callback is given. **Still open (nothing exists yet to apply them to):** deterministic
idempotent canvas/audio noise, WebGL strings, `plugins`/`mimeTypes`/`connection` caching.

## Suggested next steps
1. Use a real site run and the newly preserved JS stack traces to identify missing browser APIs. Rivestream's earlier `TypeError` came from the missing `Node.isEqualNode()` call in Next.js head updates; that method is now implemented and tested.
2. CacheStorage, `Request`/`Response`, `crypto.getRandomValues`/`randomUUID`, and a no-layout `IntersectionObserver` compatibility subset were added after this session. They are intentionally partial; fetch is still GET-only and CacheStorage is in-memory.
3. Next likely compatibility work: investigate current Rivestream `pageErrors()` and improve the DOM/API named by the stack; add POST and fetch options only when a tested site use case requires them.
4. **Selectors in `dom`**: `:not`, `:first-child/:last-child/:nth-child`, `[a^=b]`, `[a$=b]`, `[a*=b]`. Real scripts hit this fast.
5. `DocumentFragment`/`<template>`, then storage refinements and broader real-site tests. `localStorage`/`sessionStorage` already exist as per-origin storage under `nothing-data`.
