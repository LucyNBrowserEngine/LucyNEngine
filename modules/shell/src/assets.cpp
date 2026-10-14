#include <lucyn/shell/assets.h>
#include <lucyn/core/paths.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace lucyn::shell {

const char* defaultShellHtml() {
    // inject.js and hydrator.js are embedded as inline scripts so they run
    // regardless of the site URL (no relative-path resolution issues).
    // The C++ host splices them in at ensureShellFiles() time; if the user
    // customises shell.html they can reference the files however they like.
    static std::string html;
    if (html.empty()) {
        html  = "<!doctype html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n";
        html += "<script>\n";
        html += defaultInjectJs();
        html += "\n</script>\n";
        html += "<script>\n";
        html += defaultHydratorJs();
        html += "\n</script>\n";
        html += "</head>\n<body></body>\n</html>\n";
    }
    return html.c_str();
}

const char* defaultInjectJs() {
    return
// inject.js — runs inside the shell before ANY site code.
// Patches and polyfills that every page needs regardless of what site is loaded.
R"INJECT(
(function () {
  'use strict';

  // ---- requestAnimationFrame / cancelAnimationFrame -------------------------
  // Sites that drive animations via rAF would spin forever without this.
  // We implement it as a 16 ms setTimeout (≈60 fps cadence) so the settle loop
  // still drains it correctly.
  var _rafId = 0;
  var _rafCallbacks = {};
  if (!window.requestAnimationFrame) {
    window.requestAnimationFrame = function (cb) {
      var id = ++_rafId;
      _rafCallbacks[id] = cb;
      setTimeout(function () {
        if (!_rafCallbacks[id]) return;
        var fn = _rafCallbacks[id];
        delete _rafCallbacks[id];
        fn(performance.now());
      }, 16);
      return id;
    };
  }
  if (!window.cancelAnimationFrame) {
    window.cancelAnimationFrame = function (id) { delete _rafCallbacks[id]; };
  }

  // ---- performance.now() ----------------------------------------------------
  if (typeof performance === 'undefined' || !performance.now) {
    var _t0 = Date.now();
    window.performance = window.performance || {};
    window.performance.now = function () { return Date.now() - _t0; };
  }

  // ---- matchMedia -----------------------------------------------------------
  // Return a MediaQueryList that always matches false (we have no viewport).
  // Sites that gate code behind matchMedia won't crash.
  if (!window.matchMedia) {
    window.matchMedia = function (query) {
      return {
        matches: false,
        media: String(query),
        onchange: null,
        addListener: function () {},
        removeListener: function () {},
        addEventListener: function () {},
        removeEventListener: function () {},
        dispatchEvent: function () { return false; }
      };
    };
  }

  // ---- ResizeObserver stub --------------------------------------------------
  if (!window.ResizeObserver) {
    window.ResizeObserver = function (cb) {
      this._cb = cb;
    };
    window.ResizeObserver.prototype.observe    = function () {};
    window.ResizeObserver.prototype.unobserve  = function () {};
    window.ResizeObserver.prototype.disconnect = function () {};
  }

  // ---- IntersectionObserver stub -------------------------------------------
  if (!window.IntersectionObserver) {
    window.IntersectionObserver = function (cb, opts) {
      this._cb  = cb;
      this._opts = opts || {};
    };
    window.IntersectionObserver.prototype.observe    = function () {};
    window.IntersectionObserver.prototype.unobserve  = function () {};
    window.IntersectionObserver.prototype.disconnect = function () {};
    window.IntersectionObserver.prototype.takeRecords = function () { return []; };
  }

  // ---- navigator extras -----------------------------------------------------
  // Some sites read these and throw if undefined.
  if (navigator && !navigator.sendBeacon) {
    navigator.sendBeacon = function () { return false; };
  }
  if (navigator && navigator.connection === undefined) {
    Object.defineProperty(navigator, 'connection', {
      get: function () {
        return { effectiveType: '4g', downlink: 10, rtt: 50, saveData: false };
      }
    });
  }

  // ---- crypto.randomUUID / getRandomValues ----------------------------------
  if (typeof crypto !== 'undefined' && !crypto.randomUUID) {
    crypto.randomUUID = function () {
      var b = new Uint8Array(16);
      crypto.getRandomValues(b);
      b[6] = (b[6] & 0x0f) | 0x40;
      b[8] = (b[8] & 0x3f) | 0x80;
      var h = Array.from(b).map(function(v){ return ('0'+v.toString(16)).slice(-2); });
      return h[0]+h[1]+h[2]+h[3]+'-'+h[4]+h[5]+'-'+h[6]+h[7]+'-'+h[8]+h[9]+'-'+h[10]+h[11]+h[12]+h[13]+h[14]+h[15];
    };
  }

  // ---- CSS custom property reads (getComputedStyle) -------------------------
  // Sites that call getComputedStyle().getPropertyValue('--some-var') expect a string, not undefined.
  var _origGCS = window.getComputedStyle;
  if (_origGCS) {
    window.getComputedStyle = function (el, pseudo) {
      var style = _origGCS(el, pseudo);
      if (!style) return style;
      var _origGPV = style.getPropertyValue;
      if (_origGPV) {
        style.getPropertyValue = function (prop) {
          var v = _origGPV.call(this, prop);
          return v === undefined ? '' : v;
        };
      }
      return style;
    };
  }

})();
)INJECT";
}

