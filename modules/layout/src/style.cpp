#include <lucyn/layout/style.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

namespace lucyn::layout {
namespace {

std::string lowerStr(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Removes /* ... */ comments. An unterminated comment swallows the rest
// (what a browser does). Quoted strings are respected so "/*" inside quotes survives.
std::string stripComments(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    char quote = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (quote) {
            out += c;
            if (c == '\\' && i + 1 < in.size()) { out += in[++i]; }
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; out += c; continue; }
        if (c == '/' && i + 1 < in.size() && in[i + 1] == '*') {
            size_t end = in.find("*/", i + 2);
            if (end == std::string::npos) break;
            i = end + 1;
            out += ' ';
            continue;
        }
        out += c;
    }
    return out;
}

// Splits on top-level `delim` (outside quotes and parentheses).
std::vector<std::string> splitTopLevel(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::string cur;
    char quote = 0;
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            cur += c;
            if (c == '\\' && i + 1 < s.size()) cur += s[++i];
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
        if (c == '(') ++depth;
        else if (c == ')' && depth > 0) --depth;
        if (c == delim && depth == 0) { parts.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    parts.push_back(cur);
    return parts;
}

// Whitespace-separated tokens, keeping `fn(a, b)` together.
std::vector<std::string> splitTokens(const std::string& s) {
    std::vector<std::string> toks;
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == '(') ++depth;
        else if (c == ')' && depth > 0) --depth;
        if (std::isspace(static_cast<unsigned char>(c)) && depth == 0) {
            if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) toks.push_back(cur);
    return toks;
}

// Strips a trailing `!important` (any spacing/case). Returns true if it was there.
bool stripImportant(std::string& value) {
    std::string v = trim(value);
    size_t bang = v.rfind('!');
    if (bang == std::string::npos) return false;
    if (lowerStr(trim(v.substr(bang + 1))) != "important") return false;
    value = trim(v.substr(0, bang));
    return true;
}

constexpr float kMaxPx = 1.0e6f;  // keeps absurd values from overflowing layout arithmetic

// Parses an integer ("-3", "+7", "12") with no suffix.
bool parseInt(const std::string& tok, int& out) {
    if (tok.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (tok[0] == '+' || tok[0] == '-') { neg = tok[0] == '-'; i = 1; }
    if (i >= tok.size()) return false;
    long long v = 0;
    for (; i < tok.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(tok[i]))) return false;
        v = v * 10 + (tok[i] - '0');
        if (v > 1000000000LL) v = 1000000000LL;  // clamp, still an int
    }
    out = static_cast<int>(neg ? -v : v);
    return true;
}

// Expands a 1-4 token box shorthand (margin / padding) into top,right,bottom,left.
// Returns false if any token is invalid (the whole declaration is then ignored).
bool expandBox(const std::string& value, bool allowNegative, std::optional<float> (&out)[4]) {
    auto toks = splitTokens(value);
    if (toks.empty() || toks.size() > 4) return false;
    float v[4];
    for (size_t i = 0; i < toks.size(); ++i) {
        if (!parsePx(toks[i], v[i])) return false;
        if (!allowNegative && v[i] < 0) return false;
    }
    size_t n = toks.size();
    // CSS rule: 1 -> all; 2 -> (tb, lr); 3 -> (t, lr, b); 4 -> (t, r, b, l).
    float t = v[0];
    float r = n >= 2 ? v[1] : v[0];
    float b = n >= 3 ? v[2] : v[0];
    float l = n >= 4 ? v[3] : (n >= 2 ? v[1] : v[0]);
    out[SideTop] = t; out[SideRight] = r; out[SideBottom] = b; out[SideLeft] = l;
    return true;
}

// width / height / top / etc: `auto` resets to unset; otherwise px.
// Returns false when the value is not understood.
bool parseLengthOrAuto(const std::string& value, bool allowNegative, std::optional<float>& out) {
    std::string v = trim(value);
    if (v == "auto") { out.reset(); return true; }
    float px;
    if (!parsePx(v, px)) return false;
    if (!allowNegative && px < 0) return false;
    out = px;
    return true;
}

bool isBorderStyleWord(const std::string& t) {
    return t == "solid" || t == "dashed" || t == "dotted" || t == "double" || t == "groove" ||
           t == "ridge" || t == "inset" || t == "outset";
}

// `border: 1px solid #ccc`. Only the width matters to layout (one width, grey
// stroke): thin=1, medium=3, thick=5; style none/hidden => 0; a style word with no
// width means `medium` (the CSS initial value). Returns false if nothing in the
// value is recognisable as a border at all.
bool parseBorderShorthand(const std::string& value, float& widthOut, std::optional<lucyn::paint::Color>* colorOut = nullptr) {
    auto toks = splitTokens(value);
    if (toks.empty()) return false;
    bool haveWidth = false, haveStyle = false, none = false;
    float w = 0;
    for (const auto& t : toks) {
        float px;
        if (parsePx(t, px)) {
            if (px < 0) return false;
            w = px; haveWidth = true;
        } else if (t == "thin") { w = 1; haveWidth = true; }
        else if (t == "medium") { w = 3; haveWidth = true; }
        else if (t == "thick") { w = 5; haveWidth = true; }
        else if (t == "none" || t == "hidden") { none = true; haveStyle = true; }
        else if (isBorderStyleWord(t)) { haveStyle = true; }
        else if (auto c = parseColor(t)) { if (colorOut) *colorOut = *c; }
        // anything else is garbage: skipped
    }
    if (none) { widthOut = 0; return true; }
    if (haveWidth) { widthOut = w; return true; }
    if (haveStyle) { widthOut = 3; return true; }
    return false;
}

bool clipsOverflowWord(const std::string& t) {
    return t == "hidden" || t == "scroll" || t == "auto" || t == "clip";
}


// ---- colours ----------------------------------------------------------------------------------

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

uint8_t clamp255(double v) {
    if (!(v > 0)) return 0;            // also catches NaN
    if (v > 255) return 255;
    return static_cast<uint8_t>(std::lround(v));
}

// "12", "12.5", "-3", "50%". Returns the number and whether it had a percent sign.
bool parseNumberOrPercent(const std::string& tok, double& out, bool& isPercent) {
    std::string t = trim(tok);
    if (t.empty()) return false;
    isPercent = t.back() == '%';
    if (isPercent) t.pop_back();
    if (t.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (t[i] == '+' || t[i] == '-') { neg = t[i] == '-'; ++i; }
    double v = 0;
    size_t before = 0, after = 0;
    for (; i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])); ++i, ++before) v = v * 10 + (t[i] - '0');
    if (i < t.size() && t[i] == '.') {
        ++i;
        double scale = 0.1;
        for (; i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])); ++i, ++after) { v += (t[i] - '0') * scale; scale *= 0.1; }
    }
    if (i != t.size() || before + after == 0) return false;
    out = neg ? -v : v;
    return true;
}

