/**
 * nothing-browser — typed client for the LucyN headless engine.
 *
 * Spawns `lucyn` as a child process and drives it over the JSON command
 * protocol (newline-delimited JSON on stdin/stdout).
 *
 * @example
 * import Lucyn from "nothing-browser";
 *
 * const a = new Lucyn();
 * await a.visit("https://example.com");
 * const title = await a.extract("title");
 * const html  = await a.html();
 *
 * // The Moonbug wiretap. Every request the engine makes is captured.
 * a.moonbug.onRequest((req) => console.log(req.status, req.url, req.sig));
 * const dash = await a.moonbug.serve({ dashboard: 6969 });   // http://127.0.0.1:6969/
 * const last = await a.moonbug.captures(10);                 // full captures incl. headers + body preview
 * await a.close();
 */

import { spawn } from "node:child_process";
import type { ChildProcess } from "node:child_process";
import { createInterface } from "node:readline";
import type { Interface } from "node:readline";
import { EventEmitter } from "node:events";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { existsSync, readFileSync } from "node:fs";
import { connect } from "node:net";
import type { Socket } from "node:net";

// ---- binary resolution ------------------------------------------------------

const __dir = dirname(fileURLToPath(import.meta.url));

/** Locations to search for the `lucyn` binary, in priority order. */
function findBinary(override?: string): string {
  if (override) return override;
  const candidates = [
    // 1. Env var — useful in CI or when the repo is built elsewhere.
    process.env.LUCYN_BIN,
    // 2. Built next to the client folder (monorepo: lucyn/build/bin/lucyn).
    resolve(__dir, "../../build/bin/lucyn"),
    // 3. System PATH (installed globally).
    "lucyn",
  ].filter((c): c is string => Boolean(c));

  for (const c of candidates) {
    if (c === "lucyn") return c; // let the OS resolve it
    if (existsSync(c)) return c;
  }
  return "lucyn"; // fall through to PATH; will fail with a clear error
}

// ---- protocol types ---------------------------------------------------------

/** A command frame written to the engine's stdin: `cmd` plus that command's arguments. */
export type Command = Record<string, unknown>;

/**
 * One parsed line read back from the engine: either a reply to a command
 * (`id` + `ok`) or an unprompted event (`event`). Command-specific payload
 * fields (`value`, `status`, `cookies`, ...) hang off the index signature;
 * methods narrow them with their own reply interfaces.
 */
interface ReplyFrame {
  id?: number;
  ok?: boolean;
  error?: string;
  event?: string;
  raw?: number;
  signed?: number;
  dashboard?: number;
  [key: string]: unknown;
}

/** A pending command's settlement functions. */
interface PendingEntry {
  resolve: (value: unknown) => void;
  reject: (reason: Error) => void;
}

// ---- constructor options ----------------------------------------------------

/** Options for `new Lucyn()` / `Lucyn()`. */
export interface LucynOptions {
  /** JS settle timeout per visit, ms (default 5000). */
  settleTimeoutMs?: number;
  /** Echo page `console.*` to stderr (default false). */
  console?: boolean;
  /** Built-in request blocker (default true). */
  adblock?: boolean;
  /** Additional ABP/uBO-format rule file. */
  adblockList?: string;
  /**
   * Start the engine without a window (default `false`).
   *
   * By default `lucyn` opens a real Chromium window that follows whatever the
   * script does — visit() navigates it, so you can watch the scrape. Pass
   * `headless: true` on servers, in CI, or anywhere a display is not wanted;
   * everything else behaves identically (the page is rendered off-screen either
   * way, so screenshots work in both modes).
   */
  headless?: boolean;
  /** Path to the `lucyn` binary (default: `LUCYN_BIN`, monorepo build, then PATH). */
  binary?: string;
  /**
   * Hook into an engine that is ALREADY running instead of starting a new one
   * (default `true`). A `lucyn` launched in a terminal — including
   * `lucyn --listen` — advertises itself in `nothing-data/engine.json` next to
   * the binary; the client finds that file, connects to the loopback control
   * socket and drives that same browser. This is what avoids the "broken pipe"
   * / zygote errors two engines used to hit when they raced for the same
   * Chromium cache. When nothing is running, the client spawns its own engine
   * exactly as before.
   *
   * An attached `close()` only disconnects: the engine and its window keep
   * running. Set `LUCYN_ATTACH=0` (or pass `attach: false`) to always start a
   * private engine instead.
   */
  attach?: boolean;
  /** Engine JSON config (search defaults). */
  config?: string;
  /**
   * Start Moonbug listeners with the engine: `true` = defaults (raw 2005,
   * signed 2006, dashboard 6969), or a port object; 0 = any free port.
   * All listeners bind 127.0.0.1 only.
   */
  moonbug?: boolean | MoonbugStartOptions;
}

