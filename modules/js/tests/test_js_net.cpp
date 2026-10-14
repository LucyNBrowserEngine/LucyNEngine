// fetch(), Headers, XMLHttpRequest, document.cookie, FormData, Blob/File, AbortController, TextEncoder/Decoder, <script src>.
// The "network" is a scripted callback that records every request the page makes, so the tests assert on what
// the page actually SENT as well as on what it saw.
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <lucyn/js/quickjs_host.h>
#include <lucyn/shell/hydrator.h>

using namespace lucyn;
using namespace lucyn::shell;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
#define CHECK_EQ(got, want) do { std::string g_ = (got), w_ = (want); if (g_ != w_) { std::printf("FAIL %s:%d  %s\n     got:  %s\n     want: %s\n", __FILE__, __LINE__, #got, g_.c_str(), w_.c_str()); ++failures; } } while (0)

static std::string join(const std::vector<std::string>& v, const char* sep = "|") {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += sep; s += v[i]; }
    return s;
}

struct Rig {
    std::unique_ptr<dom::Document> doc;
    NativeHooks hooks;
    js::QuickJsHost host;
    std::vector<std::string> out;
    bool attached = false;
    explicit Rig(js::HostOptions o = {}, const std::string& html = "<html><head></head><body></body></html>",
                 const std::string& url = "https://site.test/p/index.html")
        : doc(dom::parseHtml(html)), host(std::move(o)) {
        hooks.add("out", [this](const std::vector<std::string>& a) { out.push_back(join(a)); return HookResult{true, ""}; });
        hooks.add("page.url", [url](const std::vector<std::string>&) { return HookResult{true, url}; });
        std::string err;
        attached = host.attach(*doc, url, hooks, &err);
        if (!attached) std::printf("attach failed: %s\n", err.c_str());
    }
    shell::ScriptOutcome run(const std::string& src, const char* name = "t") {
        UserScript u; u.name = name; u.source = src;
        return host.runUserScript(u);
    }
    std::string ev(const std::string& expr) {
        out.clear();
        auto r = run("lucyn.out(String(" + expr + "))");
        if (!r.ok) return "ERR:" + r.error;
        return out.empty() ? std::string("<no output>") : out.back();
    }
    // Runs `code`, failing the test if it throws.
    void exec(const std::string& code) {
        auto r = run(code);
        if (!r.ok) { std::printf("FAIL script threw: %s\n   in: %s\n", r.error.c_str(), code.substr(0, 120).c_str()); ++failures; }
    }
    // Spins the event loop until `expr` is "true" (settle() itself does not wait for the network).
    bool waitFor(const std::string& expr, int ms = 3000) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            host.settle(10);
            if (ev(expr) == "true") return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        return false;
    }
    void spin(int ms) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) { host.settle(10); std::this_thread::sleep_for(std::chrono::milliseconds(3)); }
    }
};

