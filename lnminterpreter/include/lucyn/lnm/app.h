// lucyn/lnm/app.h - the LNM runtime: holds the current view (a dom::Document), applies events and
// view switches (LNM_SPEC.md sections 3-8). Dev-mode behaviour: reads .lnm/.html from disk through a
// FileReader. (The spec's prod mode - compiling .lnm into a header and embedding the assets - is not
// built; a FileReader that serves embedded bytes is the intended seam for it.)
//
// What it does NOT do (honestly): it receives no input by itself. Something (the UI, a test, the
// controller's `click`) calls trigger(). It does not run JS: `runjs:` goes through a handler you
// install, and `LNM.register()` is mirrored by registerFunction(). Re-rendering after a switch is a
// whole-page rebuild (the plan's accepted first version), not a subtree update.
#pragma once
#include <lucyn/dom/dom.h>
#include <lucyn/layout/layout.h>
#include <lucyn/lnm/ast.h>
#include <lucyn/lnm/compile.h>
#include <lucyn/lnm/parser.h>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace lucyn::lnm {

// Returns a file's bytes, or nullopt. `path` is exactly as written in the .lnm ("./home.html").
using FileReader = std::function<std::optional<std::string>(const std::string& path)>;

// Reads from disk; relative paths resolve against `baseDir`.
FileReader diskReader(const std::string& baseDir);

struct ViewOptions {
    float viewportWidth = 800;
    float viewportHeight = 600;
};

enum class BuildMode { Dev, Prod };

// A point in viewport pixels (the mouse position for tooltip(cursor)).
struct Point {
    float x = 0, y = 0;
};

class App {
public:
    explicit App(FileReader reader, ViewOptions opts = {});

    // Parses `lnmSource`, loads the first view (every grab(...) html, in order) and applies the static
    // rules. False if the source has errors or no readable HTML; diagnostics() says why.
    bool load(const std::string& lnmSource);

    // diagnostics from parsing, loading and compiling (rebuilt on every view change)
    const std::vector<Diagnostic>& diagnostics() const { return diags_; }
    const Program& program() const { return program_; }
    const std::vector<std::string>& jsFiles() const { return jsFiles_; }   // grab.js paths, in order

    // The current composed view. Pointers into it are invalidated by switchReplace/switchOverlay/
    // dismissOverlay (they rebuild the document); switchTooltip/dismissTooltip/trigger-without-switch keep them.
    lucyn::dom::Document* document() { return doc_.get(); }
    const std::string& currentViewPath() const { return basePath_; }
    size_t overlayCount() const { return overlays_.size(); }
    bool hasTooltip() const { return tooltip_ != nullptr; }
    void setViewport(const ViewOptions& o) { opts_ = o; }

    // Lays out the current view in the viewport.
    lucyn::layout::Box layoutView() const;

    // ---- events -------------------------------------------------------------------------------
    // Fires every `:<event>` rule whose selector matches `el`: applies their style changes, then runs
    // their actions in order (a switch that rebuilds the document ends processing: `el` is gone).
    // Returns how many rules fired.
    int trigger(const lucyn::dom::Node* el, const std::string& event, Point cursor = {});
    // Undoes style changes trigger() made to `el` (e.g. when the pointer leaves a hovered element).
    void clearEventStyles(const lucyn::dom::Node* el);
    // LNM.style("#sel", {...}): LNM-property declarations onto every matching element. Returns the count.
    int restyle(const std::string& selector, const std::vector<std::pair<std::string, std::string>>& lnmDecls);

    // ---- view switching (also what the JS-side LNM.switch.* calls) -------------------------------
    bool switchReplace(const std::string& path);                      // the whole view becomes `path`
    bool switchOverlay(const std::string& path);                      // `path` on top of the current view
    // A small floating view anchored to `anchor` (or to `cursor` for TooltipPosition::Cursor).
    bool switchTooltip(const std::string& path, TooltipPosition pos, const lucyn::dom::Node* anchor, Point cursor = {});
    // Not in the spec, but a view you cannot close is not a feature:
    bool dismissOverlay();      // removes the topmost overlay
    bool dismissTooltip();

    // ---- runjs ---------------------------------------------------------------------------------------
    using RunJsHandler = std::function<void(const RunJs&, const lucyn::dom::Node* target)>;
    void setRunJsHandler(RunJsHandler h) { runjs_ = std::move(h); }
    void registerFunction(const std::string& name) { registered_.insert(name); }   // LNM.register({ name })
    void setMode(BuildMode m) { mode_ = m; }
    // Spec section 7: Dev -> an Error per unregistered function (the app must not launch);
    // Prod -> a Warning (the element simply does nothing).
    std::vector<Diagnostic> validateRunJs() const;

private:
    std::optional<std::unique_ptr<lucyn::dom::Document>> readHtml(const std::string& path);
    bool compose();
    void importBody(lucyn::dom::Document& dst, lucyn::dom::Node& into, lucyn::dom::Document& src);
    lucyn::dom::Node* makeLayer(const char* kind, const std::string& style);
    bool runAction(const Action& a, const lucyn::dom::Node* el, Point cursor);
    void error(const std::string& m) { diags_.push_back(Diagnostic{Diagnostic::Severity::Error, {}, m}); }
    void warning(const std::string& m) { diags_.push_back(Diagnostic{Diagnostic::Severity::Warning, {}, m}); }

    FileReader reader_;
    ViewOptions opts_;
    Program program_;
    std::vector<std::string> jsFiles_;
    std::vector<std::string> baseGrabs_;       // html files that make up the base view
    std::string basePath_;                     // first of them (or the last switch.replace target)
    std::vector<std::string> overlays_;
    std::unique_ptr<lucyn::dom::Document> doc_;
    lucyn::dom::Node* tooltip_ = nullptr;
    std::vector<Diagnostic> parseDiags_, diags_;
    std::unordered_map<const lucyn::dom::Node*, std::optional<std::string>> savedStyle_;
    RunJsHandler runjs_;
    std::set<std::string> registered_;
    BuildMode mode_ = BuildMode::Dev;
};

}  // namespace lucyn::lnm
