/**
 * nothing-browser — JS client for the LucyN headless engine.
 *
 * Spawns `lucyn` as a child process and drives it over the M6 JSON command
 * protocol (newline-delimited JSON on stdin/stdout).
 *
 * @example
 * import Lucyn from "nothing-browser";
 *
 * const a = new Lucyn({ profile: "chrome" });
 * await a.visit("https://example.com");
 * const title = await a.extract("title");
 * const html  = await a.html();
 *
 * // M5: the Moonbug wiretap. Every request the engine makes is captured.
 * a.moonbug.onRequest((req) => console.log(req.status, req.url, req.sig));
 * const dash = await a.moonbug.serve({ dashboard: 6969 });   // http://127.0.0.1:6969/
 * const last = await a.moonbug.captures(10);                 // full captures incl. headers + body preview
 * await a.close();
 */

import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
import { EventEmitter } from "node:events";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { existsSync } from "node:fs";

// ---- binary resolution ------------------------------------------------------

const __dir = dirname(fileURLToPath(import.meta.url));

/** Locations to search for the `lucyn` binary, in priority order. */
function findBinary(override) {
  if (override) return override;
  const candidates = [
    // 1. Env var — useful in CI or when the repo is built elsewhere.
    process.env.LUCYN_BIN,
    // 2. Built next to the client folder (monorepo: lucyn/build/bin/lucyn).
    resolve(__dir, "../../build/bin/lucyn"),
    // 3. System PATH (installed globally).
    "lucyn",
  ].filter(Boolean);

  for (const c of candidates) {
    if (c === "lucyn") return c; // let the OS resolve it
    if (existsSync(c)) return c;
  }
  return "lucyn"; // fall through to PATH; will fail with a clear error
}

// ---- protocol helpers -------------------------------------------------------

let _nextId = 1;
const nextId = () => _nextId++;

function serialise(cmd) {
  return JSON.stringify(cmd);
}

// ---- LucynSession -----------------------------------------------------------

/**
 * A single session wrapping one `lucyn` process.
 * All methods are async — every call crosses a process boundary.
 */
class LucynSession extends EventEmitter {
  /**
   * @param {object} [opts]
   * @param {string} [opts.profile="chrome"]       browser identity
   * @param {number} [opts.settleTimeoutMs=5000]   JS settle timeout
   * @param {boolean} [opts.console=false]          echo page console to stderr
   * @param {boolean} [opts.adblock=true]           enable the built-in request blocker
   * @param {string} [opts.adblockList]              additional ABP/uBO-format rule file
   * @param {string} [opts.binary]                  path to the `lucyn` binary
   * @param {string} [opts.config]                  engine JSON config (search defaults, LNM entry)
   * @param {boolean|object} [opts.moonbug]         start Moonbug listeners with the engine:
   *        true = defaults (raw 2005, signed 2006, dashboard 6969), or { raw, signed, dashboard, dashboardFile }
   *        where each port is a number (0 = any free port). All bind 127.0.0.1 only.
   * @param {boolean} [opts.ghostful=false]         Ghostful Mode: create a real SDL window at startup, hidden
   *        (README: "window hidden, still real"). Without this the engine still renders pages exactly the
   *        same way — layout/paint don't need a window at all — this only affects whether an SDL window
   *        object exists from the first instant versus being created lazily on first use. Needs an SDL2
   *        build of the engine; harmless (just ignored, headless as always) otherwise.
   * @param {boolean} [opts.show=false]              create and show the SDL window at startup
   */
  constructor(opts = {}) {
    super();
    this._moonbugOpts     = opts.moonbug ?? null;
    this.moonbug          = new Moonbug(this);
    this._profile         = opts.profile ?? "chrome";
    this._settleTimeoutMs = opts.settleTimeoutMs ?? 5000;
    this._console         = opts.console ?? false;
    this._adblock          = opts.adblock ?? true;
    this._adblockList      = opts.adblockList ?? "";
    this._ghostful        = opts.ghostful ?? false;
    this._show            = opts.show ?? false;
    this._config          = opts.config ?? "";
    this._binary          = findBinary(opts.binary);
    this._proc            = null;
    this._rl              = null;
    this._pending         = new Map(); // id -> { resolve, reject }
    this._started         = false;
    this._closed          = false;
  }