// The scripted network.
struct Net {
    std::mutex mu;
    std::vector<js::HttpRequest> log;
    js::HttpGetResult respond(const js::HttpRequest& r) {
        js::HttpGetResult o;
        o.status = 200; o.httpVersion = "1.1"; o.statusText = "OK";
        const std::string p = r.url.substr(r.url.find('/', 8));   // path (+query) after the host
        if (p == "/json")          { o.body = "{\"a\":1,\"b\":[true,null]}"; o.headers = {{"Content-Type", "application/json"}}; }
        else if (p == "/text")     { o.body = "hello"; o.headers = {{"Content-Type", "text/plain"}, {"X-Multi", "a"}, {"x-multi", "b"}, {"Set-Cookie", "s=1; Path=/"}}; }
        else if (p == "/redirect") { o.body = "landed"; o.effectiveUrl = "https://site.test/final#x"; o.redirects = 1; }
        else if (p == "/h2")       { o.body = "h2"; o.httpVersion = "2"; o.statusText = ""; }
        else if (p == "/missing")  { o.status = 404; o.statusText = "Not Found"; o.body = "nope"; }
        else if (p == "/empty")    { o.status = 204; o.statusText = "No Content"; }
        else if (p == "/fail")     { o.status = 0; o.error = "boom"; }
        else if (p == "/slow")     { std::this_thread::sleep_for(std::chrono::milliseconds(300)); o.body = "late"; }
        else if (p == "/bin")      { for (int i = 0; i < 256; ++i) o.body.push_back(static_cast<char>(i)); }
        else if (p == "/bom")      { o.body = "\xEF\xBB\xBF" "x"; }
        else if (p == "/lib.js")   { o.body = "window.__lib = 'loaded';"; }
        else                       { o.body = r.method + "|" + r.body; }   // echo
        return o;
    }
    js::HostOptions opts() {
        js::HostOptions o;
        o.httpFetch = [this](const js::HttpRequest& r) {
            { std::lock_guard<std::mutex> lk(mu); log.push_back(r); }
            return respond(r);
        };
        return o;
    }
    js::HttpRequest last() { std::lock_guard<std::mutex> lk(mu); return log.empty() ? js::HttpRequest{} : log.back(); }
    size_t count() { std::lock_guard<std::mutex> lk(mu); return log.size(); }
};
static std::string headerOf(const js::HttpRequest& r, const std::string& name) {
    for (auto& h : r.headers) {
        std::string a = h.first, b = name;
        for (auto& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (auto& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (a == b) return h.second;
    }
    return "<none>";
}

static void testHeaders() {
    Rig r;
    CHECK(r.attached);
    r.exec("window.h = new Headers({'Content-Type':'text/plain','X-A':'1'}); h.append('x-a','2');");
    CHECK_EQ(r.ev("h.get('content-type')"), "text/plain");
    CHECK_EQ(r.ev("h.get('X-A')"), "1, 2");
    CHECK_EQ(r.ev("h.get('nope')"), "null");
    CHECK_EQ(r.ev("h.has('X-a')"), "true");
    r.exec("h.set('X-A','3'); h.delete('content-type');");
    CHECK_EQ(r.ev("JSON.stringify(Array.from(h))"), "[[\"x-a\",\"3\"]]");
    CHECK_EQ(r.ev("JSON.stringify(Array.from(new Headers([['b','2'],['a','1']]).keys()))"), "[\"a\",\"b\"]");    // sorted
    CHECK_EQ(r.ev("new Headers(h).get('x-a')"), "3");
    CHECK_EQ(r.ev("(() => { try { new Headers({'bad name':'x'}); return 'no'; } catch (e) { return e.name; } })()"), "TypeError");
    CHECK_EQ(r.ev("(() => { try { new Headers([['only-one']]); return 'no'; } catch (e) { return e.name; } })()"), "TypeError");
    r.exec("window.sc = new Headers(); sc.append('Set-Cookie','a=1'); sc.append('set-cookie','b=2');");
    CHECK_EQ(r.ev("JSON.stringify(sc.getSetCookie())"), "[\"a=1\",\"b=2\"]");
    CHECK_EQ(r.ev("Object.prototype.toString.call(h)"), "[object Headers]");
    CHECK_EQ(r.ev("Headers.toString()"), "function Headers() { [native code] }");
    CHECK_EQ(r.ev("typeof AbortController + typeof FormData + typeof TextEncoder + typeof XMLHttpRequest"), "functionfunctionfunctionfunction");
}

static void testFetchBasics() {
    Net net;
    Rig r(net.opts());
    r.exec("window.R = []; window.RS = null;"
           "fetch('/json').then((x) => { RS = x; return x.json(); }).then((j) => R.push('json:' + j.a + ':' + j.b.length)).catch((e) => R.push('err:' + e));");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "json:1:2");
    CHECK_EQ(r.ev("RS.status + ' ' + RS.ok + ' ' + RS.statusText + ' ' + RS.url + ' ' + RS.redirected + ' ' + RS.type"), "200 true OK https://site.test/json false basic");
    CHECK_EQ(r.ev("RS.headers.get('Content-Type')"), "application/json");
    CHECK_EQ(r.ev("RS instanceof Response"), "true");
    CHECK_EQ(r.ev("RS.bodyUsed"), "true");
    CHECK_EQ(net.last().method, "GET");
    CHECK_EQ(net.last().url, "https://site.test/json");
    CHECK_EQ(net.last().pageUrl, "https://site.test/p/index.html");      // the page context is passed along
    CHECK_EQ(net.last().mode, "cors");
    CHECK_EQ(net.last().dest, "empty");
    CHECK(!net.last().hasBody);

    // a body can be read once; clone() before reading gives a second copy
    r.exec("R = []; RS.text().then((t) => R.push('ok:' + t), (e) => R.push(e.name));");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "TypeError");
    r.exec("R = []; fetch('/text').then((x) => { const c = x.clone(); return Promise.all([x.text(), c.text()]); }).then((v) => R.push(v.join('+')));");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "hello+hello");

    // binary survives intact (no UTF-8 round trip)
    r.exec("R = []; fetch('/bin').then((x) => x.arrayBuffer()).then((b) => { const u = new Uint8Array(b); let ok = u.length === 256; for (let i = 0; i < 256; i++) if (u[i] !== i) ok = false; R.push(ok); });");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "true");

    // BOM stripped by text(); 404 still resolves; 204 has no body
    r.exec("R = []; fetch('/bom').then((x) => x.text()).then((t) => R.push(t.length));");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "1");
    r.exec("R = []; fetch('/missing').then((x) => R.push(x.status + ':' + x.ok + ':' + x.statusText));");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "404:false:Not Found");
    r.exec("R = []; fetch('/empty').then((x) => x.text().then((t) => R.push(x.status + ':[' + t + ']:' + x.bodyUsed)));");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "204:[]:false");

    // redirects: the final URL (fragment stripped) and the `redirected` flag
    r.exec("R = []; fetch('/redirect').then((x) => R.push(x.url + ' ' + x.redirected));");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "https://site.test/final true");

    // statusText is empty on HTTP/2 (as in Chrome); Set-Cookie is never visible to the page
    r.exec("R = []; fetch('/h2').then((x) => R.push('[' + x.statusText + ']'));");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "[]");
    r.exec("R = []; fetch('/text').then((x) => R.push(x.headers.has('set-cookie') + ',' + x.headers.get('x-multi')));");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "false,a, b");

    // network failure: a TypeError, like Chrome's "Failed to fetch"
    r.exec("R = []; fetch('/fail').catch((e) => R.push(e.name + ':' + e.message));");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "TypeError:Failed to fetch");
}

