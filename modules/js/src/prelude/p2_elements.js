
// ---- Node / Element / Document additions ---------------------------------------------------------------------------------------
Object.setPrototypeOf(P.Node, EventTarget.prototype);
Object.setPrototypeOf(NodeC, EventTarget);
for (const [k, v] of [['ELEMENT_NODE', 1], ['TEXT_NODE', 3], ['COMMENT_NODE', 8], ['DOCUMENT_NODE', 9], ['DOCUMENT_FRAGMENT_NODE', 11]]) {
  defProp(NodeC, k, { value: v, enumerable: true }); defProp(P.Node, k, { value: v, enumerable: true });
}
const DOC = n.getDocument();
method(P.Node, 'isSameNode', function isSameNode(o) { return this === o; });
method(P.Node, 'isEqualNode', function isEqualNode(other) {
  if (!(other instanceof NodeC)) return false;
  const pending = [[this, other]];
  while (pending.length) {
    const [a, b] = pending.pop();
    if (a === b) continue;
    if (!a || !b || a.nodeType !== b.nodeType || a.nodeName !== b.nodeName || a.nodeValue !== b.nodeValue) return false;
    if (a instanceof ElementC) {
      const aa = a.getAttributeNames(), ba = b.getAttributeNames();
      if (aa.length !== ba.length) return false;
      for (const name of aa) if (!b.hasAttribute(name) || a.getAttribute(name) !== b.getAttribute(name)) return false;
    }
    const ac = a.childNodes, bc = b.childNodes;
    if (ac.length !== bc.length) return false;
    for (let i = 0; i < ac.length; ++i) pending.push([ac[i], bc[i]]);
  }
  return true;
});
method(P.Node, 'getRootNode', function getRootNode() { let x = this; while (x.parentNode) x = x.parentNode; return x; });
accessor(P.Node, 'baseURI', function () { return curHref; });
const toNode = (x) => (x instanceof NodeC ? x : DOC.createTextNode(String(x)));
const parentMixin = {
  append(...xs) { for (const x of xs) this.appendChild(toNode(x)); },
  prepend(...xs) { const ref = this.firstChild; for (const x of xs) this.insertBefore(toNode(x), ref); },
  replaceChildren(...xs) { while (this.firstChild) this.removeChild(this.firstChild); for (const x of xs) this.appendChild(toNode(x)); },
  getElementsByClassName(names) {
    const want = String(names).split(/\s+/).filter(Boolean);
    if (!want.length) return [];
    return Array.from(this.querySelectorAll('*')).filter((e) => { const c = e.getAttribute('class'); if (!c) return false; const have = c.split(/\s+/); return want.every((w) => have.includes(w)); });
  },
};
const childMixin = {
  before(...xs) { const p = this.parentNode; if (!p) return; for (const x of xs) p.insertBefore(toNode(x), this); },
  after(...xs) { const p = this.parentNode; if (!p) return; const ref = this.nextSibling; for (const x of xs) p.insertBefore(toNode(x), ref); },
  replaceWith(...xs) { const p = this.parentNode; if (!p) return; const ref = this.nextSibling; p.removeChild(this); for (const x of xs) p.insertBefore(toNode(x), ref); },
  remove() { const p = this.parentNode; if (p) p.removeChild(this); },
};
for (const [k, f] of Object.entries(parentMixin)) { method(P.Element, k, f); method(P.Document, k, f); }
for (const [k, f] of Object.entries(childMixin)) { method(P.Element, k, f); method(P.Text, k, f); method(P.Comment, k, f); }
method(P.Element, 'toggleAttribute', function toggleAttribute(name, force) {
  const has = this.hasAttribute(name);
  const want = force === undefined ? !has : !!force;
  if (want && !has) this.setAttribute(name, ''); else if (!want && has) this.removeAttribute(name);
  return want;
});
method(P.Element, 'insertAdjacentElement', function insertAdjacentElement(pos, el) {
  const p = this.parentNode;
  switch (String(pos).toLowerCase()) {
    case 'beforebegin': if (!p) return null; p.insertBefore(el, this); break;
    case 'afterbegin': this.insertBefore(el, this.firstChild); break;
    case 'beforeend': this.appendChild(el); break;
    case 'afterend': if (!p) return null; p.insertBefore(el, this.nextSibling); break;
    default: throw new DOMException("The value provided ('" + pos + "') is not one of 'beforebegin', 'afterbegin', 'beforeend', or 'afterend'.", 'SyntaxError');
  }
  return el;
});
method(P.Element, 'insertAdjacentText', function insertAdjacentText(pos, text) { this.insertAdjacentElement(pos, DOC.createTextNode(String(text))); });