  // ---- lifecycle ------------------------------------------------------------

  /** Start the engine process. Called lazily on first command. */
  async _ensureStarted() {
    if (this._started) return;
    this._started = true;

    const args = [
      "--profile", this._profile,
      "--settle",  String(this._settleTimeoutMs),
    ];
    if (this._console) args.push("--console");
    if (!this._adblock) args.push("--no-adblock");
    if (this._adblockList) args.push("--adblock-list", this._adblockList);
    if (this._ghostful) args.push("--ghostful");
    if (this._show) args.push("--show");
    if (this._config) args.push("--config", this._config);
    // The client always asks for capture events: they cost nothing until a listener is attached (see Moonbug below)
    // and a late-attached onRequest() must not miss the very first request.
    args.push("--events");
    const mb = this._moonbugOpts;
    if (mb === true) args.push("--moonbug");
    else if (mb && typeof mb === "object") {
      if (mb.raw !== undefined)       args.push("--raw", String(mb.raw));
      if (mb.signed !== undefined)    args.push("--signed", String(mb.signed));
      if (mb.dashboard !== undefined) args.push("--dashboard", String(mb.dashboard));
      if (mb.dashboardFile)           args.push("--dashboard-file", String(mb.dashboardFile));
    }

    this._proc = spawn(this._binary, args, {
      stdio: ["pipe", "pipe", "inherit"], // stdin/stdout piped; stderr inherited
    });

    this._proc.on("error", (err) => {
      const msg = `lucyn process error: ${err.message}`;
      this._rejectAll(msg);
      this.emit("error", new Error(msg));
    });

    this._proc.on("exit", (code, signal) => {
      if (!this._closed) {
        const msg = `lucyn process exited unexpectedly (code=${code} signal=${signal})`;
        this._rejectAll(msg);
        this.emit("exit", code, signal);
      }
    });

    this._rl = createInterface({ input: this._proc.stdout, crlfDelay: Infinity });
    this._rl.on("line", (line) => this._onLine(line));
  }

  _onLine(line) {
    let msg;
    try {
      msg = JSON.parse(line);
    } catch {
      // Unparseable line — emit as raw event for debugging.
      this.emit("raw", line);
      return;
    }

    // Event (unprompted — e.g. pageError, note, future Moonbug).
    if (msg.event) {
      if (msg.event === "moonbug") this.moonbug._ports = { raw: msg.raw, signed: msg.signed, dashboard: msg.dashboard };
      this.emit(msg.event, msg);
      this.emit("event", msg);
      return;
    }

    // Reply to a command.
    const id = msg.id;
    if (id !== undefined && this._pending.has(id)) {
      const { resolve, reject } = this._pending.get(id);
      this._pending.delete(id);
      if (msg.ok) resolve(msg);
      else reject(new Error(msg.error ?? "unknown error from lucyn"));
    }
  }

  _rejectAll(msg) {
    for (const { reject } of this._pending.values()) {
      reject(new Error(msg));
    }
    this._pending.clear();
  }

  /** Send a command and return the reply. */
  _send(cmd) {
    return new Promise(async (resolve, reject) => {
      try {
        await this._ensureStarted();
      } catch (e) {
        return reject(e);
      }
      if (this._closed) return reject(new Error("session is closed"));

      const id = nextId();
      this._pending.set(id, { resolve, reject });
      const line = serialise({ v: 1, id, ...cmd }) + "\n";
      this._proc.stdin.write(line, (err) => {
        if (err) {
          this._pending.delete(id);
          reject(err);
        }
      });
    });
  }

  // ---- commands -------------------------------------------------------------

