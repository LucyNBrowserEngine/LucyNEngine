# Lucy N. Engine — API Specification
### v0.2

---

## Overview

The Lucy N. engine exposes a C++ configuration API that controls everything the engine needs to know **before a page loads**. LucyN is headless: it has no window and no chrome. Page-facing behavior lives in the visited site itself; engine-facing behavior lives here.

---

## `engine.config` — Full API

### Identity

```cpp
engine.config.name       = "My Browser"
engine.config.iconpath   = "./assets/icon.png"
engine.config.motto      = "Truth over rendering."
```

| Field | Type | Description |
|---|---|---|
| `name` | string | Engine/app display name |
| `iconpath` | string | Path to icon image |
| `motto` | string | Tagline — queryable in homepage.html |

---

### UI & Asset Loading

```cpp
engine.config.homepage("./ui/index.html", "style.css", "animate.js")
engine.config.asset_mode = AssetMode::LOOSE_FILE    // dev
engine.config.asset_mode = AssetMode::BINARY_EMBED  // prod
engine.config.root_dir   = "./ui/"
```

| Field | Type | Description |
|---|---|---|
| `homepage()` | fn | Sets the default shell UI files |
| `asset_mode` | enum | `LOOSE_FILE` = hot-reload from disk, `BINARY_EMBED` = baked into binary |
| `root_dir` | string | Base path for loose UI files |

---

### Runtime config (the `lucyn` executable)

For the `lucyn` executable, runtime configuration is supplied as JSON with `--config`. The current executable reads `search.default` and `search.searxng_url`; other fields in the C++ embedding API remain independent of this file format.

```json
{
  "search": {
    "default": "brave",
    "searxng_url": "https://your-searxng-instance.example"
  }
}
```

There is no `lnm.entry`, no `window.*` section, and no `--show`/`--ghostful` flags: the LNM interpreter, the SDL window and all browser chrome were removed in Session 18 (see `engine-history/SESSION18.md`). The engine is headless by design — `screenshot` is its only rendering output.

---

### Search Engine

```cpp
engine.config.search.default     = "brave"
engine.config.search.searxng_url = "https://searx.example.com"  // optional instance
```

| Field | Type | Description |
|---|---|---|
| `default` | string | Default search engine |
| `searxng_url` | string | Custom SearXNG instance URL (only used when default = "searxng") |

The built-in providers are `brave`, `searxng`, `google`, `duckduckgo`, `bing`, `ecosia`, `startpage`, `mojeek`, `qwant`, and `swisscows`. This is a set of free-to-use web search pages, not a quality ranking. SearXNG is software with independently hosted instances, so configure `searxng_url` before selecting it.

Providers live in `lucyn::search::Registry`; adding or replacing one takes an id, display name, and URL template containing one `{query}` placeholder. The query is percent-encoded, so providers can be added without creating a new controller path.

The Node client can open a page with `await browser.open(url)` or search and load the results page with `await browser.search(query, { engine: "brave" })`. Omitting `engine` uses this config's `search.default`. These calls use normal LucyN navigation; they do not call a paid search API or scrape result data.

---

### HTTP

```cpp
engine.config.http.timeout    = 30       // seconds, default 30
engine.config.http.user_agent = "auto"   // auto = matches fingerprint_profile
engine.config.http.proxy      = ""       // optional — "http://host:port"
```

| Field | Type | Description |
|---|---|---|
| `timeout` | int | Request timeout in seconds |
| `user_agent` | string | `"auto"` mirrors the active fingerprint profile. Set manually to override |
| `proxy` | string | Optional proxy. Empty string = no proxy |

`"auto"` is the default and recommended value for `user_agent` — it keeps the user agent consistent with the fingerprint profile so the two never contradict each other.

---

### Google Authentication

```cpp
engine.config.auth.google.enabled       = true
engine.config.auth.google.client_id     = "YOUR_CLIENT_ID"
engine.config.auth.google.client_secret = "YOUR_CLIENT_SECRET"
engine.config.auth.google.redirect_port = 8742
```

| Field | Type | Description |
|---|---|---|
| `enabled` | bool | Enables Google Sign-In via `LNM.auth.google()` |
| `client_id` | string | OAuth2 client ID from Google Cloud Console |
| `client_secret` | string | OAuth2 client secret from Google Cloud Console |
| `redirect_port` | int | Local port the engine listens on to catch Google's redirect |