static void testFetchRequests() {
    Net net;
    Rig r(net.opts());
    auto go = [&](const std::string& call) {
        r.exec("window.R = []; " + call + ".then((x) => x.text()).then((t) => R.push(t), (e) => R.push('ERR:' + e.name + ':' + e.message));");
        CHECK(r.waitFor("R.length > 0"));
    };

    // POST + JSON body + header object
    go("fetch('/api', {method: 'post', headers: {'Content-Type': 'application/json', 'X-Token': 'abc'}, body: JSON.stringify({x: 1})})");
    CHECK_EQ(r.ev("R[0]"), "POST|{\"x\":1}");
    CHECK_EQ(net.last().method, "POST");
    CHECK(net.last().hasBody);
    CHECK_EQ(headerOf(net.last(), "content-type"), "application/json");
    CHECK_EQ(headerOf(net.last(), "x-token"), "abc");

    // a string body with no Content-Type gets the browser's default
    go("fetch('/api', {method: 'POST', body: 'plain'})");
    CHECK_EQ(headerOf(net.last(), "content-type"), "text/plain;charset=UTF-8");

    // URLSearchParams
    go("fetch('/api', {method: 'POST', body: new URLSearchParams({a: '1', b: 'x y'})})");
    CHECK_EQ(net.last().body, "a=1&b=x+y");
    CHECK_EQ(headerOf(net.last(), "content-type"), "application/x-www-form-urlencoded;charset=UTF-8");

    // FormData -> multipart with a boundary
    go("(() => { const f = new FormData(); f.append('name', 'Lu'); f.append('note', 'a\"b'); return fetch('/api', {method: 'POST', body: f}); })()");
    {
        std::string ct = headerOf(net.last(), "content-type");
        CHECK(ct.rfind("multipart/form-data; boundary=----", 0) == 0);
        std::string boundary = ct.substr(ct.find("boundary=") + 9);
        CHECK(net.last().body.find("--" + boundary + "\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\nLu\r\n") == 0);
        CHECK(net.last().body.find("name=\"note\"\r\n\r\na\"b\r\n") != std::string::npos);
        CHECK(net.last().body.size() > 2 && net.last().body.substr(net.last().body.size() - boundary.size() - 6) == "--" + boundary + "--\r\n");
    }

    // binary body, typed-array view with an offset
    go("(() => { const u = new Uint8Array([9, 1, 2, 3, 9]); return fetch('/api', {method: 'PUT', body: u.subarray(1, 4)}); })()");
    CHECK_EQ(net.last().method, "PUT");
    CHECK_EQ(net.last().body, std::string("\x01\x02\x03", 3));

    // verbs are passed through
    go("fetch('/api', {method: 'DELETE'})");
    CHECK_EQ(net.last().method, "DELETE");
    go("fetch('/api', {method: 'patch', body: 'p'})");
    CHECK_EQ(net.last().method, "PATCH");
    go("fetch('/api', {method: 'HEAD'})");
    CHECK_EQ(net.last().method, "HEAD");
    CHECK_EQ(r.ev("R[0]"), "");                                // HEAD: no body

    // header init forms: Headers instance, array of pairs; duplicate names combine
    go("fetch('/api', {headers: new Headers({'X-One': '1'})})");
    CHECK_EQ(headerOf(net.last(), "x-one"), "1");
    go("fetch('/api', {headers: [['X-Two', '2'], ['x-two', '3']]})");
    CHECK_EQ(headerOf(net.last(), "x-two"), "2, 3");

    // a Request object carries method, headers and body
    go("fetch(new Request('/api', {method: 'POST', body: 'rb', headers: {'X-R': 'q'}}))");
    CHECK_EQ(net.last().method, "POST");
    CHECK_EQ(net.last().body, "rb");
    CHECK_EQ(headerOf(net.last(), "x-r"), "q");
    // ...and init overrides it
    go("fetch(new Request('/api', {method: 'POST', body: 'rb'}), {method: 'PUT'})");
    CHECK_EQ(net.last().method, "PUT");

    // GET with a body is a TypeError
    go("fetch('/api', {method: 'GET', body: 'x'})");
    CHECK_EQ(r.ev("R[0].slice(0, 14)"), "ERR:TypeError:");
    // same-origin mode refuses another origin and never reaches the network
    size_t before = net.count();
    go("fetch('https://other.test/x', {mode: 'same-origin'})");
    CHECK_EQ(r.ev("R[0]"), "ERR:TypeError:Failed to fetch");
    CHECK(net.count() == before);
    // no-cors is passed on
    go("fetch('https://other.test/x', {mode: 'no-cors'})");
    CHECK_EQ(net.last().mode, "no-cors");
    CHECK_EQ(r.ev("typeof Request + (new Request('/x').method) + (new Request('/x').url)"), "functionGEThttps://site.test/x");

    // data: URLs never touch the network
    before = net.count();
    go("fetch('data:text/plain;base64,aGk=')");
    CHECK_EQ(r.ev("R[0]"), "hi");
    go("fetch('data:,a%20b')");
    CHECK_EQ(r.ev("R[0]"), "a b");
    CHECK(net.count() == before);
}

