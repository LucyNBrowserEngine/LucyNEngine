# LucyN

**A scraping-first browser engine, powered by Chromium.**

LucyN is a headless browser engine you drive with newline-delimited JSON on stdin/stdout. There is
no CDP, no remote debugging port, no automation flag and no window — the page has nothing to look
at and nothing to detect. Chromium does the browsing (HTTP, TLS, JavaScript, cookies, rendering);
LucyN does the driving: navigate, wait, fill, click, extract, capture every request on the way.

```json
{"v":1,"id":1,"cmd":"visit","url":"https://example.com"}
{"id":1,"ok":true,"status":200,"url":"https://example.com","title":"Example Domain"}
```

- **Scraping first.** `extract`/`extractAll`/`html`/`eval` read the settled DOM; `waitFor` and
  `waitIdle` handle pages that load their data late.
- **Chromium underneath.** Real navigation, real form submission, real cookies, real CSS layout —
  no reimplementation to leak through.
- **Nothing to detect.** One process, stdio only. No port is open, no `navigator.webdriver`-style
  instrumentation, no headless/user-agent faking of our own: the browser is the browser.
- **Everything is captured.** [Moonbug](docs/moonbug.md) records every request the engine makes,
  with headers in wire order, timings and an HMAC seal you can verify.

## Install

You need a C++17 compiler, CMake ≥ 3.16, python3, and the CEF (Chromium) binary distribution —
about 1.5 GB, downloaded for you.

```bash
git clone <this repo> && cd Lucynengine

./scripts/install.sh      # detects your OS/arch, asks you to confirm, unpacks CEF into third_party/cef/

cmake -S . -B build
cmake --build build -j
```

## Platform support

| Platform | Status |
|----------|--------|
| Linux x64 | ✅ Supported |
| Linux arm64 | ✅ Supported |
| Windows x64 | 🚧 Builds are currently erroring. We are investigating the cause; Windows is not supported for now. |
| macOS | ⏳ Planned |

The install script can fetch CEF for additional platforms, but that does not mean LucyN itself is
supported on them. To check for a newer CEF later: `./scripts/update_cef.sh --check`.

Verify the build:

```bash
ctest --test-dir build --output-on-failure     # 8 suites
```

Everything lands in `build/bin/` next to the CEF runtime files, so that folder is self-contained.
More detail — including every CMake option — is in [docs/building.md](docs/building.md).

## The JSON command protocol

One JSON object per line in, one JSON object per line out. Every reply echoes the `id` you sent;
errors come back as `{"id":N,"ok":false,"error":"..."}`.

| Command | Arguments | Reply |
|---|---|---|
| `visit` | `url` | `{status, url, title}` |
| `search` | `query`, `engine?` | `{status, url, title}` — opens a real results page |
| `html` | — | `{value}` — fully settled HTML |
| `extract` | `selector` | `{value}` — text of the first match |
| `extractAll` | `selector` | `{values:[...]}` — every match |
| `eval` | `js` | `{value}` — evaluated in the live page |
| `waitFor` | `selector`, `timeout?` | `{value}` — polls the live DOM |
| `waitIdle` | `quiet?`, `timeout?` | `{ok}` — network quiet **and** in-flight requests done |
| `click` | `selector` | `{value}` — a real `.click()` in the page |
| `fill` | `selector`, `value` | `{tag}` — sets the value, fires `input` + `change` |
| `submit` | `selector` | `{tag}` — submits the form and follows the navigation |
| `screenshot` | `name?`, `width?` | `{path}` — Chromium's offscreen frame as PNG |
| `download` | `url`, `name?`, `overwrite?`, `maxBytes?`, `referer?` | `{path, name, bytes, status, type, url}` |
| `cookies` | `url?` | `{cookies:[...]}` — the jar, or what a request to `url` would carry |
| `setCookie` | `name`, `value`, `domain`\|`url`, ... | `{ok}` |
| `clearCookies` | — | `{ok}` |
| `info` | — | `{url, captures}` |
| `moonbug` | `raw?`, `signed?`, `dashboard?`, `on?` | `{events, raw, signed, dashboard}` |
| `captures` | `limit?` | `{total, captures:[...]}` |
| `quit` | — | `{ok}` — clean shutdown |

Run it straight from a shell:

```bash
printf '%s\n' '{"v":1,"id":1,"cmd":"visit","url":"https://example.com"}' \
              '{"v":1,"id":2,"cmd":"extract","selector":"h1"}' \
              '{"v":1,"id":3,"cmd":"quit"}' | ./build/bin/lucyn
```

Full argument lists, reply shapes, error strings and the unprompted event lines:
**[docs/commands.md](docs/commands.md)**.

One-shot download without the JSON loop:

```bash
./build/bin/lucyn --download https://example.com/file.zip     # prints the saved path, exits 0/1
```

## Node.js client

The `client/` package (**[`@lucyn/browser`](https://jsr.io/@lucyn/browser)**, Node 18+, ESM, no
dependencies) spawns the engine and turns every command into a promise.

```bash
npx jsr add @lucyn/browser
```

> **This package lives on JSR, not npm.** Its registry entry is
> [jsr.io/@lucyn/browser](https://jsr.io/@lucyn/browser). `npm i @lucyn/browser` does **not** work
> — npm cannot fetch it directly. `npx jsr add @lucyn/browser` is the correct install command: it
> downloads from JSR and wires up the npm-compatible import for you (JSR handles the registry
> translation automatically).

### Why JSR and not npm?

You might wonder why this isn't just `npm i`. Honest answer: publishing to npm wanted about three
tokens, and we only had one — figuring out how to get the other two was a bit of a chore. JSR was
the next and nearest easy one, so that's where the package lives. Everything else works the same:

```bash
npx jsr add @lucyn/browser    # correct
npm i @lucyn/browser          # does not work — the package is not on npm
```

So: don't use `npm i`, use `npx jsr add`. It does the npm-registry compatibility for you, so your
`import` statement looks exactly like a normal package afterwards.

```js
import Lucyn from "@lucyn/browser";

const a = new Lucyn();

const { status, title } = await a.visit("https://example.com");
await a.waitFor("#ready", { timeout: 10000 });
await a.fill("input[name=q]", "Ada");
await a.submit("form#search");
await a.waitIdle({ quiet: 300 });

const heading = await a.extract("h1");
const prices  = await a.extractAll(".price");
const html    = await a.html();

const png = await a.screenshot("results.png", { width: 1280 });

await a.close();
```

It finds the binary through `opts.binary`, `LUCYN_BIN`, `build/bin/lucyn`, or `PATH`. Every method,
option and event: **[docs/node-client.md](docs/node-client.md)**.

## Moonbug

Moonbug is the wiretap built into the engine. Because LucyN *is* the HTTP client, it sees
decrypted traffic natively — no MITM certificate, no proxy, no debug port.

```bash
./build/bin/lucyn --moonbug      # raw stream :2005, signed stream :2006, dashboard :6969
```

- **raw stream** (`ws /stream`) — one JSON capture per frame: method, URL, headers in wire order,
  timings, body preview.
- **signed stream** — the same captures wrapped in a NULLBYTE envelope:
  `{"v":1,"alg":"HMAC-SHA256","sig":"...","payload":"..."}`, verifiable at `POST /verify`.
- **dashboard** — a local page at `http://127.0.0.1:6969/` that shows the captures live and checks
  their seals.

All three bind `127.0.0.1` only. From JS:

```js
const a = new Lucyn({ moonbug: { dashboard: 6969 } });
a.moonbug.onRequest((r) => console.log(r.method, r.status, r.url, r.sig));
const recent = await a.moonbug.captures(20);
```

Everything about what a capture holds, the endpoints and the sealing: **[docs/moonbug.md](docs/moonbug.md)**.

## Repository layout

```
apps/lucyn/        the engine binary (JSON over stdio)
modules/           core, net (Moonbug), security, server, search, logger,
                   adblock, cef_host (Chromium host), controller (command loop)
client/            the @lucyn/browser Node.js package
dashboard/         capture dashboard assets
scripts/           install.sh, update_cef.sh
third_party/       cef/ (binary distro, downloaded) + mongoose (vendored)
docs/              commands, node client, moonbug, building
engine-history/    build history: SESSION*.md, HANDOFF.md, CHANGES.md, AGENTS.md, ORG_README.md
```

## Documentation

| Doc | |
|---|---|
| [docs/commands.md](docs/commands.md) | every JSON command, its arguments, reply shape and errors |
| [docs/node-client.md](docs/node-client.md) | the `@lucyn/browser` package, method by method |
| [docs/moonbug.md](docs/moonbug.md) | captures, the dashboard, NULLBYTE signing |
| [docs/building.md](docs/building.md) | prerequisites, CMake options, tests, troubleshooting |
| [CONTRIBUTING.md](CONTRIBUTING.md) | how to propose changes |
| [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) | licences of what ships with this |
| [LUCYN_ENGINE_API_SPEC.md](LUCYN_ENGINE_API_SPEC.md) | engine configuration, such as it is |

`engine-history/` is the running build log of how LucyN got here — useful context, not user
documentation.

## Licence

LucyN-authored code is MIT (see [LICENSE](LICENSE)). Note that linking `modules/server` pulls in
Mongoose, whose terms are **GPL-2.0-only or a paid commercial licence** — read
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) before you distribute a binary.
