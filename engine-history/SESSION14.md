# Session 14 — free search providers and JS client navigation

## What changed

- Added `modules/search/`, a standalone C++17 provider registry with URL encoding and a `lucyn-search` helper. Its built-ins are Brave, SearXNG, Google, DuckDuckGo, Bing, Ecosia, Startpage, Mojeek, Qwant, and Swisscows. New providers can be added/replaced with one `Registry::add({id, name, template})` entry.
- Added the controller JSON command `search` with `query` and optional `engine`. It builds the provider URL and visits that normal result page with LucyN. The engine config's `search.default` is used when no provider is supplied.
- Added SearXNG instance configuration through `search.searxng_url`. This is needed because SearXNG is software with separately hosted instances rather than one canonical search site.
- Added `open(url)` and `search(query, {engine})` to the Node client. `open()` shares the existing `visit()` pipeline. The client can pass a config file with `{ config: "./engine.json" }`.
- A successful `visit` now renders the visited document into an existing SDL window, so opening a URL/search after showing the window updates its content.
- Updated the engine API and client docs. This does not add paid search APIs, scrape search result content, or wire in-page `location.href` / LNM `runjs` navigation.

## Usage

```js
import Lucyn from "nothing-browser";

const browser = new Lucyn({ config: "./engine.json" });
await browser.search("LucyN browser", { engine: "brave" });
await browser.open("https://example.org");
await browser.close();
```

Pass `{ show: true }` to the client constructor to display the SDL window from startup; otherwise use `await browser.setVisible(true)`.

`lucyn-search list` prints the provider ids. Use `lucyn-search searxng <query> --searxng https://your-instance` to inspect a SearXNG URL without launching the engine.

## Verification

- `cmake -S . -B build && cmake --build build -j` succeeded after the changes.
- CTest was not run after these changes. The prior session's baseline test result is recorded in `SESSION13.md` and predates this work.
- Search result pages require network access to the chosen provider. SearXNG public instances can disappear or change; LucyN does not pick or endorse a public instance automatically.
