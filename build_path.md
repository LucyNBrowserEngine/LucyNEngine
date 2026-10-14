# Lucy N. Build Pipeline

---

## Overview

The Lucy N. build pipeline converts `.lnm`, `.html`, and `.js` source files into a single native binary. At no point does CMake touch or know about `.lnm` files directly. All LNM compilation happens in a pre-build step via `lnmc`, the LNM compiler tool.

---

## Tools Involved

| Tool | Role |
|---|---|
| `lnmc` | LNM compiler — parses `.lnm`, minifies HTML/JS, outputs `.h` files |
| `uglify-js` | JS minifier — runs inside `lnmc`'s embedded QuickJS instance |
| CMake | Builds the C++ engine and links everything into the binary |
| C++ compiler | Compiles the engine + generated headers |

`uglify-js` lives in `third_party/uglify-js/`. It is bundled with the engine source and never fetched at build time.

`lnmc` is a self-contained C++ binary. It embeds QuickJS internally and runs `uglify-js` inside that QuickJS instance during the minification step. No Node.js required. No external JS runtime required. The only build dependencies are a C++ compiler and CMake.

---

## Full Pipeline

```
Source files
    .lnm  .html  .js
          │
          ▼
        lnmc
          │
    ┌─────┴──────┐
    │            │
    ▼            ▼
minify .html   uglify-js on .js
    │            │
    ▼            ▼
const char*  const char*
SETTINGS_HTML  SETTINGS_JS
    │            │
    └─────┬──────┘
          │
          ▼
    settings_assets.h
          │
    parse .lnm
          │
          ▼
    LNMDocument struct
          │
          ▼
    settings_lnm.h
          │
          ▼
        CMake
          │
          ▼
    Single native binary
```

---

## What `lnmc` Produces

For every `.lnm` file, `lnmc` outputs two header files:

### `settings_assets.h`
Contains the minified HTML and JS as embedded C string literals:

```cpp
static const char* SETTINGS_HTML = "<div id=\"settings-page\">...</div>";
static const char* SETTINGS_JS   = "LNM.register({setTheme,logout});function setTheme(m){...}";
```

### `settings_lnm.h`
Contains the compiled LNM declarations as a C++ struct:

```cpp
static const LNMDocument SETTINGS_LNM = {
    .grabs        = { { HTML, "./settings.html" }, { JS, "./settings.js" } },
    .styles       = { { "#settings-page", { {"color","#111111"}, {"padding","24px"} } } },
    .events       = { { "#dark-btn", ON_CLICK, RUNJS, "setTheme", {"dark"} } },
    .html_source  = SETTINGS_HTML,
    .js_source    = SETTINGS_JS
};
```

---

## CMake Integration

CMake runs `lnmc` as a pre-build custom command before any C++ compiles:

```cmake
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/lnm_generated
    COMMAND lnmc ${CMAKE_SOURCE_DIR}/ui/*.lnm --out ${CMAKE_BINARY_DIR}/lnm_generated
    DEPENDS ${LNM_FILES}
    COMMENT "[LNM] 🔧 Compiling .lnm files..."
)

add_custom_target(lnm_compile ALL DEPENDS ${CMAKE_BINARY_DIR}/lnm_generated)
add_dependencies(LucyN lnm_compile)
```

CMake never sees a `.lnm` file. It only compiles the `.h` output.

---

## Dev vs Prod Build Differences

| | Dev Build | Prod Build |
|---|---|---|
| `.lnm` files | Read from disk, hot-reloaded | Compiled to `.h` struct, embedded |
| HTML files | Read from disk | Minified, embedded as `const char*` |
| JS files | Read from disk | Uglified, embedded as `const char*` |
| Unregistered `runjs:` | Fatal — app does not launch | Warn — element silently does nothing |
| Debug logs | Visible | Suppressed |
| Output | Debug binary | Single production binary, no asset files |

---

## Third Party Directory

```
third_party/
└── uglify-js/       ← bundled, runs inside lnmc's embedded QuickJS — no Node required
```

---

## Build Output

Production build produces one file:

```
build/
└── lucyn            ← single binary, contains everything
```

No HTML files. No JS files. No LNM files. No asset folder.

---

*Lucy N. Build Pipeline — Ernest Tech House*