// ---- classList ----------------------------------------------------------------------------------------------------------------
const TL = new WeakMap();
const toks = (el) => { const v = el.getAttribute('class'); return v ? v.split(/[ \t\n\f\r]+/).filter(Boolean) : []; };
const checkTok = (t) => {
  t = String(t);
  if (t === '') throw new DOMException("The token provided must not be empty.", 'SyntaxError');
  if (/[ \t\n\f\r]/.test(t)) throw new DOMException("The token provided ('" + t + "') contains HTML space characters, which are not valid in tokens.", 'InvalidCharacterError');
  return t;
};
class DOMTokenList {
  get length() { return toks(TL.get(this)).length; }
  get value() { return TL.get(this).getAttribute('class') || ''; }
  set value(v) { TL.get(this).setAttribute('class', String(v)); }
  item(i) { const t = toks(TL.get(this)); return i >= 0 && i < t.length ? t[i] : null; }
  contains(t) { return toks(TL.get(this)).includes(String(t)); }
  add(...ts) { const el = TL.get(this); const cur = toks(el); for (const t of ts.map(checkTok)) if (!cur.includes(t)) cur.push(t); el.setAttribute('class', cur.join(' ')); }
  remove(...ts) { const el = TL.get(this); const rm = ts.map(checkTok); el.setAttribute('class', toks(el).filter((t) => !rm.includes(t)).join(' ')); }
  toggle(t, force) {
    t = checkTok(t); const has = this.contains(t);
    if (has && force !== true) { this.remove(t); return false; }
    if (!has && force !== false) { this.add(t); return true; }
    return has;
  }
  replace(a, b) { const el = TL.get(this); const cur = toks(el); const i = cur.indexOf(checkTok(a)); if (i < 0) return false; cur[i] = checkTok(b); el.setAttribute('class', [...new Set(cur)].join(' ')); return true; }
  supports() { return true; }
  forEach(cb, thisArg) { toks(TL.get(this)).forEach((t, i) => cb.call(thisArg, t, i, this)); }
  keys() { return toks(TL.get(this)).keys(); }
  values() { return toks(TL.get(this)).values(); }
  entries() { return toks(TL.get(this)).entries(); }
  [Symbol.iterator]() { return toks(TL.get(this)).values(); }
  toString() { return this.value; }
}
markClass(DOMTokenList, 'DOMTokenList');
const CLS = new WeakMap();
accessor(P.Element, 'classList', function () {
  let l = CLS.get(this);
  if (!l) { l = Object.create(DOMTokenList.prototype); TL.set(l, this); CLS.set(this, l); }
  return l;
}, function (v) { this.setAttribute('class', String(v)); });

// ---- style (inline only: there is no cascade) ---------------------------------------------------------------------------------
const kebab = (s) => (s === 'cssFloat' ? 'float' : s.replace(/[A-Z]/g, (m) => '-' + m.toLowerCase()));
const camel = (s) => s.replace(/-([a-z])/g, (m, c) => c.toUpperCase());
const splitDecls = (css) => {
  const out = []; let cur = '', depth = 0, q = '';
  for (const ch of String(css)) {
    if (q) { cur += ch; if (ch === q) q = ''; continue; }
    if (ch === '"' || ch === "'") { q = ch; cur += ch; continue; }
    if (ch === '(') depth++; else if (ch === ')') depth = Math.max(0, depth - 1);
    if (ch === ';' && depth === 0) { out.push(cur); cur = ''; } else cur += ch;
  }
  out.push(cur);
  const decls = [];
  for (const part of out) { const i = part.indexOf(':'); if (i > 0) { const k = part.slice(0, i).trim().toLowerCase(); const v = part.slice(i + 1).trim(); if (k && v) decls.push([k, v]); } }
  return decls;
};
const STYLE_EL = new WeakMap();
const readDecls = (el) => splitDecls(el.getAttribute('style') || '');
const writeDecls = (el, d) => { const s = d.map(([k, v]) => k + ': ' + v + ';').join(' '); if (s === '' && !el.hasAttribute('style')) return; el.setAttribute('style', s); };
class CSSStyleDeclaration {
  get cssText() { return STYLE_EL.get(this).getAttribute('style') || ''; }
  set cssText(v) { STYLE_EL.get(this).setAttribute('style', String(v)); }
  get length() { return readDecls(STYLE_EL.get(this)).length; }
  item(i) { const d = readDecls(STYLE_EL.get(this)); return i >= 0 && i < d.length ? d[i][0] : ''; }
  getPropertyValue(name) { const k = String(name).toLowerCase(); const e = readDecls(STYLE_EL.get(this)).find((x) => x[0] === k); return e ? e[1].replace(/\s*!important$/i, '') : ''; }
  getPropertyPriority(name) { const k = String(name).toLowerCase(); const e = readDecls(STYLE_EL.get(this)).find((x) => x[0] === k); return e && /!important$/i.test(e[1]) ? 'important' : ''; }
  setProperty(name, value, priority) {
    const el = STYLE_EL.get(this); const k = String(name).toLowerCase(); const d = readDecls(el).filter((x) => x[0] !== k);
    value = value === null || value === undefined ? '' : String(value);
    if (value !== '') d.push([k, value + (priority === 'important' ? ' !important' : '')]);
    writeDecls(el, d);
  }
  removeProperty(name) { const old = this.getPropertyValue(name); this.setProperty(name, ''); return old; }
}
markClass(CSSStyleDeclaration, 'CSSStyleDeclaration');
const STY = new WeakMap();
const styleOf = (el) => {
  let p = STY.get(el);
  if (!p) {
    const base = Object.create(CSSStyleDeclaration.prototype); STYLE_EL.set(base, el);
    p = new Proxy(base, {
      get(t, k, r) { if (typeof k === 'symbol' || k in t) return Reflect.get(t, k, t); return t.getPropertyValue(kebab(k)); },
      set(t, k, v) { if (typeof k === 'symbol' || k === 'cssText') { t[k] = v; return true; } t.setProperty(kebab(k), v); return true; },
      has(t, k) { return k in t || (typeof k === 'string' && readDecls(STYLE_EL.get(t)).some((x) => x[0] === kebab(k))); },
    });
    STYLE_EL.set(p, el); STY.set(el, p);
  }
  return p;
};
accessor(P.Element, 'style', function () { return styleOf(this); }, function (v) { this.setAttribute('style', String(v)); });