static void testAbort() {
    Net net;
    Rig r(net.opts());
    r.exec("window.R = []; const c = new AbortController(); c.abort(); fetch('/text', {signal: c.signal}).catch((e) => R.push(e.name));");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "AbortError");
    CHECK(net.count() == 0);                                   // already aborted: nothing was sent

    r.exec("R = []; window.C = new AbortController(); fetch('/slow', {signal: C.signal}).then(() => R.push('resolved'), (e) => R.push(e.name));"
           "setTimeout(() => C.abort(), 20);");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R.join()"), "AbortError");
    r.spin(450);                                               // the late response must be dropped, not delivered
    CHECK_EQ(r.ev("R.join()"), "AbortError");

    CHECK_EQ(r.ev("(() => { const s = AbortSignal.abort('why'); return s.aborted + ':' + s.reason; })()"), "true:why");
    CHECK_EQ(r.ev("(() => { const c = new AbortController(); let n = 0; c.signal.onabort = () => n++; c.signal.addEventListener('abort', () => n++); c.abort(); c.abort(); return n; })()"), "2");
    CHECK_EQ(r.ev("(() => { try { new AbortSignal(); return 'no'; } catch (e) { return e.name; } })()"), "TypeError");
}

static void testXhrAsync() {
    Net net;
    Rig r(net.opts());
    r.exec("window.EV = []; window.x = new XMLHttpRequest();"
           "x.addEventListener('readystatechange', () => EV.push('rs' + x.readyState));"
           "x.onloadstart = () => EV.push('loadstart'); x.onprogress = () => EV.push('progress');"
           "x.onload = () => EV.push('load'); x.onloadend = () => EV.push('loadend'); x.onerror = () => EV.push('ERROR');"
           "x.open('GET', '/text'); EV.push('opened'); x.send(); EV.push('sent');");
    CHECK_EQ(r.ev("EV.join()"), "rs1,opened,loadstart,sent");
    CHECK(r.waitFor("x.readyState === 4 && EV.indexOf('loadend') >= 0"));
    CHECK_EQ(r.ev("EV.join()"), "rs1,opened,loadstart,sent,rs2,rs3,progress,rs4,load,loadend");
    CHECK_EQ(r.ev("x.status + '|' + x.statusText + '|' + x.responseText + '|' + x.response + '|' + x.responseURL"), "200|OK|hello|hello|https://site.test/text");
    CHECK_EQ(r.ev("x.getResponseHeader('CONTENT-type')"), "text/plain");
    CHECK_EQ(r.ev("x.getResponseHeader('x-multi')"), "a, b");
    CHECK_EQ(r.ev("x.getResponseHeader('set-cookie')"), "null");            // hidden from pages
    CHECK_EQ(r.ev("x.getResponseHeader('nope')"), "null");
    CHECK_EQ(r.ev("JSON.stringify(x.getAllResponseHeaders())"), "\"content-type: text/plain\\r\\nx-multi: a, b\\r\\n\"");
    CHECK_EQ(net.last().pageUrl, "https://site.test/p/index.html");
    CHECK_EQ(net.last().mode, "cors");

    // on* properties start as null and accept null again
    CHECK_EQ(r.ev("(() => { const q = new XMLHttpRequest(); const a = q.onload; q.onload = () => 1; const b = typeof q.onload; q.onload = null; return a + ',' + b + ',' + q.onload; })()"), "null,function,null");
    CHECK_EQ(r.ev("XMLHttpRequest.DONE + ',' + x.DONE + ',' + x.UNSENT"), "4,4,0");

    // redirect: responseURL is the final URL
    r.exec("window.y = new XMLHttpRequest(); y.open('GET', '/redirect'); y.send();");
    CHECK(r.waitFor("y.readyState === 4"));
    CHECK_EQ(r.ev("y.responseURL"), "https://site.test/final");

    // an HTTP error status is still a successful load
    r.exec("window.EV = []; window.z = new XMLHttpRequest(); z.onload = () => EV.push('load:' + z.status); z.onerror = () => EV.push('error');"
           "z.open('GET', '/missing'); z.send();");
    CHECK(r.waitFor("EV.length > 0"));
    CHECK_EQ(r.ev("EV.join()"), "load:404");

    // responseType json / arraybuffer; responseText is off-limits for them
    r.exec("window.j = new XMLHttpRequest(); j.open('GET', '/json'); j.responseType = 'json'; j.send();");
    CHECK(r.waitFor("j.readyState === 4"));
    CHECK_EQ(r.ev("j.response.a + ',' + j.response.b.length"), "1,2");
    CHECK_EQ(r.ev("(() => { try { return j.responseText; } catch (e) { return e.name; } })()"), "InvalidStateError");
    r.exec("window.ab = new XMLHttpRequest(); ab.open('GET', '/bin'); ab.responseType = 'arraybuffer'; ab.send();");
    CHECK(r.waitFor("ab.readyState === 4"));
    CHECK_EQ(r.ev("ab.response instanceof ArrayBuffer && ab.response.byteLength === 256 && new Uint8Array(ab.response)[255] === 255"), "true");

    // network error
    r.exec("window.EV = []; window.e = new XMLHttpRequest(); e.onerror = () => EV.push('error:' + e.status + ':' + e.readyState); e.onload = () => EV.push('LOAD');"
           "e.onloadend = () => EV.push('loadend'); e.open('GET', '/fail'); e.send();");
    CHECK(r.waitFor("EV.length >= 2"));
    CHECK_EQ(r.ev("EV.join()"), "error:0:4,loadend");
    CHECK_EQ(r.ev("e.getAllResponseHeaders() + e.responseText + e.statusText"), "");

    // reuse after completion
    r.exec("window.EV = []; x.onload = () => EV.push('again:' + x.responseText); x.open('POST', '/echo'); x.send('payload');");
    CHECK(r.waitFor("EV.some((e) => e.startsWith('again'))"));
    CHECK_EQ(r.ev("EV.filter((e) => e.startsWith('again')).join()"), "again:POST|payload");
}

