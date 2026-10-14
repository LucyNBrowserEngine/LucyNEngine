# lucyn::shell

"Open a site INSIDE LucyN's own page, let the user's scripts touch it, and keep a before/after copy."
Needs `lucyn::dom` and `lucyn::core`. Builds and tests on its own. **No JS engine in here**: that is the next module, and it plugs in through one seam (`host.h`).

```
cmake -S modules/shell -B build && cmake --build build && ctest --test-dir build
./build/lucyn-shell --init                      # writes shell.html, inject.js, hydrator.js into nothing-data/
./build/lucyn-shell --file page.html            # treat a local file as "the site"
./build/lucyn-shell https://example.com         # (root build only: needs lucyn::net)
./build/lucyn-shell --list-scripts
```

## The idea

Every site opens inside a **shell page**, the way a Vite/Next app boots from an almost empty `index.html`:

```html
<script src="./inject.js"></script>      <!-- user scripts, any number of them, ordered by @run-at -->
<script src="./hydrator.js"></script>    <!-- loads the site into this page -->
```

Because LucyN owns the shell, injecting JS is trivial: the user's code runs before, during and after the site's own.

```
fetch site  ->  start scripts  ->  mount site into shell  ->  site's own scripts  ->  wait for idle  ->  end scripts
   |                                                                                                        |
 "before" snapshot (raw bytes)                                                                    "after" snapshot + diff
```

## Mounting (why favicon, title and `<body>` still work)

The site is merged into the shell's real `<html>/<head>/<body>`, **not** dropped in a `<div>`:

| Site part | Goes to |
|---|---|
| `<head>` children (`<title>`, `<link rel=icon>`, `<meta>`, styles, scripts) | appended to the shell `<head>`; the site's `<title>` replaces any shell title |
| `<html>` and `<body>` attributes | copied onto the shell's (so `body.dark`, `html[lang]` selectors work) |
| `<body>` children | appended to the shell `<body>` (after anything the shell put there) |

A `<div id=root>` mount would lose all of the above and break any script that reads `document.body` or `document.head`.
`faviconUrl()` finds the icon (`rel~=icon`, then `apple-touch-icon`, then `<origin>/favicon.ico`, relative to `<base href>`).

The shell's own nodes are tracked **by pointer, never by an attribute**, so nothing in the page can see them. They are left out of the "after" snapshot.

## User scripts

One `.js` file per script, in `nothing-data/scripts/`. Optional header:

```js
// ==LucyScript==
// @name      hide-banner
// @run-at    start          // start = before the site's scripts (site not mounted yet) | end = after the page went idle
// @match     https://*.example.com/*     // repeatable; default = every page.  * = anything, ? = one character
// @priority  -5             // lower runs first; default 0
// ==/LucyScript==
```

Order on a page: `start` scripts (priority, then order added) -> page -> `end` scripts (same rule). A script that throws is reported and **the others still run**.
A bad header (unknown `@run-at`, non-numeric `@priority`, duplicate `@name`) rejects that one script with a clear message; the rest load.

## Snapshots and diff

```
nothing-data/snapshots/<site>/<YYYYmmdd-HHMMSS>/before.html   exactly what the server sent
                                                after.html    the live page once everything ran (shell scaffolding left out)
                                                diff.txt      what changed
                                                <label>.html  anything a script saved with snapshot.take
```

```
lucyn-dom --diff before.html after.html
elements: 7 -> 8
new tags: canvas
tag counts:
  + canvas  (0 -> 1)
elements:
  + canvas  (0 -> 1)
```

Compares **tags and element signatures** (`div#app.big.card`, classes sorted). Text-only and class-order changes are not changes.
With `NullHost` (a build without the `js` module) nothing runs, so before == after, and `lucyn-shell` says so instead of pretending.
(That is also a test: mounting a page without JS must change nothing.) With `lucyn::js::QuickJsHost` the scripts run and the diff is real.

## C++ API (`namespace lucyn::shell`)

