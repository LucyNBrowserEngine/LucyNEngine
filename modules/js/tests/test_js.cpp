#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <lucyn/core/paths.h>
#include <lucyn/js/quickjs_host.h>
#include <lucyn/shell/hydrator.h>

using namespace lucyn;
using namespace lucyn::shell;
namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
#define CHECK_EQ(got, want) do { std::string g_ = (got), w_ = (want); if (g_ != w_) { std::printf("FAIL %s:%d  %s\n     got:  %s\n     want: %s\n", __FILE__, __LINE__, #got, g_.c_str(), w_.c_str()); ++failures; } } while (0)

static std::string join(const std::vector<std::string>& v, const char* sep = "|") {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += sep; s += v[i]; }
    return s;
}

// A shell page + hooks + the real host, attached. `ev("expr")` evaluates an expression inside a user script and returns String(expr).
struct Rig {
    std::unique_ptr<dom::Document> doc;
    NativeHooks hooks;
    js::QuickJsHost host;          // declared after doc and hooks: destroyed (and detached) first
    std::vector<std::string> out;
    bool attached = false;

    explicit Rig(js::HostOptions o = {}, const std::string& html = "<html><head></head><body></body></html>",
                 const std::string& url = "https://site.test/p/index.html")
        : doc(dom::parseHtml(html)), host(std::move(o)) {
        hooks.add("out", [this](const std::vector<std::string>& a) { out.push_back(join(a)); return HookResult{true, ""}; });
        hooks.add("page.url", [url](const std::vector<std::string>&) { return HookResult{true, url}; });
        hooks.add("fail", [](const std::vector<std::string>&) { return HookResult{false, "nope"}; });
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
    void mount(const std::string& bodyHtml) { doc->body()->setInnerHTML(bodyHtml); }   // site content arrives after attach
};

static js::Persona testPersona() {
    js::Persona p;
    p.surface.userAgent = "Mozilla/5.0 (X11; Linux x86_64) TestBrowser/1.0";
    p.surface.platform = "Linux x86_64"; p.surface.vendor = "Test Inc."; p.surface.productSub = "20030107";
    p.surface.languages = {"de-DE", "de", "en"}; p.surface.maxTouchPoints = 0;
    p.surface.hasUserAgentData = true; p.surface.hasDeviceMemory = true;
    p.surface.brands = {{"Not;A=Brand", "99"}, {"TestBrowser", "1"}};
    p.surface.fullVersionList = {{"Not;A=Brand", "99.0.0.0"}, {"TestBrowser", "1.2.3.4"}};
    p.surface.secChUaPlatform = "Linux"; p.surface.uaArchitecture = "x86"; p.surface.uaBitness = "64";
    p.device.cpuCores = 12; p.device.memoryGb = 16; p.device.screenW = 1920; p.device.screenH = 1080;
    p.device.pixelRatio = 1.25; p.device.reservedBottom = 40;
    return p;
}

static Fetcher fetchOf(std::string body) {
    return [=](const std::string&) { FetchResult r; r.ok = true; r.status = 200; r.body = body; return r; };
}

int main() {
    fs::path tmp = fs::temp_directory_path() / "lucyn_js_test";
    fs::remove_all(tmp);
    core::setDataRootForTests((tmp / "nothing-data").string());

    // ---- engine basics + the lucyn object -----------------------------------------------------------------------------------------
    {
        Rig r;
        CHECK(r.attached);
        CHECK_EQ(r.ev("1+1"), "2");
        CHECK_EQ(r.ev("lucyn.page.url()"), "https://site.test/p/index.html");
        CHECK_EQ(r.ev("Object.isFrozen(lucyn)"), "true");
        CHECK_EQ(r.ev("Object.keys(lucyn).length"), "0");                                  // not enumerable
        CHECK_EQ(r.ev("(()=>{try{lucyn.fail()}catch(e){return e.message}})()"), "nope");  // a failed hook is a JS Error
        r.out.clear();
        CHECK(r.run("lucyn.out('a',1,{x:[1,2]},null,undefined)").ok);
        CHECK_EQ(join(r.out, ";"), "a|1|{\"x\":[1,2]}|null|undefined");                    // hook args are strings
        auto bad = r.run("this is not js");
        CHECK(!bad.ok && bad.error.find("SyntaxError") != std::string::npos);
        auto thr = r.run("\n\nnull.x", "my-script");
        CHECK(!thr.ok && thr.error.find("my-script:3") != std::string::npos);               // line numbers are the author's
        CHECK_EQ(r.ev("2*21"), "42");                                                       // still alive after errors
    }
    {   // `lucyn` is invisible to the page
        Rig r;
        // a function defined inside a user script closes over its own `lucyn`; what matters is that PAGE code cannot see it
        r.mount("<script>window.pageSees = [typeof lucyn, 'lucyn' in window, typeof window.lucyn].join();</script>");
        CHECK(r.host.runPageScripts().ok);
        CHECK_EQ(r.ev("pageSees"), "undefined,false,undefined");
        CHECK_EQ(r.ev("'lucyn' in window"), "false");
        CHECK_EQ(r.ev("Object.getOwnPropertyNames(window).includes('lucyn')"), "false");
        CHECK_EQ(r.ev("(()=>{let f=false;for(const k in window)if(k==='lucyn')f=true;return f})()"), "false");
        CHECK(r.run("var topLevelVar = 1; function topLevelFn(){}").ok);
        CHECK_EQ(r.ev("typeof topLevelVar"), "undefined");                                  // scripts live in a function scope
    }

    // ---- DOM bridge ----------------------------------------------------------------------------------------------------------------
    {
        Rig r;
        r.mount("<div id=\"a\" class=\"x y\" data-foo-bar=\"1\"><span>one</span><b>two</b></div><p id=\"p\">text</p>");
        CHECK_EQ(r.ev("document.getElementById('a') === document.getElementById('a')"), "true");
        CHECK_EQ(r.ev("(()=>{const a=document.createElement('div');a.setAttribute('data-x','1');a.appendChild(document.createTextNode('same'));const b=a.cloneNode(true);return a.isEqualNode(b)+':'+a.isSameNode(b)})()"), "true:false");
        CHECK_EQ(r.ev("(()=>{const a=document.createElement('div'),b=document.createElement('div');a.textContent='one';b.textContent='two';return a.isEqualNode(b)})()"), "false");
        CHECK_EQ(r.ev("document.querySelector('span') === document.getElementById('a').firstChild"), "true");
        CHECK_EQ(r.ev("document.getElementById('a').tagName + document.getElementById('a').nodeType"), "DIV1");
        CHECK_EQ(r.ev("document.getElementById('a').textContent"), "onetwo");
        CHECK_EQ(r.ev("document.getElementById('a').children.length"), "2");
        CHECK_EQ(r.ev("document.getElementById('a').getAttribute('data-foo-bar')"), "1");
        CHECK_EQ(r.ev("document.getElementById('a').dataset.fooBar"), "1");
        CHECK_EQ(r.ev("(()=>{const a=document.getElementById('a');a.dataset.newKey='v';return a.getAttribute('data-new-key')+Object.keys(a.dataset)})()"), "vfooBar,newKey");
        CHECK_EQ(r.ev("document.getElementById('missing')"), "null");
        CHECK_EQ(r.ev("document.querySelectorAll('div > span').length"), "1");
        CHECK_EQ(r.ev("(()=>{const e=document.createElement('section');e.id='s';e.textContent='hi';document.body.appendChild(e);return document.getElementById('s').outerHTML})()"), "<section id=\"s\">hi</section>");
        CHECK_EQ(r.ev("(()=>{const p=document.getElementById('p');const a=document.getElementById('a');a.appendChild(p);return p.parentNode===a})()"), "true");
        CHECK_EQ(r.ev("(()=>{const a=document.getElementById('a');a.removeChild(a.lastChild);return a.children.length})()"), "2");
        CHECK_EQ(r.ev("(()=>{try{document.body.removeChild(document.createElement('i'))}catch(e){return e.name+':'+(e instanceof DOMException)+':'+e.code}})()"), "NotFoundError:true:8");
        CHECK_EQ(r.ev("(()=>{try{document.createElement('1bad')}catch(e){return e.name}})()"), "InvalidCharacterError");
        CHECK_EQ(r.ev("(()=>{const a=document.getElementById('a');a.innerHTML='<i>n</i>';return a.innerHTML+a.children.length})()"), "<i>n</i>1");
        CHECK_EQ(r.ev("(()=>{const a=document.getElementById('a');a.insertAdjacentHTML('beforeend','<u>z</u>');a.insertAdjacentHTML('afterbegin','<em>y</em>');return a.innerHTML})()"), "<em>y</em><i>n</i><u>z</u>");
        CHECK_EQ(r.ev("(()=>{const a=document.getElementById('a');a.append('t',document.createElement('hr'));a.prepend('P');return a.textContent})()"), "Pynzt");
        CHECK_EQ(r.ev("(()=>{const a=document.getElementById('a');const c=a.cloneNode(true);return c!==a&&c.parentNode===null&&c.innerHTML===a.innerHTML})()"), "true");
        CHECK_EQ(r.ev("(()=>{const a=document.getElementById('a');a.firstElementChild.remove();return a.firstElementChild.tagName})()"), "I");
        CHECK_EQ(r.ev("(()=>{const d=document.createElement('div');d.outerHTML='<x/>'})()").substr(0, 4), "ERR:");   // no parent
        CHECK_EQ(r.ev("document.title"), "");
        CHECK_EQ(r.ev("(()=>{document.title='  Hello  World ';return document.title+'|'+document.head.querySelector('title').textContent})()"), "Hello World|  Hello  World ");
        CHECK_EQ(r.ev("document.documentElement.nodeName + document.body.nodeName + document.nodeName"), "HTMLBODY#document");
        CHECK_EQ(r.ev("document.body.parentNode === document.documentElement && document.documentElement.parentNode === document"), "true");
        CHECK_EQ(r.ev("document.body.ownerDocument === document && document.ownerDocument === null"), "true");
        CHECK_EQ(r.ev("document.createTextNode('x').nodeType + ':' + document.createComment('c').nodeType + ':' + document.createTextNode('x').data"), "3:8:x");
        CHECK_EQ(r.ev("document.body instanceof HTMLElement && document.body instanceof Element && document.body instanceof Node && document.body instanceof EventTarget"), "true");
        CHECK_EQ(r.ev("document.createElement('a') instanceof HTMLAnchorElement && !(document.createElement('div') instanceof HTMLAnchorElement)"), "true");
        CHECK_EQ(r.ev("Object.prototype.toString.call(document.body) + Object.prototype.toString.call(document)"), "[object HTMLBodyElement][object HTMLDocument]");
        CHECK_EQ(r.ev("(()=>{const x=document.createElement('div');return x.isConnected+':'+(document.body.appendChild(x),x.isConnected)})()"), "false:true");
        CHECK_EQ(r.ev("(()=>{const a=document.createElement('a');a.setAttribute('href','/x?y=1');return a.href+'|'+a.pathname+'|'+a.search+'|'+a.hostname})()"), "https://site.test/x?y=1|/x|?y=1|site.test");
        CHECK_EQ(r.ev("(()=>{const i=document.createElement('input');i.disabled=true;i.value='v';return i.outerHTML})()"), "<input disabled=\"\" value=\"v\">");
        // matches / closest, including on detached trees
        CHECK_EQ(r.ev("document.getElementById('a').matches('div.x')"), "true");
        CHECK_EQ(r.ev("document.querySelector('em, i, u').closest('div').id"), "a");
        CHECK_EQ(r.ev("(()=>{const d=document.createElement('div');d.className='k';const s=document.createElement('span');d.appendChild(s);return d.matches('.k')+':'+(s.closest('.k')===d)+':'+s.matches('.k')})()"), "true:true:false");
        // classList / style
        CHECK_EQ(r.ev("(()=>{const a=document.getElementById('a');a.classList.add('z');a.classList.remove('x');const t=a.classList.toggle('q');return a.className+'|'+t+'|'+a.classList.contains('y')+'|'+a.classList.length})()"), "y z q|true|true|3");
        CHECK_EQ(r.ev("(()=>{try{document.body.classList.add('a b')}catch(e){return e.name}})()"), "InvalidCharacterError");
        CHECK_EQ(r.ev("(()=>{const a=document.getElementById('a');a.style.display='none';a.style.backgroundColor='red';return a.getAttribute('style')+'|'+a.style.display+'|'+a.style.getPropertyValue('background-color')})()"),
                 "display: none; background-color: red;|none|red");
        CHECK_EQ(r.ev("(()=>{const a=document.getElementById('a');a.style.cssText='color:blue; background:url(data:x;y)';return a.style.color+'|'+a.style.background+'|'+a.style.length})()"), "blue|url(data:x;y)|2");
        CHECK_EQ(r.ev("(()=>{const a=document.getElementById('a');a.style.display='';return a.style.display+'|'+a.hasAttribute('style')})()"), "|true");
        CHECK_EQ(r.ev("document.getElementById('a').getBoundingClientRect().width"), "0");
        CHECK_EQ(r.ev("getComputedStyle(document.body) !== null"), "true");
    }

    // ---- looks native --------------------------------------------------------------------------------------------------------------
    {
        Rig r;
        CHECK_EQ(r.ev("Event.toString()"), "function Event() { [native code] }");
        CHECK_EQ(r.ev("document.createElement.toString()"), "function createElement() { [native code] }");
        CHECK_EQ(r.ev("document.body.classList.add.toString()"), "function add() { [native code] }");
        CHECK_EQ(r.ev("Function.prototype.toString.call(Function.prototype.toString)"), "function toString() { [native code] }");
        CHECK_EQ(r.ev("setTimeout.toString()"), "function setTimeout() { [native code] }");
        CHECK_EQ(r.ev("(function mine(){ return 1 }).toString()"), "function mine(){ return 1 }");   // real functions are untouched
        CHECK_EQ(r.ev("Object.getOwnPropertyNames(window).includes('__n') || typeof __n"), "undefined");
        CHECK_EQ(r.ev("Object.keys(window).includes('Event')"), "false");                  // constructors are not enumerable
        CHECK_EQ(r.ev("Object.keys(window).includes('setTimeout')"), "true");
        CHECK_EQ(r.ev("window === self && window === top && window === globalThis && window.window === window"), "true");
        CHECK_EQ(r.ev("typeof require + typeof process + typeof module"), "undefinedundefinedundefined");
    }

    // ---- events --------------------------------------------------------------------------------------------------------------------
    {
        Rig r;
        r.mount("<div id=\"outer\"><button id=\"btn\">b</button></div>");
        CHECK(r.run(R"(
            const log = []; window.__log = log;
            const o = document.getElementById('outer'), b = document.getElementById('btn');
            document.addEventListener('click', e => log.push('doc:' + e.eventPhase));
            window.addEventListener('click', e => log.push('win'));
            o.addEventListener('click', e => log.push('o-cap'), true);
            o.addEventListener('click', e => log.push('o-bub'));
            b.addEventListener('click', e => log.push('b:' + e.eventPhase + ':' + (e.target === b) + ':' + (e.currentTarget === b) + ':' + e.isTrusted));
            b.addEventListener('click', function once(e) { log.push('once'); }, { once: true });
            b.onclick = () => log.push('onclick');
            b.click(); b.click();
        )").ok);
        CHECK_EQ(r.ev("__log.join(',')"),
                 "o-cap,b:2:true:true:false,once,onclick,o-bub,doc:3,win,o-cap,b:2:true:true:false,onclick,o-bub,doc:3,win");
        CHECK_EQ(r.ev("(()=>{const l=[];const e=document.createElement('div');e.addEventListener('x',ev=>{l.push(ev.detail.k);ev.stopImmediatePropagation()});e.addEventListener('x',()=>l.push('no'));"
                      "e.dispatchEvent(new CustomEvent('x',{detail:{k:7}}));return l.join()})()"), "7");
        CHECK_EQ(r.ev("(()=>{const e=document.createElement('div');const ev=new Event('q',{cancelable:true});e.addEventListener('q',x=>x.preventDefault());return e.dispatchEvent(ev)+':'+ev.defaultPrevented})()"), "false:true");
        CHECK_EQ(r.ev("(()=>{const ev=new MouseEvent('click',{clientX:5,ctrlKey:true});return ev.clientX+':'+ev.ctrlKey+':'+ev.altKey+':'+(ev instanceof UIEvent)+':'+(ev instanceof Event)})()"), "5:true:false:true:true");
        CHECK_EQ(r.ev("(()=>{const l=[];const e=document.createElement('div');e.addEventListener('t',()=>{throw new Error('boom')});e.addEventListener('t',()=>l.push('after'));e.dispatchEvent(new Event('t'));return l.join()})()"), "after");
        CHECK(!r.host.pageErrors().empty() && r.host.pageErrors().back().find("boom") != std::string::npos);   // reported, not thrown
        CHECK_EQ(r.ev("(()=>{let n=0;addEventListener('zz',()=>n++);dispatchEvent(new Event('zz'));window.dispatchEvent(new Event('zz'));return n})()"), "2");   // bare addEventListener works
    }

    // ---- MutationObserver ----------------------------------------------------------------------------------------------------------
    {
        Rig r;
        r.mount("<div id=\"root\"></div>");
        CHECK(r.run(R"(
            window.__m = [];
            const root = document.getElementById('root');
            const mo = new MutationObserver((recs, obs) => { for (const x of recs) __m.push(x.type + ':' + (x.addedNodes[0] ? x.addedNodes[0].nodeName : '') + ':' + (x.removedNodes[0] ? x.removedNodes[0].nodeName : '') + ':' + x.attributeName + ':' + (obs === mo)); });
            mo.observe(root, { childList: true, attributes: true, attributeFilter: ['data-a'], subtree: true });
            window.__mo = mo;
            root.appendChild(document.createElement('p'));
            root.setAttribute('data-a', '1');
            root.setAttribute('data-b', '2');                       // filtered out
            root.firstChild.appendChild(document.createElement('em'));   // subtree
            window.__sync = __m.length;                            // records arrive in a microtask, not synchronously
        )").ok);
        CHECK_EQ(r.ev("__sync"), "0");
        CHECK(r.host.settle(100));
        CHECK_EQ(r.ev("__m.join(' | ')"), "childList:P::null:true | attributes:::data-a:true | childList:EM::null:true");
        CHECK_EQ(r.ev("(()=>{__mo.disconnect();document.getElementById('root').appendChild(document.createElement('i'));return __mo.takeRecords().length})()"), "0");
        CHECK(r.host.settle(50));
        CHECK_EQ(r.ev("__m.length"), "3");
        Rig r2;    // start scripts see a site that is mounted later
        CHECK(r2.run("window.__seen=[]; new MutationObserver(rs=>rs.forEach(x=>x.addedNodes.forEach(n=>__seen.push(n.nodeName)))).observe(document.body,{childList:true});").ok);
        r2.mount("<main>x</main>");
        CHECK(r2.host.runPageScripts().ok);
        CHECK_EQ(r2.ev("__seen.join()"), "MAIN");
    }

    // ---- timers + settle (gaps are >= 20 ms so the order does not depend on scheduling noise) ---------------------------------------
    {
        Rig r;
        CHECK(r.run(R"(
            window.__t = [];
            setTimeout(() => __t.push('b'), 120);
            setTimeout(() => __t.push('a'), 25, 'x');
            setTimeout((v) => __t.push('arg:' + v), 1, 'x');
            const id = setTimeout(() => __t.push('NEVER'), 10); clearTimeout(id);
            let n = 0; const iv = setInterval(() => { __t.push('i' + (++n)); if (n === 3) clearInterval(iv); }, 50);
            Promise.resolve().then(() => __t.push('micro'));
            queueMicrotask(() => __t.push('qm'));
            requestAnimationFrame(ts => __t.push('raf:' + (typeof ts)));
            __t.push('sync');
        )").ok);
        CHECK(r.host.settle(2000));
        CHECK_EQ(r.ev("__t.filter(x => !x.startsWith('raf')).join()"), "sync,micro,qm,arg:x,a,i1,i2,b");   // the interval alone never holds the page open: no i3
        CHECK_EQ(r.ev("__t.includes('raf:number')"), "true");
        Rig r2;    // a never-ending interval does not block idleness...
        CHECK(r2.run("setInterval(()=>{}, 10);").ok);
        CHECK(r2.host.settle(60));
        Rig r3;    // ...a chain of one-shots does
        CHECK(r3.run("window.n=0; (function tick(){ window.n++; setTimeout(tick, 5); })();").ok);
        CHECK(!r3.host.settle(80));
        CHECK_EQ(r3.ev("n > 3"), "true");
        Rig r4;
        CHECK(r4.run("setTimeout(()=>{ window.late = 1 }, 60000);").ok);
        CHECK(r4.host.settle(50));                                   // a timer beyond the window does not hold the page
        CHECK_EQ(r4.ev("typeof late"), "undefined");
        Rig r5;
        CHECK(r5.run("setTimeout(()=>{ throw new TypeError('in timer') }, 1); setTimeout(()=>{ window.after = 1 }, 25);").ok);
        CHECK(r5.host.settle(500));
        CHECK_EQ(r5.ev("after"), "1");
        CHECK(r5.host.pageErrors().size() == 1 && r5.host.pageErrors()[0].find("in timer") != std::string::npos);
        CHECK_EQ(r5.ev("(()=>{try{setTimeout('code')}catch(e){return e.name}})()"), "TypeError");
    }

    // ---- page scripts + lifecycle --------------------------------------------------------------------------------------------------
    {
        Rig r(js::HostOptions{}, "<html><head><script>window.__shell = 1;</script></head><body></body></html>");
        CHECK(r.run("window.__order=[]; document.addEventListener('DOMContentLoaded', ()=>__order.push('dcl:'+document.readyState));"
                    "window.addEventListener('load', ()=>__order.push('load:'+document.readyState));"
                    "document.addEventListener('readystatechange', ()=>__order.push('rsc:'+document.readyState));"
                    "__order.push('start:'+document.readyState);").ok);
        r.mount("<script>__order.push('p1:' + document.currentScript.tagName); window.v = 1;</script>"
                "<script type=\"application/json\">{\"a\":1}</script>"
                "<script src=\"/a.js\"></script>"
                "<script>__order.push('p2:' + (typeof v) + ':' + (document.currentScript !== null));</script>");
        CHECK(r.host.runPageScripts().ok);
        CHECK(r.host.settle(50));
        CHECK_EQ(r.ev("__order.join()"), "start:loading,p1:SCRIPT,p2:number:true,rsc:interactive,dcl:interactive,rsc:complete,load:complete");
        CHECK_EQ(r.ev("typeof __shell"), "undefined");                  // the shell's own inline script is not "the page's"
        CHECK_EQ(r.ev("document.currentScript"), "null");
        bool noted = false;
        for (auto& n : r.host.notes()) if (n.find("https://site.test/a.js") != std::string::npos) noted = true;
        CHECK(noted);                                                  // <script src> says so instead of failing silently
        Rig r2;    // a throwing page script does not stop the others, and is reported
        r2.mount("<script>window.a = 1</script><script>throw new Error('bad one')</script><script>window.c = 3</script>");
        auto o2 = r2.host.runPageScripts();
        CHECK(!o2.ok && o2.error.find("1 page script(s) failed") != std::string::npos && o2.error.find("bad one") != std::string::npos);
        CHECK_EQ(r2.ev("a + ',' + c"), "1,3");
        Rig r3;
        CHECK(r3.run("window.onload = () => { window.loaded = 'yes' };").ok);
        r3.host.runPageScripts(); r3.host.settle(10);
        CHECK_EQ(r3.ev("loaded"), "yes");
    }
    {   // dynamically inserted scripts: JS-made ones run, innerHTML-made ones do not
        Rig r;
        r.host.runPageScripts();
        CHECK(r.run("const s = document.createElement('script'); s.textContent = 'window.dyn = (window.dyn||0) + 1'; document.body.appendChild(s);").ok);
        CHECK_EQ(r.ev("typeof dyn + ':' + dyn"), "number:1");           // ran, as a microtask after the inserting script
        CHECK(r.run("document.body.innerHTML = '<script>window.nope = 1</script>';").ok);
        r.host.settle(10);
        CHECK_EQ(r.ev("typeof nope"), "undefined");
        CHECK(r.run("const t = document.createElement('div'); const s2 = document.createElement('script'); s2.text='window.dyn2=1'; t.appendChild(s2); document.body.appendChild(t);").ok);
        r.host.settle(10);
        CHECK_EQ(r.ev("typeof dyn2"), "number");                        // a script inside a subtree inserted later still runs
    }

    // ---- watchdog ------------------------------------------------------------------------------------------------------------------
    {
        js::HostOptions o; o.scriptTimeoutMs = 150;
        Rig r(o);
        auto t0 = std::chrono::steady_clock::now();
        auto loop = r.run("while (true) {}");
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(!loop.ok && loop.error.find("timed out after 150 ms") != std::string::npos);
        CHECK(ms < 3000);
        CHECK_EQ(r.ev("'next script still runs'"), "next script still runs");
        auto swallow = r.run("try { while (true) {} } catch (e) { window.caught = 1 }");   // a timeout is not catchable
        CHECK(!swallow.ok);
        CHECK_EQ(r.ev("typeof caught"), "undefined");
        CHECK(r.run("setTimeout(()=>{ while(true){} }, 1); setTimeout(()=>{ window.afterLoop = 1 }, 25);").ok);
        r.host.settle(2000);
        CHECK_EQ(r.ev("afterLoop"), "1");                               // a hung timer callback does not take the page down
        CHECK(!r.host.pageErrors().empty());
        auto deep = r.run("function d(){ return d() + 1 } d()");
        CHECK(!deep.ok && deep.error.find("stack") != std::string::npos);   // stack overflow is a RangeError, not a crash
        Rig rj(o);
        CHECK(rj.run("function f(){ Promise.resolve().then(f); } f();").ok);   // endless microtasks are cut off too
        CHECK_EQ(rj.ev("'alive'"), "alive");
        js::HostOptions m; m.memoryLimitBytes = 32u << 20;
        Rig rm(m);
        auto big = rm.run("const a = []; while (true) a.push(new Array(100000).fill('x'.repeat(100)));");
        CHECK(!big.ok);
        CHECK_EQ(rm.ev("1"), "1");
    }

    // ---- detach safety -------------------------------------------------------------------------------------------------------------
    {
        // 1) the Document is destroyed BEFORE detach() (what the Hydrator's error paths do)
        auto doc = dom::parseHtml("<html><head></head><body><p id=\"x\">1</p></body></html>");
        NativeHooks hooks;
        js::QuickJsHost host;
        std::string err;
        CHECK(host.attach(*doc, "https://site.test/", hooks, &err));
        UserScript u; u.name = "s"; u.source = "window.w = document.getElementById('x'); new MutationObserver(()=>{}).observe(document.body,{childList:true}); setTimeout(()=>{},1000);";
        CHECK(host.runUserScript(u).ok);
        doc.reset();
        host.detach();                                    // must not touch the dead Document
        host.detach();                                    // idempotent
        CHECK(!host.runUserScript(u).ok);                 // not attached
        // 2) the Document OUTLIVES the host and keeps being mutated (the success path)
        auto doc2 = dom::parseHtml("<html><head></head><body></body></html>");
        {
            js::QuickJsHost h2;
            NativeHooks hk2;
            CHECK(h2.attach(*doc2, "https://site.test/", hk2, &err));
            UserScript v; v.name = "v"; v.source = "new MutationObserver(()=>{}).observe(document.body,{childList:true,attributes:true,subtree:true});"
                                                    "const s=document.createElement('script');document.body.appendChild(s);";
            CHECK(h2.runUserScript(v).ok);
        }                                                  // host destroyed here
        for (int i = 0; i < 20; ++i) {
            dom::Node* e = doc2->createElement("script");
            doc2->body()->appendChild(e);
            doc2->body()->setAttr("data-i", std::to_string(i));
            doc2->body()->removeChild(e);
        }
        CHECK(doc2->body() != nullptr);
        // 3) attach twice on one host
        js::QuickJsHost h3;
        NativeHooks hk3;
        CHECK(h3.attach(*doc2, "https://a.test/", hk3, &err) && h3.attach(*doc2, "https://b.test/", hk3, &err));
        UserScript w; w.name = "w"; w.source = "window.__x = location.href";
        CHECK(h3.runUserScript(w).ok);
    }

    // ---- location / history / url utilities ----------------------------------------------------------------------------------------
    {
        Rig r(js::HostOptions{}, "<html><head></head><body></body></html>", "https://user:pw@Site.Test:8443/a/b.html?q=1#frag");
        CHECK_EQ(r.ev("location.href"), "https://user:pw@site.test:8443/a/b.html?q=1#frag");
        CHECK_EQ(r.ev("[location.protocol,location.host,location.hostname,location.port,location.pathname,location.search,location.hash,location.origin].join(' ')"),
                 "https: site.test:8443 site.test 8443 /a/b.html ?q=1 #frag https://site.test:8443");
        CHECK_EQ(r.ev("document.location === location && String(location) === location.href && document.URL === location.href"), "true");
        CHECK_EQ(r.ev("(()=>{history.pushState({a:1},'', '/next?x=2'); return location.pathname+location.search+history.length+JSON.stringify(history.state)})()"), "/next?x=22{\"a\":1}");
        CHECK_EQ(r.ev("(()=>{location.hash='top';return location.hash})()"), "#top");
        CHECK(r.host.notes().empty());
        CHECK(r.run("location.href = 'https://elsewhere.test/'; location.assign('/other');").ok);
        {   // the page asked to leave: recorded (newest wins), not followed, not "ignored"
            auto nav = r.host.takeNavigation();
            CHECK(nav && nav->method == "GET" && nav->url.find("/other") != std::string::npos && nav->url.find("elsewhere.test") == std::string::npos);
            CHECK(!r.host.takeNavigation());                                          // taking it clears it
            CHECK(r.host.notes().empty());
        }
        CHECK_EQ(r.ev("location.hostname"), "site.test");
        CHECK_EQ(r.ev("(()=>{const u=new URL('../x/y?a=1&b=2#h','https://e.com/p/q/r');u.searchParams.append('c','3 4');u.hash='';return u.href+'|'+u.searchParams.get('b')+'|'+u.origin})()"), "https://e.com/p/x/y?a=1&b=2&c=3+4|2|https://e.com");
        CHECK_EQ(r.ev("(()=>{try{new URL('nope')}catch(e){return e.name}})()"), "TypeError");
        CHECK_EQ(r.ev("[...new URLSearchParams('a=1&a=2&b=%20x')].join(';') + new URLSearchParams({q:'a b'}).toString()"), "a,1;a,2;b, xq=a+b");
        CHECK_EQ(r.ev("btoa('hello, world!')"), "aGVsbG8sIHdvcmxkIQ==");
        CHECK_EQ(r.ev("atob('aGVsbG8sIHdvcmxkIQ==')"), "hello, world!");
        CHECK_EQ(r.ev("atob(btoa('\\xff\\x00a')) === '\\xff\\x00a'"), "true");
        CHECK_EQ(r.ev("(()=>{try{atob('***')}catch(e){return e.name}})()"), "InvalidCharacterError");
    }

    // ---- navigator / screen come from the persona -------------------------------------------------------------------------------------
    {
        js::HostOptions o;
        o.persona = [] { return std::optional<js::Persona>(testPersona()); };
        Rig r(o);
        CHECK_EQ(r.ev("navigator.userAgent"), "Mozilla/5.0 (X11; Linux x86_64) TestBrowser/1.0");
        CHECK_EQ(r.ev("navigator.appVersion"), "5.0 (X11; Linux x86_64) TestBrowser/1.0");
        CHECK_EQ(r.ev("[navigator.platform,navigator.vendor,navigator.language,navigator.languages.join('+'),navigator.hardwareConcurrency,navigator.deviceMemory,navigator.maxTouchPoints,navigator.webdriver].join(' ')"),
                 "Linux x86_64 Test Inc. de-DE de-DE+de+en 12 8 0 false");
        CHECK_EQ(r.ev("navigator === navigator && navigator.languages === navigator.languages && navigator.userAgentData === navigator.userAgentData && navigator.userAgentData.brands === navigator.userAgentData.brands"), "true");
        CHECK_EQ(r.ev("navigator.userAgentData.brands.map(b=>b.brand+'/'+b.version).join()"), "Not;A=Brand/99,TestBrowser/1");
        CHECK_EQ(r.ev("Object.isFrozen(navigator.languages)"), "true");
        CHECK_EQ(r.ev("[screen.width,screen.height,screen.availHeight,screen.colorDepth,devicePixelRatio,innerWidth,outerHeight].join()"), "1920,1080,1040,24,1.25,1920,1040");
        CHECK_EQ(r.ev("(navigator instanceof Navigator) + Object.prototype.toString.call(navigator)"), "true[object Navigator]");
        CHECK(r.run("window.huh = null; navigator.userAgentData.getHighEntropyValues(['architecture','uaFullVersion','nothing']).then(v => { window.huh = JSON.stringify(v); });").ok);
        r.host.settle(20);
        CHECK_EQ(r.ev("huh"), "{\"brands\":[{\"brand\":\"Not;A=Brand\",\"version\":\"99\"},{\"brand\":\"TestBrowser\",\"version\":\"1\"}],\"mobile\":false,\"platform\":\"Linux\",\"architecture\":\"x86\",\"uaFullVersion\":\"1.2.3.4\"}");
        Rig blank;    // no persona: present but blank (never invented), and no userAgentData at all
        CHECK_EQ(blank.ev("navigator.userAgent + '|' + screen.width + '|' + typeof navigator.userAgentData + '|' + typeof navigator.deviceMemory"), "|0|undefined|undefined");
    }

    // ---- console -------------------------------------------------------------------------------------------------------------------
    {
        std::vector<std::string> lines;
        js::HostOptions o;
        o.onConsole = [&](const std::string& lv, const std::string& l) { lines.push_back(lv + ":" + l); };
        Rig r(o);
        CHECK(r.run("console.log('a', 1); console.warn('w'); console.assert(1 === 2, 'x'); console.group('g'); console.table([1]);").ok);
        CHECK_EQ(join(lines, ","), "log:a 1,warn:w,error:Assertion failed: x");
        CHECK(r.run("console.error(new TypeError('bad call'))", "site-bundle.js").ok);
        CHECK(lines.back().find("TypeError: bad call") != std::string::npos);
        CHECK(lines.back().find("site-bundle.js:1") != std::string::npos);  // caught page errors need useful call sites
        CHECK(r.run("console.error('stringified framework error')", "site-bundle.js").ok);
        CHECK(lines.back().find("site-bundle.js:1") != std::string::npos);  // also retain callsites when the framework logs only text
        Rig quiet;                                                       // default: silent
        CHECK(quiet.run("console.log('x'); console.error('y');").ok);
    }

    // ---- CacheStorage basics -----------------------------------------------------------------------------------------------
    {
        Rig r;
        CHECK(r.run("window.__cacheCheck = ''; caches.open('lucyn-test').then(async c => {"
                    "await c.put(new Request('/item'), new Response('{\\\"ok\\\":true}', {status: 201, headers: {'content-type':'application/json'}}));"
                    "const hit = await c.match('/item'); const data = await hit.json();"
                    "__cacheCheck = [hit.status, data.ok, hit.headers.get('content-type'), (await c.keys()).length, await caches.has('lucyn-test')].join('|');"
                    "});").ok);
        CHECK(r.host.settle(100));
        CHECK_EQ(r.ev("__cacheCheck"), "201|true|application/json|1|true");
        CHECK(r.run("caches.delete('lucyn-test').then(v => window.__cacheDeleted = v);").ok);
        CHECK(r.host.settle(50));
        CHECK_EQ(r.ev("__cacheDeleted"), "true");
    }
    {
        Rig r;
        r.mount("<img id='lazy'>");
        CHECK(r.run("window.__intersection = ''; const io = new IntersectionObserver((entries, observer) => {"
                    "window.__intersection = [entries.length, entries[0].target.id, entries[0].isIntersecting, observer === io].join('|');"
                    "}); io.observe(document.getElementById('lazy'));").ok);
        CHECK(r.host.settle(50));
        CHECK_EQ(r.ev("__intersection"), "1|lazy|true|true");
    }
    {
        Rig r;
        CHECK_EQ(r.ev("(()=>{const a=new Uint8Array(16);return crypto.getRandomValues(a)===a && a.length===16 && crypto.randomUUID().length===36})()"), "true");
        CHECK_EQ(r.ev("(()=>{try{crypto.getRandomValues(new Float32Array(2))}catch(e){return e.name}})()"), "TypeError");
        CHECK_EQ(r.ev("(()=>{try{crypto.getRandomValues(new Uint8Array(65537))}catch(e){return e.name}})()"), "TypeError");
    }

    // ---- the real Hydrator + the real host, end to end ------------------------------------------------------------------------------
    {
        ScriptRegistry sr;
        sr.add("// ==LucyScript==\n// @name early\n// @run-at start\n// ==/LucyScript==\n"
               "lucyn.snapshot.take('start'); window.__trace = ['start:' + document.readyState + ':' + (document.getElementById('root') === null)];\n"
               "new MutationObserver(rs => rs.forEach(r => r.addedNodes.forEach(n => __trace.push('saw:' + n.nodeName)))).observe(document.body, {childList: true, subtree: true});\n", "x");
        sr.add("// ==LucyScript==\n// @name late\n// @run-at end\n// ==/LucyScript==\n"
               "const root = document.getElementById('root');\n"
               "root.insertAdjacentHTML('beforeend', '<section class=\"added\">late</section>');\n"
               "document.documentElement.setAttribute('data-trace', __trace.join('>'));\n"
               "lucyn.log('late ran', document.querySelectorAll('section').length);\n"
               "lucyn.snapshot.take('mid');\n", "x");
        sr.add("// ==LucyScript==\n// @name broken\n// @run-at end\n// ==/LucyScript==\nnull.boom;", "x");
        sr.add("// ==LucyScript==\n// @name hang\n// @run-at end\n// ==/LucyScript==\nwhile(true){}", "x");
        sr.add("// ==LucyScript==\n// @name last\n// @run-at end\n// @priority -5\n// ==/LucyScript==\nlucyn.log('last still ran');", "x");

        const char* page = R"(<!doctype html><html><head><title>E2E</title><script>window.headScript = 1;</script></head>
<body><div id="root"><p>hello</p></div>
<script>document.getElementById('root').setAttribute('data-hydrated', 'yes'); setTimeout(function(){ document.getElementById('root').appendChild(document.createElement('footer')); }, 10);</script>
</body></html>)";
        js::HostOptions ho; ho.scriptTimeoutMs = 200;
        js::QuickJsHost host(ho);
        Hydrator hy(fetchOf(page), sr, host);
        hy.options.saveSnapshots = false;
        auto res = hy.hydrate("https://site.test/index.html");
        CHECK(res.ok);
        CHECK(res.jsAvailable && res.settled);
        CHECK(res.afterHtml.find("data-hydrated=\"yes\"") != std::string::npos);        // the site's own inline script ran
        CHECK(res.afterHtml.find("<footer></footer>") != std::string::npos);            // ...and its timer, during settle
        CHECK(res.afterHtml.find("<section class=\"added\">late</section>") != std::string::npos);
        CHECK(res.afterHtml.find("data-trace=\"start:loading:true&gt;saw:") != std::string::npos);   // the start script's observer saw the mount (">" is escaped in attributes)
        CHECK(res.afterHtml.find("saw:FOOTER") != std::string::npos);                    // ...and the footer the page's own timer added in settle
        CHECK(res.log.size() == 2);
        CHECK(join(res.log, ";").find("late ran 1") != std::string::npos && join(res.log, ";").find("last still ran") != std::string::npos);
        int ran = 0, failed = 0;
        for (auto& s : res.scripts) { if (s.status == ScriptReport::Status::Ran) ++ran; if (s.status == ScriptReport::Status::Failed) ++failed; }
        CHECK(ran == 3 && failed == 2);                                                  // broken + hung fail; the rest carry on
        bool hung = false;
        for (auto& s : res.scripts) if (s.name == "hang" && s.detail.find("timed out") != std::string::npos) hung = true;
        CHECK(hung);
        CHECK(host.pageErrors().empty());
    }

    // ---- page navigation: reload / href / forms / link clicks / noscript / settle ----------------------------------------------------
    {
        Rig r;
        r.mount("<a id=l href=\"/next?x=1#top\">go</a><a id=js href=\"javascript:void(0)\">j</a><a id=dl href=\"/f.zip\" download>d</a>"
                "<a id=mail href=\"mailto:a@b.c\">m</a>"
                "<form id=g action=\"/s?old=1\" method=\"get\"><input name=q value=\"a b&c\"><input name=skip disabled value=no>"
                "<button id=b type=submit name=go value=1>Go</button></form>"
                "<form id=p action=\"/post\" method=\"POST\"><input name=q value=\"x\"></form>");
        // reload keeps the URL, drops the fragment
        CHECK(r.run("location.reload();").ok);
        { auto n = r.host.takeNavigation(); CHECK(n && n->method == "GET" && n->url == "https://site.test/p/index.html"); }
        // a fragment-only change is NOT a navigation; a same-URL assignment without a fragment IS (it reloads)
        CHECK(r.run("location.href = '#x'; location.hash = 'y';").ok);
        CHECK(!r.host.takeNavigation());
        CHECK(r.run("location.href = location.href.split('#')[0];").ok);
        CHECK(r.host.takeNavigation().has_value());
        // location.search / pathname setters navigate
        CHECK(r.run("location.search = 'q=1';").ok);
        { auto n = r.host.takeNavigation(); CHECK(n); if (n) CHECK_EQ(n->url, "https://site.test/p/index.html?q=1#y"); }   // the hash set above stays, like a browser
        CHECK(r.run("location.pathname = 'z';").ok);
        { auto n = r.host.takeNavigation(); CHECK(n); if (n) CHECK_EQ(n->url, "https://site.test/z#y"); }
        // link click: default action follows; javascript:/download/mailto do not
        CHECK(r.run("document.getElementById('l').click();").ok);
        { auto n = r.host.takeNavigation(); CHECK(n && n->url == "https://site.test/next?x=1#top"); }
        CHECK(r.run("document.getElementById('js').click(); document.getElementById('dl').click();").ok);
        CHECK(!r.host.takeNavigation());
        CHECK(r.run("document.getElementById('mail').click();").ok);
        CHECK(!r.host.takeNavigation());
        { bool said = false; for (auto& n : r.host.notes()) if (n.find("mailto:") != std::string::npos) said = true; CHECK(said); }
        // preventDefault cancels the default action
        CHECK(r.run("document.getElementById('l').addEventListener('click', e => e.preventDefault()); document.getElementById('l').click();").ok);
        CHECK(!r.host.takeNavigation());
        // GET form via its submit button: action's old query replaced, fields urlencoded, disabled skipped, submitter included
        CHECK(r.run("document.getElementById('b').click();").ok);
        { auto n = r.host.takeNavigation(); CHECK(n && n->method == "GET"); if (n) CHECK_EQ(n->url, "https://site.test/s?q=a+b%26c&go=1"); }
        // submit event can cancel requestSubmit(); form.submit() fires no event at all
        CHECK(r.run("const g = document.getElementById('g'); g.addEventListener('submit', e => e.preventDefault()); g.requestSubmit();").ok);
        CHECK(!r.host.takeNavigation());
        CHECK(r.run("document.getElementById('g').submit();").ok);
        CHECK(r.host.takeNavigation().has_value());
        // POST form
        CHECK(r.run("document.getElementById('p').submit();").ok);
        { auto n = r.host.takeNavigation();
          CHECK(n && n->method == "POST" && n->url == "https://site.test/post" && n->body == "q=x" && n->contentType == "application/x-www-form-urlencoded"); }
        // newest wins
        CHECK(r.run("location.href = '/one'; location.href = '/two';").ok);
        { auto n = r.host.takeNavigation(); CHECK(n && n->url == "https://site.test/two"); }
        CHECK(r.host.notes().size() == 1);                              // only the mailto: note
    }
    {   // a page that leaves stops running its remaining scripts and settle() returns at once
        ScriptRegistry sr;
        js::QuickJsHost host;
        const char* page = R"(<!doctype html><html><head></head><body><div id="o"></div>
<script>document.getElementById('o').setAttribute('a','1'); setTimeout(function(){ location.reload(); }, 5);</script>
<script>setTimeout(function(){ document.getElementById('o').setAttribute('late','1'); }, 400);</script></body></html>)";
        Hydrator hy(fetchOf(page), sr, host);
        hy.options.saveSnapshots = false;
        hy.options.keepAlive = true;
        auto t0 = std::chrono::steady_clock::now();
        auto res = hy.hydrate("https://site.test/c.html");
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(res.ok && res.jsAvailable);
        CHECK(res.afterHtml.find("a=\"1\"") != std::string::npos);
        CHECK(res.afterHtml.find("late=") == std::string::npos);        // the 400 ms timer never ran: the page was already leaving
        CHECK(ms < 350);
        auto nav = host.takeNavigation();
        CHECK(nav && nav->url == "https://site.test/c.html");
    }
    {   // <noscript> content is not part of a page that runs scripts (gumbo would otherwise render it)
        ScriptRegistry sr;
        js::QuickJsHost host;
        const char* page = R"(<!doctype html><html><head><noscript>JavaScript is required to complete this challenge.</noscript></head>
<body><p id="p">hi</p><noscript><div id="ns">Enable JS</div></noscript>
<script>var s = '<noscript>x</noscript>'; document.getElementById('p').setAttribute('s', s);</script></body></html>)";
        Hydrator hy(fetchOf(page), sr, host);
        hy.options.saveSnapshots = false;
        auto res = hy.hydrate("https://site.test/n.html");
        CHECK(res.ok && res.jsAvailable);
        CHECK(res.afterHtml.find("JavaScript is required") == std::string::npos);
        CHECK(res.afterHtml.find("id=\"ns\"") == std::string::npos);
        CHECK(res.afterHtml.find("<noscript>x</noscript>") != std::string::npos);   // a "<noscript>" inside a JS string is left alone
        CHECK(res.beforeHtml.find("JavaScript is required") != std::string::npos);  // the fetched source is untouched
    }

    fs::remove_all(tmp);
    std::printf(failures ? "js: %d FAILED\n" : "js: all passed\n", failures);
    return failures ? 1 : 0;
}