static void testXhrRequests() {
    Net net;
    Rig r(net.opts());
    r.exec("window.x = new XMLHttpRequest(); x.open('POST', '/api'); x.setRequestHeader('X-A', '1'); x.setRequestHeader('x-a', '2');"
           "x.setRequestHeader('Cookie', 'evil=1'); x.setRequestHeader('Host', 'evil.test'); x.setRequestHeader('Sec-Fetch-Mode', 'x');"
           "x.setRequestHeader('Content-Type', 'application/json'); x.send('{\"k\":1}');");
    CHECK(r.waitFor("x.readyState === 4"));
    CHECK_EQ(net.last().method, "POST");
    CHECK_EQ(net.last().body, "{\"k\":1}");
    CHECK_EQ(headerOf(net.last(), "x-a"), "1, 2");                        // setRequestHeader appends
    CHECK_EQ(headerOf(net.last(), "content-type"), "application/json");
    CHECK_EQ(headerOf(net.last(), "cookie"), "<none>");                   // forbidden headers are refused...
    CHECK_EQ(headerOf(net.last(), "host"), "<none>");
    CHECK_EQ(headerOf(net.last(), "sec-fetch-mode"), "<none>");           // ...so the page cannot spoof them

    // default content type for a string body, and none for GET
    r.exec("window.x = new XMLHttpRequest(); x.open('POST', '/api'); x.send('abc');");
    CHECK(r.waitFor("x.readyState === 4"));
    CHECK_EQ(headerOf(net.last(), "content-type"), "text/plain;charset=UTF-8");
    r.exec("window.x = new XMLHttpRequest(); x.open('GET', '/api'); x.send('ignored');");
    CHECK(r.waitFor("x.readyState === 4"));
    CHECK(!net.last().hasBody);

    // PUT/DELETE and FormData
    r.exec("window.x = new XMLHttpRequest(); x.open('PUT', '/api'); x.send('p');");
    CHECK(r.waitFor("x.readyState === 4"));
    CHECK_EQ(net.last().method, "PUT");
    r.exec("window.x = new XMLHttpRequest(); x.open('delete', '/api'); x.send();");
    CHECK(r.waitFor("x.readyState === 4"));
    CHECK_EQ(net.last().method, "DELETE");
    r.exec("window.x = new XMLHttpRequest(); const f = new FormData(); f.append('a', '1'); x.open('POST', '/api'); x.send(f);");
    CHECK(r.waitFor("x.readyState === 4"));
    CHECK(headerOf(net.last(), "content-type").rfind("multipart/form-data; boundary=", 0) == 0);

    // basic auth from open()
    r.exec("window.x = new XMLHttpRequest(); x.open('GET', '/api', true, 'user', 'pw'); x.send();");
    CHECK(r.waitFor("x.readyState === 4"));
    CHECK_EQ(headerOf(net.last(), "authorization"), "Basic dXNlcjpwdw==");

    // upload events fire for a request with a body
    r.exec("window.EV = []; window.x = new XMLHttpRequest(); x.upload.onloadstart = () => EV.push('u-start'); x.upload.onload = () => EV.push('u-load');"
           "x.upload.onloadend = () => EV.push('u-end'); x.onload = () => EV.push('load'); x.open('POST', '/api'); x.send('12345');");
    CHECK(r.waitFor("EV.indexOf('load') >= 0"));
    CHECK_EQ(r.ev("EV.join()"), "u-start,u-load,u-end,load");

    // state errors
    CHECK_EQ(r.ev("(() => { try { new XMLHttpRequest().send(); return 'no'; } catch (e) { return e.name; } })()"), "InvalidStateError");
    CHECK_EQ(r.ev("(() => { try { new XMLHttpRequest().setRequestHeader('a', 'b'); return 'no'; } catch (e) { return e.name; } })()"), "InvalidStateError");
    CHECK_EQ(r.ev("(() => { try { const q = new XMLHttpRequest(); q.open('GET', '/x'); q.send(); q.send(); return 'no'; } catch (e) { return e.name; } })()"), "InvalidStateError");
    CHECK_EQ(r.ev("(() => { try { new XMLHttpRequest().open('TRACE', '/x'); return 'no'; } catch (e) { return e.name; } })()"), "SecurityError");
    CHECK_EQ(r.ev("(() => { try { new XMLHttpRequest().open('bad method', '/x'); return 'no'; } catch (e) { return e.name; } })()"), "SyntaxError");
    CHECK_EQ(r.ev("Object.prototype.toString.call(new XMLHttpRequest())"), "[object XMLHttpRequest]");
}

