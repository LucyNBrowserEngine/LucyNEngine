
// ---- MutationObserver ---------------------------------------------------------------------------------------------------------
const OS = new WeakMap();
const obsSet = new Set();
class MutationRecord {}
markClass(MutationRecord, 'MutationRecord');
class MutationObserver {
  constructor(cb) {
    if (typeof cb !== 'function') throw new TypeError("Failed to construct 'MutationObserver': The callback provided as parameter 1 is not a function.");
    OS.set(this, { cb, regs: [], queue: [] });
  }
  observe(target, options) {
    const st = OS.get(this);
    if (!(target instanceof NodeC)) throw new TypeError("Failed to execute 'observe' on 'MutationObserver': parameter 1 is not of type 'Node'.");
    const o = Object.assign({}, options || {});
    if (o.attributes === undefined && (o.attributeFilter !== undefined || o.attributeOldValue !== undefined)) o.attributes = true;
    if (o.characterData === undefined && o.characterDataOldValue !== undefined) o.characterData = true;
    if (!o.childList && !o.attributes && !o.characterData)
      throw new TypeError("Failed to execute 'observe' on 'MutationObserver': The options object must set at least one of 'attributes', 'characterData', or 'childList' to true.");
    const i = st.regs.findIndex((r) => r.target === target);
    if (i >= 0) st.regs[i] = { target, o }; else st.regs.push({ target, o });
    obsSet.add(this); n.setObserving(true);
  }
  disconnect() { const st = OS.get(this); st.regs.length = 0; st.queue.length = 0; obsSet.delete(this); if (!obsSet.size) n.setObserving(false); }
  takeRecords() { const st = OS.get(this); const q = st.queue; st.queue = []; return q; }
}
markClass(MutationObserver, 'MutationObserver');
const mkRec = (type, target) => {
  const r = Object.create(MutationRecord.prototype);
  Object.assign(r, { type, target, addedNodes: [], removedNodes: [], previousSibling: null, nextSibling: null, attributeName: null, attributeNamespace: null, oldValue: null });
  return r;
};
n.setMutationSink(function (raw) {
  const touched = new Set();
  for (const [kind, target, node, name] of raw) {
    for (const ob of obsSet) {
      const st = OS.get(ob);
      for (const reg of st.regs) {
        const t = reg.target;
        if (!(t === target || (reg.o.subtree && t.contains(target)))) continue;
        let rec = null;
        if (kind === 0 || kind === 1) {
          if (!reg.o.childList) continue;
          rec = mkRec('childList', target);
          if (kind === 0) { rec.addedNodes = [node]; rec.previousSibling = node.previousSibling; rec.nextSibling = node.nextSibling; } else rec.removedNodes = [node];
        } else if (kind === 2) {
          if (!reg.o.attributes) continue;
          if (reg.o.attributeFilter && !reg.o.attributeFilter.includes(name)) continue;
          rec = mkRec('attributes', target); rec.attributeName = name;
        } else {
          if (!reg.o.characterData) continue;
          rec = mkRec('characterData', target);
        }
        st.queue.push(rec); touched.add(ob);
        break;
      }
    }
  }
  for (const ob of touched) {
    const st = OS.get(ob);
    if (!st.queue.length) continue;
    const q = st.queue; st.queue = [];
    try { st.cb.call(ob, q, ob); } catch (e) { n.reportError(e); }
  }
});

