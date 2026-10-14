(function (n, window) {
'use strict';
const defProp = Object.defineProperty;
const P = n.protos;
const NodeC = P.Node.constructor, ElementC = P.Element.constructor, TextC = P.Text.constructor,
      CommentC = P.Comment.constructor, DocumentC = P.Document.constructor;
let curHref = n.pageUrl;

// ---- native-looking functions: Function.prototype.toString must not reveal this file -----------------------------------------
const nativeNames = new WeakMap();
const origToString = Function.prototype.toString;
const NATIVE_TAIL = /\{\s*\[native code\]\s*\}$/;
const patchedToString = function toString() {
  const nm = nativeNames.get(this);
  if (nm !== undefined) return 'function ' + nm + '() { [native code] }';
  const s = origToString.call(this);
  return NATIVE_TAIL.test(s) ? s.replace(NATIVE_TAIL, '{ [native code] }') : s;
};
nativeNames.set(patchedToString, 'toString');
defProp(Function.prototype, 'toString', { value: patchedToString, writable: true, configurable: true, enumerable: false });
const mark = (fn, name) => { nativeNames.set(fn, name); defProp(fn, 'name', { value: name, configurable: true }); return fn; };
const defGlobal = (name, value, enumerable) => defProp(window, name, { value, writable: true, configurable: true, enumerable: !!enumerable });
const method = (obj, name, fn) => { mark(fn, name); defProp(obj, name, { value: fn, writable: true, configurable: true, enumerable: true }); };
const accessor = (obj, name, get, set) => {
  mark(get, 'get ' + name); if (set) mark(set, 'set ' + name);
  defProp(obj, name, { get, set, configurable: true, enumerable: true });
};
const tagOf = (C, name) => defProp(C.prototype, Symbol.toStringTag, { value: name, configurable: true });
const markClass = (C, name) => {
  mark(C, name);
  for (const k of Object.getOwnPropertyNames(C.prototype)) {
    if (k === 'constructor') continue;
    const d = Object.getOwnPropertyDescriptor(C.prototype, k);
    if (typeof d.value === 'function') mark(d.value, k);
    else { if (d.get) mark(d.get, 'get ' + k); if (d.set) mark(d.set, 'set ' + k); }
    d.enumerable = true; defProp(C.prototype, k, d);
  }
  tagOf(C, name);
  return C;
};
const illegal = (name) => { const F = function () { throw new TypeError('Illegal constructor'); }; mark(F, name); return F; };

// ---- DOMException -------------------------------------------------------------------------------------------------------------
const DOM_CODES = { IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4, InvalidCharacterError: 5, NoModificationAllowedError: 7,
  NotFoundError: 8, NotSupportedError: 9, InvalidStateError: 11, SyntaxError: 12, InvalidModificationError: 13, NamespaceError: 14,
  InvalidAccessError: 15, TypeMismatchError: 17, SecurityError: 18, NetworkError: 19, AbortError: 20, URLMismatchError: 21,
  QuotaExceededError: 22, TimeoutError: 23, InvalidNodeTypeError: 24, DataCloneError: 25 };
class DOMException extends Error {
  constructor(message, name) {
    super(message === undefined ? '' : String(message));
    const nm = name === undefined ? 'Error' : String(name);
    defProp(this, 'name', { value: nm, writable: true, configurable: true });
    defProp(this, 'code', { value: DOM_CODES[nm] || 0, configurable: true });
  }
}
markClass(DOMException, 'DOMException');
n.setDomException(DOMException);

// ---- URL parsing (location, <a>, URL) -----------------------------------------------------------------------------------------
const URL_RE = /^([a-zA-Z][a-zA-Z0-9+.\-]*:)(?:\/\/(?:([^@\/?#]*)@)?(\[[^\]]*\]|[^:\/?#]*)(?::(\d*))?)?([^?#]*)(\?[^#]*)?(#.*)?$/;
const SPECIAL = { 'http:': '80', 'https:': '443', 'ws:': '80', 'wss:': '443', 'ftp:': '21', 'file:': '' };
const buildHref = (p) => p.protocol + (p.hasAuth ? '//' + (p.userinfo ? p.userinfo + '@' : '') + p.host : '') + p.pathname + p.search + p.hash;
const parseUrl = (href) => {
  const m = URL_RE.exec(String(href));
  if (!m) return null;
  const protocol = m[1].toLowerCase();
  const hasAuth = m[3] !== undefined;
  const hostname = hasAuth ? m[3].toLowerCase() : '';
  let port = m[4] || '';
  if (port === SPECIAL[protocol]) port = '';
  let pathname = m[5] || '';
  if (hasAuth && SPECIAL[protocol] !== undefined && pathname === '') pathname = '/';
  const userinfo = m[2] || '';
  const ui = userinfo.split(':');
  const p = { protocol, hasAuth, hostname, port, pathname, userinfo, username: ui[0] || '', password: ui.slice(1).join(':'),
    search: m[6] && m[6] !== '?' ? m[6] : '', hash: m[7] && m[7] !== '#' ? m[7] : '' };
  p.host = hostname + (port ? ':' + port : '');
  p.origin = (protocol in SPECIAL && protocol !== 'file:') ? protocol + '//' + p.host : 'null';
  p.href = buildHref(p);
  return p;
};

// ---- events -------------------------------------------------------------------------------------------------------------------
const LISTENERS = new WeakMap();
const ES = new WeakMap();
const FV = new WeakMap();
const selfOf = (t) => (t === undefined || t === null) ? window : t;
const parentOf = (x, type) => {
  if (x === window) return null;
  if (x instanceof NodeC) {
    const p = x.parentNode;
    if (p) return p;
    if (x.nodeType === 9) return type === 'load' ? null : window;
  }
  return null;
};
const callListener = (cb, cur, ev) => {
  try {
    if (typeof cb === 'function') cb.call(cur, ev);
    else if (cb && typeof cb.handleEvent === 'function') cb.handleEvent(ev);
  } catch (e) { n.reportError(e); }
};
const invoke = (cur, s, ev, phase) => {
  s.currentTarget = cur;
  const m = LISTENERS.get(cur);
  const arr = m && m.get(s.type);
  if (arr && arr.length) {
    for (const r of arr.slice()) {
      if (r.removed) continue;
      if (phase === 1 && !r.capture) continue;
      if (phase === 3 && r.capture) continue;
      if (r.once) { r.removed = true; const i = arr.indexOf(r); if (i >= 0) arr.splice(i, 1); }
      callListener(r.cb, cur, ev);
      if (s.stopImm) return;
    }
  }
  if (phase !== 1) {
    let h; try { h = cur['on' + s.type]; } catch (e) { h = undefined; }
    if (typeof h === 'function') callListener(h, cur, ev);
  }
};
const dispatch = (target, ev) => {
  const s = ES.get(ev);
  if (!s) throw new TypeError("Failed to execute 'dispatchEvent' on 'EventTarget': parameter 1 is not of type 'Event'.");
  if (s.phase !== 0) throw new DOMException('The event is already being dispatched.', 'InvalidStateError');
  s.target = target; s.stop = false; s.stopImm = false;
  const path = [];
  for (let cur = target; cur; cur = parentOf(cur, s.type)) path.push(cur);
  s.path = path;
  s.phase = 1;
  for (let i = path.length - 1; i > 0 && !s.stop; i--) invoke(path[i], s, ev, 1);
  if (!s.stop) { s.phase = 2; invoke(path[0], s, ev, 2); }
  if (s.bubbles) { s.phase = 3; for (let i = 1; i < path.length && !s.stop; i++) invoke(path[i], s, ev, 3); }
  s.phase = 0; s.currentTarget = null;
  return !s.canceled;
};

class EventTarget {
  addEventListener(type, cb, opts) {
    const self = selfOf(this);
    if (cb === null || cb === undefined) return;
    const capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
    const once = !!(opts && typeof opts === 'object' && opts.once);
    type = String(type);
    let m = LISTENERS.get(self);
    if (!m) { m = new Map(); LISTENERS.set(self, m); }
    let arr = m.get(type);
    if (!arr) { arr = []; m.set(type, arr); }
    for (const r of arr) if (r.cb === cb && r.capture === capture) return;
    arr.push({ cb, capture, once, removed: false });
  }
  removeEventListener(type, cb, opts) {
    const self = selfOf(this);
    const capture = typeof opts === 'boolean' ? opts : !!(opts && opts.capture);
    const m = LISTENERS.get(self); if (!m) return;
    const arr = m.get(String(type)); if (!arr) return;
    for (let i = 0; i < arr.length; i++) if (arr[i].cb === cb && arr[i].capture === capture) { arr[i].removed = true; arr.splice(i, 1); return; }
  }
  dispatchEvent(ev) { return dispatch(selfOf(this), ev); }
}
markClass(EventTarget, 'EventTarget');

class Event {
  constructor(type, init) {
    if (arguments.length === 0) throw new TypeError("Failed to construct 'Event': 1 argument required, but only 0 present.");
    const o = init || {};
    ES.set(this, { type: String(type), bubbles: !!o.bubbles, cancelable: !!o.cancelable, composed: !!o.composed,
      target: null, currentTarget: null, phase: 0, stop: false, stopImm: false, canceled: false, path: [], t: n.now() });
    defProp(this, 'isTrusted', { value: false, enumerable: true, configurable: false });
  }
  get type() { return ES.get(this).type; }
  get target() { return ES.get(this).target; }
  get srcElement() { return ES.get(this).target; }
  get currentTarget() { return ES.get(this).currentTarget; }
  get eventPhase() { return ES.get(this).phase; }
  get bubbles() { return ES.get(this).bubbles; }
  get cancelable() { return ES.get(this).cancelable; }
  get composed() { return ES.get(this).composed; }
  get defaultPrevented() { return ES.get(this).canceled; }
  get timeStamp() { return ES.get(this).t; }
  get cancelBubble() { return ES.get(this).stop; }
  set cancelBubble(v) { if (v) ES.get(this).stop = true; }
  preventDefault() { const s = ES.get(this); if (s.cancelable) s.canceled = true; }
  stopPropagation() { ES.get(this).stop = true; }
  stopImmediatePropagation() { const s = ES.get(this); s.stop = true; s.stopImm = true; }
  composedPath() { const s = ES.get(this); return s.phase === 0 ? [] : s.path.slice(); }
  initEvent(type, bubbles, cancelable) { const s = ES.get(this); if (s.phase !== 0) return; s.type = String(type); s.bubbles = !!bubbles; s.cancelable = !!cancelable; s.canceled = false; }
}
markClass(Event, 'Event');
for (const [k, v] of [['NONE', 0], ['CAPTURING_PHASE', 1], ['AT_TARGET', 2], ['BUBBLING_PHASE', 3]]) {
  defProp(Event, k, { value: v, enumerable: true }); defProp(Event.prototype, k, { value: v, enumerable: true });
}
const mk = (name, Base, fields) => {
  const C = class extends Base {
    constructor(type, init) {
      super(type, init);
      const o = init || {}; const v = FV.get(this) || {};
      for (const k in fields) v[k] = (o[k] !== undefined) ? o[k] : fields[k];
      FV.set(this, v);
    }
  };
  for (const k in fields) defProp(C.prototype, k, { get: mark(function () { return FV.get(this)[k]; }, 'get ' + k), enumerable: true, configurable: true });
  mark(C, name); tagOf(C, name);
  return C;
};
const mods = { ctrlKey: false, shiftKey: false, altKey: false, metaKey: false };
const CustomEvent = mk('CustomEvent', Event, { detail: null });
const UIEvent = mk('UIEvent', Event, { view: null, detail: 0 });
const MouseEvent = mk('MouseEvent', UIEvent, Object.assign({ screenX: 0, screenY: 0, clientX: 0, clientY: 0, button: 0, buttons: 0, relatedTarget: null }, mods));
const KeyboardEvent = mk('KeyboardEvent', UIEvent, Object.assign({ key: '', code: '', location: 0, repeat: false, isComposing: false }, mods));
const FocusEvent = mk('FocusEvent', UIEvent, { relatedTarget: null });
const InputEvent = mk('InputEvent', UIEvent, { data: null, inputType: '', isComposing: false });