  /**
   * Navigate to a URL and wait for the page to settle.
   * @param {string} url
   * @param {object} [opts]
   * @param {string} [opts.profile]  switch profile for this visit
   * @returns {Promise<{status:number, url:string, title:string}>}
   */
  async visit(url, opts = {}) {
    const cmd = { cmd: "visit", url };
    if (opts.profile) cmd.profile = opts.profile;
    const r = await this._send(cmd);
    return { status: r.status, url: r.url, title: r.title };
  }

  /**
   * Open a website in LucyN. This is the same navigation pipeline as visit().
   * @param {string} url  http:// or https:// URL
   * @param {object} [opts]
   * @returns {Promise<{status:number, url:string, title:string}>}
   */
  async open(url, opts = {}) {
    return this.visit(url, opts);
  }

  /**
   * Open a provider's normal search results page in LucyN.
   * @param {string} query
   * @param {object} [opts]
   * @param {string} [opts.engine]  provider id; defaults to engine config (Brave by default)
   * @returns {Promise<{status:number, url:string, title:string}>}
   */
  async search(query, opts = {}) {
    const cmd = { cmd: "search", query };
    if (opts.engine) cmd.engine = opts.engine;
    const r = await this._send(cmd);
    return { status: r.status, url: r.url, title: r.title };
  }

  /**
   * Return the fully-settled HTML of the current page.
   * @returns {Promise<string>}
   */
  async html() {
    const r = await this._send({ cmd: "html" });
    return r.value;
  }

  /**
   * Extract the text content of the first element matching `selector`.
   * Rejects if no match.
   * @param {string} selector  CSS selector
   * @returns {Promise<string>}
   */
  async extract(selector) {
    const r = await this._send({ cmd: "extract", selector });
    return r.value;
  }

  /**
   * Extract text content of ALL elements matching `selector`.
   * @param {string} selector  CSS selector
   * @returns {Promise<string[]>}
   */
  async extractAll(selector) {
    const r = await this._send({ cmd: "extractAll", selector });
    return r.values ?? [];
  }

  /**
   * Evaluate a JS expression in the live page context (requires a prior visit()).
   * The JS context stays alive after visit() settles (keepAlive mode).
   * @param {string} js  JavaScript expression or statement(s)
   * @returns {Promise<string>}  the result coerced to a string
   */
  async eval(js) {
    const r = await this._send({ cmd: "eval", js });
    return r.value;
  }

  /**
   * Wait until `selector` matches an element in the live DOM.
   * Keeps pumping timers / jobs until the element appears or the timeout elapses.
   * @param {string} selector  CSS selector
   * @param {object} [opts]
   * @param {number} [opts.timeout=settleTimeoutMs]  ms before giving up
   * @returns {Promise<string>}  the selector (so you can chain)
   */
  async waitFor(selector, opts = {}) {
    const cmd = { cmd: "waitFor", selector };
    if (opts.timeout !== undefined) cmd.timeout = opts.timeout;
    const r = await this._send(cmd);
    return r.value;
  }

  /**
   * Wait until no new network requests have been captured for `quiet` ms.
   * Useful to wait for SPA navigation or lazy image loads to finish.
   * @param {object} [opts]
   * @param {number} [opts.quiet=500]    ms of network silence required
   * @param {number} [opts.timeout=settleTimeoutMs]  total timeout ms
   * @returns {Promise<void>}
   */
  async waitIdle(opts = {}) {
    const cmd = { cmd: "waitIdle" };
    if (opts.quiet   !== undefined) cmd.quiet   = opts.quiet;
    if (opts.timeout !== undefined) cmd.timeout = opts.timeout;
    await this._send(cmd);
  }

  /**
   * Click the first element matching `selector` in the live DOM.
   * Fires a real `.click()` inside the page JS context and settles briefly.
   * @param {string} selector  CSS selector
   * @returns {Promise<string>}  the clicked element's tagName
   */
  async click(selector) {
    const r = await this._send({ cmd: "click", selector });
    return r.value;
  }