/** Moonbug listeners started together with the engine (`moonbug` option). */
export interface MoonbugStartOptions {
  raw?: number;
  signed?: number;
  dashboard?: number;
  /** Extra file path to expose captures to (same origin as the dashboard). */
  dashboardFile?: string;
}

/** Moonbug listeners started/changed later via `session.moonbug.serve()`. */
export interface MoonbugServeOptions {
  raw?: number;
  signed?: number;
  dashboard?: number;
  /** Extra file path to expose captures to. */
  file?: string;
}

/** Ports bound by the Moonbug listeners (0 = that listener is off). */
export interface MoonbugPorts {
  raw: number;
  signed: number;
  dashboard: number;
}

// ---- command options --------------------------------------------------------

/** Reserved for future navigation options; `visit()`/`open()` take none today. */
export interface VisitOptions {}

/** Provider selection for `search()`. */
export interface SearchOptions {
  /** Provider id; defaults to the engine config (Brave by default). */
  engine?: string;
}

/** Options for `waitFor()`. */
export interface WaitForOptions {
  /** ms before giving up (default: the session's settle timeout). */
  timeout?: number;
}

/** Options for `waitIdle()`. */
export interface WaitIdleOptions {
  /** ms of network silence required (default 500). */
  quiet?: number;
  /** Total timeout ms (default: the session's settle timeout). */
  timeout?: number;
}

/** Options for `download()`. */
export interface DownloadOptions {
  /** File name to save as (any path part is dropped). */
  name?: string;
  /** Replace an existing file of that name. */
  overwrite?: boolean;
  /** Abort and delete if the download grows past this (0 = no limit). */
  maxBytes?: number;
  /** `Referer` header to send. */
  referer?: string;
}

/** Options for `screenshot()`. */
export interface ScreenshotOptions {
  /** Viewport width in px used for layout (default 800). */
  width?: number;
}

// ---- reply / result shapes --------------------------------------------------

/** Navigation reply shared by `visit`, `open` and `search`. */
export interface VisitResult {
  status: number;
  url: string;
  title: string;
}

/** Reply of `info`. */
export interface InfoResult {
  /** Last URL visited. */
  url: string;
  /** How many requests Moonbug has captured. */
  captures: number;
}

/** Reply of `download`. */
export interface DownloadResult {
  /** Absolute path of the saved file. */
  path: string;
  name: string;
  bytes: number;
  status: number;
  /** Content-Type reported by the server. */
  type: string;
  url: string;
}

/** A cookie in the engine's jar. */
export interface Cookie {
  name: string;
  value: string;
  domain: string;
  path: string;
  hostOnly: boolean;
  secure: boolean;
  httpOnly: boolean;
  /** Unix seconds; 0 = session cookie. */
  expires: number;
}

/** Input of `setCookie()`. Give `domain`, or a `url` to take it from. */
export interface SetCookieOptions {
  name: string;
  value: string;
  /** Cookie domain (host-only unless `hostOnly: false`). */
  domain?: string;
  /** Derive domain/path/secure from this URL instead of `domain`. */
  url?: string;
  path?: string;
  secure?: boolean;
  httpOnly?: boolean;
  hostOnly?: boolean;
  /** Unix seconds; an `expires` in the past deletes the cookie. */
  expires?: number;
}

