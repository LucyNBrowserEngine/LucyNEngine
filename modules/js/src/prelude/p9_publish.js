// ---- publish constructors (non-enumerable, like a browser) --------------------------------------------------------------------------
const classes = { Node: NodeC, Element: ElementC, Text: TextC, Comment: CommentC, Document: DocumentC, EventTarget, Event, CustomEvent, UIEvent, MouseEvent,
  KeyboardEvent, FocusEvent, InputEvent, DOMException, MutationObserver, MutationRecord, DOMTokenList, CSSStyleDeclaration, DOMStringMap, Location, History, Request, Response,
  Headers, Blob, File, AbortController, AbortSignal, FormData, ProgressEvent, XMLHttpRequestEventTarget, XMLHttpRequestUpload, XMLHttpRequest, TextEncoder, TextDecoder,
  Navigator, Screen, NavigatorUAData, URL, URLSearchParams, Performance, Crypto, IntersectionObserver,
  WebSocket, CloseEvent, MessageEvent };
for (const k of Object.keys(classes)) defGlobal(k, classes[k], false);
defGlobal('fetch', fetch, true);
for (const k of Object.keys(htmlClasses)) defGlobal(k, htmlClasses[k], false);
if (!SF.hasUserAgentData) delete window.NavigatorUAData;

// ---- inline on* attribute compilation --------------------------------------------------------------------------------------------
// Runs once at 'interactive', before DOMContentLoaded. For every element with an on* attribute (e.g. onclick="doX()"),
// compile the attribute value into a function (scoped to `event` like a browser) and attach it as a listener.
// The element itself is `this` inside the handler, matching browser behaviour.
// Attributes on <body>/<frameset> that are window events (onload, onerror, onresize…) are wired to window instead.
const WINDOW_BODY_EVENTS = new Set(['load','unload','beforeunload','error','focus','blur','resize','scroll',
  'hashchange','popstate','pagehide','pageshow','storage','online','offline','message','messageerror',
  'languagechange','rejectionhandled','unhandledrejection']);

function compileInlineHandlers() {
  const all = DOC.querySelectorAll('*');
  for (let i = 0; i < all.length; i++) {
    const el = all[i];
    const names = el.getAttributeNames();
    for (let j = 0; j < names.length; j++) {
      const attr = names[j];
      if (attr.length < 3 || attr[0] !== 'o' || attr[1] !== 'n') continue;
      const eventType = attr.slice(2);
      const code = el.getAttribute(attr);
      if (!code) continue;
      let fn;
      try {
        // Compile: function body sees `event` as a parameter; `this` = element (set via bind).
        // Using Function constructor matches browser semantics (no closure over page variables
        // other than what is globally accessible — good enough for typical onclick="...").
        fn = new Function('event', code);
      } catch (e) {
        // Syntax error in the inline handler — skip silently, like browsers do.
        continue;
      }
      const tag = el.tagName ? el.tagName.toLowerCase() : '';
      if ((tag === 'body' || tag === 'frameset') && WINDOW_BODY_EVENTS.has(eventType)) {
        // Body-level window events delegate to window.
        window.addEventListener(eventType, fn);
      } else {
        el.addEventListener(eventType, fn.bind(el));
      }
    }
  }
}

// ---- lifecycle (driven by the host) ----------------------------------------------------------------------------------------------
n.setLifecycle(function (stage) {
  if (stage === 'parsed') {
    // Wire inline on* attributes before any page <script> fires — matches browser behaviour.
    compileInlineHandlers();
  } else if (stage === 'interactive') {
    DOC.dispatchEvent(new Event('readystatechange'));
    DOC.dispatchEvent(new Event('DOMContentLoaded', { bubbles: true }));
  } else if (stage === 'complete') {
    DOC.dispatchEvent(new Event('readystatechange'));
    window.dispatchEvent(new Event('load'));
  }
});
})