  /**
   * Switch the browser identity. Affects the next visit().
   * @param {string} name  e.g. "safari184", "firefox147", "chrome"
   * @returns {Promise<string>}  the resolved profile name
   */
  async profile(name) {
    const r = await this._send({ cmd: "profile", name });
    return r.value;
  }

  /**
   * Return current engine info: active profile, last URL, impersonation status.
   * @returns {Promise<{profile:string, url:string, impersonating:boolean}>}
   */
  async info() {
    const r = await this._send({ cmd: "info" });
    return { profile: r.profile, url: r.url, impersonating: r.impersonating };
  }

  /**
   * Cookies in the engine's jar. With a url: exactly what a request to it would carry (HttpOnly cookies
   * included, flagged). Without: the whole jar. The jar is shared by page scripts (document.cookie, which
   * cannot see HttpOnly) and every request the engine makes, and it starts empty after a profile switch.
   * @param {string} [url]
   * @returns {Promise<Array<{name:string, value:string, domain:string, path:string, hostOnly:boolean,
   *                          secure:boolean, httpOnly:boolean, expires:number}>>}  expires: unix seconds, 0 = session
   */
  async cookies(url) {
    const r = await this._send(url ? { cmd: "cookies", url } : { cmd: "cookies" });
    return r.cookies;
  }

  /**
   * Put a cookie in the jar (e.g. to restore a logged-in session). Give `domain`, or a `url` to take it from.
   * An `expires` in the past deletes the cookie.
   * @param {{name:string, value:string, domain?:string, url?:string, path?:string,
   *          secure?:boolean, httpOnly?:boolean, hostOnly?:boolean, expires?:number}} cookie
   */
  async setCookie(cookie) {
    await this._send({ cmd: "setCookie", ...cookie });
  }

  /**
   * Download a URL straight into the engine's data folder (`nothing-data/downloads/`). Uses the same identity,
   * cookie jar and Moonbug capture as everything else; the file is written only after a complete 2xx transfer.
   * Nothing is overwritten unless asked: a name that exists becomes "name (1).ext".
   * @param {string} url  http:// or https:// only
   * @param {object} [opts]
   * @param {string}  [opts.name]       file name to save as (any path part is dropped); default: from the server or the URL
   * @param {boolean} [opts.overwrite]  replace an existing file of that name
   * @param {number}  [opts.maxBytes]   abort and delete if the download grows past this (0 = no limit)
   * @param {string}  [opts.referer]    Referer header to send
   * @returns {Promise<{path:string, name:string, bytes:number, status:number, type:string, url:string}>}
   */
  async download(url, opts = {}) {
    const cmd = { cmd: "download", url };
    if (opts.name) cmd.name = opts.name;
    if (opts.overwrite) cmd.overwrite = true;
    if (opts.maxBytes > 0) cmd.maxBytes = Math.floor(opts.maxBytes);
    if (opts.referer) cmd.referer = opts.referer;
    const r = await this._send(cmd);
    return { path: r.path, name: r.name, bytes: r.bytes, status: r.status, type: r.type, url: r.url };
  }

  /**
   * Render the current page (through layout + the software painter, no SDL/display needed) into
   * `nothing-data/screenshots/` and return its path. Works even on an engine built without SDL2 —
   * this is pure offscreen rendering, not a capture of a real window.
   * @param {string} [name="screenshot.png"]
   * @param {object} [opts]
   * @param {number} [opts.width]   viewport width in px used for layout (default: 800)
   * @returns {Promise<string>} the saved PNG's absolute path
   */
  async screenshot(name, opts = {}) {
    const cmd = { cmd: "screenshot" };
    if (name) cmd.name = name;
    if (opts.width > 0) cmd.width = Math.floor(opts.width);
    const r = await this._send(cmd);
    return r.path;
  }