The engine spins a temporary local HTTP server on `redirect_port` during the auth flow only. It dies immediately after the auth code is captured. Any unused port works — 8742 is the default.

`client_secret` should never appear in plain text in source. Obfuscate it or load it from an environment variable before passing it to the config.

---

### Privacy

```cpp
engine.config.yes_ad_no_ad          = true
engine.config.fingerprint_profile   = "chrome130"  // chrome130 | firefox120 | custom
```

| Field | Type | Description |
|---|---|---|
| `yes_ad_no_ad` | bool | Stealth ad sink — ads pinged but discarded, site sees them as loaded |
| `fingerprint_profile` | string | TLS/HTTP fingerprint to impersonate |

---

### Moonbug (Network Inspector)

```cpp
engine.config.moonbug.active = true
```

| Field | Type | Description |
|---|---|---|
| `active` | bool | Turns network capture on/off |

Moonbug is not a UI tab. It is a capture layer between libcurl and the engine, controlled from the JSON protocol (`{"cmd":"moonbug"}`) or `lucyn --moonbug`; the dashboard is served over loopback HTTP.

---

### Logger

```cpp
engine.config.log.level = LogLevel::DEBUG   // dev — all output visible
engine.config.log.level = LogLevel::WARN    // prod — only warnings and above
```

| Value | When Used |
|---|---|
| `LogLevel::DEBUG` | Development — shows everything including debug and info |
| `LogLevel::INFO` | General — hides debug output |
| `LogLevel::WARN` | Production — warnings, errors, and fatals only |
| `LogLevel::ERROR` | Errors and fatals only |
| `LogLevel::FATAL` | Fatal only |

Default is `LogLevel::DEBUG` in dev builds and `LogLevel::WARN` in prod builds. Matches the build mode automatically unless overridden here.

---

## `engine.iconshow` — Branding API

```cpp
engine.iconshow.imageandname("./icon.png", "My Browser", "The best browser on LucyN")
```

| Param | Type | Description |
|---|---|---|
| path | string | Icon image path |
| name | string | App display name |
| description | string | Subtitle / app description |

---

## Distribution Modes

### Development
```cpp
engine.config.asset_mode = AssetMode::LOOSE_FILE
```
- Shell/UI files read from disk
- Edit HTML/JS → refresh → see changes instantly
- No recompile needed

### Production
```cpp
engine.config.asset_mode = AssetMode::BINARY_EMBED
```
- HTML/JS baked into the binary
- Single executable, zero loose files
- Tamper-proof

---

## What the Engine Config Does NOT Control

| Concern | Handled by |
|---|---|
| Web page rendering | Painter (C++) — offscreen, PNG screenshots only |
| Page behavior / UI logic | The page's own JS (QuickJS), driven through `eval`/`click` |
| Browser chrome | Nothing — LucyN has none |

---

## Full Setup Example

```cpp
#include "lucyn/engine.hpp"

int main() {
    lucyn::Engine engine;

    // Identity
    engine.config.name     = "Phantom";
    engine.config.iconpath = "./assets/phantom.png";
    engine.config.motto    = "Raw. Fast. Honest.";

    // UI shell (script injection surface)
    engine.config.homepage("./ui/shell.html", "style.css", "app.js");
    engine.config.asset_mode = AssetMode::LOOSE_FILE;

    // Search
    engine.config.search.default = "brave";

    // HTTP
    engine.config.http.timeout    = 30;
    engine.config.http.user_agent = "auto";

    // Google Auth
    engine.config.auth.google.enabled       = true;
    engine.config.auth.google.client_id     = "YOUR_CLIENT_ID";
    engine.config.auth.google.client_secret = "YOUR_CLIENT_SECRET";
    engine.config.auth.google.redirect_port = 8742;

    // Privacy
    engine.config.yes_ad_no_ad        = true;
    engine.config.fingerprint_profile = "firefox120";

    // Moonbug
    engine.config.moonbug.active        = true;
    engine.config.moonbug.openable      = true;
    engine.config.moonbug.tab_closeable = false;

    // Logger
    engine.config.log.level = LogLevel::DEBUG;

    engine.run();
    return 0;
}
```

---

*Lucy N. Engine API Specification v0.2 — Ernest Tech House*
