#include <lucyn/lnm/app.h>

#include <fstream>
#include <sstream>

namespace lucyn::lnm {

FileReader diskReader(const std::string& baseDir) {
    return [baseDir](const std::string& path) -> std::optional<std::string> {
        std::string full = path;
        if (!path.empty() && path[0] != '/' && !baseDir.empty()) full = baseDir + (baseDir.back() == '/' ? "" : "/") + path;
        std::ifstream f(full, std::ios::binary);
        if (!f) return std::nullopt;
        std::ostringstream o;
        o << f.rdbuf();
        return o.str();
    };
}

namespace {
// The same translation warning can be raised more than once (a tooltip re-applies the static rules to its own
// subtree); keep the first of each identical (severity, location, message).
void dedupe(std::vector<Diagnostic>& v) {
    std::vector<Diagnostic> out;
    for (auto& d : v) {
        bool seen = false;
        for (auto& o : out)
            if (o.severity == d.severity && o.loc.line == d.loc.line && o.loc.col == d.loc.col && o.message == d.message) { seen = true; break; }
        if (!seen) out.push_back(d);
    }
    v = std::move(out);
}
std::string num(float v) {
    std::ostringstream o;
    o << v;
    return o.str();
}
const lucyn::layout::Box* findBox(const lucyn::layout::Box& b, const lucyn::dom::Node* n) {
    if (b.node == n) return &b;
    for (const auto& c : b.children)
        if (const auto* r = findBox(c, n)) return r;
    return nullptr;
}
}  // namespace

App::App(FileReader reader, ViewOptions opts) : reader_(std::move(reader)), opts_(opts) {}

lucyn::layout::Box App::layoutView() const {
    lucyn::layout::LayoutOptions lo;
    lo.viewportWidth = opts_.viewportWidth;
    lo.viewportHeight = opts_.viewportHeight;
    return lucyn::layout::layoutDocument(*doc_, lo);
}

bool App::load(const std::string& src) {
    doc_.reset();
    tooltip_ = nullptr;
    overlays_.clear();
    savedStyle_.clear();
    program_ = Program{};
    jsFiles_.clear();
    baseGrabs_.clear();
    diags_.clear();

    ParseResult r = parse(src);
    parseDiags_ = r.diagnostics;
    diags_ = parseDiags_;
    if (!r.ok()) return false;
    program_ = std::move(r.program);
    for (const auto& g : program_.grabs) (g.kind == Grab::Kind::Js ? jsFiles_ : baseGrabs_).push_back(g.path);
    if (baseGrabs_.empty()) {
        error("no grab(...) of an html file: LNM has nothing to style");
        return false;
    }
    basePath_ = baseGrabs_[0];
    return compose();
}

std::optional<std::unique_ptr<lucyn::dom::Document>> App::readHtml(const std::string& path) {
    auto bytes = reader_ ? reader_(path) : std::nullopt;
    if (!bytes) {
        error("cannot read '" + path + "'");
        return std::nullopt;
    }
    return lucyn::dom::parseHtml(*bytes);
}

void App::importBody(lucyn::dom::Document& dst, lucyn::dom::Node& into, lucyn::dom::Document& src) {
    lucyn::dom::Node* body = src.body();
    if (!body) return;
    for (const auto& child : body->children) {
        lucyn::dom::Node* copy = dst.importNode(*child, true);
        if (copy) into.appendChild(copy);
    }
}

lucyn::dom::Node* App::makeLayer(const char* kind, const std::string& style) {
    lucyn::dom::Node* div = doc_->createElement("div");
    if (!div) return nullptr;
    div->setAttr("data-lnm-layer", kind);
    div->setAttr("style", style);
    doc_->body()->appendChild(div);
    return div;
}

// Rebuilds the whole view from source: base html(s), overlays, then every static rule.
bool App::compose() {
    diags_ = parseDiags_;
    savedStyle_.clear();
    tooltip_ = nullptr;

    std::unique_ptr<lucyn::dom::Document> doc;
    for (size_t i = 0; i < baseGrabs_.size(); ++i) {
        auto d = readHtml(baseGrabs_[i]);
        if (!d) { if (i == 0) return false; continue; }   // an extra grab that is missing is reported, not fatal
        if (i == 0) doc = std::move(*d);
        else if (doc->body()) importBody(*doc, *doc->body(), **d);   // further grabs join the first one's <body>
    }
    if (!doc || !doc->body()) { error("the base view has no <body>"); return false; }
    doc_ = std::move(doc);

    for (size_t i = 0; i < overlays_.size(); ++i) {
        auto d = readHtml(overlays_[i]);
        if (!d) continue;
        lucyn::dom::Node* layer = makeLayer("overlay", "position:absolute;left:0;top:0;width:" + num(opts_.viewportWidth) +
                                                           "px;height:" + num(opts_.viewportHeight) + "px;z-index:" + std::to_string(100 + i));
        if (layer) importBody(*doc_, *layer, **d);
    }
    applyStaticRules(*doc_, program_, &diags_);
    dedupe(diags_);
    return true;
}

bool App::switchReplace(const std::string& path) {
    if (!reader_ || !reader_(path)) { error("switch.replace: cannot read '" + path + "'"); return false; }
    baseGrabs_ = {path};
    basePath_ = path;
    overlays_.clear();
    return compose();
}

bool App::switchOverlay(const std::string& path) {
    if (!reader_ || !reader_(path)) { error("switch.overlay: cannot read '" + path + "'"); return false; }
    overlays_.push_back(path);
    return compose();
}

bool App::dismissOverlay() {
    if (overlays_.empty()) return false;
    overlays_.pop_back();
    return compose();
}

bool App::dismissTooltip() {
    if (!tooltip_) return false;
    tooltip_->remove();
    tooltip_ = nullptr;
    return true;
}

bool App::switchTooltip(const std::string& path, TooltipPosition pos, const lucyn::dom::Node* anchor, Point cursor) {
    if (!doc_) return false;
    if (pos == TooltipPosition::None) { error("switch.tooltip needs a position"); return false; }
    auto d = readHtml(path);
    if (!d) return false;

    float vw = opts_.viewportWidth, vh = opts_.viewportHeight;
    std::string place;
    if (pos == TooltipPosition::Cursor) {
        place = "left:" + num(cursor.x) + "px;top:" + num(cursor.y) + "px";
    } else {
        lucyn::layout::Box root = layoutView();
        const lucyn::layout::Box* a = anchor ? findBox(root, anchor) : nullptr;
        if (!a) { error("switch.tooltip: the anchor element has no box (is it display:none?)"); return false; }
        switch (pos) {
            case TooltipPosition::Top:    place = "left:" + num(a->x) + "px;bottom:" + num(vh - a->y) + "px"; break;
            case TooltipPosition::Bottom: place = "left:" + num(a->x) + "px;top:" + num(a->y + a->height) + "px"; break;
            case TooltipPosition::Left:   place = "right:" + num(vw - a->x) + "px;top:" + num(a->y) + "px"; break;
            case TooltipPosition::Right:  place = "left:" + num(a->x + a->width) + "px;top:" + num(a->y) + "px"; break;
            default: break;
        }
    }
    dismissTooltip();   // one tooltip at a time
    lucyn::dom::Node* layer = makeLayer("tooltip", "position:absolute;z-index:1000;" + place);
    if (!layer) return false;
    importBody(*doc_, *layer, **d);
    tooltip_ = layer;
    applyStaticRules(*doc_, program_, &diags_, layer);   // style only the new subtree
    dedupe(diags_);
    return true;
}

bool App::runAction(const Action& a, const lucyn::dom::Node* el, Point cursor) {
    if (const auto* s = std::get_if<SwitchAction>(&a)) {
        switch (s->mode) {
            case SwitchMode::Replace: switchReplace(s->target); return true;
            case SwitchMode::Overlay: switchOverlay(s->target); return true;
            case SwitchMode::Tooltip: switchTooltip(s->target, s->position, el, cursor); return s->mode != SwitchMode::Tooltip;
        }
    } else if (const auto* j = std::get_if<RunJs>(&a)) {
        if (!registered_.count(j->function)) {
            warning("runjs: " + j->function + "() was called but is not registered (LNM.register)");
        } else if (runjs_) {
            runjs_(*j, el);
        }
    }
    return false;
}

int App::trigger(const lucyn::dom::Node* el, const std::string& event, Point cursor) {
    if (!doc_ || !el) return 0;
    std::vector<const Rule*> fired;
    for (const Rule& r : program_.rules)
        if (r.event == event && matchesSelector(*doc_, r.selector, el)) fired.push_back(&r);

    // 1) style changes first (they belong to the element as it is NOW)...
    auto* mut = const_cast<lucyn::dom::Node*>(el);
    for (const Rule* r : fired) {
        std::vector<CssDecl> css;
        for (const auto& d : r->style)
            for (auto& c : translateStyle(d, &diags_)) css.push_back(std::move(c));
        if (css.empty()) continue;
        if (!savedStyle_.count(el)) {
            const std::string* old = el->attr("style");
            savedStyle_[el] = old ? std::optional<std::string>(*old) : std::nullopt;
        }
        appendInlineStyle(*mut, css);
    }
    // 2) ...then the actions, in order. A rebuild invalidates `el`, so it ends the run.
    for (const Rule* r : fired)
        for (const Action& a : r->actions)
            if (runAction(a, el, cursor)) return static_cast<int>(fired.size());
    return static_cast<int>(fired.size());
}

void App::clearEventStyles(const lucyn::dom::Node* el) {
    auto it = savedStyle_.find(el);
    if (it == savedStyle_.end()) return;
    auto* mut = const_cast<lucyn::dom::Node*>(el);
    if (it->second) mut->setAttr("style", *it->second);
    else mut->removeAttr("style");
    savedStyle_.erase(it);
}

int App::restyle(const std::string& selector, const std::vector<std::pair<std::string, std::string>>& lnmDecls) {
    if (!doc_) return 0;
    std::vector<CssDecl> css;
    for (const auto& kv : lnmDecls) {
        StyleDecl d;
        d.name = kv.first;
        d.value = kv.second;
        for (auto& c : translateStyle(d, &diags_)) css.push_back(std::move(c));
    }
    int n = 0;
    for (const auto& part : splitSelectorList(selector))
        for (lucyn::dom::Node* node : doc_->querySelectorAll(part)) {
            appendInlineStyle(*node, css);
            ++n;
        }
    return n;
}

std::vector<Diagnostic> App::validateRunJs() const {
    std::vector<Diagnostic> out;
    std::string files;
    for (size_t i = 0; i < jsFiles_.size(); ++i) files += (i ? ", " : "") + jsFiles_[i];
    if (files.empty()) files = "a grabbed .js file";
    for (const Rule& r : program_.rules)
        for (const Action& a : r.actions)
            if (const auto* j = std::get_if<RunJs>(&a); j && !registered_.count(j->function)) {
                std::string where = r.selector + (r.event.empty() ? "" : ":" + r.event);
                if (mode_ == BuildMode::Dev)
                    out.push_back({Diagnostic::Severity::Error, j->loc,
                                   "runjs: " + j->function + "() \xE2\x80\x94 function not registered in " + files +
                                       "\n\xE2\x86\x92 Register it with LNM.register({ " + j->function + " }) and rebuild."});
                else
                    out.push_back({Diagnostic::Severity::Warning, j->loc,
                                   j->function + "() was called on " + where + " but is not registered.\n\xE2\x86\x92 " + r.selector +
                                       " will not respond as expected."});
            }
    return out;
}

}  // namespace lucyn::lnm