const std::unordered_map<std::string, lucyn::paint::Color>& namedColors() {
    using lucyn::paint::Color;
    static const std::unordered_map<std::string, Color> m = {
        {"black", {0, 0, 0, 255}},        {"white", {255, 255, 255, 255}},  {"red", {255, 0, 0, 255}},
        {"lime", {0, 255, 0, 255}},       {"blue", {0, 0, 255, 255}},       {"yellow", {255, 255, 0, 255}},
        {"cyan", {0, 255, 255, 255}},     {"aqua", {0, 255, 255, 255}},     {"magenta", {255, 0, 255, 255}},
        {"fuchsia", {255, 0, 255, 255}},  {"silver", {192, 192, 192, 255}}, {"gray", {128, 128, 128, 255}},
        {"grey", {128, 128, 128, 255}},   {"maroon", {128, 0, 0, 255}},     {"olive", {128, 128, 0, 255}},
        {"green", {0, 128, 0, 255}},      {"purple", {128, 0, 128, 255}},   {"teal", {0, 128, 128, 255}},
        {"navy", {0, 0, 128, 255}},       {"orange", {255, 165, 0, 255}},   {"pink", {255, 192, 203, 255}},
        {"brown", {165, 42, 42, 255}},    {"gold", {255, 215, 0, 255}},     {"darkgray", {169, 169, 169, 255}},
        {"darkgrey", {169, 169, 169, 255}}, {"lightgray", {211, 211, 211, 255}}, {"lightgrey", {211, 211, 211, 255}},
        {"darkred", {139, 0, 0, 255}},    {"darkblue", {0, 0, 139, 255}},   {"darkgreen", {0, 100, 0, 255}},
        {"lightblue", {173, 216, 230, 255}}, {"transparent", {0, 0, 0, 0}},
    };
    return m;
}

}  // namespace