/** One request/response pair as returned by `moonbug.captures()`. */
export interface Capture {
  /** 1, 2, 3 ... per Moonbug instance. */
  id: number;
  /** Unix milliseconds when the request started. */
  ts: number;
  method: string;
  /** URL as requested. */
  url: string;
  /** After redirects. */
  finalUrl: string;
  /** Legacy impersonation target (always "" since the CEF migration). */
  profile: string;
  /** Legacy impersonation flag (always false since the CEF migration). */
  impersonating: boolean;
  /** True once the response (or failure) landed. */
  done: boolean;
  /** 0 = no response (see `error`). */
  status: number;
  /** Empty on success. */
  error: string;
  httpVersion: string;
  remoteIp: string;
  remotePort: number;
  redirects: number;
  timing: CaptureTiming;
  /** `[name, value]` pairs in wire order; duplicates kept. */
  requestHeaders: HeaderPair[];
  requestBodyBytes: number;
  responseHeaders: HeaderPair[];
  /** True size, even when `body` was cut. */
  bodyBytes: number;
  bodyTruncated: boolean;
  /** Which encoding `body` uses — cut on a UTF-8 boundary when utf8. */
  bodyEncoding: "utf8" | "base64";
  /** Response body preview (first ~1 MiB). */
  body: string;
}

/** A header as it went over the wire: `[name, value]`. */
export type HeaderPair = [name: string, value: string];

/** Cumulative timings in milliseconds, like curl's. */
export interface CaptureTiming {
  dnsMs: number;
  connectMs: number;
  tlsMs: number;
  firstByteMs: number;
  totalMs: number;
}

// ---- events -----------------------------------------------------------------

/** Emitted for every completed request the engine makes (load, script, fetch/XHR...). */
export interface RequestEvent {
  event: "request";
  id: number;
  method: string;
  url: string;
  status: number;
  /** Total time in ms. */
  ms: number;
  /** Response bytes. */
  bytes: number;
  /** Empty on success. */
  error: string;
  /** NULLBYTE's HMAC-SHA256 seal of the full capture — the same `id` + `sig`
   * appear on the signed stream and in the dashboard. */
  sig: string;
}

/** An engine note (blocked subresource, skipped action, ...). */
export interface NoteEvent {
  event: "note";
  message: string;
}

/** An uncaught page error. */
export interface PageErrorEvent {
  event: "pageError";
  message: string;
}

/** Moonbug listeners came up; the ports actually bound. */
export interface MoonbugPortsEvent {
  event: "moonbug";
  raw: number;
  signed: number;
  dashboard: number;
}

/** Any unprompted engine event — the `event` channel carries every one of them. */
export interface EngineEvent {
  event: string;
  [key: string]: unknown;
}

/** All events the session emits, with their callback argument tuples. */
export interface SessionEvents {
  request: [request: RequestEvent];
  note: [event: NoteEvent];
  pageError: [event: PageErrorEvent];
  moonbug: [ports: MoonbugPortsEvent];
  /** Catch-all: every unprompted event, whatever its name. */
  event: [event: EngineEvent];
  /** An stdout line that was not valid JSON (debugging). */
  raw: [line: string];
  /** The engine process died unexpectedly. */
  exit: [code: number | null, signal: string | null];
  /** The engine process could not be spawned or talked to. */
  error: [error: Error];
}

// ---- protocol helpers -------------------------------------------------------

let _nextId = 1;
const nextId = (): number => _nextId++;

function serialise(cmd: Command): string {
  return JSON.stringify(cmd);
}

// ---- LucynSession -----------------------------------------------------------

/**
 * Typed overloads for the EventEmitter methods, merged into the class below.
 * The first signature wins for every known event, so callbacks are inferred:
 *
 * ```ts
 * session.on("request", (req) => req.status); // req: RequestEvent
 * ```
 */
