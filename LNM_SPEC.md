# Lucy Native Markup (LNM)
### Specification v0.2

---

## Overview

**Lucy Native Markup (LNM)** is the declarative UI wiring language of the Lucy N. engine. It is not a programming language. It is not a styling language. It is not HTML.

LNM is glue. It grabs HTML files, styles their elements, wires events to style changes or JS function calls, and controls view switching — then gets out of the way. All complex logic lives in `.js` files. All structure lives in `.html` files. LNM connects them.

LNM files are compiled before the app builds. At runtime, no `.lnm` file exists — only the compiled output embedded in the binary.

---

## File Extension

```
.lnm
```

---

## Syntax Rules

- Declarations are newline-significant — no semicolons required
- Semicolons are accepted as optional separators
- Blocks use `{ }`
- Selectors follow CSS selector syntax: `#id`, `.class`, `tag`
- Comments use `#`

```lnm
# this is a comment
```

---

## Core Philosophy

| Concern | Handled by |
|---|---|
| Structure | `.html` files |
| Wiring, styling, events | `.lnm` files |
| Application logic | `.js` files |
| Rendering, animation | C++ painter via interpreter |
| Engine/window config | C++ engine config |

---

## 1. Grabbing Resources

LNM starts by declaring which HTML and JS files it controls.

```lnm
grab(./shell.html)
grab.js(./api.js)
```

- `grab()` — links an HTML file whose elements LNM can target
- `grab.js()` — loads a JS file into the QuickJS context

Multiple grabs allowed:

```lnm
grab(./shell.html)
grab(./sidebar.html)
grab.js(./api.js)
grab.js(./utils.js)
```

---

## 2. Styling Elements

Target any element from the grabbed HTML and declare styles directly.

```lnm
grab(./shell.html)

#nav-bar {
    color: #1a1a1a
    padding: 8px
    radius: 4px
}

.nav-item {
    textcolor: #ffffff
    padding: 4px 12px
}
```

### Style Properties

| LNM Property | Effect |
|---|---|
| `color` | Background color |
| `textcolor` | Text color |
| `radius` | Border radius |
| `padding` | Padding |
| `margin` | Margin |
| `opacity` | Opacity |
| `scale` | Transform scale |
| `border` | Border |
| `shadow` | Box shadow |
| `transition` | Animation duration (default: `100ms`) |

### Color Formats

All of these are valid anywhere a color value is accepted:

```lnm
color: #1a1a1a
color: #fff
color: rgb(255, 0, 0)
color: rgba(0, 0, 0, 0.5)
color: red
color: transparent
```

---

## 3. Event Triggers

Events are declared inline using `:eventname` on a selector.

```lnm
#home-btn:onclick {
    ...
}

.nav-item:onhover {
    ...
}

#title-bar:ondoubleclick {
    ...
}
```

### Supported Events

| Trigger | Description |
|---|---|
| `:onclick` | Mouse click |
| `:onhover` | Mouse enter |
| `:ondoubleclick` | Double click |
| `:onsubmit` | Form submission |
| `:onfocus` | Input focus |
| `:onblur` | Input blur |
| `:onrightclick` | Right mouse click |

More events will be added as the engine grows.

---

## 4. Inline Style on Event

Change element styles directly inside an event block. The interpreter passes the change to the painter, which animates the transition.

```lnm
.nav-item:onhover {
    scale: 1.05
    opacity: 1.0
    textcolor: #ffffff
    transition: 150ms
}
```

If `transition` is not declared, the painter uses the default of **100ms**.

---

## 5. View Switching

LNM controls which view is displayed using `switch`.

### `switch.replace`
Replaces the current view entirely.
```lnm
#home-btn:onclick {
    switch.replace: ./home.html
}
```

### `switch.overlay`
Renders the new view on top of the current view.
```lnm
#settings-btn:onclick {
    switch.overlay: ./settings.html
}
```

### `switch.tooltip`
Renders a small floating view anchored to a position. Position is required.