// ---- dataset ------------------------------------------------------------------------------------------------------------------
const DS = new WeakMap();
class DOMStringMap {}
markClass(DOMStringMap, 'DOMStringMap');
accessor(P.Element, 'dataset', function () {
  let p = DS.get(this);
  if (!p) {
    const el = this;
    p = new Proxy(Object.create(DOMStringMap.prototype), {
      get(t, k) { if (typeof k === 'symbol') return t[k]; const v = el.getAttribute('data-' + kebab(k)); return v === null ? undefined : v; },
      set(t, k, v) { el.setAttribute('data-' + kebab(String(k)), String(v)); return true; },
      has(t, k) { return typeof k === 'string' && el.hasAttribute('data-' + kebab(k)); },
      deleteProperty(t, k) { el.removeAttribute('data-' + kebab(String(k))); return true; },
      ownKeys() { return el.getAttributeNames().filter((a) => a.startsWith('data-')).map((a) => camel(a.slice(5))); },
      getOwnPropertyDescriptor(t, k) { if (typeof k !== 'string') return undefined; const v = el.getAttribute('data-' + kebab(k)); return v === null ? undefined : { value: v, writable: true, enumerable: true, configurable: true }; },
    });
    DS.set(this, p);
  }
  return p;
});

// ---- HTMLElement + per-tag classes ----------------------------------------------------------------------------------------------
const HTMLElement = illegal('HTMLElement');
HTMLElement.prototype = Object.create(P.Element, { constructor: { value: HTMLElement, writable: true, configurable: true } });
Object.setPrototypeOf(HTMLElement, ElementC);
tagOf(HTMLElement, 'HTMLElement');
const reflect = (proto, specs) => {
  for (const [prop, kind, attr] of specs) {
    const a = attr || prop.toLowerCase();
    let get, set;
    if (kind === 'u') { get = function () { const v = this.getAttribute(a); return v === null ? '' : n.resolveUrl(curHref, v); }; set = function (v) { this.setAttribute(a, String(v)); }; }
    else if (kind === 'b') { get = function () { return this.hasAttribute(a); }; set = function (v) { if (v) this.setAttribute(a, ''); else this.removeAttribute(a); }; }
    else if (kind === 'i') { get = function () { return parseInt(this.getAttribute(a), 10) || 0; }; set = function (v) { this.setAttribute(a, String(Math.trunc(Number(v)) || 0)); }; }
    else { get = function () { const v = this.getAttribute(a); return v === null ? '' : v; }; set = function (v) { this.setAttribute(a, String(v)); }; }
    accessor(proto, prop, get, set);
  }
};
const H = HTMLElement.prototype;
reflect(H, [['title', 's'], ['lang', 's'], ['dir', 's'], ['hidden', 'b'], ['tabIndex', 'i', 'tabindex']]);
accessor(H, 'innerText', function () { return this.textContent; }, function (v) { this.textContent = String(v); });
accessor(H, 'outerText', function () { return this.textContent; }, function (v) { this.textContent = String(v); });
method(H, 'click', function click() { if (this.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true, composed: true })) !== false) activate(this); });
method(H, 'focus', function focus() {});
method(H, 'blur', function blur() {});
// No layout engine yet: geometry is honestly zero.
for (const k of ['offsetWidth', 'offsetHeight', 'offsetTop', 'offsetLeft', 'clientWidth', 'clientHeight', 'clientTop', 'clientLeft', 'scrollWidth', 'scrollHeight']) accessor(H, k, function () { return 0; });
accessor(H, 'offsetParent', function () { return null; });
for (const k of ['scrollTop', 'scrollLeft']) accessor(H, k, function () { return 0; }, function () {});
const rect = () => ({ x: 0, y: 0, width: 0, height: 0, top: 0, right: 0, bottom: 0, left: 0 });
method(P.Element, 'getBoundingClientRect', function getBoundingClientRect() { const r = rect(); r.toJSON = () => rect(); return r; });
method(P.Element, 'getClientRects', function getClientRects() { return []; });
method(P.Element, 'scrollIntoView', function scrollIntoView() {});
const TAGS = [
  ['a', 'HTMLAnchorElement', [['href', 'u'], ['target', 's'], ['rel', 's'], ['download', 's'], ['type', 's'], ['hreflang', 's']]],
  ['img', 'HTMLImageElement', [['src', 'u'], ['alt', 's'], ['width', 'i'], ['height', 'i'], ['srcset', 's'], ['loading', 's']]],
  ['input', 'HTMLInputElement', [['type', 's'], ['name', 's'], ['value', 's'], ['placeholder', 's'], ['checked', 'b'], ['disabled', 'b'], ['required', 'b'], ['readOnly', 'b', 'readonly'], ['min', 's'], ['max', 's'], ['step', 's']]],
  ['button', 'HTMLButtonElement', [['type', 's'], ['name', 's'], ['value', 's'], ['disabled', 'b']]],
  ['form', 'HTMLFormElement', [['action', 'u'], ['method', 's'], ['name', 's'], ['target', 's']]],
  ['script', 'HTMLScriptElement', [['src', 'u'], ['type', 's'], ['async', 'b'], ['defer', 'b'], ['charset', 's'], ['crossOrigin', 's', 'crossorigin']]],
  ['link', 'HTMLLinkElement', [['href', 'u'], ['rel', 's'], ['type', 's'], ['as', 's'], ['media', 's']]],
  ['meta', 'HTMLMetaElement', [['name', 's'], ['content', 's'], ['httpEquiv', 's', 'http-equiv']]],
  ['iframe', 'HTMLIFrameElement', [['src', 'u'], ['name', 's']]],
  ['div', 'HTMLDivElement', []], ['span', 'HTMLSpanElement', []], ['p', 'HTMLParagraphElement', []], ['body', 'HTMLBodyElement', []],
  ['head', 'HTMLHeadElement', []], ['html', 'HTMLHtmlElement', []], ['ul', 'HTMLUListElement', []], ['li', 'HTMLLIElement', []],
];
const tagProtos = {};
const htmlClasses = { HTMLElement };
for (const [tag, cname, specs] of TAGS) {
  const C = illegal(cname);
  C.prototype = Object.create(H, { constructor: { value: C, writable: true, configurable: true } });
  Object.setPrototypeOf(C, HTMLElement);
  tagOf(C, cname);
  reflect(C.prototype, specs);
  tagProtos[tag] = C.prototype; htmlClasses[cname] = C;
}
accessor(tagProtos.script, 'text', function () { return this.textContent; }, function (v) { this.textContent = String(v); });
for (const k of ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin', 'username', 'password'])
  accessor(tagProtos.a, k, function () { const p = parseUrl(this.href); return p ? p[k] : ''; });
// ---- SVGElement -----------------------------------------------------------------------------------------------------------------
const SVGElement = illegal('SVGElement');
SVGElement.prototype = Object.create(P.Element, { constructor: { value: SVGElement, writable: true, configurable: true } });
Object.setPrototypeOf(SVGElement, ElementC);
tagOf(SVGElement, 'SVGElement');
const SVG_TAGS = ['svg', 'path', 'g', 'circle', 'rect', 'line', 'polyline', 'polygon', 'ellipse', 'text', 'tspan',
  'defs', 'use', 'symbol', 'clipPath', 'linearGradient', 'radialGradient', 'stop', 'mask', 'pattern', 'filter',
  'foreignObject', 'image', 'marker', 'switch', 'textPath', 'animate', 'animateTransform', 'animateMotion', 'set',
  'feGaussianBlur', 'feOffset', 'feMerge', 'feMergeNode', 'feColorMatrix', 'feBlend', 'feComposite', 'feFlood'];
for (const tag of SVG_TAGS) tagProtos[tag] = SVGElement.prototype;
htmlClasses.SVGElement = SVGElement;
n.setTagProtos(tagProtos, H);

// ---- Document additions -------------------------------------------------------------------------------------------------------
const D = P.Document;
accessor(D, 'URL', function () { return curHref; });
accessor(D, 'documentURI', function () { return curHref; });
// ---- document.cookie ------------------------------------------------------------------------------------------------------------
// With a host cookie store (n.cookieAvailable) reads and writes go to it, so scripts and HTTP requests share ONE jar and HttpOnly
// cookies stay invisible here. Without one, a small per-page jar keeps document.cookie behaving (nothing is sent on requests).
const localJar = new Map();
const cookieDirOf = (pathname) => { const i = pathname.lastIndexOf('/'); return (pathname[0] !== '/' || i <= 0) ? '/' : pathname.slice(0, i); };
const cookiePathMatches = (cp, rp) => rp === cp || (rp.startsWith(cp) && (cp.endsWith('/') || rp[cp.length] === '/'));
accessor(D, 'cookie', function () {
  if (n.cookieAvailable) return n.cookieGet(curHref);
  const u = parseUrl(curHref); const rp = u ? (u.pathname || '/') : '/'; const now = Date.now();
  const hit = [];
  for (const [k, c] of localJar) { if (c.exp !== 0 && c.exp <= now) { localJar.delete(k); continue; } if (cookiePathMatches(c.path, rp)) hit.push(c); }
  hit.sort((a, b) => b.path.length - a.path.length);
  return hit.map((c) => (c.name ? c.name + '=' + c.value : c.value)).join('; ');
}, function (v) {
  v = String(v);
  if (n.cookieAvailable) { n.cookieSet(curHref, v); return; }
  const parts = v.split(';'); const first = parts[0]; const eq = first.indexOf('=');
  const name = eq < 0 ? '' : first.slice(0, eq).trim(); const value = (eq < 0 ? first : first.slice(eq + 1)).trim();
  if (!name && !value) return;
  let path = null, exp = 0, del = false, haveMaxAge = false;
  for (let i = 1; i < parts.length; i++) {
    const a = parts[i].trim(); const e = a.indexOf('=');
    const key = (e < 0 ? a : a.slice(0, e)).trim().toLowerCase(); const val = e < 0 ? '' : a.slice(e + 1).trim();
    if (key === 'httponly') return;
    if (key === 'path' && val[0] === '/') path = val;
    else if (key === 'max-age' && /^-?\d+$/.test(val)) { haveMaxAge = true; const sec = Number(val); if (sec <= 0) del = true; else { del = false; exp = Date.now() + sec * 1000; } }
    else if (key === 'expires' && !haveMaxAge) { const t = Date.parse(val); if (!isNaN(t)) { if (t <= Date.now()) del = true; else exp = t; } }
  }
  if (path === null) { const u = parseUrl(curHref); path = cookieDirOf(u ? (u.pathname || '/') : '/'); }
  const key = name + '\n' + path;
  if (del) localJar.delete(key); else localJar.set(key, { name, value, path, exp });
});
accessor(D, 'referrer', function () { return ''; });
accessor(D, 'domain', function () { const p = parseUrl(curHref); return p ? p.hostname : ''; });
accessor(D, 'characterSet', function () { return 'UTF-8'; });
accessor(D, 'contentType', function () { return 'text/html'; });
accessor(D, 'compatMode', function () { return 'CSS1Compat'; });
accessor(D, 'doctype', function () { return null; });
// Ghostful Mode (Session 11): all three read the one flag window.__lucynSetHidden()
// sets, so they can never disagree with each other or with the focus/blur events —
// see that function's definition in p3_window.js for the "consistent lies while
// hidden" design note.
accessor(D, 'hidden', function () { return __lucynHidden; });
accessor(D, 'visibilityState', function () { return __lucynHidden ? 'hidden' : 'visible'; });
accessor(D, 'scripts', function () { return this.getElementsByTagName('script'); });
method(D, 'hasFocus', function hasFocus() { return !__lucynHidden; });
tagOf(DocumentC, 'HTMLDocument'); tagOf(NodeC, 'Node'); tagOf(ElementC, 'Element'); tagOf(TextC, 'Text'); tagOf(CommentC, 'Comment');
