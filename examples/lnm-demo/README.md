# LNM search and open demo

From the repository root, build LucyN and launch the demo in the visible SDL
window:

```sh
cmake -S . -B build
cmake --build build -j
build/bin/lucyn --config examples/lnm-demo/engine.json --show
```

At the prompt, enter a search command:

```json
{"v":1,"id":1,"cmd":"search","query":"LucyN browser engine"}
```

Or open a website directly:

```json
{"v":1,"id":2,"cmd":"visit","url":"https://example.org"}
```

The demo config loads `shell.lnm` and sets Brave as the default search
provider. You can click the search field in the SDL window, type a query, and
press Enter; the results page replaces the demo screen. The engine also accepts
JSON commands in the terminal and replies with the page status, title, and URL.
Enter `{"v":1,"id":3,"cmd":"quit"}` to close LucyN. Search and website
loading require a network connection and a desktop SDL display.

`run.js` is an optional example of driving the same engine through the Node.js
client, including its `search()` and `open()` methods.

Other built-in search providers include Google, DuckDuckGo, Bing, Ecosia,
Startpage, Mojeek, Qwant, Swisscows, and SearXNG. SearXNG requires an instance
URL in `engine.json` under `search.searxng_url`.