static void testXhrAbortTimeoutSync() {
    Net net;
    Rig r(net.opts());

    // abort() while in flight: readystatechange(4), abort, loadend, then UNSENT; the late response is dropped
    r.exec("window.EV = []; window.x = new XMLHttpRequest(); x.onreadystatechange = () => EV.push('rs' + x.readyState);"
           "x.onabort = () => EV.push('abort'); x.onload = () => EV.push('LOAD'); x.onloadend = () => EV.push('loadend');"
           "x.open('GET', '/slow'); x.send(); x.abort(); EV.push('after:' + x.readyState + ':' + x.status);");
    CHECK_EQ(r.ev("EV.join()"), "rs1,rs4,abort,loadend,after:0:0");
    r.spin(450);
    CHECK_EQ(r.ev("EV.join()"), "rs1,rs4,abort,loadend,after:0:0");

    // abort() on an idle request does nothing
    CHECK_EQ(r.ev("(() => { const q = new XMLHttpRequest(); q.abort(); q.open('GET', '/x'); q.abort(); return q.readyState; })()"), "1");

    // timeout
    r.exec("window.EV = []; window.t = new XMLHttpRequest(); t.timeout = 60; t.ontimeout = () => EV.push('timeout:' + t.status + ':' + t.readyState);"
           "t.onload = () => EV.push('LOAD'); t.onloadend = () => EV.push('loadend'); t.open('GET', '/slow'); t.send();");
    CHECK(r.waitFor("EV.length >= 2"));
    CHECK_EQ(r.ev("EV.join()"), "timeout:0:4,loadend");
    r.spin(400);
    CHECK_EQ(r.ev("EV.join()"), "timeout:0:4,loadend");

    // synchronous XHR returns with the result in place
    r.exec("window.EV = []; window.s = new XMLHttpRequest(); s.onreadystatechange = () => EV.push('rs' + s.readyState); s.onload = () => EV.push('load');"
           "s.open('GET', '/text', false); s.send(); EV.push('returned:' + s.status + ':' + s.responseText);");
    CHECK_EQ(r.ev("EV.join()"), "rs1,rs4,load,returned:200:hello");
    CHECK_EQ(r.ev("(() => { try { const q = new XMLHttpRequest(); q.open('GET', '/fail', false); q.send(); return 'no'; } catch (e) { return e.name; } })()"), "NetworkError");
    CHECK_EQ(r.ev("(() => { try { const q = new XMLHttpRequest(); q.open('GET', '/x', false); q.timeout = 5; return 'no'; } catch (e) { return e.name; } })()"), "InvalidAccessError");
    CHECK_EQ(r.ev("(() => { try { const q = new XMLHttpRequest(); q.open('GET', '/x', false); q.responseType = 'json'; return 'no'; } catch (e) { return e.name; } })()"), "InvalidAccessError");

    // an exception in a handler is reported and does not break the rest of the chain
    r.exec("window.EV = []; window.b = new XMLHttpRequest(); b.onload = () => { throw new Error('handler boom'); }; b.onloadend = () => EV.push('loadend');"
           "b.open('GET', '/text'); b.send();");
    CHECK(r.waitFor("EV.length > 0"));
    bool reported = false;
    for (auto& e : r.host.pageErrors()) if (e.find("handler boom") != std::string::npos) reported = true;
    CHECK(reported);
}

static void testCookies() {
    // no host store: a per-page in-memory jar
    {
        Rig r;
        r.exec("document.cookie = 'a=1'; document.cookie = 'b=2; path=/'; document.cookie = 'h=1; HttpOnly';");
        CHECK_EQ(r.ev("document.cookie"), "a=1; b=2");                 // /p is a longer path than /, so `a` comes first
        r.exec("document.cookie = 'a=; max-age=0';");
        CHECK_EQ(r.ev("document.cookie"), "b=2");
        r.exec("document.cookie = 'c=3; expires=Thu, 01 Jan 1970 00:00:00 GMT; path=/'; document.cookie = 'd=4; max-age=100; path=/';");
        CHECK_EQ(r.ev("document.cookie"), "b=2; d=4");
        r.exec("document.cookie = 'b=9; path=/';");
        CHECK_EQ(r.ev("document.cookie"), "b=9; d=4");
    }
    // with a host store: reads and writes go through it, with the document's URL
    {
        std::string store, getUrl, setUrl;
        js::HostOptions o;
        o.cookieGet = [&](const std::string& u) { getUrl = u; return store; };
        o.cookieSet = [&](const std::string& u, const std::string& c) { setUrl = u; store = c; };
        Rig r(std::move(o));
        r.exec("document.cookie = 'x=1; Path=/; Secure';");
        CHECK_EQ(store, "x=1; Path=/; Secure");
        CHECK_EQ(setUrl, "https://site.test/p/index.html");
        store = "server=set; theme=dark";
        CHECK_EQ(r.ev("document.cookie"), "server=set; theme=dark");
        CHECK_EQ(getUrl, "https://site.test/p/index.html");
        r.exec("history.pushState({}, '', '/other/page');");              // the document URL moves with pushState
        r.ev("document.cookie");
        CHECK_EQ(getUrl, "https://site.test/other/page");
    }
}