export interface LucynSession {
  on<K extends keyof SessionEvents>(event: K, listener: (...args: SessionEvents[K]) => void): this;
  on(event: string | symbol, listener: (frame: EngineEvent) => void): this;
  once<K extends keyof SessionEvents>(event: K, listener: (...args: SessionEvents[K]) => void): this;
  once(event: string | symbol, listener: (frame: EngineEvent) => void): this;
  off<K extends keyof SessionEvents>(event: K, listener: (...args: SessionEvents[K]) => void): this;
  off(event: string | symbol, listener: (frame: EngineEvent) => void): this;
  emit<K extends keyof SessionEvents>(event: K, ...args: SessionEvents[K]): boolean;
  emit(event: string | symbol, ...args: unknown[]): boolean;
}

/**
 * A single session wrapping one `lucyn` process.
 * All methods are async — every call crosses a process boundary.
 */
export class LucynSession extends EventEmitter {
  private readonly _moonbugOpts: boolean | MoonbugStartOptions | null;
  /** The Moonbug wiretap, from this client. */
  readonly moonbug: Moonbug;
  private readonly _settleTimeoutMs: number;
  private readonly _console: boolean;
  private readonly _adblock: boolean;
  private readonly _adblockList: string;
  private readonly _headless: boolean;
  private readonly _config: string;
  private readonly _binary: string;
  private readonly _attach: boolean;
  private _proc: ChildProcess | null = null;
  private _sock: Socket | null = null;
  private _attached = false;
  private _rl: Interface | null = null;
  private readonly _pending = new Map<number, PendingEntry>();
  private _started = false;
  private _closed = false;

  constructor(opts: LucynOptions = {}) {
    super();
    this._moonbugOpts = opts.moonbug ?? null;
    this.moonbug = new Moonbug(this);
    this._settleTimeoutMs = opts.settleTimeoutMs ?? 5000;
    this._console = opts.console ?? false;
    this._adblock = opts.adblock ?? true;
    this._adblockList = opts.adblockList ?? "";
    this._headless = opts.headless ?? false;
    this._config = opts.config ?? "";
    this._binary = findBinary(opts.binary);
    this._attach = opts.attach ?? true;
  }

  // ---- lifecycle ------------------------------------------------------------

  /** Start the engine — or hook into one that is already running. Called lazily on first command. */
  private async _ensureStarted(): Promise<void> {
    if (this._started) return;
    this._started = true;

    // An engine that is already up (launched in a terminal, --listen, or by an
    // earlier script) wins over starting a second Chromium: attach to it.
    if (this._attach && (await this._tryAttach())) return;

    const args: string[] = [
      "--settle", String(this._settleTimeoutMs),
    ];
    if (this._console) args.push("--console");
    if (!this._adblock) args.push("--no-adblock");
    if (this._adblockList) args.push("--adblock-list", this._adblockList);
    if (this._headless) args.push("--headless");
    if (this._config) args.push("--config", this._config);
    // The client always asks for capture events: they cost nothing until a listener is attached
    // and a late-attached onRequest() must not miss the very first request.
    args.push("--events");
    const mb = this._moonbugOpts;
    if (mb === true) args.push("--moonbug");
    else if (mb && typeof mb === "object") {
      if (mb.raw !== undefined) args.push("--raw", String(mb.raw));
      if (mb.signed !== undefined) args.push("--signed", String(mb.signed));
      if (mb.dashboard !== undefined) args.push("--dashboard", String(mb.dashboard));
      if (mb.dashboardFile) args.push("--dashboard-file", String(mb.dashboardFile));
    }

    const proc = spawn(this._binary, args, {
      stdio: ["pipe", "pipe", "inherit"], // stdin/stdout piped; stderr inherited
    });
    this._proc = proc;

    proc.on("error", (err: Error) => {
      const msg = `lucyn process error: ${err.message}`;
      this._rejectAll(msg);
      this.emit("error", new Error(msg));
    });

    proc.on("exit", (code: number | null, signal: string | null) => {
      if (!this._closed) {
        const msg = `lucyn process exited unexpectedly (code=${code} signal=${signal})`;
        this._rejectAll(msg);
        this.emit("exit", code, signal);
      }
    });

    if (!proc.stdout) throw new Error("lucyn process has no stdout pipe");
    this._rl = createInterface({ input: proc.stdout, crlfDelay: Infinity });
    this._rl.on("line", (line: string) => this._onLine(line));
  }

