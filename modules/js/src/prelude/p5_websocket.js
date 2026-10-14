// ---- WebSocket -------------------------------------------------------------------------------
// Implements the WebSocket API (https://websockets.spec.whatwg.org/).
// The native side lives in websocket_bridge.cpp and is exposed on the private `n` object as:
//   n.wsConnect(url, protocols)      -> numeric id (0 = not available)
//   n.wsSend(id, data, isBinary)
//   n.wsClose(id, code, reason)
//   n.wsSetCallbacks(id, onopen, onmessage, onclose, onerror)
//   n.wsForget(id)
//   n.wsAvailable                    -> bool

const WS_CONNECTING = 0, WS_OPEN = 1, WS_CLOSING = 2, WS_CLOSED = 3;

class WebSocket extends EventTarget {
  constructor(url, protocols) {
    super();
    if (arguments.length < 1) throw new TypeError("WebSocket requires a url argument");
    if (typeof url !== 'string') throw new TypeError("WebSocket url must be a string");

    // Normalise protocols to a comma-joined string for the native layer.
    let protoStr = '';
    if (protocols !== undefined) {
      if (typeof protocols === 'string') protoStr = protocols;
      else if (Array.isArray(protocols)) protoStr = protocols.join(',');
      else throw new TypeError("WebSocket protocols must be a string or array of strings");
    }

    this._url       = url;
    this._protocol  = '';
    this._extensions = '';
    this._readyState = WS_CONNECTING;
    this._bufferedAmount = 0;
    this._binaryType = 'blob';
    this._id        = 0;

    // on* properties — mirrored to EventTarget listeners.
    this._onopen    = null;
    this._onmessage = null;
    this._onclose   = null;
    this._onerror   = null;

    if (!n.wsAvailable) {
      // Queue a synthetic error + close so the object ends in CLOSED state as spec requires.
      const self = this;
      setTimeout(function () {
        self._readyState = WS_CLOSED;
        self.dispatchEvent(new Event('error'));
        self.dispatchEvent(new CloseEvent('close', { wasClean: false, code: 1006, reason: 'WebSocket not available' }));
      }, 0);
      return;
    }

    const id = n.wsConnect(url, protoStr);
    if (!id) {
      const self = this;
      setTimeout(function () {
        self._readyState = WS_CLOSED;
        self.dispatchEvent(new Event('error'));
        self.dispatchEvent(new CloseEvent('close', { wasClean: false, code: 1006, reason: 'Connection failed' }));
      }, 0);
      return;
    }
    this._id = id;

    const self = this;
    n.wsSetCallbacks(id,
      function () {                          // onopen
        self._readyState = WS_OPEN;
        self.dispatchEvent(new Event('open'));
      },
      function (data, binary) {             // onmessage
        // binaryType: 'arraybuffer' returns ArrayBuffer; 'blob' also returns ArrayBuffer here
        // (Blob is not yet implemented; arraybuffer is always safe)
        const ev = new MessageEvent('message', { data: binary ? n.utf8Decode(data) : data, origin: self._url, lastEventId: '', source: null });
        self.dispatchEvent(ev);
      },
      function (code, reason) {             // onclose
        self._readyState = WS_CLOSED;
        n.wsForget(id);
        const wasClean = code === 1000 || code === 1001;
        self.dispatchEvent(new CloseEvent('close', { wasClean, code, reason }));
      },
      function (msg) {                      // onerror
        self.dispatchEvent(Object.assign(new Event('error'), { message: msg }));
      }
    );
  }

  get url()              { return this._url; }
  get readyState()       { return this._readyState; }
  get bufferedAmount()   { return this._bufferedAmount; }
  get protocol()         { return this._protocol; }
  get extensions()       { return this._extensions; }
  get binaryType()       { return this._binaryType; }
  set binaryType(v)      { this._binaryType = v === 'arraybuffer' ? 'arraybuffer' : 'blob'; }

  // on* property shortcuts (convenience properties; full addEventListener also works)
  get onopen()           { return this._onopen; }
  set onopen(fn)         { this._setHandler('open',    '_onopen',    fn); }
  get onmessage()        { return this._onmessage; }
  set onmessage(fn)      { this._setHandler('message', '_onmessage', fn); }
  get onclose()          { return this._onclose; }
  set onclose(fn)        { this._setHandler('close',   '_onclose',   fn); }
  get onerror()          { return this._onerror; }
  set onerror(fn)        { this._setHandler('error',   '_onerror',   fn); }

  _setHandler(type, prop, fn) {
    if (this[prop]) this.removeEventListener(type, this[prop]);
    this[prop] = (fn && typeof fn === 'function') ? fn : null;
    if (this[prop]) this.addEventListener(type, this[prop]);
  }

  send(data) {
    if (this._readyState === WS_CONNECTING)
      throw new DOMException('WebSocket is still CONNECTING', 'InvalidStateError');
    if (this._readyState !== WS_OPEN) return;
    const binary = (data instanceof ArrayBuffer || ArrayBuffer.isView(data));
    n.wsSend(this._id, data, binary);
  }

  close(code, reason) {
    if (this._readyState === WS_CLOSING || this._readyState === WS_CLOSED) return;
    if (code === undefined) code = 1000;
    if (typeof code !== 'number') throw new TypeError('code must be a number');
    if (reason === undefined) reason = '';
    this._readyState = WS_CLOSING;
    n.wsClose(this._id, code, reason);
  }

  static get CONNECTING() { return WS_CONNECTING; }
  static get OPEN()       { return WS_OPEN; }
  static get CLOSING()    { return WS_CLOSING; }
  static get CLOSED()     { return WS_CLOSED; }
}

// CloseEvent (used by WebSocket close handler above)
class CloseEvent extends Event {
  constructor(type, init) {
    super(type, init);
    init = init || {};
    this.wasClean = !!init.wasClean;
    this.code     = init.code   !== undefined ? init.code   : 0;
    this.reason   = init.reason !== undefined ? init.reason : '';
  }
}

// MessageEvent (used by WebSocket message handler above)
class MessageEvent extends Event {
  constructor(type, init) {
    super(type, init);
    init = init || {};
    this.data        = init.data        !== undefined ? init.data        : null;
    this.origin      = init.origin      !== undefined ? init.origin      : '';
    this.lastEventId = init.lastEventId !== undefined ? init.lastEventId : '';
    this.source      = init.source      !== undefined ? init.source      : null;
    this.ports       = init.ports       !== undefined ? init.ports       : [];
  }
}