bool parsePx(const std::string& tokenIn, float& out) {
    std::string t = lowerStr(trim(tokenIn));
    if (t.empty()) return false;
    if (t.size() >= 2 && t.compare(t.size() - 2, 2, "px") == 0) t.resize(t.size() - 2);
    if (t.empty()) return false;

    size_t i = 0;
    bool neg = false;
    if (t[i] == '+' || t[i] == '-') { neg = t[i] == '-'; ++i; }
    size_t digitsBefore = 0, digitsAfter = 0;
    double v = 0;
    for (; i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])); ++i, ++digitsBefore)
        v = v * 10 + (t[i] - '0');
    bool sawDot = false;
    if (i < t.size() && t[i] == '.') {
        sawDot = true;
        ++i;
        double scale = 0.1;
        for (; i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])); ++i, ++digitsAfter) {
            v += (t[i] - '0') * scale;
            scale *= 0.1;
        }
    }
    if (i != t.size()) return false;                     // trailing junk: "%", "em", "e3", ...
    if (digitsBefore + digitsAfter == 0) return false;   // "+", ".", "-px"
    if (sawDot && digitsAfter == 0) return false;        // "7." is not a CSS number
    if (v > kMaxPx) v = kMaxPx;
    out = static_cast<float>(neg ? -v : v);
    return true;
}


std::optional<lucyn::paint::Color> parseColor(const std::string& textIn) {
    using lucyn::paint::Color;
    std::string t = lowerStr(trim(textIn));
    if (t.empty()) return std::nullopt;

    if (t[0] == '#') {
        std::string h = t.substr(1);
        for (char c : h) if (hexVal(c) < 0) return std::nullopt;
        auto pair = [&](size_t i) { return static_cast<uint8_t>(hexVal(h[i]) * 16 + hexVal(h[i + 1])); };
        auto nib = [&](size_t i) { return static_cast<uint8_t>(hexVal(h[i]) * 17); };
        if (h.size() == 3) return Color{nib(0), nib(1), nib(2), 255};
        if (h.size() == 4) return Color{nib(0), nib(1), nib(2), nib(3)};
        if (h.size() == 6) return Color{pair(0), pair(2), pair(4), 255};
        if (h.size() == 8) return Color{pair(0), pair(2), pair(4), pair(6)};
        return std::nullopt;
    }

    bool isRgba = t.compare(0, 5, "rgba(") == 0;
    bool isRgb = t.compare(0, 4, "rgb(") == 0;
    if (isRgb || isRgba) {
        if (t.back() != ')') return std::nullopt;
        std::string inner = t.substr(isRgba ? 5 : 4, t.size() - (isRgba ? 5 : 4) - 1);
        for (auto& c : inner) if (c == ',' || c == '/') c = ' ';
        auto parts = splitTokens(inner);
        if (parts.size() != 3 && parts.size() != 4) return std::nullopt;
        uint8_t ch[3];
        for (int i = 0; i < 3; ++i) {
            double v; bool pct;
            if (!parseNumberOrPercent(parts[static_cast<size_t>(i)], v, pct)) return std::nullopt;
            ch[i] = clamp255(pct ? v / 100.0 * 255.0 : v);   // not v*2.55: 50% must be exactly 127.5 -> 128
        }
        uint8_t a = 255;
        if (parts.size() == 4) {
            double v; bool pct;
            if (!parseNumberOrPercent(parts[3], v, pct)) return std::nullopt;
            double frac = pct ? v / 100.0 : v;
            a = clamp255(frac * 255.0);
        }
        return Color{ch[0], ch[1], ch[2], a};
    }

    auto it = namedColors().find(t);
    if (it != namedColors().end()) return it->second;
    return std::nullopt;
}