  /**
   * Attach to an engine that is already running: find its descriptor
   * (`nothing-data/engine.json` next to the binary), connect to the loopback
   * control socket and verify it speaks the protocol. Resolves false when
   * there is nothing to attach to — the caller then spawns as usual.
   */
  private async _tryAttach(): Promise<boolean> {
    if (process.env.LUCYN_ATTACH === "0") return false;
    // A PATH-resolved bare "lucyn" gives us no folder to look in.
    if (!this._binary.includes("/") && !this._binary.includes("\\")) return false;

    const desc = `${dirname(resolve(this._binary))}/nothing-data/engine.json`;
    let ep: { port?: unknown; host?: unknown } | null = null;
    try {
      if (existsSync(desc)) ep = JSON.parse(readFileSync(desc, "utf8")) as { port?: unknown; host?: unknown };
    } catch {
      ep = null; // stale or garbled descriptor: ignore it
    }
    const port = typeof ep?.port === "number" ? ep.port : 0;
    if (!port) return false;
    const host = typeof ep?.host === "string" ? ep.host : "127.0.0.1";

    const sock = connect({ host, port });
    const connected = await new Promise<boolean>((res) => {
      let settled = false;
      const done = (v: boolean) => { if (!settled) { settled = true; res(v); } };
      sock.setTimeout(1500, () => { sock.destroy(); done(false); });
      sock.once("error", () => done(false));
      sock.once("connect", () => {
        sock.setTimeout(0);   // clear the connect timeout: this socket is long-lived
        done(true);
      });
    });
    if (!connected) return false;

    this._sock = sock;
    this._attached = true;
    this._rl = createInterface({ input: sock, crlfDelay: Infinity });
    this._rl.on("line", (line: string) => this._onLine(line));
    sock.on("error", () => { /* "close" always follows; handled there */ });
    sock.on("close", () => {
      if (this._closed) return;
      this._rejectAll("engine disconnected (the attached engine shut down)");
      this.emit("exit", null, null);
    });

    // Protocol check: a real engine answers `moonbug` with its current ports.
    try {
      this.moonbug._ports = await this._probeAttach();
      return true;
    } catch {
      this._attached = false;
      this._sock = null;
      this._rl = null;
      try { sock.destroy(); } catch { /* ignore */ }
      return false;
    }
  }

  /** Ask the attached engine for its Moonbug ports; times out when it is not an engine. */
  private _probeAttach(): Promise<MoonbugPorts> {
    return new Promise<MoonbugPorts>((res, reject) => {
      const timer = setTimeout(() => {
        this._pending.delete(0);
        reject(new Error("attach probe timed out"));
      }, 2500);
      this._pending.set(0, {
        resolve: (v) => { clearTimeout(timer); res(v as MoonbugPorts); },
        reject: (e) => { clearTimeout(timer); reject(e); },
      });
      this._sock?.write(JSON.stringify({ v: 1, id: 0, cmd: "moonbug" }) + "\n", (err?: Error | null) => {
        if (err) {
          clearTimeout(timer);
          this._pending.delete(0);
          reject(err);
        }
      });
    });
  }

  private _onLine(line: string): void {
    let frame: ReplyFrame;
    try {
      frame = JSON.parse(line) as ReplyFrame;
    } catch {
      // Unparseable line — emit as raw event for debugging.
      this.emit("raw", line);
      return;
    }

    // Event (unprompted — e.g. pageError, note, request, Moonbug ports).
    if (frame.event) {
      if (frame.event === "moonbug") {
        this.moonbug._ports = {
          raw: frame.raw ?? 0,
          signed: frame.signed ?? 0,
          dashboard: frame.dashboard ?? 0,
        };
      }
      const evt = frame as EngineEvent;
      this.emit(frame.event, evt);
      this.emit("event", evt);
      return;
    }

    // Reply to a command.
    const id = frame.id;
    if (id !== undefined && this._pending.has(id)) {
      const pending = this._pending.get(id);
      if (!pending) return;
      this._pending.delete(id);
      if (frame.ok) pending.resolve(frame);
      else pending.reject(new Error(typeof frame.error === "string" ? frame.error : "unknown error from lucyn"));
    }
  }

