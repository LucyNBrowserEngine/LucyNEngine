# Session 17 - pages can navigate (JS challenges finish), noscript is not rendered, curl-impersonate only from third_party/

## Symptom (from the author's desktop)
A search endpoint (Mojeek) rendered its bot-challenge page ("JavaScript is required to complete this challenge...") and stayed
there; every other search endpoint was "breaking" too. QuickJS was in the engine but looked unused.

## Root cause
QuickJS DID run the page scripts. What it could not do was act on them:
- `location.href = ...`, `location.assign/replace/reload()` were hard-wired to "navigation ignored (a hydrated page cannot
  navigate)". A JS challenge solves something, sets a cookie and calls `location.reload()`: the reload was dropped.
- there was no `form.submit()` / `requestSubmit()`, and `el.click()` had no default action (links, submit buttons).
- gumbo has no scripting flag, so `<noscript>` content was parsed as live markup (a `<noscript>` in `<head>` holding text even
  spills it into `<body>`), which is a way for "JavaScript is required..." to get painted on a page whose scripts ran.
- `third_party/curl-impersonate/` did not exist in the tree and the CMake silently fell back to STOCK libcurl (curl's own TLS
  fingerprint -> bot challenges / 429). Before that the library lived outside the repo (`LUCYN_CURL_IMPERSONATE_ROOT`, SESSION2).

## Changes
- js: `n.navigate(url, method, body, contentType)` native + `State::navPending/nav`; `QuickJsHost::takeNavigation()` (newest
  request wins; taking clears it). `settle()` returns at once and `runPageScripts()` stops once a navigation is pending.
  Prelude: location setters/`assign`/`replace`/`reload`, `form.submit()`, `form.requestSubmit()` (fires a cancelable `submit`
  event, `submit()` does not), `el.click()` default action (link -> navigate; `javascript:` / `download` skipped; submit
  button -> its form, submitter name/value included). Fragment-only changes are not navigations. GET forms replace the action's
  query; POST forms are urlencoded (multipart is sent urlencoded and says so in a note). Non-http(s) targets are noted, not followed.
- shell: Hydrator strips `<noscript>` content before parsing whenever a script host is available (comments, script/style/
  textarea/title bodies are skipped, so a "<noscript>" inside a JS string is untouched; if the engine then fails to attach the
  page is re-parsed unstripped). `beforeHtml` stays the fetched source.
- controller: `cmdVisit` -> `loadPageFollowing()`: loads a page, then follows what it asked for (script navigation, then
  `<meta http-equiv=refresh>` to a different page within 10 s) up to 8 loads, same cookie jar, Referer (strict-origin-when-
  cross-origin) and, for POST, Origin + Content-Type. `click` and `eval` follow navigations too (`followPageNavigation()`).
  Each hop emits a `note` event ("page navigated: GET https://...").
- net: `modules/net/CMakeLists.txt` looks for libcurl-impersonate ONLY in `third_party/curl-impersonate/` (headers + library),
  no `-D` override, no stock-libcurl fallback, no system search. Missing => FATAL_ERROR with the download steps; a library without
  `curl_easy_impersonate` => FATAL_ERROR. The folder is added to the rpath. Stale cached results are unset on every configure.
  Messages in `http_client.cpp` / `lucyn-fetch` / `third_party/README.md` updated; `third_party/curl-impersonate/README.md` added.

## Verification
Sandbox without cmake/gumbo/libcurl: `js` and `shell` tests built with a throw-away stand-in HTML parser + real QuickJS + real
DOM/Hydrator: `test_js` (incl. new navigation, noscript and leave-early cases) and `test_shell` pass. `controller.cpp` passes
`-fsyntax-only` (the only diagnostic, `readLineWithWindowEvents`, is identical on the untouched tree: feature defines missing).

## Not verified
- The CMake change (never configured), the controller hop loop at run time, the real gumbo path of the noscript strip, any live site.
- Whether Mojeek's challenge then completes: needs the author's run. If not, `--console` shows the `note`/`pageError` events
  that say which API the challenge script trips on (`Worker`, `crypto.subtle` are the first suspects).

## Known limits (unchanged)
`<script type=module>` is skipped (JS-only engines stay broken), `<style>` is not parsed (pages look raw), multipart/file
form navigation is not built, `window.open` is not wired.

## After pulling this
`rm -rf build`, unpack libcurl-impersonate into `third_party/curl-impersonate/`, reconfigure.