static void testBytesAndText() {
    Rig r;
    CHECK_EQ(r.ev("new TextEncoder().encode('h\\u00e9\\u20ac').length"), "6");
    CHECK_EQ(r.ev("new TextDecoder().decode(new TextEncoder().encode('h\\u00e9\\u20ac'))"), "h\xC3\xA9\xE2\x82\xAC");
    CHECK_EQ(r.ev("new TextDecoder('utf8').decode(new Uint8Array([0xEF, 0xBB, 0xBF, 0x41]))"), "A");
    CHECK_EQ(r.ev("(() => { try { new TextDecoder('latin1'); return 'no'; } catch (e) { return e.name; } })()"), "RangeError");
    CHECK_EQ(r.ev("JSON.stringify(new TextEncoder().encodeInto('abc\\u20ac', new Uint8Array(4)))"), "{\"read\":3,\"written\":3}");   // never splits a character
    CHECK_EQ(r.ev("(async () => 1)() instanceof Promise"), "true");
    // Response / Request constructors
    CHECK_EQ(r.ev("(() => { try { new Response('x', {status: 100}); return 'no'; } catch (e) { return e.name; } })()"), "RangeError");
    CHECK_EQ(r.ev("(() => { const q = new Response('abc'); return q.status + ',' + q.ok + ',' + q.headers.get('content-type'); })()"), "200,true,text/plain;charset=UTF-8");
    CHECK_EQ(r.ev("Response.error().type + Response.json({a: 1}).headers.get('content-type') + Response.redirect('/x', 301).headers.get('location')"), "errorapplication/jsonhttps://site.test/x");
    r.exec("window.R = []; new Response('{\"k\":[1]}').json().then((j) => R.push(j.k[0]));");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "1");
    // the Cache API still works on top of the new Response
    r.exec("window.R = []; caches.open('c').then((c) => c.put('/k', new Response('cached', {headers: {'X-H': '1'}})).then(() => c.match('/k')))"
           ".then((m) => m.text().then((t) => R.push(t + ':' + m.headers.get('x-h'))));");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "cached:1");
}

static void testBlobAndFile() {
    Net net;
    Rig r(net.opts());

    // Blob basics: concatenates parts, lower-cases type, slice()/text()/arrayBuffer() resolve immediately (no streams)
    CHECK_EQ(r.ev("new Blob(['ab', new Uint8Array([99, 100])], {type: 'Text/Plain'}).type"), "text/plain");
    CHECK_EQ(r.ev("new Blob(['ab', new Uint8Array([99, 100])]).size"), "4");
    CHECK_EQ(r.ev("new Blob().size + ',' + new Blob().type"), "0,");
    r.exec("window.R = []; new Blob(['hello world']).slice(2, 5).text().then((t) => R.push(t));");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "llo");
    CHECK_EQ(r.ev("Blob.toString()"), "function Blob() { [native code] }");

    // File extends Blob and requires a name
    CHECK_EQ(r.ev("(() => { const f = new File(['abc'], 'x.txt', {type: 'text/plain'}); return f.name + ',' + f.size + ',' + (f instanceof Blob) + ',' + (typeof f.lastModified); })()"), "x.txt,3,true,number");
    CHECK_EQ(r.ev("(() => { try { new File(['a']); return 'no'; } catch (e) { return e.name; } })()"), "TypeError");

    // FormData: a bare Blob becomes a File entry named 'blob'; an appended File keeps its own name
    r.exec("window.fd = new FormData(); fd.append('plain', 'hi'); fd.append('b', new Blob(['xy'], {type: 'text/csv'})); fd.append('f', new File(['zzz'], 'r.bin', {type: 'application/octet-stream'}));");
    CHECK_EQ(r.ev("fd.get('b').name + ',' + fd.get('b').type + ',' + (fd.get('b') instanceof File)"), "blob,text/csv,true");
    CHECK_EQ(r.ev("fd.get('f').name"), "r.bin");

    // fetch() with that FormData sends the raw file bytes inside the multipart envelope
    r.exec("window.R = []; fetch('/api', {method: 'POST', body: fd}).then((x) => x.text()).then((t) => R.push(t));");
    CHECK(r.waitFor("R.length > 0"));
    {
        std::string ct = headerOf(net.last(), "content-type");
        CHECK(ct.rfind("multipart/form-data; boundary=----", 0) == 0);
        const std::string& body = net.last().body;
        CHECK(body.find("name=\"plain\"\r\n\r\nhi\r\n") != std::string::npos);
        CHECK(body.find("name=\"b\"; filename=\"blob\"\r\nContent-Type: text/csv\r\n\r\nxy\r\n") != std::string::npos);
        CHECK(body.find("name=\"f\"; filename=\"r.bin\"\r\nContent-Type: application/octet-stream\r\n\r\nzzz\r\n") != std::string::npos);
    }

    // fetch() with a Blob body: its type becomes Content-Type, bytes go out untouched (no UTF-8 round trip)
    r.exec("R = []; fetch('/api', {method: 'PUT', body: new Blob([new Uint8Array([1, 2, 3])], {type: 'application/x-test'})}).then((x) => R.push('ok'));");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(headerOf(net.last(), "content-type"), "application/x-test");
    CHECK_EQ(net.last().body, std::string("\x01\x02\x03", 3));

    // Response.blob(): Content-Type carried onto the Blob, binary bytes round-trip exactly
    r.exec("R = []; fetch('/bin').then((x) => x.blob()).then((b) => b.arrayBuffer()).then((ab) => { const u = new Uint8Array(ab); let ok = u.length === 256; for (let i = 0; i < 256; i++) if (u[i] !== i) ok = false; R.push(ok); });");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "true");
    r.exec("R = []; fetch('/text').then((x) => x.blob()).then((b) => R.push(b.type + ':' + b.size));");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "text/plain:5");

    // a Response body can only be read once, blob() included
    r.exec("window.R = []; const resp = new Response('x'); resp.text(); resp.blob().catch((e) => R.push(e.name));");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "TypeError");

    // XMLHttpRequest responseType 'blob': cached across reads, not rebuilt per access
    r.exec("window.x = new XMLHttpRequest(); x.open('GET', '/text'); x.responseType = 'blob'; x.send();");
    CHECK(r.waitFor("x.readyState === 4"));
    CHECK_EQ(r.ev("x.response === x.response"), "true");
    r.exec("window.R = []; x.response.text().then((t) => R.push(x.response.type + ':' + t));");
    CHECK(r.waitFor("R.length > 0"));
    CHECK_EQ(r.ev("R[0]"), "text/plain:hello");
}