| Piece | What it does |
|---|---|
| `Hydrator(fetch, scripts, host)` | `hydrate(url)` runs the whole pipeline. Never throws; failures come back in `HydrateResult`. |
| `HydrateResult` | `ok`, `error`, `url` (after redirects), `title`, `faviconUrl`, `scripts` (ran / failed / skipped + why), `warnings`, `log`, `beforeHtml`, `afterHtml`, `diff`, `snapshotDir`, `document` (the live shell). |
| `PhaseHooks` | Optional callbacks: `onFetchStart`, `onFetched`, `onStartScripts`, `onMounted`, `onSettled`, `onEndScripts`, `onDone`. |
| `ScriptRegistry` | `add`, `loadDir`, `remove`, `forUrl(url, RunAt)`. |
| `Snapshots` | Labelled copies, memory + disk, `diff(a, b)`. Labels: letters, digits, `-`, `_`. Max 64 per run. |
| `mountSite(shell, site)` | The merge described above. |
| `resolveUrl`, `originOf`, `hostKey`, `faviconUrl` | Small URL helpers. |
| `ensureShellFiles()` / `loadShellHtml()` | Writes missing files into `nothing-data/shell/` (never overwrites); a user's own `shell.html` wins over the built-in. |

```cpp
shell::ScriptRegistry scripts;  scripts.loadDir(shell::ScriptRegistry::defaultDir());
shell::NullHost host;                                    // or lucyn::js::QuickJsHost (modules/js): the real engine
shell::Hydrator hy(myFetcher, scripts, host);
auto r = hy.hydrate("https://example.com");
// r.title, r.faviconUrl, r.diff.newTags(), r.snapshotDir ...
```

## For the JS session: the seam (`host.h`)

Implement `ScriptHost` with QuickJS and the DOM bridge. The Hydrator calls, in this order:

| Call | Meaning |
|---|---|
| `available()` | `true` once there is an engine. While `false` the Hydrator reports every matched script as *skipped: no JS engine*. |
| `attach(shell, url, hooks, err)` | New context for this page. Bind `document`/`window` to `shell` (a `dom::Document`, already mutable). |
| `runUserScript(script)` | Once per matching script. `start` ones run **before** the site is mounted (empty body), `end` ones after. |
| `runPageScripts()` | The site's own `<script>` elements, in document order (the DOM is already mounted). |
| `settle(timeoutMs)` | Pump timers/fetches; `true` = idle, `false` = timed out (reported as a warning). |
| `detach()` | Page over. |

**`NativeHooks`**: C++ functions for injected scripts. `lucyn::js` exposes them as `lucyn.*` (a parameter of each script, never a global):

| Hook | Args | Returns |
|---|---|---|
| `page.url` | | final URL |
| `log` | any | (collected into `HydrateResult::log`) |
| `snapshot.take` | label | saves the live page (`before` and `after` are reserved) |
| `snapshot.get` | label | that snapshot's HTML |
| `snapshot.labels` | | labels, one per line |
| `snapshot.diff` | labelA, labelB | the diff report |

`hooks.names()` lists them; add your own with `hy.native().add(name, fn)` before `hydrate()`. Hooks never throw across the boundary (`call()` returns `{ok=false, message}`).
Expose them as `lucyn.page.url()`, `lucyn.snapshot.take(...)` etc. **only inside injected scripts**: never a page-visible global, never an enumerable property, never a console line (see HANDOFF "lessons").

The DOM already gives the bridge what it needs: `Document::version()` (changed since I looked?), `onMutation(...)` (MutationObserver), `createElement/Text/Comment`, `importNode`, `appendChild/insertBefore/replaceChild/removeChild`, `setAttr`, `setInnerHTML`, `cloneNode`. Removed nodes are kept alive in the Document's detached pool so a JS reference never dangles.

## What is NOT here yet
- Any JS execution (next session). Today `lucyn-shell` shows scripts as *skipped*.
- `inject.js` / `hydrator.js` are placeholders that say so. Mounting is done in C++ for now.
- Redirect/cookie handling beyond what `net` does; `<base>` is honoured only for the favicon.
- `<script src>` fetching, `<link rel=stylesheet>` fetching, iframes.