const char* defaultHydratorJs() {
    return
// hydrator.js — runs inside the shell after inject.js.
// Exposes window.__lucyn for scripts that want to know they are inside LucyN,
// and provides a lightweight re-hydration API for advanced user scripts.
R"HYDRATE(
(function () {
  'use strict';

  // ---- LucyN identity -------------------------------------------------------
  // window.__lucyn is the public surface that user scripts and site code can read.
  // It deliberately exposes very little — just enough for scripts to detect the
  // engine and call back into it.
  Object.defineProperty(window, '__lucyn', {
    value: Object.freeze({
      version: '0.1.0',
      engine:  'QuickJS',
      // true inside LucyN, undefined in a real browser — use this for feature gates
      headless: true,
    }),
    writable: false,
    configurable: false,
    enumerable: false,     // not visible to for..in or Object.keys — stealthy
  });

  // ---- document.readyState helpers -----------------------------------------
  // Some frameworks poll document.readyState before doing anything.
  // The C++ host sets it correctly, but do a belt-and-braces check here.
  if (!document.readyState) {
    Object.defineProperty(document, 'readyState', {
      value: 'loading', writable: true, configurable: true
    });
  }

  // ---- Custom event helpers ------------------------------------------------
  // dispatchEvent with a CustomEvent is common; make sure it works.
  if (typeof CustomEvent === 'undefined') {
    window.CustomEvent = function (type, params) {
      params = params || {};
      var e = new Event(type, { bubbles: params.bubbles || false, cancelable: params.cancelable || false });
      e.detail = params.detail !== undefined ? params.detail : null;
      return e;
    };
    window.CustomEvent.prototype = Event.prototype;
  }

  // ---- Clipboard API stub --------------------------------------------------
  // Many SPAs try navigator.clipboard.writeText on user action. Stub it so they
  // don't throw (we return a resolved Promise so await works too).
  if (navigator && !navigator.clipboard) {
    Object.defineProperty(navigator, 'clipboard', {
      get: function () {
        return {
          writeText:  function () { return Promise.resolve(); },
          readText:   function () { return Promise.resolve(''); },
          write:      function () { return Promise.resolve(); },
          read:       function () { return Promise.resolve([]); },
        };
      }
    });
  }

  // ---- history.scrollRestoration -------------------------------------------
  if (window.history && window.history.scrollRestoration === undefined) {
    try { window.history.scrollRestoration = 'manual'; } catch(e) {}
  }

  // ---- Re-hydration API (advanced user scripts) ----------------------------
  // window.__lucyn_hydrate(url) — placeholder for future C++ bridge.
  // Today it just records the request; once the server module is wired in
  // it will trigger a full hydrate() cycle and return a Promise<HydrateResult>.
  var _pending = [];
  window.__lucyn_hydrate = function (url) {
    _pending.push(url);
    // Return a Promise that resolves when the C++ bridge picks it up.
    // For now it resolves undefined after a tick so callers don't hang.
    return new Promise(function (resolve) { setTimeout(resolve, 0); });
  };
  window.__lucyn_pendingHydrations = function () {
    var p = _pending.slice();
    _pending = [];
    return p;
  };

})();
)HYDRATE";
}

const char* exampleScript() {
    return R"(// ==LucyScript==
// @name      example
// @run-at    end
// @match     *
// @priority  0
// ==/LucyScript==
// Rename this file to example.js to turn it on.
//
// `lucyn` is a parameter of your script, not a global: the page cannot see it. Your top-level `var`s stay inside the script
// too; to share something with the page, assign it to `window`.
//   lucyn.page.url()           lucyn.log(msg, ...)
//   lucyn.snapshot.take(name)  lucyn.snapshot.get(name)  lucyn.snapshot.labels()  lucyn.snapshot.diff(a, b)
// @run-at start runs before the site is mounted (document.body is empty); @run-at end runs after the site's own scripts have settled.

const banner = document.getElementById('cookie-banner');
if (banner) banner.style.display = 'none';
lucyn.log('example ran on', lucyn.page.url(), banner ? '(hid #cookie-banner)' : '(no #cookie-banner here)');
)";
}

std::string loadShellHtml() {
    fs::path p = fs::path(core::dataRoot()) / "shell" / "shell.html";
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) {
        std::ifstream in(p, std::ios::binary);
        if (in) {
            std::string s((std::istreambuf_iterator<char>(in)), {});
            // Only use the on-disk file if the user actually customised it
            // (i.e. it doesn't start with the old placeholder comment).
            if (!s.empty() && s.find("STATUS: not executed") == std::string::npos) return s;
        }
    }
    return defaultShellHtml();
}

static bool writeIfMissing(const fs::path& p, const char* content, std::string* err) {
    std::error_code ec;
    if (fs::exists(p, ec)) return true;
    std::ofstream out(p, std::ios::binary);
    if (out) out << content;
    if (!out) { if (err) *err = "cannot write " + p.string(); return false; }
    return true;
}

bool ensureShellFiles(std::string* err) {
    std::string shellDir = core::dataDir("shell");
    std::string scriptDir = core::dataDir("scripts");
    if (shellDir.empty() || scriptDir.empty()) { if (err) *err = "cannot resolve nothing-data"; return false; }
    return writeIfMissing(fs::path(shellDir) / "shell.html", defaultShellHtml(), err) &&
           writeIfMissing(fs::path(shellDir) / "inject.js", defaultInjectJs(), err) &&
           writeIfMissing(fs::path(shellDir) / "hydrator.js", defaultHydratorJs(), err) &&
           writeIfMissing(fs::path(scriptDir) / "example.js.disabled", exampleScript(), err);
}

}  // namespace lucyn::shell