static void testHostPlumbing() {
    // <script src> goes through the HTTP callback as a no-cors "script" request from the page
    {
        Net net;
        ScriptRegistry sr;
        const char* page = "<!doctype html><html><head><script src=\"/lib.js\"></script>"
                           "<script>document.documentElement.setAttribute('data-lib', String(window.__lib));</script></head><body></body></html>";
        Fetcher fetcher = [page](const std::string&) { FetchResult r; r.ok = true; r.status = 200; r.body = page; return r; };
        js::QuickJsHost host(net.opts());
        Hydrator hy(fetcher, sr, host);
        hy.options.saveSnapshots = false;
        auto res = hy.hydrate("https://site.test/p/index.html");
        CHECK(res.ok);
        CHECK(res.afterHtml.find("data-lib=\"loaded\"") != std::string::npos);   // downloaded, evaluated, and visible to the next script
        CHECK_EQ(net.last().url, "https://site.test/lib.js");
        CHECK_EQ(net.last().dest, "script");
        CHECK_EQ(net.last().mode, "no-cors");
        CHECK_EQ(net.last().pageUrl, "https://site.test/p/index.html");
        CHECK_EQ(net.last().method, "GET");
    }
    // legacy callbacks: httpGet alone serves GET and refuses other methods
    {
        js::HostOptions o;
        o.httpGet = [](const std::string& url) { js::HttpGetResult g; g.status = 200; g.body = "got:" + url; return g; };
        Rig r(std::move(o));
        r.exec("window.R = []; fetch('/g').then((x) => x.text()).then((t) => R.push(t));");
        CHECK(r.waitFor("R.length > 0"));
        CHECK_EQ(r.ev("R[0]"), "got:https://site.test/g");
        r.exec("R = []; fetch('/g', {method: 'POST', body: 'x'}).catch((e) => R.push(e.name));");
        CHECK(r.waitFor("R.length > 0"));
        CHECK_EQ(r.ev("R[0]"), "TypeError");
    }
    // legacy callbacks: httpRequest gets the method, body and headers
    {
        std::string seen;
        js::HostOptions o;
        o.httpRequest = [&seen](const std::string& m, const std::string& u, const std::string& b, const std::vector<std::pair<std::string, std::string>>& h) {
            js::HttpGetResult g; g.status = 200; g.body = "ok";
            seen = m + " " + u + " " + b + " " + std::to_string(h.size());
            return g;
        };
        Rig r(std::move(o));
        r.exec("window.R = []; fetch('/p', {method: 'PUT', body: 'b', headers: {a: '1'}}).then((x) => x.text()).then((t) => R.push(t));");
        CHECK(r.waitFor("R.length > 0"));
        CHECK_EQ(seen, "PUT https://site.test/p b 2");                    // a + the default Content-Type
        CHECK_EQ(r.ev("R[0]"), "ok");
    }
    // no network at all: fetch rejects, XHR errors
    {
        Rig r;
        r.exec("window.R = []; fetch('/x').catch((e) => R.push(e.name));");
        CHECK(r.waitFor("R.length > 0"));
        CHECK_EQ(r.ev("R[0]"), "TypeError");
        r.exec("window.EV = []; window.x = new XMLHttpRequest(); x.onerror = () => EV.push('error'); x.open('GET', '/x'); x.send();");
        CHECK(r.waitFor("EV.length > 0"));
    }
    // tearing a page down with a request still in flight neither crashes nor leaks a callback into a dead context
    {
        Net net;
        auto t0 = std::chrono::steady_clock::now();
        {
            Rig r(net.opts());
            r.exec("fetch('/slow'); const x = new XMLHttpRequest(); x.open('GET', '/slow'); x.send();");
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 2500);
    }
}

int main() {
    testHeaders();
    testFetchBasics();
    testFetchRequests();
    testAbort();
    testXhrAsync();
    testXhrRequests();
    testXhrAbortTimeoutSync();
    testCookies();
    testBytesAndText();
    testBlobAndFile();
    testHostPlumbing();
    std::printf(failures ? "js_net: %d FAILED\n" : "js_net: all passed\n", failures);
    return failures ? 1 : 0;
}