// ---- location / history ----------------------------------------------------------------------------------------------------------
const setCur = (u) => { const p = parseUrl(u); curHref = p ? p.href : String(u); };
setCur(curHref);                                                       // normalised from the start: location.href === document.URL
const navIgnored = (u) => n.note('navigation to ' + n.resolveUrl(curHref, String(u)) + ' ignored (a hydrated page cannot navigate)');
const setHref = (u) => {
  const abs = n.resolveUrl(curHref, String(u));
  const strip = (x) => x.replace(/#.*$/, '');
  if (strip(abs) === strip(curHref)) setCur(abs); else navIgnored(u);   // a hash-only change is not a navigation
};
class Location {
  toString() { return curHref; }
  assign(u) { setHref(u); }
  replace(u) { setHref(u); }
  reload() { navIgnored(curHref); }
}
markClass(Location, 'Location');
for (const k of ['href', 'protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin']) {
  accessor(Location.prototype, k, function () { const p = parseUrl(curHref); return p ? p[k] : ''; },
    k === 'origin' ? undefined : function (v) {
      if (k === 'href') return setHref(v);
      const p = parseUrl(curHref); if (!p) return;
      v = String(v);
      if (k === 'hash') { p.hash = v === '' || v === '#' ? '' : (v[0] === '#' ? v : '#' + v); setCur(buildHref(p)); return; }
      navIgnored(v);
    });
}
const LOC = Object.create(Location.prototype);
let histLen = 1, histState = null;
class History {
  get length() { return histLen; }
  get state() { return histState; }
  get scrollRestoration() { return 'auto'; }
  set scrollRestoration(v) {}
  pushState(state, title, url) { if (url !== undefined && url !== null) setCur(n.resolveUrl(curHref, String(url))); histState = state === undefined ? null : state; histLen++; }
  replaceState(state, title, url) { if (url !== undefined && url !== null) setCur(n.resolveUrl(curHref, String(url))); histState = state === undefined ? null : state; }
  back() {} forward() {} go() {}
}
markClass(History, 'History');
const HIST = Object.create(History.prototype);

// ---- navigator / screen (from the persona; blank, never invented, when there is none) ---------------------------------------------
const PER = n.persona ? JSON.parse(n.persona) : null;
const SF = PER ? PER.surface : { userAgent: '', platform: '', vendor: '', productSub: '', languages: [], maxTouchPoints: 0, mobile: false, brands: [], fullVersionList: [], uaPlatform: '', uaPlatformVersion: '', uaArchitecture: '', uaBitness: '', uaModel: '', hasUserAgentData: false, hasDeviceMemory: false };
const DV = PER ? PER.device : { cpuCores: 0, memoryGb: 0, screenW: 0, screenH: 0, pixelRatio: 1, reservedBottom: 0 };
const LANGS = Object.freeze(SF.languages.slice());
const freezeBrands = (a) => Object.freeze(a.map((b) => Object.freeze({ brand: b.brand, version: b.version })));
const BR = freezeBrands(SF.brands), FL = freezeBrands(SF.fullVersionList);
class NavigatorUAData {
  get brands() { return BR; }
  get mobile() { return SF.mobile; }
  get platform() { return SF.uaPlatform; }
  getHighEntropyValues(hints) {
    if (!Array.isArray(hints)) return Promise.reject(new TypeError("Failed to execute 'getHighEntropyValues' on 'NavigatorUAData': The provided value cannot be converted to a sequence."));
    const main = FL.find((b) => !/^Not/i.test(b.brand) && b.brand !== 'Chromium') || FL.find((b) => b.brand === 'Chromium') || FL[0];
    const all = { architecture: SF.uaArchitecture, bitness: SF.uaBitness, model: SF.uaModel, platformVersion: SF.uaPlatformVersion, uaFullVersion: main ? main.version : '', fullVersionList: FL, wow64: false };
    const out = { brands: BR, mobile: SF.mobile, platform: SF.uaPlatform };
    for (const h of hints) if (h in all) out[h] = all[h];
    return Promise.resolve(out);
  }
  toJSON() { return { brands: BR.slice(), mobile: SF.mobile, platform: SF.uaPlatform }; }
}
markClass(NavigatorUAData, 'NavigatorUAData');
const UAD = Object.create(NavigatorUAData.prototype);
class Navigator {}
tagOf(Navigator, 'Navigator'); mark(Navigator, 'Navigator');
const navVals = { userAgent: SF.userAgent, appVersion: SF.userAgent.replace(/^Mozilla\//, ''), appName: 'Netscape', appCodeName: 'Mozilla', product: 'Gecko',
  platform: SF.platform, vendor: SF.vendor, vendorSub: '', productSub: SF.productSub, language: LANGS[0] || '', languages: LANGS,
  hardwareConcurrency: DV.cpuCores, maxTouchPoints: SF.maxTouchPoints, webdriver: false, cookieEnabled: true, onLine: true };
if (SF.hasDeviceMemory) navVals.deviceMemory = Math.min(DV.memoryGb, 8);
if (SF.hasUserAgentData) navVals.userAgentData = UAD;
for (const k of Object.keys(navVals)) { const v = navVals[k]; accessor(Navigator.prototype, k, function () { return v; }); }
const NAV = Object.create(Navigator.prototype);
class Screen {}
tagOf(Screen, 'Screen'); mark(Screen, 'Screen');
const availH = Math.max(0, DV.screenH - DV.reservedBottom);
const scrVals = { width: DV.screenW, height: DV.screenH, availWidth: DV.screenW, availHeight: availH, availLeft: 0, availTop: 0, colorDepth: 24, pixelDepth: 24 };
for (const k of Object.keys(scrVals)) { const v = scrVals[k]; accessor(Screen.prototype, k, function () { return v; }); }
const SCR = Object.create(Screen.prototype);

// ---- URL / URLSearchParams / atob / btoa --------------------------------------------------------------------------------------
const dec = (x) => { x = x.replace(/\+/g, ' '); try { return decodeURIComponent(x); } catch (e) { return x; } };
const enc = (x) => encodeURIComponent(x).replace(/%20/g, '+').replace(/[!'()~]/g, (ch) => '%' + ch.charCodeAt(0).toString(16).toUpperCase());
const parseQuery = (s) => { s = String(s); if (s[0] === '?') s = s.slice(1); const out = []; for (const part of s.split('&')) { if (!part) continue; const i = part.indexOf('='); out.push([dec(i < 0 ? part : part.slice(0, i)), dec(i < 0 ? '' : part.slice(i + 1))]); } return out; };
const PARAMS = new WeakMap();
const USTATE = new WeakMap();
const syncUrl = (sp) => { const st = PARAMS.get(sp); if (st.url) { const u = USTATE.get(st.url); const q = sp.toString(); u.search = q ? '?' + q : ''; u.href = buildHref(u); } };
class URLSearchParams {
  constructor(init) {
    let list = [];
    if (typeof init === 'string') list = parseQuery(init);
    else if (init !== undefined && init !== null && typeof init[Symbol.iterator] === 'function') list = Array.from(init, (p) => [String(p[0]), String(p[1])]);
    else if (init !== undefined && init !== null) list = Object.keys(init).map((k) => [k, String(init[k])]);
    PARAMS.set(this, { list, url: null });
  }
  get size() { return PARAMS.get(this).list.length; }
  append(k, v) { PARAMS.get(this).list.push([String(k), String(v)]); syncUrl(this); }
  delete(k) { const st = PARAMS.get(this); st.list = st.list.filter((e) => e[0] !== String(k)); syncUrl(this); }
  get(k) { const e = PARAMS.get(this).list.find((x) => x[0] === String(k)); return e ? e[1] : null; }
  getAll(k) { return PARAMS.get(this).list.filter((x) => x[0] === String(k)).map((x) => x[1]); }
  has(k) { return PARAMS.get(this).list.some((x) => x[0] === String(k)); }
  set(k, v) { const st = PARAMS.get(this); k = String(k); const i = st.list.findIndex((x) => x[0] === k); if (i < 0) st.list.push([k, String(v)]); else { st.list[i][1] = String(v); st.list = st.list.filter((x, j) => j <= i || x[0] !== k); } syncUrl(this); }
  sort() { PARAMS.get(this).list.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)); syncUrl(this); }
  forEach(cb, t) { for (const [k, v] of PARAMS.get(this).list.slice()) cb.call(t, v, k, this); }
  keys() { return PARAMS.get(this).list.map((e) => e[0]).values(); }
  values() { return PARAMS.get(this).list.map((e) => e[1]).values(); }
  entries() { return PARAMS.get(this).list.map((e) => [e[0], e[1]]).values(); }
  [Symbol.iterator]() { return this.entries(); }
  toString() { return PARAMS.get(this).list.map(([k, v]) => enc(k) + '=' + enc(v)).join('&'); }
}
markClass(URLSearchParams, 'URLSearchParams');
class URL {
  constructor(url, base) {
    if (arguments.length === 0) throw new TypeError("Failed to construct 'URL': 1 argument required, but only 0 present.");
    let abs = String(url);
    if (base !== undefined) abs = n.resolveUrl(String(base), abs);
    const p = /^[a-zA-Z][a-zA-Z0-9+.\-]*:/.test(abs) ? parseUrl(abs) : null;
    if (!p) throw new TypeError("Failed to construct 'URL': Invalid URL");
    USTATE.set(this, p);
    const sp = new URLSearchParams(p.search); PARAMS.get(sp).url = this; USTATE.get(this).sp = sp;
  }
  static canParse(u, b) { try { new URL(u, b); return true; } catch (e) { return false; } }
  get searchParams() { return USTATE.get(this).sp; }
  toString() { return USTATE.get(this).href; }
  toJSON() { return USTATE.get(this).href; }
}
for (const k of ['href', 'origin', 'protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'username', 'password']) {
  accessor(URL.prototype, k, function () { return USTATE.get(this)[k]; },
    k === 'origin' ? undefined : function (v) {
      const u = USTATE.get(this); v = String(v);
      if (k === 'href') { const p = parseUrl(v); if (!p) throw new TypeError("Failed to set the 'href' property on 'URL': Invalid URL"); const sp = u.sp; Object.assign(u, p); PARAMS.get(sp).list = parseQuery(p.search); return; }
      if (k === 'search') { u.search = v === '' || v === '?' ? '' : (v[0] === '?' ? v : '?' + v); PARAMS.get(u.sp).list = parseQuery(u.search); }
      else if (k === 'hash') u.hash = v === '' || v === '#' ? '' : (v[0] === '#' ? v : '#' + v);
      else if (k === 'protocol') u.protocol = v.replace(/:?$/, ':').toLowerCase();
      else if (k === 'hostname') { u.hostname = v.toLowerCase(); }
      else if (k === 'port') u.port = v;
      else if (k === 'pathname') u.pathname = v[0] === '/' ? v : '/' + v;
      else u[k] = v;
      u.host = u.hostname + (u.port ? ':' + u.port : ''); u.href = buildHref(u);
    });
}
markClass(URL, 'URL');
const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
const btoa = function btoa(s) {
  s = String(s); let out = '';
  for (let i = 0; i < s.length; i += 3) {
    const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
    if (a > 255 || b > 255 || c > 255) throw new DOMException("Failed to execute 'btoa' on 'Window': The string to be encoded contains characters outside of the Latin1 range.", 'InvalidCharacterError');
    const x = (a << 16) | ((b || 0) << 8) | (c || 0);
    out += B64[(x >> 18) & 63] + B64[(x >> 12) & 63] + (i + 1 < s.length ? B64[(x >> 6) & 63] : '=') + (i + 2 < s.length ? B64[x & 63] : '=');
  }
  return out;
};
const atob = function atob(s) {
  s = String(s).replace(/[\t\n\f\r ]+/g, '');
  if (s.length % 4 === 0) s = s.replace(/==?$/, '');
  if (s.length % 4 === 1 || /[^A-Za-z0-9+/]/.test(s)) throw new DOMException("Failed to execute 'atob' on 'Window': The string to be decoded is not correctly encoded.", 'InvalidCharacterError');
  let out = '', bits = 0, acc = 0;
  for (const ch of s) { acc = (acc << 6) | B64.indexOf(ch); bits += 6; if (bits >= 8) { bits -= 8; out += String.fromCharCode((acc >> bits) & 255); acc &= (1 << bits) - 1; } }
  return out;
};

// ---- localStorage / sessionStorage -----------------------------------------
// Backed by n.storage* natives (storage_bridge.cpp).  The Proxy makes named
// property access work: storage['myKey'] and storage.myKey both work correctly.
function makeStorage(isLocal) {
  const label = isLocal ? 'localStorage' : 'sessionStorage';
  const target = {
    get length() { return n.storageLength(isLocal); },
    key(idx) { return n.storageKey(isLocal, idx >>> 0); },
    getItem(k) { return n.storageGet(isLocal, String(k)); },
    setItem(k, v) {
      if (arguments.length < 2) throw new TypeError("Failed to execute 'setItem' on 'Storage': 2 arguments required.");
      n.storageSet(isLocal, String(k), String(v));
    },
    removeItem(k) { n.storageRemove(isLocal, String(k)); },
    clear() { n.storageClear(isLocal); },
  };
  mark(target.key,        label + '.key');
  mark(target.getItem,    label + '.getItem');
  mark(target.setItem,    label + '.setItem');
  mark(target.removeItem, label + '.removeItem');
  mark(target.clear,      label + '.clear');
  const builtins = new Set(['length', 'key', 'getItem', 'setItem', 'removeItem', 'clear']);
  return new Proxy(target, {
    get(t, prop) {
      if (builtins.has(prop)) return t[prop];
      if (typeof prop === 'symbol') return undefined;
      return n.storageGet(isLocal, String(prop));
    },
    set(t, prop, val) {
      if (builtins.has(prop)) return false;
      if (typeof prop === 'symbol') return false;
      n.storageSet(isLocal, String(prop), String(val));
      return true;
    },
    deleteProperty(t, prop) {
      if (typeof prop === 'symbol') return false;
      n.storageRemove(isLocal, String(prop));
      return true;
    },
    has(t, prop) {
      if (builtins.has(prop)) return true;
      if (typeof prop === 'symbol') return false;
      return n.storageGet(isLocal, String(prop)) !== null;
    },
  });
}
const LS = makeStorage(true);
const SS = makeStorage(false);

// ---- window ---------------------------------------------------------------------------------------------------------------------
Object.setPrototypeOf(window, EventTarget.prototype);

// Ghostful Mode visibility (Session 11): the controller calls window.__lucynSetHidden(bool)
// via jsHost_->eval() whenever the real SDL window's visible/hidden state changes (window
// creation at startup, and every "window" command). document.hidden/visibilityState/hasFocus()
// in p2_elements.js all read this one flag, and this function is the only place that fires
// 'visibilitychange'/'focus'/'blur' — so a script watching all of these cannot catch Ghostful
// Mode in a lie: they always agree. Default true: no window exists until something asks for
// one, same as a background/minimized tab.
let __lucynHidden = true;
defProp(window, '__lucynSetHidden', {
  value: mark(function (hidden) {
    hidden = !!hidden;
    if (hidden === __lucynHidden) return;
    __lucynHidden = hidden;
    try { DOC.dispatchEvent(new Event('visibilitychange')); } catch (e) {}
    try { window.dispatchEvent(new Event(hidden ? 'blur' : 'focus')); } catch (e) {}
  }, '__lucynSetHidden'),
  configurable: true, enumerable: false, writable: false,
});
const getter = (name, fn) => defProp(window, name, { get: mark(fn, 'get ' + name), configurable: true, enumerable: true });
getter('document', function () { return DOC; });
getter('location', function () { return LOC; });
getter('history', function () { return HIST; });
getter('localStorage', function () { return LS; });
getter('sessionStorage', function () { return SS; });
getter('navigator', function () { return NAV; });
getter('screen', function () { return SCR; });
defProp(window, 'location', { get: Object.getOwnPropertyDescriptor(window, 'location').get, set: mark(function (v) { setHref(v); }, 'set location'), configurable: true, enumerable: true });
accessor(D, 'location', function () { return LOC; }, function (v) { setHref(v); });
for (const k of ['window', 'self', 'top', 'parent', 'frames']) defGlobal(k, window, true);
const innerH = Math.max(0, availH - 85);
for (const [k, v] of [['name', ''], ['closed', false], ['devicePixelRatio', DV.pixelRatio || 1], ['innerWidth', DV.screenW], ['innerHeight', innerH],
  ['outerWidth', DV.screenW], ['outerHeight', availH], ['screenX', 0], ['screenY', 0], ['scrollX', 0], ['scrollY', 0], ['pageXOffset', 0], ['pageYOffset', 0],
  ['isSecureContext', /^https:/.test(n.pageUrl)]]) defGlobal(k, v, true);
getter('origin', function () { const p = parseUrl(curHref); return p ? p.origin : 'null'; });
const wfn = (name, fn) => { mark(fn, name); defGlobal(name, fn, true); };
wfn('atob', atob); wfn('btoa', btoa);
for (const k of ['scrollTo', 'scrollBy', 'scroll', 'focus', 'blur', 'stop', 'print']) wfn(k, function () {});
wfn('alert', function () {}); wfn('confirm', function () { return false; }); wfn('prompt', function () { return null; });
wfn('getComputedStyle', function (el) {
  if (!(el instanceof ElementC)) throw new TypeError("Failed to execute 'getComputedStyle' on 'Window': parameter 1 is not of type 'Element'.");
  return styleOf(el);      // inline declarations only: the cascade does not exist yet
});
wfn('matchMedia', function (q) {
  const mql = { matches: false, media: String(q), onchange: null, addListener() {}, removeListener() {}, addEventListener() {}, removeEventListener() {}, dispatchEvent() { return true; } };
  return mql;
});
wfn('requestIdleCallback', function (cb) { return window.setTimeout(() => cb({ didTimeout: false, timeRemaining: () => 50 }), 1); });
wfn('cancelIdleCallback', function (id) { window.clearTimeout(id); });
class Performance {
  now() { return n.now(); }
  get timeOrigin() { return Date.now() - n.now(); }
  mark() {} measure() {} clearMarks() {} clearMeasures() {}
  getEntries() { return []; } getEntriesByName() { return []; } getEntriesByType() { return []; }
  toJSON() { return { timeOrigin: this.timeOrigin }; }
}
markClass(Performance, 'Performance');
const PERF = Object.create(Performance.prototype);
defProp(window, 'performance', { get: mark(function () { return PERF; }, 'get performance'), configurable: true, enumerable: true });

// ---- Request / Response and in-memory CacheStorage -------------------------------------------
// A per-page Cache API subset. It supports the common application pattern of
// opening a named cache, putting a response under a Request/URL, and reading it
// back. Cache data is intentionally in-memory for now (no ServiceWorker/offline
// lifecycle); each attached page receives a fresh cache map.
// Request / Response / Headers / fetch live in p4_net.js (the cache below only uses them at call time).

const cacheMap = new Map();
const cacheKey = (request) => {
  const r = request instanceof Request ? request : new Request(request);
  return r.method === 'GET' ? r.url : r.method + ' ' + r.url;
};
const makeCache = (name) => {
  const entries = cacheMap.get(name);
  return {
    match(request) { const hit = entries.get(cacheKey(request)); return Promise.resolve(hit ? hit.clone() : undefined); },
    matchAll(request) {
      if (request === undefined) return Promise.resolve(Array.from(entries.values(), (r) => r.clone()));
      const hit = entries.get(cacheKey(request)); return Promise.resolve(hit ? [hit.clone()] : []);
    },
    put(request, response) {
      if (!(response instanceof Response) && (!response || typeof response.clone !== 'function' || typeof response.text !== 'function'))
        return Promise.reject(new TypeError("Failed to execute 'put' on 'Cache': parameter 2 is not a Response."));
      const key = cacheKey(request);
      return response.clone().text().then((body) => {
        entries.set(key, new Response(body, { status: response.status, statusText: response.statusText, headers: response.headers, url: response.url }));
      });
    },
    add(request) {
      const req = request instanceof Request ? request : new Request(request);
      return fetch(req.url).then((response) => {
        if (!response.ok) throw new TypeError('Cache.add received a non-success response');
        return this.put(req, response);
      });
    },
    addAll(requests) { return Promise.all(Array.from(requests, (request) => this.add(request))).then(() => undefined); },
    delete(request) { return Promise.resolve(entries.delete(cacheKey(request))); },
    keys(request) {
      const keys = Array.from(entries.keys());
      if (request === undefined) return Promise.resolve(keys.map((url) => new Request(url)));
      const wanted = cacheKey(request);
      return Promise.resolve(keys.includes(wanted) ? [new Request(wanted)] : []);
    }
  };
};
const caches = {
  open(name) {
    name = String(name);
    if (!cacheMap.has(name)) cacheMap.set(name, new Map());
    return Promise.resolve(makeCache(name));
  },
  has(name) { return Promise.resolve(cacheMap.has(String(name))); },
  delete(name) { return Promise.resolve(cacheMap.delete(String(name))); },
  keys() { return Promise.resolve(Array.from(cacheMap.keys())); },
  match(request) {
    for (const name of cacheMap.keys()) {
      const hit = cacheMap.get(name).get(cacheKey(request));
      if (hit) return Promise.resolve(hit.clone());
    }
    return Promise.resolve(undefined);
  }
};
getter('caches', function () { return caches; });

class Crypto {
  getRandomValues(array) { return n.randomValues(array); }
  randomUUID() {
    const b = this.getRandomValues(new Uint8Array(16));
    b[6] = (b[6] & 15) | 64; b[8] = (b[8] & 63) | 128;
    const h = Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
    return h.slice(0, 8) + '-' + h.slice(8, 12) + '-' + h.slice(12, 16) + '-' + h.slice(16, 20) + '-' + h.slice(20);
  }
}
markClass(Crypto, 'Crypto');
const CRYPTO = Object.create(Crypto.prototype);
getter('crypto', function () { return CRYPTO; });

class IntersectionObserver {
  constructor(callback, options) {
    if (typeof callback !== 'function') throw new TypeError("Failed to construct 'IntersectionObserver': parameter 1 is not a function.");
    const opts = options || {};
    this.root = opts.root || null;
    this.rootMargin = opts.rootMargin || '0px';
    this.thresholds = Object.freeze((Array.isArray(opts.threshold) ? opts.threshold : [opts.threshold === undefined ? 0 : opts.threshold]).map(Number).sort((a, b) => a - b));
    this.__callback = callback;
    this.__targets = new Set();
    this.__records = [];
  }
  observe(target) {
    if (!(target instanceof ElementC)) throw new TypeError("Failed to execute 'observe' on 'IntersectionObserver': parameter 1 is not an Element.");
    if (this.__targets.has(target)) return;
    this.__targets.add(target);
    // LucyN has no layout engine yet. Report an intersecting, zero-area entry
    // so common visibility hooks can initialize their content without layout.
    const rect = target.getBoundingClientRect();
    const entry = { time: performance.now(), target, rootBounds: null, boundingClientRect: rect,
      intersectionRect: rect, isIntersecting: true, intersectionRatio: 1 };
    this.__records.push(entry);
    queueMicrotask(() => {
      const records = this.takeRecords();
      if (!records.length) return;
      try { this.__callback.call(this, records, this); } catch (e) { n.reportError(e); }
    });
  }
  unobserve(target) { this.__targets.delete(target); }
  disconnect() { this.__targets.clear(); this.__records.length = 0; }
  takeRecords() { const records = this.__records; this.__records = []; return records; }
}
markClass(IntersectionObserver, 'IntersectionObserver');