  private _rejectAll(msg: string): void {
    for (const { reject } of this._pending.values()) {
      reject(new Error(msg));
    }
    this._pending.clear();
  }

  private _sendRaw(cmd: Command): Promise<unknown> {
    return new Promise((resolve, reject) => {
      this._ensureStarted().then(() => {
        if (this._closed) {
          reject(new Error("session is closed"));
          return;
        }
        const id = nextId();
        this._pending.set(id, { resolve, reject });
        const line = serialise({ v: 1, id, ...cmd }) + "\n";
        const sink = this._sock ?? this._proc?.stdin;
        if (!sink) {
          this._pending.delete(id);
          reject(new Error("lucyn engine is not running (no attached socket and no process)"));
          return;
        }
        sink.write(line, (err?: Error | null) => {
          if (err) {
            this._pending.delete(id);
            reject(err);
          }
        });
      }).catch(reject);
    });
  }

  /** Send a command and return its reply, narrowed to `T` by the caller. */
  async _send<T = unknown>(cmd: Command): Promise<T> {
    return (await this._sendRaw(cmd)) as T;
  }

  // ---- commands -------------------------------------------------------------

  /**
   * Navigate to a URL and wait for the page to settle.
   * @param url  http:// or https:// URL
   */
  async visit(url: string, _opts: VisitOptions = {}): Promise<VisitResult> {
    const r = await this._send<VisitResult>({ cmd: "visit", url });
    return { status: r.status, url: r.url, title: r.title };
  }

  /**
   * Open a website in LucyN. This is the same navigation pipeline as visit().
   * @param url  http:// or https:// URL
   */
  async open(url: string, opts?: VisitOptions): Promise<VisitResult> {
    return this.visit(url, opts);
  }

  /**
   * Open a provider's normal search results page in LucyN.
   * @param query  what to search for
   */
  async search(query: string, opts: SearchOptions = {}): Promise<VisitResult> {
    const cmd: Command = { cmd: "search", query };
    if (opts.engine) cmd.engine = opts.engine;
    const r = await this._send<VisitResult>(cmd);
    return { status: r.status, url: r.url, title: r.title };
  }

  /** Return the fully-settled HTML of the current page. */
  async html(): Promise<string> {
    const r = await this._send<{ value: string }>({ cmd: "html" });
    return r.value;
  }

  /**
   * Extract the text content of the first element matching `selector`.
   * Rejects if no match.
   * @param selector  CSS selector
   */
  async extract(selector: string): Promise<string> {
    const r = await this._send<{ value: string }>({ cmd: "extract", selector });
    return r.value;
  }

  /**
   * Extract text content of ALL elements matching `selector`.
   * @param selector  CSS selector
   */
  async extractAll(selector: string): Promise<string[]> {
    const r = await this._send<{ values?: string[] }>({ cmd: "extractAll", selector });
    return r.values ?? [];
  }

  /**
   * Evaluate a JS expression in the live page context (requires a prior visit()).
   * The JS context stays alive after visit() settles (keepAlive mode).
   * @param js  JavaScript expression or statement(s)
   * @returns the result coerced to a string
   */
  async eval(js: string): Promise<string> {
    const r = await this._send<{ value: string }>({ cmd: "eval", js });
    return r.value;
  }

  /**
   * Wait until `selector` matches an element in the live DOM.
   * Keeps pumping timers / jobs until the element appears or the timeout elapses.
   * @param selector  CSS selector
   * @returns the selector (so you can chain)
   */
  async waitFor(selector: string, opts: WaitForOptions = {}): Promise<string> {
    const cmd: Command = { cmd: "waitFor", selector };
    if (opts.timeout !== undefined) cmd.timeout = opts.timeout;
    const r = await this._send<{ value: string }>(cmd);
    return r.value;
  }

