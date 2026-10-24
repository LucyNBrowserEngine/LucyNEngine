# Session 3 (7 Oct 2026): mutable DOM, shell, injection, snapshots, nothing-data

## What the author asked for
1. Every site opens inside a **shell page** (`inject.js` + `hydrator.js`), the way Vite/React hydrate, so injecting JS is easy and a user can inject **20+ scripts per page**.
2. Scripts run **before or after** the page (`@run-at start|end`).
3. A way to **save the page before and after** and see **which tags are new**.
4. JS (QuickJS) comes **next session**: leave hooks to expose.
5. Data **always** goes to `nothing-data/` at the root of the running binary.

## What was built
| Module | New / changed |
|---|---|
| `core` (new) | `exeDir()`, `dataRoot()`, `dataDir(sub)`. Rejects `..` and absolute paths. |
| `profile` | `IdentityStore::defaultDir()` is now `<binary>/nothing-data/identities`. `lucyn-profile identity --name/--regenerate` without `--dir` saves there. Depends on `core`. |
| `dom` | Now **mutable** (create/append/insert/replace/remove/clone/import, `setAttr`, `setTextContent`, `setInnerHTML`), non-const `Node*` overloads, `version()` + `onMutation()`, iterative destructor/clone, `diffHtml/formatDiff`, `lucyn-dom --diff`. |
| `shell` (new) | `Hydrator`, `mountSite`, `ScriptRegistry` (`@name @run-at @match @priority`), `Snapshots`, URL/favicon helpers, `NativeHooks`, `ScriptHost` + `NullHost`, shell assets, `lucyn-shell`. |
| root | `LUCYN_MODULE_CORE`, `LUCYN_MODULE_SHELL`; all binaries go to `build/bin` (one shared `nothing-data`). |

Verified: 7 ctest suites pass in the normal build, under ASan+UBSan, and each module builds and tests standalone.
Not verified: anything against a live site (sandbox network is blocked); `lucyn-shell <url>` path with real curl was compiled but only `--file` was run.

## Decisions to keep
- **Merge, not `<div>` mount.** The site's `<head>`/`<html>`/`<body>` merge into the shell's, so `document.body`, favicon, title and `body.dark` selectors work. Shell nodes are tracked by pointer, never by attribute.
- **`start` scripts run before the site is mounted** (true document-start), `end` after idle.
- **Failing script never stops the others**; a bad header rejects only that script, loudly.
- **Hooks never throw across the boundary.** Hooks are removed after each run (they point into its stack).
- **Nodes are never freed on removal** (detached pool), so JS references stay valid. Call `purgeDetached()` only when nothing points at them.
- `before` = raw server bytes, `after` = live page minus shell scaffolding; both compared as parsed trees. `before`/`after` labels are reserved.

## THE NEXT SESSION: JS module (`modules/js`)
1. Vendor QuickJS into `third_party/quickjs`, build it as a static lib.
2. Implement `shell::ScriptHost` (see `modules/shell/include/lucyn/shell/host.h`). Order of calls: `attach` -> `runUserScript` (start) -> mount -> `runPageScripts` -> `settle` -> `runUserScript` (end) -> `detach`.
3. DOM bridge on `dom::Document` (priority list in README "QuickJS and the DOM Bridge"). Use `Document::onMutation` for MutationObserver and `version()` for idle detection. Cache wrapper objects (`el === el`).
4. Expose `NativeHooks` as `lucyn.*` **only to injected scripts**: not a page global, not enumerable, no console output.
5. Fill in the real `inject.js` / `hydrator.js` in `modules/shell/src/assets.cpp` (they are placeholders now).
6. Make `lucyn-shell` use the real host instead of `NullHost`; the existing `FakeHost` tests in `tests/test_shell.cpp` describe the expected call order.

## Known gaps
- `parseHtml` and `outerHTML` still recurse (100k-deep pages could overflow the stack); build/clone/free are iterative.
- No `<script src>` / stylesheet / iframe fetching yet.
- `lucyn-shell` for real URLs untested end to end here (no network).
- Test file `test_dom_mutate` covers mutation heavily; selectors still limited to the subset in `dom.h`.
