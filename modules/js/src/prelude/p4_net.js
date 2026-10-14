
// ================================================================================================================================
// Networking: Headers, FormData, Request, Response, fetch, AbortController, XMLHttpRequest, ProgressEvent, TextEncoder/TextDecoder.
// Everything here sits on a few natives (fetch_bridge.cpp):
//   n.httpSend(spec, cb) -> id | n.httpSendSync(spec) | n.httpCancel(id) | n.utf8Encode(str) | n.utf8Decode(bytes)
// Not implemented: streams (response.body is null; Blob/File have no .stream()), CORS enforcement, redirect:'manual'|'error', credentials:'omit'.
// ================================================================================================================================
const EMPTY_BUF = new ArrayBuffer(0);
const TOKEN_RE = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
const toBuffer = (data) => (typeof data === 'string' ? n.utf8Encode(data) : data);
const bufferOfView = (v) => v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength);
const illegal2 = () => new TypeError('Illegal invocation');
const sameOriginUrl = (url) => { const a = parseUrl(url), b = parseUrl(curHref); return !!(a && b && a.origin !== 'null' && a.origin === b.origin); };
const stripFragment = (u) => String(u).replace(/#.*$/, '');

// ---- on* event-handler attributes ----------------------------------------------------------------------------------------------
const HANDLERS = new WeakMap();
const defHandlers = (C, types) => {
  for (const t of types) {
    accessor(C.prototype, 'on' + t, function () { const h = HANDLERS.get(this); return (h && h[t]) || null; },
      function (v) { let h = HANDLERS.get(this); if (!h) { h = Object.create(null); HANDLERS.set(this, h); } h[t] = typeof v === 'function' ? v : null; });
  }
};

// ---- Headers -------------------------------------------------------------------------------------------------------------------
const HDR = new WeakMap();
const hdrList = (h) => { const l = HDR.get(h); if (!l) throw illegal2(); return l; };
const headerValue = (v) => String(v).replace(/^[\t\n\r ]+|[\t\n\r ]+$/g, '');
const headerName = (name, who) => {
  name = String(name);
  if (!TOKEN_RE.test(name)) throw new TypeError("Failed to execute '" + who + "' on 'Headers': Invalid name");
  return name;
};
// iteration order: sorted by lower-cased name, values of one name combined; Set-Cookie stays separate
const sortedHeaderEntries = (h) => {
  const out = []; const byName = new Map();
  for (const e of hdrList(h)) {
    if (e.lc === 'set-cookie') { out.push([e.lc, e.value]); continue; }
    if (byName.has(e.lc)) byName.get(e.lc)[1] += ', ' + e.value; else { const p = [e.lc, e.value]; byName.set(e.lc, p); out.push(p); }
  }
  return out.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
};
// what goes on the wire: original name casing, first-seen order, values of one name combined
const headerPairs = (h) => {
  const out = []; const byName = new Map();
  for (const e of hdrList(h)) {
    if (byName.has(e.lc)) byName.get(e.lc)[1] += ', ' + e.value; else { const p = [e.name, e.value]; byName.set(e.lc, p); out.push(p); }
  }
  return out;
};
class Headers {
  constructor(init) {
    HDR.set(this, []);
    if (init === undefined || init === null) return;
    if (init instanceof Headers) { for (const e of HDR.get(init)) HDR.get(this).push({ name: e.name, lc: e.lc, value: e.value }); return; }
    if (typeof init === 'object' && typeof init[Symbol.iterator] === 'function') {
      for (const pair of init) {
        const p = Array.from(pair);
        if (p.length !== 2) throw new TypeError("Failed to construct 'Headers': Invalid value");
        this.append(p[0], p[1]);
      }
    } else if (typeof init === 'object' || typeof init === 'function') {
      for (const k of Object.keys(init)) this.append(k, init[k]);
    } else {
      throw new TypeError("Failed to construct 'Headers': The provided value is not of type '(record<ByteString, ByteString> or sequence<sequence<ByteString>>)'.");
    }
  }
  append(name, value) { const l = hdrList(this); const nm = headerName(name, 'append'); l.push({ name: nm, lc: nm.toLowerCase(), value: headerValue(value) }); }
  delete(name) { const l = hdrList(this); const lc = headerName(name, 'delete').toLowerCase(); for (let i = l.length - 1; i >= 0; i--) if (l[i].lc === lc) l.splice(i, 1); }
  get(name) { const lc = headerName(name, 'get').toLowerCase(); const v = hdrList(this).filter((e) => e.lc === lc).map((e) => e.value); return v.length ? v.join(', ') : null; }
  getSetCookie() { return hdrList(this).filter((e) => e.lc === 'set-cookie').map((e) => e.value); }
  has(name) { const lc = headerName(name, 'has').toLowerCase(); return hdrList(this).some((e) => e.lc === lc); }
  set(name, value) {
    const l = hdrList(this); const nm = headerName(name, 'set'); const lc = nm.toLowerCase(); const v = headerValue(value);
    let found = false;
    for (let i = 0; i < l.length; i++) {
      if (l[i].lc !== lc) continue;
      if (!found) { l[i].value = v; found = true; } else { l.splice(i, 1); i--; }
    }
    if (!found) l.push({ name: nm, lc, value: v });
  }
  forEach(cb, thisArg) {
    if (typeof cb !== 'function') throw new TypeError("Failed to execute 'forEach' on 'Headers': parameter 1 is not of type 'Function'.");
    for (const [k, v] of sortedHeaderEntries(this)) cb.call(thisArg, v, k, this);
  }
  entries() { return sortedHeaderEntries(this)[Symbol.iterator](); }
  keys() { return sortedHeaderEntries(this).map((e) => e[0])[Symbol.iterator](); }
  values() { return sortedHeaderEntries(this).map((e) => e[1])[Symbol.iterator](); }
}
markClass(Headers, 'Headers');
defProp(Headers.prototype, Symbol.iterator, { value: Headers.prototype.entries, writable: true, configurable: true });

// ---- Blob / File (in-memory; no streams: arrayBuffer()/text()/bytes() resolve immediately) -------------------------------------
const BLOB = new WeakMap();
const blobState = (b) => { const s = BLOB.get(b); if (!s) throw illegal2(); return s; };
// Blob's "type" algorithm: ASCII only, lower-cased; anything else (non-ASCII, bad structure) collapses to "".
const normalizeBlobType = (t) => {
  t = String(t);
  for (let i = 0; i < t.length; i++) { const c = t.charCodeAt(i); if (c < 0x20 || c > 0x7e) return ''; }
  return t.toLowerCase();
};
const concatBytes = (chunks) => {
  let total = 0;
  for (const c of chunks) total += c.byteLength;
  const out = new Uint8Array(total);
  let off = 0;
  for (const c of chunks) { out.set(c instanceof Uint8Array ? c : new Uint8Array(c), off); off += c.byteLength; }
  return out.buffer;
};
const blobPartBytes = (part) => {
  if (part instanceof Blob) return new Uint8Array(blobState(part).buf);
  if (part instanceof ArrayBuffer) return new Uint8Array(part);
  if (ArrayBuffer.isView(part)) return new Uint8Array(bufferOfView(part));
  return new Uint8Array(n.utf8Encode(String(part)));
};
const newBlob = (buf, type) => { const b = Object.create(Blob.prototype); BLOB.set(b, { buf, type }); return b; };
class Blob {
  constructor(parts, options) {
    const chunks = [];
    if (parts !== undefined && parts !== null) {
      if (typeof parts[Symbol.iterator] !== 'function') throw new TypeError("Failed to construct 'Blob': The provided value cannot be converted to a sequence.");
      for (const p of parts) chunks.push(blobPartBytes(p));
    }
    const type = options && options.type !== undefined ? normalizeBlobType(options.type) : '';
    BLOB.set(this, { buf: concatBytes(chunks), type });
  }
  get size() { return blobState(this).buf.byteLength; }
  get type() { return blobState(this).type; }
  slice(start, end, contentType) {
    const s = blobState(this), len = s.buf.byteLength;
    let a = start === undefined ? 0 : Number(start), b = end === undefined ? len : Number(end);
    a = a < 0 ? Math.max(len + a, 0) : Math.min(a, len);
    b = b < 0 ? Math.max(len + b, 0) : Math.min(b, len);
    return newBlob(b > a ? s.buf.slice(a, b) : new ArrayBuffer(0), contentType === undefined ? '' : normalizeBlobType(contentType));
  }
  arrayBuffer() { return Promise.resolve(blobState(this).buf.slice(0)); }
  bytes() { return Promise.resolve(new Uint8Array(blobState(this).buf.slice(0))); }
  text() { try { return Promise.resolve(n.utf8Decode(blobState(this).buf)); } catch (e) { return Promise.reject(e); } }
}
markClass(Blob, 'Blob');

const FILE = new WeakMap();
class File extends Blob {
  constructor(parts, name, options) {
    if (arguments.length < 2) throw new TypeError("Failed to construct 'File': 2 arguments required, but only " + arguments.length + ' present.');
    super(parts, options);
    FILE.set(this, { name: String(name), lastModified: options && options.lastModified !== undefined ? Number(options.lastModified) : Date.now() });
  }
  get name() { return FILE.get(this).name; }
  get lastModified() { return FILE.get(this).lastModified; }
  get webkitRelativePath() { return ''; }
}
markClass(File, 'File');

// ---- FormData (strings, Blob and File parts) ------------------------------------------------------------------------------------
const FDATA = new WeakMap();
const fdList = (f) => { const l = FDATA.get(f); if (!l) throw illegal2(); return l; };
const collectForm = (form, out) => {
  const els = form.querySelectorAll('input, select, textarea');
  for (let i = 0; i < els.length; i++) {
    const el = els[i];
    const name = el.getAttribute('name');
    if (!name || el.hasAttribute('disabled')) continue;
    const tag = el.tagName.toLowerCase();
    if (tag === 'input') {
      const type = (el.getAttribute('type') || 'text').toLowerCase();
      if (type === 'submit' || type === 'button' || type === 'reset' || type === 'image' || type === 'file') continue;
      if (type === 'checkbox' || type === 'radio') { if (!el.checked) continue; out.push([name, el.hasAttribute('value') ? el.getAttribute('value') : 'on']); continue; }
      out.push([name, String(el.value === undefined ? '' : el.value)]);
    } else if (tag === 'select') {
      const opts = el.querySelectorAll('option'); let any = false;
      for (let j = 0; j < opts.length; j++) if (opts[j].selected) { any = true; out.push([name, String(opts[j].value)]); if (!el.hasAttribute('multiple')) break; }
      if (!any && !el.hasAttribute('multiple') && opts.length) out.push([name, String(opts[0].value)]);
    } else {
      out.push([name, String(el.value === undefined ? '' : el.value)]);
    }
  }
};
// A Blob value becomes a File entry (default filename "blob"); a File keeps its own name unless overridden.
const fdEntryValue = (value, filename) => {
  if (!(value instanceof Blob)) return String(value);
  const fname = filename !== undefined ? String(filename) : (value instanceof File ? value.name : 'blob');
  if (value instanceof File && filename === undefined) return value;
  return new File([value], fname, { type: value.type, lastModified: value instanceof File ? value.lastModified : undefined });
};
class FormData {
  constructor(form) {
    FDATA.set(this, []);
    if (form === undefined || form === null) return;
    if (typeof form.querySelectorAll !== 'function') throw new TypeError("Failed to construct 'FormData': parameter 1 is not of type 'HTMLFormElement'.");
    collectForm(form, FDATA.get(this));
  }
  append(name, value, filename) { fdList(this).push([String(name), fdEntryValue(value, filename)]); }
  delete(name) { const l = fdList(this); name = String(name); for (let i = l.length - 1; i >= 0; i--) if (l[i][0] === name) l.splice(i, 1); }
  get(name) { name = String(name); const e = fdList(this).find((x) => x[0] === name); return e ? e[1] : null; }
  getAll(name) { name = String(name); return fdList(this).filter((x) => x[0] === name).map((x) => x[1]); }
  has(name) { name = String(name); return fdList(this).some((x) => x[0] === name); }
  set(name, value, filename) {
    const l = fdList(this); name = String(name); const v = fdEntryValue(value, filename); let found = false;
    for (let i = 0; i < l.length; i++) {
      if (l[i][0] !== name) continue;
      if (!found) { l[i][1] = v; found = true; } else { l.splice(i, 1); i--; }
    }
    if (!found) l.push([name, v]);
  }
  forEach(cb, thisArg) { for (const [k, v] of fdList(this).slice()) cb.call(thisArg, v, k, this); }
  entries() { return fdList(this).map((e) => [e[0], e[1]])[Symbol.iterator](); }
  keys() { return fdList(this).map((e) => e[0])[Symbol.iterator](); }
  values() { return fdList(this).map((e) => e[1])[Symbol.iterator](); }
}
markClass(FormData, 'FormData');
defProp(FormData.prototype, Symbol.iterator, { value: FormData.prototype.entries, writable: true, configurable: true });
const formBoundary = () => {
  const gecko = !!(SF && SF.userAgent && /Firefox\//.test(SF.userAgent));
  const hex = '0123456789abcdef', alnum = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
  let s = gecko ? '----geckoformboundary' : '----WebKitFormBoundary';
  const set = gecko ? hex : alnum; const len = gecko ? 32 : 16;
  for (let i = 0; i < len; i++) s += set[Math.floor(Math.random() * set.length)];
  return s;
};
// Builds the multipart body as bytes (not a string) so File/Blob parts carry their raw bytes untouched.
const serializeForm = (fd, boundary) => {
  const esc = (s) => s.replace(/\r/g, '%0D').replace(/\n/g, '%0A').replace(/"/g, '%22');
  const chunks = [];
  for (const [k, v] of fdList(fd)) {
    chunks.push(n.utf8Encode('--' + boundary + '\r\n'));
    if (v instanceof File) {
      chunks.push(n.utf8Encode('Content-Disposition: form-data; name="' + esc(k) + '"; filename="' + esc(v.name) + '"\r\nContent-Type: ' + (v.type || 'application/octet-stream') + '\r\n\r\n'));
      chunks.push(blobState(v).buf);
      chunks.push(n.utf8Encode('\r\n'));
    } else {
      chunks.push(n.utf8Encode('Content-Disposition: form-data; name="' + esc(k) + '"\r\n\r\n' + v.replace(/\r\n|\r|\n/g, '\r\n') + '\r\n'));
    }
  }
  chunks.push(n.utf8Encode('--' + boundary + '--\r\n'));
  return concatBytes(chunks);
};

// ---- request/response bodies ---------------------------------------------------------------------------------------------------
// -> { data: string | ArrayBuffer, type: Content-Type to default to (or null) }
const extractBody = (body) => {
  if (typeof body === 'string') return { data: body, type: 'text/plain;charset=UTF-8' };
  if (body instanceof Blob) return { data: blobState(body).buf.slice(0), type: blobState(body).type || null };
  if (body instanceof URLSearchParams) return { data: body.toString(), type: 'application/x-www-form-urlencoded;charset=UTF-8' };
  if (body instanceof FormData) { const b = formBoundary(); return { data: serializeForm(body, b), type: 'multipart/form-data; boundary=' + b }; }
  if (body instanceof ArrayBuffer) return { data: body.slice(0), type: null };
  if (ArrayBuffer.isView(body)) return { data: bufferOfView(body), type: null };
  return { data: String(body), type: 'text/plain;charset=UTF-8' };
};
const bodyBytes = (d) => (typeof d === 'string' ? n.utf8Encode(d) : d);

// ---- Response ------------------------------------------------------------------------------------------------------------------
const RESP = new WeakMap();
const respState = (r) => { const s = RESP.get(r); if (!s) throw illegal2(); return s; };
const takeRespBody = (r, who) => {
  const s = respState(r);
  if (s.used) throw new TypeError("Failed to execute '" + who + "' on 'Response': body stream already read");
  if (s.body) s.used = true;
  return s.body || EMPTY_BUF;
};
const newResponse = (st) => { const r = Object.create(Response.prototype); RESP.set(r, st); return r; };
class Response {
  constructor(body, init) {
    init = (init === undefined || init === null) ? {} : init;
    if (typeof init !== 'object') throw new TypeError("Failed to construct 'Response': The provided value is not of type 'ResponseInit'.");
    const status = init.status === undefined ? 200 : Number(init.status);
    if (!Number.isInteger(status) || status < 200 || status > 599)
      throw new RangeError("Failed to construct 'Response': The status provided (" + init.status + ") is outside the range [200, 599].");
    const headers = new Headers(init.headers);
    let buf = null;
    if (body !== undefined && body !== null) {
      if (status === 204 || status === 205 || status === 304) throw new TypeError("Failed to construct 'Response': Response with null body status cannot have body");
      const ex = extractBody(body);
      buf = toBuffer(ex.data);
      if (ex.type && !headers.has('content-type')) headers.set('content-type', ex.type);
    }
    RESP.set(this, { status, statusText: init.statusText === undefined ? '' : String(init.statusText), headers, url: init.url ? String(init.url) : '',
      redirected: false, type: 'default', body: buf, used: false });
  }
  get status() { return respState(this).status; }
  get statusText() { return respState(this).statusText; }
  get ok() { const s = respState(this).status; return s >= 200 && s <= 299; }
  get headers() { return respState(this).headers; }
  get url() { return respState(this).url; }
  get redirected() { return respState(this).redirected; }
  get type() { return respState(this).type; }
  get body() { respState(this); return null; }   // streams are not implemented
  get bodyUsed() { return respState(this).used; }
  text() { try { return Promise.resolve(n.utf8Decode(takeRespBody(this, 'text'))); } catch (e) { return Promise.reject(e); } }
  json() { try { return Promise.resolve(JSON.parse(n.utf8Decode(takeRespBody(this, 'json')))); } catch (e) { return Promise.reject(e); } }
  arrayBuffer() { try { return Promise.resolve(takeRespBody(this, 'arrayBuffer').slice(0)); } catch (e) { return Promise.reject(e); } }
  blob() { try { return Promise.resolve(newBlob(takeRespBody(this, 'blob').slice(0), normalizeBlobType(respState(this).headers.get('content-type') || ''))); } catch (e) { return Promise.reject(e); } }
  bytes() { try { return Promise.resolve(new Uint8Array(takeRespBody(this, 'bytes').slice(0))); } catch (e) { return Promise.reject(e); } }
  clone() {
    const s = respState(this);
    if (s.used) throw new TypeError("Failed to execute 'clone' on 'Response': Response body is already used");
    return newResponse({ status: s.status, statusText: s.statusText, headers: new Headers(s.headers), url: s.url, redirected: s.redirected,
      type: s.type, body: s.body ? s.body.slice(0) : null, used: false });
  }
}
markClass(Response, 'Response');
method(Response, 'error', function () { return newResponse({ status: 0, statusText: '', headers: new Headers(), url: '', redirected: false, type: 'error', body: null, used: false }); });
method(Response, 'json', function (data, init) {
  const text = JSON.stringify(data);
  if (text === undefined) throw new TypeError("Failed to execute 'json' on 'Response': The data is not JSON serializable");
  const headers = new Headers(init && init.headers);
  if (!headers.has('content-type')) headers.set('content-type', 'application/json');   // not the string default, text/plain
  return new Response(text, Object.assign({}, init, { headers }));
});
method(Response, 'redirect', function (url, status) {
  status = status === undefined ? 302 : Number(status);
  if (![301, 302, 303, 307, 308].includes(status)) throw new RangeError("Failed to execute 'redirect' on 'Response': Invalid status code");
  const r = new Response(null, { status });
  r.headers.set('location', n.resolveUrl(curHref, String(url)));
  return r;
});

// ---- AbortController / AbortSignal ---------------------------------------------------------------------------------------------
const SIGS = new WeakMap();
let makingSignal = false;
const sigState = (s) => { const x = SIGS.get(s); if (!x) throw illegal2(); return x; };
class AbortSignal extends EventTarget {
  constructor() {
    if (!makingSignal) throw new TypeError('Illegal constructor');
    super();
    SIGS.set(this, { aborted: false, reason: undefined });
  }
  get aborted() { return sigState(this).aborted; }
  get reason() { return sigState(this).reason; }
  throwIfAborted() { const s = sigState(this); if (s.aborted) throw s.reason; }
}
markClass(AbortSignal, 'AbortSignal');
defHandlers(AbortSignal, ['abort']);
const newSignal = () => { makingSignal = true; try { return new AbortSignal(); } finally { makingSignal = false; } };
const fireAbort = (sig, reason) => {
  const s = SIGS.get(sig);
  if (!s || s.aborted) return;
  s.aborted = true;
  s.reason = reason === undefined ? new DOMException('signal is aborted without reason', 'AbortError') : reason;
  sig.dispatchEvent(new Event('abort'));
};
method(AbortSignal, 'abort', function (reason) { const s = newSignal(); fireAbort(s, reason); return s; });
method(AbortSignal, 'timeout', function (ms) { const s = newSignal(); window.setTimeout(() => fireAbort(s, new DOMException('signal timed out', 'TimeoutError')), Number(ms) || 0); return s; });
method(AbortSignal, 'any', function (signals) {
  const out = newSignal();
  const list = Array.from(signals);
  for (const s of list) if (s.aborted) { fireAbort(out, s.reason); return out; }
  for (const s of list) s.addEventListener('abort', () => fireAbort(out, s.reason), { once: true });
  return out;
});
const CTRL = new WeakMap();
class AbortController {
  constructor() { CTRL.set(this, newSignal()); }
  get signal() { const s = CTRL.get(this); if (!s) throw illegal2(); return s; }
  abort(reason) { fireAbort(this.signal, reason); }
}
markClass(AbortController, 'AbortController');

// ---- Request -------------------------------------------------------------------------------------------------------------------
const REQ = new WeakMap();
const reqState = (r) => { const s = REQ.get(r); if (!s) throw illegal2(); return s; };
const STD_METHODS = ['DELETE', 'GET', 'HEAD', 'OPTIONS', 'POST', 'PUT'];
const takeReqBody = (r, who) => {
  const s = reqState(r);
  if (s.used) throw new TypeError("Failed to execute '" + who + "' on 'Request': body stream already read");
  if (s.body) s.used = true;
  return s.body ? bodyBytes(s.body.data) : EMPTY_BUF;
};
class Request {
  constructor(input, init) {
    init = (init === undefined || init === null) ? {} : init;
    const src = input instanceof Request ? REQ.get(input) : null;
    if (src && src.used) throw new TypeError("Failed to construct 'Request': Cannot construct a Request with a Request object that has already been used.");
    const url = src ? src.url : n.resolveUrl(curHref, String(input));
    const pu = parseUrl(url);
    if (!/^data:/i.test(url) && (!pu || (pu.protocol !== 'http:' && pu.protocol !== 'https:' && pu.protocol !== 'file:')))
      throw new TypeError("Failed to construct 'Request': Failed to parse URL from " + String(input));
    let method = init.method !== undefined ? String(init.method) : (src ? src.method : 'GET');
    if (!TOKEN_RE.test(method)) throw new TypeError("Failed to construct 'Request': '" + method + "' is not a valid HTTP method.");
    const up = method.toUpperCase();
    if (up === 'CONNECT' || up === 'TRACE' || up === 'TRACK') throw new TypeError("Failed to construct 'Request': '" + method + "' HTTP method is unsupported.");
    if (STD_METHODS.includes(up)) method = up;
    const headers = init.headers !== undefined ? new Headers(init.headers) : new Headers(src ? src.headers : undefined);
    let body = src ? src.body : null;
    if (init.body !== undefined) body = init.body === null ? null : extractBody(init.body);
    if (body && (method === 'GET' || method === 'HEAD')) throw new TypeError("Failed to construct 'Request': Request with GET/HEAD method cannot have body.");
    if (body && body.type && !headers.has('content-type')) headers.set('content-type', body.type);
    let signal = src ? src.signal : null;
    if (init.signal !== undefined) {
      if (init.signal !== null && !(init.signal instanceof AbortSignal)) throw new TypeError("Failed to construct 'Request': member signal is not of type AbortSignal.");
      signal = init.signal;
    }
    const pick = (k, d) => (init[k] !== undefined ? String(init[k]) : (src ? src[k] : d));
    REQ.set(this, { url, method, headers, body, signal, credentials: pick('credentials', 'same-origin'), mode: pick('mode', 'cors'), cache: pick('cache', 'default'),
      redirect: pick('redirect', 'follow'), referrer: pick('referrer', 'about:client'), referrerPolicy: pick('referrerPolicy', ''), integrity: pick('integrity', ''),
      keepalive: init.keepalive !== undefined ? !!init.keepalive : (src ? src.keepalive : false), used: false });
  }
  get url() { return reqState(this).url; }
  get method() { return reqState(this).method; }
  get headers() { return reqState(this).headers; }
  get credentials() { return reqState(this).credentials; }
  get mode() { return reqState(this).mode; }
  get cache() { return reqState(this).cache; }
  get redirect() { return reqState(this).redirect; }
  get referrer() { return reqState(this).referrer; }
  get referrerPolicy() { return reqState(this).referrerPolicy; }
  get integrity() { return reqState(this).integrity; }
  get keepalive() { return reqState(this).keepalive; }
  get destination() { reqState(this); return ''; }
  get signal() { const s = reqState(this); if (!s.signal) s.signal = newSignal(); return s.signal; }
  get body() { reqState(this); return null; }
  get bodyUsed() { return reqState(this).used; }
  text() { try { return Promise.resolve(n.utf8Decode(takeReqBody(this, 'text'))); } catch (e) { return Promise.reject(e); } }
  json() { try { return Promise.resolve(JSON.parse(n.utf8Decode(takeReqBody(this, 'json')))); } catch (e) { return Promise.reject(e); } }
  arrayBuffer() { try { return Promise.resolve(takeReqBody(this, 'arrayBuffer').slice(0)); } catch (e) { return Promise.reject(e); } }
  blob() { try { return Promise.resolve(newBlob(takeReqBody(this, 'blob').slice(0), normalizeBlobType(reqState(this).headers.get('content-type') || ''))); } catch (e) { return Promise.reject(e); } }
  clone() { reqState(this); return new Request(this); }
}
markClass(Request, 'Request');

// ---- fetch ---------------------------------------------------------------------------------------------------------------------
const dataUrlResponse = (url) => {
  try {
    const m = /^data:([^,]*?)(;base64)?,([\s\S]*)$/i.exec(url);
    if (!m) return null;
    let buf;
    if (m[2]) { const bin = atob(decodeURIComponent(m[3])); const u = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) u[i] = bin.charCodeAt(i); buf = u.buffer; }
    else buf = n.utf8Encode(decodeURIComponent(m[3]));
    return newResponse({ status: 200, statusText: 'OK', headers: new Headers({ 'content-type': m[1] || 'text/plain;charset=US-ASCII' }), url, redirected: false, type: 'basic', body: buf, used: false });
  } catch (e) { return null; }
};
const fetchResponse = (res, st) => {
  const headers = new Headers();
  for (const [k, v] of res.headers) { if (/^set-cookie2?$/i.test(k)) continue; try { headers.append(k, v); } catch (e) { /* a header name we cannot represent */ } }
  const nullBody = st.method === 'HEAD' || res.status === 101 || res.status === 204 || res.status === 205 || res.status === 304;
  return newResponse({ status: res.status, statusText: res.statusText, headers, url: stripFragment(res.url), redirected: !!res.redirected,
    type: sameOriginUrl(res.url) ? 'basic' : 'cors', body: nullBody ? null : res.body, used: false });
};
function fetch(input, init) {
  return new Promise((resolve, reject) => {
    let req;
    try { req = new Request(input, init); } catch (e) { reject(e); return; }
    const st = REQ.get(req);
    const sig = st.signal;
    if (sig && sig.aborted) { reject(sig.reason); return; }
    if (/^data:/i.test(st.url)) { const r = dataUrlResponse(st.url); if (r) resolve(r); else reject(new TypeError('Failed to fetch')); return; }
    if ((st.mode === 'same-origin' && !sameOriginUrl(st.url)) || !n.httpAvailable) { reject(new TypeError('Failed to fetch')); return; }
    let done = false, id = 0;
    const onAbort = () => { if (done) return; done = true; if (id) n.httpCancel(id); reject(sig.reason); };
    if (sig) sig.addEventListener('abort', onAbort, { once: true });
    try {
      id = n.httpSend({ method: st.method, url: st.url, body: st.body ? st.body.data : undefined, headers: headerPairs(st.headers),
        pageUrl: curHref, mode: st.mode === 'no-cors' ? 'no-cors' : 'cors', dest: 'empty' }, (res) => {
        if (done) return;
        done = true;
        if (sig) sig.removeEventListener('abort', onAbort);
        if (res.error) { reject(new TypeError('Failed to fetch')); return; }
        resolve(fetchResponse(res, st));
      });
    } catch (e) {
      done = true;
      reject(new TypeError('Failed to fetch'));
    }
  });
}
mark(fetch, 'fetch');

// ---- ProgressEvent + XMLHttpRequest --------------------------------------------------------------------------------------------
const ProgressEvent = mk('ProgressEvent', Event, { lengthComputable: false, loaded: 0, total: 0 });
class XMLHttpRequestEventTarget extends EventTarget {}
markClass(XMLHttpRequestEventTarget, 'XMLHttpRequestEventTarget');
defHandlers(XMLHttpRequestEventTarget, ['loadstart', 'progress', 'abort', 'error', 'load', 'timeout', 'loadend']);
const makeUpload = () => { const u = Object.create(XMLHttpRequestUpload.prototype); return u; };
class XMLHttpRequestUpload extends XMLHttpRequestEventTarget {
  constructor() { throw new TypeError('Illegal constructor'); }
}
markClass(XMLHttpRequestUpload, 'XMLHttpRequestUpload');

const XHR = new WeakMap();
const xs = (x) => { const s = XHR.get(x); if (!s) throw illegal2(); return s; };
const FORBIDDEN_REQ_HEADERS = new Set(['accept-charset', 'accept-encoding', 'access-control-request-headers', 'access-control-request-method', 'connection',
  'content-length', 'cookie', 'cookie2', 'date', 'dnt', 'expect', 'host', 'keep-alive', 'origin', 'referer', 'set-cookie', 'te', 'trailer',
  'transfer-encoding', 'upgrade', 'via']);
const forbiddenHeader = (lc) => FORBIDDEN_REQ_HEADERS.has(lc) || lc.startsWith('proxy-') || lc.startsWith('sec-');
const xhrFire = (target, type, loaded, total) => {
  if (type === 'readystatechange') target.dispatchEvent(new Event(type));
  else target.dispatchEvent(new ProgressEvent(type, { lengthComputable: total > 0, loaded: loaded || 0, total: total || 0 }));
};
const xhrClearTimer = (s) => { if (s.timer) { window.clearTimeout(s.timer); s.timer = 0; } };
const xhrCancel = (s) => { if (s.id) { n.httpCancel(s.id); s.id = 0; } xhrClearTimer(s); };
const bodyLen = (d) => (d === undefined || d === null ? 0 : (typeof d === 'string' ? n.utf8Encode(d).byteLength : d.byteLength));
const xhrContentType = (s) => { const h = s.respHeaders.find((x) => x[0].toLowerCase() === 'content-type'); return h ? normalizeBlobType(h[1]) : ''; };

// A request that ends without a response: 'error' | 'timeout' | 'abort'. Async requests report it through events,
// synchronous ones throw from send().
const xhrFail = (x, s, kind) => {
  const t = ++s.token;
  xhrCancel(s);
  s.sendFlag = false; s.errorFlag = true; s.readyState = 4;
  s.status = 0; s.statusText = ''; s.respHeaders = []; s.body = null; s.responseURL = ''; s.cache = {};
  if (!s.async) return;
  xhrFire(x, 'readystatechange'); if (s.token !== t) return;
  if (s.uploadLen >= 0) { xhrFire(s.upload, kind, 0, 0); xhrFire(s.upload, 'loadend', 0, 0); }
  xhrFire(x, kind, 0, 0); if (s.token !== t) return;
  xhrFire(x, 'loadend', 0, 0);
};
const xhrSucceed = (x, s, res, token) => {
  xhrClearTimer(s);
  s.id = 0;
  s.status = res.status; s.statusText = res.statusText;
  s.respHeaders = res.headers.filter((h) => !/^set-cookie2?$/i.test(h[0]));
  s.responseURL = stripFragment(res.url);
  s.body = res.body; s.cache = {};
  const len = res.body ? res.body.byteLength : 0;
  if (s.uploadLen >= 0) { xhrFire(s.upload, 'progress', s.uploadLen, s.uploadLen); xhrFire(s.upload, 'load', s.uploadLen, s.uploadLen); xhrFire(s.upload, 'loadend', s.uploadLen, s.uploadLen); }
  if (s.async) {
    s.readyState = 2; xhrFire(x, 'readystatechange'); if (s.token !== token) return;
    s.readyState = 3; xhrFire(x, 'readystatechange'); if (s.token !== token) return;
    xhrFire(x, 'progress', len, len); if (s.token !== token) return;
  }
  s.sendFlag = false; s.readyState = 4;
  xhrFire(x, 'readystatechange'); if (s.token !== token) return;
  xhrFire(x, 'load', len, len); if (s.token !== token) return;
  xhrFire(x, 'loadend', len, len);
};

class XMLHttpRequest extends XMLHttpRequestEventTarget {
  constructor() {
    super();
    XHR.set(this, { readyState: 0, method: 'GET', url: '', async: true, headers: new Headers(), user: undefined, password: undefined, sendFlag: false,
      errorFlag: false, status: 0, statusText: '', respHeaders: [], body: null, responseURL: '', responseType: '', timeout: 0, withCredentials: false,
      upload: makeUpload(), id: 0, timer: 0, token: 0, cache: {}, uploadLen: -1 });
  }
  get readyState() { return xs(this).readyState; }
  get status() { return xs(this).status; }
  get statusText() { return xs(this).statusText; }
  get responseURL() { return xs(this).responseURL; }
  get upload() { return xs(this).upload; }
  get withCredentials() { return xs(this).withCredentials; }
  set withCredentials(v) {
    const s = xs(this);
    if ((s.readyState !== 0 && s.readyState !== 1) || s.sendFlag) throw new DOMException("Failed to set the 'withCredentials' property on 'XMLHttpRequest': The value may only be set if the object's state is UNSENT or OPENED.", 'InvalidStateError');
    s.withCredentials = !!v;
  }
  get timeout() { return xs(this).timeout; }
  set timeout(v) {
    const s = xs(this);
    if (!s.async && s.readyState >= 1) throw new DOMException("Failed to set the 'timeout' property on 'XMLHttpRequest': Timeouts cannot be set for synchronous requests made from a document.", 'InvalidAccessError');
    s.timeout = Math.max(0, Math.floor(Number(v)) || 0);
  }
  get responseType() { return xs(this).responseType; }
  set responseType(v) {
    const s = xs(this);
    v = String(v);
    if (!['', 'arraybuffer', 'blob', 'document', 'json', 'text'].includes(v)) return;
    if (s.readyState === 3 || s.readyState === 4) throw new DOMException("Failed to set the 'responseType' property on 'XMLHttpRequest': The response type cannot be set if the object's state is LOADING or DONE.", 'InvalidStateError');
    if (!s.async && s.readyState >= 1) throw new DOMException("Failed to set the 'responseType' property on 'XMLHttpRequest': The response type cannot be changed for synchronous requests made from a document.", 'InvalidAccessError');
    s.responseType = v;
  }
  get responseText() {
    const s = xs(this);
    if (s.responseType !== '' && s.responseType !== 'text')
      throw new DOMException("Failed to read the 'responseText' property from 'XMLHttpRequest': The value is only accessible if the object's 'responseType' is '' or 'text' (was '" + s.responseType + "').", 'InvalidStateError');
    if (s.errorFlag || (s.readyState !== 3 && s.readyState !== 4) || !s.body) return '';
    if (s.cache.text === undefined) s.cache.text = n.utf8Decode(s.body);
    return s.cache.text;
  }
  get responseXML() {
    const s = xs(this);
    if (s.responseType !== '' && s.responseType !== 'document')
      throw new DOMException("Failed to read the 'responseXML' property from 'XMLHttpRequest': The value is only accessible if the object's 'responseType' is '' or 'document' (was '" + s.responseType + "').", 'InvalidStateError');
    return null;   // documents are not parsed
  }
  get response() {
    const s = xs(this);
    if (s.responseType === '' || s.responseType === 'text') return this.responseText;
    if (s.readyState !== 4 || s.errorFlag || !s.body) {
      if (s.readyState !== 4 || s.errorFlag) return null;
      if (s.responseType === 'arraybuffer') return new ArrayBuffer(0);
      if (s.responseType === 'blob') return newBlob(new ArrayBuffer(0), xhrContentType(s));
      return null;
    }
    if (s.responseType === 'arraybuffer') { if (!s.cache.ab) s.cache.ab = s.body.slice(0); return s.cache.ab; }
    if (s.responseType === 'blob') { if (!s.cache.blob) s.cache.blob = newBlob(s.body.slice(0), xhrContentType(s)); return s.cache.blob; }
    if (s.responseType === 'json') {
      if (!('json' in s.cache)) { try { s.cache.json = JSON.parse(n.utf8Decode(s.body)); } catch (e) { s.cache.json = null; } }
      return s.cache.json;
    }
    return null;   // 'document' is not supported
  }
  open(method, url, async, user, password) {
    const s = xs(this);
    if (arguments.length < 2) throw new TypeError("Failed to execute 'open' on 'XMLHttpRequest': 2 arguments required, but only " + arguments.length + ' present.');
    method = String(method);
    if (!TOKEN_RE.test(method)) throw new DOMException("Failed to execute 'open' on 'XMLHttpRequest': '" + method + "' is not a valid HTTP method.", 'SyntaxError');
    const up = method.toUpperCase();
    if (up === 'CONNECT' || up === 'TRACE' || up === 'TRACK') throw new DOMException("Failed to execute 'open' on 'XMLHttpRequest': '" + method + "' HTTP method is unsupported.", 'SecurityError');
    if (STD_METHODS.includes(up)) method = up;
    const full = n.resolveUrl(curHref, String(url));
    const pu = parseUrl(full);
    if (!pu || (pu.protocol !== 'http:' && pu.protocol !== 'https:' && pu.protocol !== 'file:'))
      throw new DOMException("Failed to execute 'open' on 'XMLHttpRequest': Invalid URL", 'SyntaxError');
    s.token++; xhrCancel(s);
    s.method = method; s.url = full; s.async = arguments.length >= 3 ? !!async : true;
    s.user = user === undefined ? undefined : (user === null ? undefined : String(user));
    s.password = password === undefined || password === null ? undefined : String(password);
    s.headers = new Headers(); s.sendFlag = false; s.errorFlag = false; s.status = 0; s.statusText = ''; s.respHeaders = []; s.body = null;
    s.responseURL = ''; s.cache = {}; s.uploadLen = -1;
    if (s.readyState !== 1) { s.readyState = 1; xhrFire(this, 'readystatechange'); } else s.readyState = 1;
  }
  setRequestHeader(name, value) {
    const s = xs(this);
    if (arguments.length < 2) throw new TypeError("Failed to execute 'setRequestHeader' on 'XMLHttpRequest': 2 arguments required, but only " + arguments.length + ' present.');
    if (s.readyState !== 1 || s.sendFlag) throw new DOMException("Failed to execute 'setRequestHeader' on 'XMLHttpRequest': The object's state must be OPENED.", 'InvalidStateError');
    name = String(name);
    if (!TOKEN_RE.test(name)) throw new DOMException("Failed to execute 'setRequestHeader' on 'XMLHttpRequest': '" + name + "' is not a valid HTTP header field name.", 'SyntaxError');
    if (forbiddenHeader(name.toLowerCase())) return;   // browsers refuse these silently (a console message only)
    s.headers.append(name, value);
  }
  overrideMimeType(mime) {
    const s = xs(this);
    if (s.readyState === 3 || s.readyState === 4) throw new DOMException("Failed to execute 'overrideMimeType' on 'XMLHttpRequest': MimeType cannot be overridden when the state is LOADING or DONE.", 'InvalidStateError');
    String(mime);
  }
  send(body) {
    const s = xs(this);
    if (s.readyState !== 1 || s.sendFlag) throw new DOMException("Failed to execute 'send' on 'XMLHttpRequest': The object's state must be OPENED.", 'InvalidStateError');
    let ex = null;
    if (s.method !== 'GET' && s.method !== 'HEAD' && body !== undefined && body !== null) ex = extractBody(body);
    if (ex && ex.type && !s.headers.has('content-type')) s.headers.set('content-type', ex.type);
    if (s.user !== undefined && !s.headers.has('authorization')) s.headers.set('authorization', 'Basic ' + btoa(s.user + ':' + (s.password || '')));
    s.errorFlag = false; s.sendFlag = true;
    s.uploadLen = ex ? bodyLen(ex.data) : -1;
    const spec = { method: s.method, url: s.url, body: ex ? ex.data : undefined, headers: headerPairs(s.headers), pageUrl: curHref, mode: 'cors', dest: 'empty' };
    const token = ++s.token;
    if (!s.async) {
      let res;
      try { res = n.httpSendSync(spec); } catch (e) { res = { error: String((e && e.message) || e) }; }
      if (res.error) { xhrFail(this, s, 'error'); throw new DOMException("Failed to execute 'send' on 'XMLHttpRequest': Failed to load '" + s.url + "'.", 'NetworkError'); }
      xhrSucceed(this, s, res, token);
      return;
    }
    xhrFire(this, 'loadstart', 0, 0); if (s.token !== token) return;
    if (s.uploadLen >= 0) { xhrFire(s.upload, 'loadstart', 0, s.uploadLen); if (s.token !== token) return; }
    if (!n.httpAvailable) { window.setTimeout(() => { if (s.token === token) xhrFail(this, s, 'error'); }, 0); return; }
    try {
      s.id = n.httpSend(spec, (res) => {
        if (s.token !== token) return;
        if (res.error) xhrFail(this, s, 'error'); else xhrSucceed(this, s, res, token);
      });
    } catch (e) { xhrFail(this, s, 'error'); return; }
    if (s.timeout > 0) s.timer = window.setTimeout(() => { if (s.token === token) xhrFail(this, s, 'timeout'); }, s.timeout);
  }
  abort() {
    const s = xs(this);
    const active = (s.readyState === 1 && s.sendFlag) || s.readyState === 2 || s.readyState === 3;
    if (active) xhrFail(this, s, 'abort'); else { s.token++; xhrCancel(s); }
    if (s.readyState === 4) { s.readyState = 0; s.errorFlag = false; }   // back to UNSENT, no event
  }
  getResponseHeader(name) {
    const s = xs(this);
    if (arguments.length < 1) throw new TypeError("Failed to execute 'getResponseHeader' on 'XMLHttpRequest': 1 argument required, but only 0 present.");
    if (s.readyState < 2 || s.errorFlag) return null;
    const lc = String(name).toLowerCase();
    const v = s.respHeaders.filter((h) => h[0].toLowerCase() === lc).map((h) => h[1]);
    return v.length ? v.join(', ') : null;
  }
  getAllResponseHeaders() {
    const s = xs(this);
    if (s.readyState < 2 || s.errorFlag) return '';
    const by = new Map();
    for (const [k, v] of s.respHeaders) { const lc = k.toLowerCase(); by.set(lc, by.has(lc) ? by.get(lc) + ', ' + v : v); }
    return Array.from(by.keys()).sort().map((k) => k + ': ' + by.get(k) + '\r\n').join('');
  }
}
markClass(XMLHttpRequest, 'XMLHttpRequest');
defHandlers(XMLHttpRequest, ['readystatechange']);
for (const [k, v] of [['UNSENT', 0], ['OPENED', 1], ['HEADERS_RECEIVED', 2], ['LOADING', 3], ['DONE', 4]]) {
  defProp(XMLHttpRequest, k, { value: v, enumerable: true }); defProp(XMLHttpRequest.prototype, k, { value: v, enumerable: true });
}

// ---- TextEncoder / TextDecoder (UTF-8 only) ------------------------------------------------------------------------------------
class TextEncoder {
  get encoding() { return 'utf-8'; }
  encode(input) { return new Uint8Array(n.utf8Encode(input === undefined ? '' : String(input))); }
  encodeInto(src, dest) {
    src = String(src);
    let read = 0, written = 0;
    for (const ch of src) {
      const b = new Uint8Array(n.utf8Encode(ch));
      if (written + b.length > dest.length) break;
      dest.set(b, written); written += b.length; read += ch.length;
    }
    return { read, written };
  }
}
markClass(TextEncoder, 'TextEncoder');
const TDEC = new WeakMap();
class TextDecoder {
  constructor(label, options) {
    const l = label === undefined ? 'utf-8' : String(label).trim().toLowerCase();
    if (l !== 'utf-8' && l !== 'utf8' && l !== 'unicode-1-1-utf-8') throw new RangeError("Failed to construct 'TextDecoder': The encoding label provided ('" + label + "') is invalid.");
    TDEC.set(this, { fatal: !!(options && options.fatal), ignoreBOM: !!(options && options.ignoreBOM) });
  }
  get encoding() { return 'utf-8'; }
  get fatal() { return TDEC.get(this).fatal; }
  get ignoreBOM() { return TDEC.get(this).ignoreBOM; }
  decode(input) { return input === undefined ? '' : n.utf8Decode(input); }
}
markClass(TextDecoder, 'TextDecoder');