  /**
   * Wait until no new network requests have been captured for `quiet` ms.
   * Useful to wait for SPA navigation or lazy image loads to finish.
   */
  async waitIdle(opts: WaitIdleOptions = {}): Promise<void> {
    const cmd: Command = { cmd: "waitIdle" };
    if (opts.quiet !== undefined) cmd.quiet = opts.quiet;
    if (opts.timeout !== undefined) cmd.timeout = opts.timeout;
    await this._send(cmd);
  }

  /**
   * Click the first element matching `selector` in the live DOM.
   * Fires a real `.click()` inside the page JS context and settles briefly.
   * @param selector  CSS selector
   * @returns the clicked element's tagName
   */
  async click(selector: string): Promise<string> {
    const r = await this._send<{ value: string }>({ cmd: "click", selector });
    return r.value;
  }

  /**
   * Type a value into a form field: sets the element's `value` in the DOM, then dispatches
   * bubbling `input` and `change` events (in that order, like a browser), so React/Vue-style
   * listeners pick the change up.
   * @param selector  CSS selector
   * @returns the field element's tagName (lowercase)
   */
  async fill(selector: string, value: string): Promise<string> {
    const r = await this._send<{ tag: string }>({ cmd: "fill", selector, value });
    return r.tag;
  }

  /**
   * Submit a form. Pass the form itself (`form.submit()`, which navigates without firing a
   * submit event — browser semantics) or an input/button inside it (a real click, which runs
   * the form's submit handler first and honours cancellation). Waits for the page to settle
   * and follows the navigation, like visit().
   * @param selector  CSS selector for the form (or a submit input/button in it)
   * @returns the element's tagName (lowercase)
   */
  async submit(selector: string): Promise<string> {
    const r = await this._send<{ tag: string }>({ cmd: "submit", selector });
    return r.tag;
  }

  /**
   * Return current engine info: last URL visited and how many requests Moonbug has captured.
   */
  async info(): Promise<InfoResult> {
    const r = await this._send<InfoResult>({ cmd: "info" });
    return { url: r.url, captures: r.captures };
  }

  /**
   * Cookies in the engine's jar. With a url: exactly what a request to it would carry (HttpOnly cookies
   * included, flagged). Without: the whole jar. The jar is shared by page scripts (document.cookie, which
   * cannot see HttpOnly) and every request the engine makes.
   */
  async cookies(url?: string): Promise<Cookie[]> {
    const r = await this._send<{ cookies: Cookie[] }>(url ? { cmd: "cookies", url } : { cmd: "cookies" });
    return r.cookies;
  }

  /**
   * Put a cookie in the jar (e.g. to restore a logged-in session). Give `domain`, or a `url` to take it from.
   * An `expires` in the past deletes the cookie.
   */
  async setCookie(cookie: SetCookieOptions): Promise<void> {
    await this._send({ cmd: "setCookie", ...cookie });
  }

  /**
   * Download a URL straight into the engine's data folder (`nothing-data/downloads/`). Uses the same cookie
   * jar and Moonbug capture as everything else; the file is written only after a complete 2xx transfer.
   * Nothing is overwritten unless asked: a name that exists becomes "name (1).ext".
   * @param url  http:// or https:// only
   */
  async download(url: string, opts: DownloadOptions = {}): Promise<DownloadResult> {
    const cmd: Command = { cmd: "download", url };
    if (opts.name) cmd.name = opts.name;
    if (opts.overwrite) cmd.overwrite = true;
    const maxBytes = opts.maxBytes;
    if (maxBytes !== undefined && maxBytes > 0) cmd.maxBytes = Math.floor(maxBytes);
    if (opts.referer) cmd.referer = opts.referer;
    const r = await this._send<DownloadResult>(cmd);
    return { path: r.path, name: r.name, bytes: r.bytes, status: r.status, type: r.type, url: r.url };
  }

  /**
   * Render the current page into `nothing-data/screenshots/` and return its path.
   * Pure offscreen rendering: the engine has no window at all.
   * @param name  file name (default "screenshot.png")
   */
  async screenshot(name?: string, opts: ScreenshotOptions = {}): Promise<string> {
    const cmd: Command = { cmd: "screenshot" };
    if (name) cmd.name = name;
    const width = opts.width;
    if (width !== undefined && width > 0) cmd.width = Math.floor(width);
    const r = await this._send<{ path: string }>(cmd);
    return r.path;
  }