  /**
   * Show, hide, or resize the engine's real SDL window (Ghostful Mode's `setVisible`, from JS).
   * `ok:false` with a clear error if the engine was built without SDL2.
   * @param {object} [opts]
   * @param {boolean} [opts.visible]
   * @param {number}  [opts.width]
   * @param {number}  [opts.height]
   * @returns {Promise<{visible:boolean, width:number, height:number}>}
   */
  async setWindow(opts = {}) {
    const cmd = { cmd: "window" };
    if (opts.visible !== undefined) cmd.visible = !!opts.visible;
    if (opts.width > 0) cmd.width = Math.floor(opts.width);
    if (opts.height > 0) cmd.height = Math.floor(opts.height);
    const r = await this._send(cmd);
    return { visible: r.visible, width: r.width, height: r.height };
  }

  /** Convenience: `setWindow({ visible })`. Matches the `engine.setVisible(false)` examples in README.md. */
  async setVisible(visible) {
    return this.setWindow({ visible: !!visible });
  }

  /** Empty the cookie jar. */
  async clearCookies() {
    await this._send({ cmd: "clearCookies" });
  }

  /**
   * Gracefully shut down the engine process.
   */
  async close() {
    if (this._closed) return;
    this._closed = true;
    try {
      await this._send({ cmd: "quit" });
    } catch {
      // Ignore — process may already be gone.
    }
    if (this._proc) {
      this._proc.stdin.end();
      this._proc.kill();
    }
  }
}

// ---- Moonbug (M5) -----------------------------------------------------------

/**
 * The wiretap, from JS. LucyN is the HTTP client, so it sees decrypted traffic natively:
 * no MITM certificate, no proxy, no CDP. Reached ONLY through this client or the loopback listeners.
 * Available as `session.moonbug`.
 */
class Moonbug {
  constructor(session) {
    this._s = session;
    this._ports = { raw: 0, signed: 0, dashboard: 0 };
  }

  /**
   * Called for every completed request the engine makes (page loads, scripts, fetch()...).
   * `req` = { id, method, url, status, ms, bytes, error, sig }. `sig` is NULLBYTE's HMAC-SHA256 seal of the
   * full capture; the same id + sig appear on the signed stream and in the dashboard.
   * @param {(req: object) => void} fn
   * @returns {() => void} unsubscribe
   */
  onRequest(fn) {
    this._s.on("request", fn);
    return () => this._s.off("request", fn);
  }

  /** Alias of onRequest: captures are reported once the response (or failure) is in. */
  onResponse(fn) { return this.onRequest(fn); }

  /**
   * Start Moonbug listeners (127.0.0.1 only). Each value is a port; 0 = any free port; omit to leave alone.
   * @param {{raw?:number, signed?:number, dashboard?:number, file?:string}} [o]  default: all three on their usual ports
   * @returns {Promise<{raw:number, signed:number, dashboard:number}>} the ports actually bound (0 = off)
   */
  async serve(o = { raw: 2005, signed: 2006, dashboard: 6969 }) {
    const cmd = { cmd: "moonbug" };
    for (const k of ["raw", "signed", "dashboard"]) if (o[k] !== undefined) cmd[k] = o[k];
    if (o.file) cmd.file = o.file;
    const r = await this._s._send(cmd);
    return { raw: r.raw, signed: r.signed, dashboard: r.dashboard };
  }

  /**
   * The most recent captures, oldest first. Unlike onRequest events these carry request/response headers
   * (wire order), timing, and a body preview.
   * @param {number} [limit=20]  1..500
   * @returns {Promise<object[]>}
   */
  async captures(limit = 20) {
    const r = await this._s._send({ cmd: "captures", limit });
    return r.captures ?? [];
  }
}

// ---- Default export ---------------------------------------------------------

/**
 * Create a new LucyN session. Each instance is an isolated browser context
 * (separate process, separate identity, separate cookies).
 *
 * @param {object} [opts]
 * @param {string} [opts.profile="chrome"]
 * @param {number} [opts.settleTimeoutMs=5000]
 * @param {boolean} [opts.console=false]
 * @param {string} [opts.binary]
 * @returns {LucynSession}
 */
export default function Lucyn(opts = {}) {
  return new LucynSession(opts);
}

export { LucynSession, Moonbug };
