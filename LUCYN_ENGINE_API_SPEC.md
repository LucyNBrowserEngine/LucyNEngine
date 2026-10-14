# Lucy N. Engine — API Specification
### v0.2

---

## Overview

The Lucy N. engine exposes a C++ configuration API that controls everything the engine needs to know **before the UI loads**. All visual customization, page design, and user-facing settings live in HTML/LNM files — not here.

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
| `name` | string | App name shown in taskbar / window title |
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

### LNM Entry Point

```cpp
engine.config.lnm.entry = "./ui/shell.lnm"
```

| Field | Type | Description |
|---|---|---|
| `entry` | string | Root `.lnm` file the engine loads on startup |

This is the first LNM file the engine parses. All other `.lnm` files are reached through `switch.replace`, `switch.overlay`, or `switch.tooltip` from here.

---

### Window

```cpp
engine.config.window.title     = "My Browser"
engine.config.window.icon      = "./assets/icon.png"
engine.config.window.frameless = false
```

| Field | Type | Description |
|---|---|---|
| `title` | string | OS window title |
| `icon` | string | OS window / taskbar icon |
| `frameless` | bool | Remove native OS window chrome |

---

### Tabs

```cpp
engine.config.allow_new_tab_addition = true
engine.config.max_tabs               = 20
```

| Field | Type | Description |
|---|---|---|
| `allow_new_tab_addition` | bool | Whether user can open new tabs |
| `max_tabs` | int | Max concurrent tabs allowed |

---

### Search Engine

```cpp
engine.config.search.default     = "brave"
engine.config.search.searxng_url = "https://searx.example.com"
```

| Field | Type | Description |
|---|---|---|
| `default` | string | Default search engine |
| `searxng_url` | string | Custom SearXNG instance URL (only used when default = "searxng") |

**Supported values for `default`:**
- `"google"`
- `"brave"`
- `"duckduckgo"`
- `"searxng"`

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
engine.config.moonbug.active        = true
engine.config.moonbug.openable      = true
engine.config.moonbug.tab_closeable = false
engine.config.moonbug.allow_remove  = false
engine.config.moonbug.allow_hide    = true
```

| Field | Type | Description |
|---|---|---|
| `active` | bool | Turns network capture on/off |
| `openable` | bool | Whether user can open the moonbug tab |
| `tab_closeable` | bool | Whether the moonbug tab can be closed |
| `allow_remove` | bool | Fully disables moonbug from the build |
| `allow_hide` | bool | Lets user hide moonbug tab without removing it |

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
- UI files read from disk
- Edit HTML/LNM/JS → refresh → see changes instantly
- No recompile needed

### Production
```cpp
engine.config.asset_mode = AssetMode::BINARY_EMBED
```
- HTML/JS minified and baked into the binary by `lnmc`
- Single executable, zero loose files
- Tamper-proof

---

## What the Engine Config Does NOT Control

| Concern | Handled by |
|---|---|
| Settings UI | `settings.html` + LNM |
| No internet page | `offline.html` |
| Color themes | LNM style blocks |
| Error pages | `error.html` |
| New tab page | `newtab.html` |
| Web page rendering | Painter (C++) |
| Complex UI logic | JS + LNM bridge |

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

    // UI
    engine.config.homepage("./ui/shell.html", "style.css", "app.js");
    engine.config.lnm.entry  = "./ui/shell.lnm";
    engine.config.asset_mode = AssetMode::LOOSE_FILE;

    // Window
    engine.config.window.title     = "Phantom";
    engine.config.window.frameless = false;

    // Tabs
    engine.config.allow_new_tab_addition = true;
    engine.config.max_tabs               = 10;

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