std::vector<Declaration> parseDeclarations(const std::string& styleText) {
    std::vector<Declaration> out;
    std::string cleaned = stripComments(styleText);
    for (const std::string& chunk : splitTopLevel(cleaned, ';')) {
        // Split name from value at the FIRST ':' outside quotes/parens.
        std::string name, value;
        {
            char quote = 0;
            int depth = 0;
            size_t colon = std::string::npos;
            for (size_t i = 0; i < chunk.size(); ++i) {
                char c = chunk[i];
                if (quote) {
                    if (c == '\\') ++i;
                    else if (c == quote) quote = 0;
                    continue;
                }
                if (c == '"' || c == '\'') { quote = c; continue; }
                if (c == '(') ++depth;
                else if (c == ')' && depth > 0) --depth;
                else if (c == ':' && depth == 0) { colon = i; break; }
            }
            if (colon == std::string::npos) continue;
            name = lowerStr(trim(chunk.substr(0, colon)));
            value = trim(chunk.substr(colon + 1));
        }
        if (name.empty()) continue;
        Declaration d;
        d.important = stripImportant(value);
        d.name = name;
        d.value = lowerStr(trim(value));
        if (d.value.empty()) continue;
        out.push_back(std::move(d));
    }
    return out;
}

Style styleFromDeclarations(const std::vector<Declaration>& decls) {
    Style st;
    bool overflowX = false, overflowY = false;

    auto apply = [&](const Declaration& d) {
        const std::string& n = d.name;
        const std::string& v = d.value;

        if (n == "display") {
            if (v == "block" || v == "list-item" || v == "flex" || v == "grid" || v == "table" ||
                v == "flow-root")
                st.display = Display::Block;   // flex/grid/table: not implemented, render as plain blocks
            else if (v == "inline" || v == "inline-block" || v == "inline-flex" || v == "inline-grid")
                st.display = Display::Inline;  // inline-block: flattened to text like any inline (v0)
            else if (v == "none")
                st.display = Display::None;
            return;  // anything else: ignored
        }
        if (n == "width")  { parseLengthOrAuto(v, false, st.width);  return; }
        if (n == "height") { parseLengthOrAuto(v, false, st.height); return; }

        if (n == "margin")  { std::optional<float> t[4]; if (expandBox(v, true,  t)) for (int i = 0; i < 4; ++i) st.margin[i]  = t[i]; return; }
        if (n == "padding") { std::optional<float> t[4]; if (expandBox(v, false, t)) for (int i = 0; i < 4; ++i) st.padding[i] = t[i]; return; }
        static const char* sides[4] = {"top", "right", "bottom", "left"};
        for (int i = 0; i < 4; ++i) {
            if (n == std::string("margin-") + sides[i]) {
                float px; if (parsePx(v, px)) st.margin[i] = px;
                return;
            }
            if (n == std::string("padding-") + sides[i]) {
                float px; if (parsePx(v, px) && px >= 0) st.padding[i] = px;
                return;
            }
            if (n == sides[i]) {  // top / right / bottom / left
                parseLengthOrAuto(v, true, st.offset[i]);
                return;
            }
        }

        if (n == "border-width") {
            // 1-4 values in CSS; Box has ONE border width, so the first (top) value is used.
            std::optional<float> t[4];
            if (expandBox(v, false, t)) st.borderWidth = t[SideTop];
            return;
        }
        if (n == "border") {
            float w;
            std::optional<lucyn::paint::Color> c;
            if (parseBorderShorthand(v, w, &c)) {
                st.borderWidth = w;
                if (c) st.borderColor = c;
            }
            return;
        }
        if (n == "border-color") {
            auto toks = splitTokens(v);
            if (toks.empty() || toks.size() > 4) return;
            auto c = parseColor(toks[0]);        // one colour for all sides: the first (top) value
            for (const auto& t : toks) if (!parseColor(t)) return;
            st.borderColor = c;
            return;
        }
        if (n == "background-color") { if (auto c = parseColor(v)) st.backgroundColor = c; return; }
        if (n == "background") {
            for (const auto& t : splitTokens(v))   // shorthand: the colour token, if any (images etc. are ignored)
                if (auto c = parseColor(t)) { st.backgroundColor = c; return; }
            return;
        }
        if (n == "color") { if (auto c = parseColor(v)) st.color = c; return; }

        if (n == "font-size") {
            float px;
            if (parsePx(v, px) && px > 0) { st.fontSize = px; return; }
            if (v == "xx-small")  { st.fontSize = 9.0f;  return; }
            if (v == "x-small")   { st.fontSize = 10.0f; return; }
            if (v == "small")     { st.fontSize = 13.0f; return; }
            if (v == "medium")    { st.fontSize = 16.0f; return; }
            if (v == "large")     { st.fontSize = 18.0f; return; }
            if (v == "x-large")   { st.fontSize = 24.0f; return; }
            if (v == "xx-large")  { st.fontSize = 32.0f; return; }
            if (v == "xxx-large") { st.fontSize = 48.0f; return; }
            if (v == "smaller" || v == "larger") { return; }
            return;
        }

        if (n == "position") {
            if (v == "static") st.position = Position::Static;
            else if (v == "relative") st.position = Position::Relative;
            else if (v == "absolute") st.position = Position::Absolute;
            return;  // fixed / sticky / garbage: ignored
        }
        if (n == "visibility") {
            if (v == "visible") st.visible = true;
            else if (v == "hidden" || v == "collapse") st.visible = false;
            return;
        }
        if (n == "z-index") {
            if (v == "auto") { st.zIndex.reset(); return; }
            int z;
            if (parseInt(v, z)) st.zIndex = z;
            return;
        }
        if (n == "overflow" || n == "overflow-x" || n == "overflow-y") {
            auto toks = splitTokens(v);
            if (toks.empty() || toks.size() > 2) return;
            for (const auto& t : toks)
                if (t != "visible" && !clipsOverflowWord(t)) return;  // invalid: ignore whole decl
            bool x = clipsOverflowWord(toks[0]);
            bool y = clipsOverflowWord(toks.size() > 1 ? toks[1] : toks[0]);
            if (n == "overflow")   { overflowX = x; overflowY = y; }
            if (n == "overflow-x") overflowX = x;
            if (n == "overflow-y") overflowY = (clipsOverflowWord(toks[0]));
            return;
        }
        // Every other property is unknown to layout and ignored.
    };

    for (const auto& d : decls) if (!d.important) apply(d);
    for (const auto& d : decls) if (d.important)  apply(d);

    st.clipsOverflow = overflowX || overflowY;
    return st;
}

Style styleFromText(const std::string& styleText) {
    return styleFromDeclarations(parseDeclarations(styleText));
}

Style styleForElement(const lucyn::dom::Node& el) {
    Style st;
    if (!el.isElement()) return st;
    std::vector<Declaration> decls;
    if (el.hasAttr("hidden")) decls.push_back(Declaration{"display", "none", false});  // UA rule: author style can override
    if (const std::string* s = el.attr("style")) {
        auto parsed = parseDeclarations(*s);
        decls.insert(decls.end(), parsed.begin(), parsed.end());
    }
    return styleFromDeclarations(decls);
}

}  // namespace lucyn::layout