```lnm
#tooltip-icon:onhover {
    switch.tooltip(top): ./tooltip.html
}

#dropdown-btn:onclick {
    switch.tooltip(bottom): ./dropdown.html
}

#context-item:onrightclick {
    switch.tooltip(cursor): ./context-menu.html
}
```

### Tooltip Positions

| Value | Behavior |
|---|---|
| `top` | Above the element |
| `bottom` | Below the element |
| `left` | Left of the element |
| `right` | Right of the element |
| `cursor` | At the current mouse position |

---

## 6. JS Bridge (`runjs`)

For logic beyond declaration, LNM hands off to a registered JS function.

```lnm
grab(./login.html)
grab.js(./api.js)

#submit-btn:onclick {
    runjs: handleLogin()
}
```

Arguments are supported:

```lnm
#dark-btn:onclick {
    runjs: setTheme("dark")
}
```

---

## 7. JS Registration (`LNM.register`)

Every function callable via `runjs:` must be explicitly registered in the JS file using `LNM.register()` before LNM can call it.

```js
// api.js
LNM.register({ handleLogin, setTheme, logout })

function handleLogin() { ... }
function setTheme(mode) { ... }
function logout() { ... }
```

### Unregistered Function Behavior

**Dev build:**
```
[LNM] ❌ Build failed.
→ runjs: setTheme() — function not registered in settings.js
→ Register it with LNM.register({ setTheme }) and rebuild.
```
The app does not launch.

**Prod build:**
```
[LNM] ⚠️  setTheme() was called on #dark-btn:onclick but is not registered.
→ #dark-btn will not respond as expected.
```
The app runs. That element does nothing.

---

## 8. The `LNM` Global (JS Side)

The engine injects an `LNM` global object into every QuickJS context. JS files can call back into the engine using it.

```js
LNM.switch.replace("./dashboard.html")   // full view swap
LNM.switch.overlay("./modal.html")       // overlay view
LNM.switch.tooltip("./tip.html", "top")  // tooltip view
LNM.style("#selector", { color: "#fff", radius: "4px" }) // restyle element
```

---

## 9. Form Handling

```lnm
grab(./login.html)
grab.js(./api.js)

form#login-form {
    :onsubmit {
        runjs: handleLogin()
    }
}
```

---

## 10. Animations

LNM does not animate directly. When a style change fires (on event or otherwise), the interpreter reads the before and after state and passes both to the painter with the transition duration. The painter interpolates between them natively in C++.

LNM only declares the end state and optionally the duration:

```lnm
.nav-item:onhover {
    scale: 1.05
    color: #333333
    transition: 200ms
}
```

Default transition is **100ms** when not declared.

---

## 11. Full Example

```lnm
grab(./shell.html)
grab.js(./api.js)

# Styles
#nav-bar {
    color: #111111
    padding: 10px
    radius: 0px
}

.nav-item {
    textcolor: #aaaaaa
    padding: 4px 12px
    transition: 100ms
}

.nav-item:onhover {
    textcolor: #ffffff
    scale: 1.03
}

# Navigation
#home-btn:onclick {
    switch.replace: ./home.html
}

#settings-btn:onclick {
    switch.overlay: ./settings.html
}

#tooltip-icon:onhover {
    switch.tooltip(top): ./tooltip.html
}

# Forms
form#search-bar {
    :onsubmit {
        runjs: handleSearch()
    }
}
```

---

## What LNM Does NOT Do

- No variables
- No conditionals
- No loops
- No network calls — use JS
- No complex logic — use JS
- No window or engine config — use C++ config
- No execution outside the Lucy N. engine

---

## Runtime Behavior

| Build | `.lnm` files | HTML/JS assets |
|---|---|---|
| Dev | Read from disk, hot-reloaded | Read from disk |
| Prod | Compiled to `.h` struct, embedded in binary | Minified, embedded in binary |

---

*Lucy Native Markup Specification v0.2 — Ernest Tech House*