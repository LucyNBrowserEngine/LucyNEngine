# Changes from the original lucyn-nav upload

The uploaded `lucyn-nav.zip` had two problems that stopped
https://nothing-browser-docs.pages.dev/ from rendering correctly:

1. **ES modules were silently dropped.** `<script type="module" src="...">`
   was skipped (`<script type=module> skipped (not supported yet)` in
   `host.cpp`), AND `<script src>` with module-syntax code threw
   `SyntaxError: Unexpected token '{'` because the engine evaluated it as
   a classic script with `JS_EVAL_TYPE_GLOBAL`. VitePress's `app.D95lPkrg.js`
   starts with `import { ... }`, which is the import-statement that triggered
   the error.
2. **No CSS support.** `<style>` blocks and external
   `<link rel="stylesheet">` files were ignored — only inline `style=""`
   attributes were honoured. The site is a VitePress build; without its
   stylesheets it paints as a long plain-text dump.

This build fixes both. Source files changed (paths relative to the repo
root):

```
modules/js/src/impl.h              +evalModule, +moduleLoaderCb, +moduleNormalizeCb,
                                   +fetchScriptSource, +fetchAndRunModuleScript
modules/js/src/host.cpp            check type="module" BEFORE src; new
                                   State::evalModule; register module loader in
                                   QuickJsHost::attach
modules/js/src/fetch_bridge.cpp    new fetchScriptSource (the HTTP/file fetch
                                   half of fetchAndRunScript, factored out);
                                   new fetchAndRunModuleScript (evals with
                                   JS_EVAL_TYPE_MODULE); new moduleLoaderCb /
                                   moduleNormalizeCb (QuickJS callbacks that
                                   resolve relative URLs against the importing
                                   module and fetch them over HTTP)
modules/layout/include/lucyn/layout/css.h   NEW: Stylesheet + selector matcher
modules/layout/src/css.cpp                  NEW: implementation
modules/layout/include/lucyn/layout/layout.h  +layoutDocument(doc, opts, sheet)
modules/layout/src/layout.cpp     thread_local const Stylesheet* g_sheet;
                                   layoutDocument sets/clears it; styleForElement
                                   calls now pass g_sheet
modules/layout/CMakeLists.txt      +src/css.cpp
modules/controller/src/controller.cpp   +include layout/css.h; new
                                       std::unique_ptr<Stylesheet> stylesheet_;
                                       new buildStylesheetFor(doc, pageUrl) that
                                       walks <style> and <link rel=stylesheet>;
                                       called after a successful visit; the
                                       screenshot/window render paths now pass
                                       stylesheet_.get() to layoutDocument
modules/ui/CMakeLists.txt         SDL2::SDL2 link fix for the in-tree root build
                                 (IMPORTED_GLOBAL + PUBLIC link). Only needed if
                                 you rebuild from source — the shipped binary
                                 was already linked successfully.
```

## How the ES module fix works

`JS_SetModuleLoaderFunc(rt, moduleNormalizeCb, moduleLoaderCb, nullptr)` is
called once per `QuickJsHost::attach()`. The normalizer
(`moduleNormalizeCb`) uses the same URL resolver as `<a href>` / `fetch()` /
etc. (`shell::resolveUrl`) so relative imports like `./chunks/theme.js`
resolve against the importing module's URL. The loader
(`moduleLoaderCb`) fetches the resolved URL via the host's HTTP callback
(the same path `fetch()` and `<script src>` use — adblock and impersonation
apply) and compiles the source as a module with
`JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY`. QuickJS then
instantiates and evaluates it; for nested imports it calls the loader
recursively. `import.meta.url` is the module's URL by default (QuickJS
sets it from the module name we pass to `JS_Eval`).

`State::runScriptNode` now checks `type="module"` BEFORE checking `src`
(the previous order checked `src` first, which sent module scripts down
the classic-script path). A module script with a `src` calls the new
`fetchAndRunModuleScript`; an inline module script calls the new
`State::evalModule` (which is `evalGlobal` with `JS_EVAL_TYPE_MODULE`
instead of `JS_EVAL_TYPE_GLOBAL`).

## How the CSS fix works

The new `modules/layout/src/css.cpp` parses CSS text into a list of
`(compound selector chain, declarations)`. Supported selectors:

* `*`, `tag`, `.class`, `#id`, `tag.class#id` (compound)
* descendant combinator (whitespace); `>`, `+`, `~` degrade to descendant
* `[attr]`, `[attr=value]` (only the `=` operator)
* `:root` (matches `<html>`); other pseudo-classes/elements are dropped
  (the rule still matches, as if the pseudo wasn't there — what a browser
  does for unknown pseudo-classes)

CSS custom properties (`--foo`) on `:root` / `.dark` are collected into two
maps. `var(--name)` and `var(--name, fallback)` are recursively resolved
in `resolveVars`; the `.dark` variants win when the document's `<html>`
has `class="dark"` (VitePress's dark-mode toggle). `@media` / `@supports`
are flattened (always matching); `@font-face` / `@keyframes` / `@import` /
`@charset` / `@namespace` are skipped — the controller fetches top-level
stylesheets itself, so `@import`-ed stylesheets inside `<style>` blocks
are not chased.

The controller (`buildStylesheetFor` in `controller.cpp`) walks the live
document's `<style>` blocks and `<link rel="stylesheet" href="...">` URLs,
fetches the latter via the HTTP client (`dest: style` — same path as a
real browser stylesheet fetch), and feeds everything into one
`Stylesheet`. The stylesheet is rebuilt on every visit and passed to
`layoutDocument` for `screenshot` and `window`. `styleForElement` now
applies the cascade: UA defaults (`[hidden]`), then matched stylesheet
rules (sorted by specificity and source order, `!important` in a second
pass), then inline `style=""` last.

## What still doesn't render perfectly

* **Flexbox / grid.** VitePress's `.VPFeatures` is `display: grid; grid-
  template-columns: repeat(2, 1fr)`. The layout module renders `display:
  grid` as a plain block stack, so the feature cards appear as a vertical
  list instead of a 2xN grid. Implementing flex/grid is a substantial layout
  rewrite and was out of scope here.
* **Pseudo-classes other than `:root`.** `:hover`, `:nth-child`, `:where`,
  `:is`, etc. are dropped from the selector — the rule matches as if they
  weren't there. (Most of these don't affect the static render anyway.)
* **`%` / `em` / `rem` / `calc()` lengths.** Layout still only understands
  `px` (and bare `0`). Many VitePress style rules use `padding: 1em 1.5em`
  etc.; those declarations are ignored. Adding `em`/`rem` resolution
  (against the element's inherited `font-size`) is the next obvious step
  if you want pixel-perfect output.
* **Background images, box-shadow, border-radius, opacity, transforms,
  transitions.** Layout/paint don't know about them; the declarations are
  parsed (kept in the cascade) but have no effect on the output.

Even with those limits, the VitePress docs site renders with its real
navigation, hero, dark feature cards with green borders, the pink alert
box, and the footer — a far cry from the unstyled text dump that came out
of the original upload.