  /** Empty the cookie jar. */
  async clearCookies(): Promise<void> {
    await this._send({ cmd: "clearCookies" });
  }

  /**
   * Shut the session down. A session that SPAWNED its engine sends `quit` and
   * lets it exit cleanly (that removes its `engine.json` descriptor), force-
   * killing only a straggler; an ATTACHED session only disconnects — the engine
   * and its window keep running for whoever owns it.
   */
  async close(): Promise<void> {
    if (this._closed) return;
    if (this._attached) {
      this._closed = true;
      try { this._rl?.close(); } catch { /* ignore */ }
      try { this._sock?.end(); } catch { /* ignore */ }
      return;
    }
    try {
      await this._send({ cmd: "quit" });
    } catch {
      // Ignore — process may already be gone.
    }
    this._closed = true;
    if (this._proc) {
      const proc = this._proc;
      proc.stdin?.end();
      // Give the engine a moment to exit on its own (clean quit = descriptor
      // removed), then force-kill only if it is somehow still around.
      await new Promise<void>((r) => setTimeout(r, 400));
      if (proc.exitCode === null && proc.signalCode === null) proc.kill();
    }
  }
}

// ---- Moonbug ----------------------------------------------------------------

/**
 * The wiretap, from JS. LucyN is the HTTP client, so it sees decrypted traffic natively:
 * no MITM certificate, no proxy, no CDP. Reached ONLY through this client or the loopback listeners.
 * Available as `session.moonbug`.
 */
export class Moonbug {
  /** Ports last reported by the engine's `moonbug` event (0 = that listener is off). @internal */
  _ports: MoonbugPorts = { raw: 0, signed: 0, dashboard: 0 };
  private readonly _s: LucynSession;

  constructor(session: LucynSession) {
    this._s = session;
  }

  /**
   * Called for every completed request the engine makes (page loads, scripts, fetch()...).
   * `req` carries { id, method, url, status, ms, bytes, error, sig }. `sig` is NULLBYTE's
   * HMAC-SHA256 seal of the full capture; the same id + sig appear on the signed stream
   * and in the dashboard.
   * @returns unsubscribe function
   */
  onRequest(fn: (req: RequestEvent) => void): () => void {
    this._s.on("request", fn);
    return () => this._s.off("request", fn);
  }

  /** Alias of onRequest: captures are reported once the response (or failure) is in. */
  onResponse(fn: (req: RequestEvent) => void): () => void {
    return this.onRequest(fn);
  }

  /**
   * Start Moonbug listeners (127.0.0.1 only). Each value is a port; 0 = any free port; omit to leave alone.
   * @returns the ports actually bound (0 = off)
   */
  async serve(o: MoonbugServeOptions = { raw: 2005, signed: 2006, dashboard: 6969 }): Promise<MoonbugPorts> {
    const cmd: Command = { cmd: "moonbug" };
    if (o.raw !== undefined) cmd.raw = o.raw;
    if (o.signed !== undefined) cmd.signed = o.signed;
    if (o.dashboard !== undefined) cmd.dashboard = o.dashboard;
    if (o.file) cmd.file = o.file;
    const r = await this._s._send<MoonbugPorts>(cmd);
    return { raw: r.raw, signed: r.signed, dashboard: r.dashboard };
  }

  /**
   * The most recent captures, oldest first. Unlike onRequest events these carry request/response
   * headers (wire order), timing, and a body preview.
   * @param limit  1..500 (default 20)
   */
  async captures(limit = 20): Promise<Capture[]> {
    const r = await this._s._send<{ captures?: Capture[] }>({ cmd: "captures", limit });
    return r.captures ?? [];
  }
}

// ---- Default export ---------------------------------------------------------

/**
 * Create a new LucyN session. Each instance is an isolated browser context
 * (separate process, separate cookie jar).
 */
export default function Lucyn(opts: LucynOptions = {}): LucynSession {
  return new LucynSession(opts);
}

