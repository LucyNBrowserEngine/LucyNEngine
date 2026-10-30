// lucyn/net/deobfuscate.cpp — see deobfuscate.h for the plan.
//
// Design notes:
//   * Everything is token-driven, never regex-over-source: strings, templates,
//     comments and regex literals are single tokens, so no re-indent or rewrite
//     can land inside them and corrupt the code.
//   * Rewrites only ever DELETE tokens (+ folding, index replacement) or replace
//     a token with a string literal. Whitespace is only ever ADDED between
//     tokens, and `needSpace` refuses to glue two bytes that would lex as one
//     (that is what keeps `a++ +b` from becoming `a++ +b` -> `a+ +b` wrong).
//   * The string-array decoder is conservative: a name whose uses are not
//     exactly "declared, read as [literal], rotated by a recognized IIFE, or
//     read through a recognized one-expression wrapper" is left untouched.
//     Wrong strings are worse than no strings.
#include "lucyn/net/deobfuscate.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lucyn::net {
namespace {

// ---------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------
enum class K { Ident, Num, Str, Template, Comment, Regex, Punct };

struct Tok {
    K kind;
    std::string raw;   // exact source text (for Str: original, quotes included)
    std::string val;   // Str: decoded value
    bool decoded = false;  // Str: emit encodeString(val) instead of raw
};

bool wordChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == '$';
}

// ---------------------------------------------------------------------------
// String codec
// ---------------------------------------------------------------------------

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode the body of a string literal (NO surrounding quotes) to its value.
// `obfEscapes` counts \xNN / \uNNNN / octal escapes (the obfuscation signal);
// `allEscapes` counts every escape sequence found.
std::string decodeBody(const std::string& s, std::size_t* allEscapes,
                       std::size_t* obfEscapes) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        const char e = s[i + 1];
        if (allEscapes) ++(*allEscapes);
        switch (e) {
            case 'x': {
                if (i + 3 < s.size() && hexVal(s[i + 2]) >= 0 && hexVal(s[i + 3]) >= 0) {
                    const int v = hexVal(s[i + 2]) * 16 + hexVal(s[i + 3]);
                    out += static_cast<char>(v);
                    if (obfEscapes) ++(*obfEscapes);
                    i += 3;
                    continue;
                }
                break;
            }
            case 'u': {
                if (i + 2 < s.size() && s[i + 2] == '{') {
                    std::size_t j = i + 3;
                    long cp = -1;
                    while (j < s.size() && s[j] != '}') {
                        const int h = hexVal(s[j]);
                        if (h < 0 || cp < 0) { cp = -1; break; }
                        cp = cp * 16 + h;
                        ++j;
                    }
                    if (cp >= 0 && j < s.size() && s[j] == '}') {
                        if (cp < 0x80) out += static_cast<char>(cp);
                        else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        if (obfEscapes) ++(*obfEscapes);
                        i = j;
                        continue;
                    }
                    break;
                }
                if (i + 5 < s.size() && hexVal(s[i + 2]) >= 0 && hexVal(s[i + 3]) >= 0 &&
                    hexVal(s[i + 4]) >= 0 && hexVal(s[i + 5]) >= 0) {
                    long cp = hexVal(s[i + 2]) * 4096 + hexVal(s[i + 3]) * 256 +
                              hexVal(s[i + 4]) * 16 + hexVal(s[i + 5]);
                    if (cp < 0x80) out += static_cast<char>(cp);
                    else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    if (obfEscapes) ++(*obfEscapes);
                    i += 5;
                    continue;
                }
                break;
            }
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7': {
                long v = 0;
                int digits = 0;
                while (digits < 3 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7') {
                    v = v * 8 + (s[i + 1] - '0');
                    ++i;
                    ++digits;
                }
                if (v > 0 && v < 256) {
                    out += static_cast<char>(v);
                    if (obfEscapes) ++(*obfEscapes);
                    continue;
                }
                out += s[i];
                continue;
            }
            case 'n': out += '\n'; ++i; continue;
            case 'r': out += '\r'; ++i; continue;
            case 't': out += '\t'; ++i; continue;
            case 'b': out += '\b'; ++i; continue;
            case 'f': out += '\f'; ++i; continue;
            case 'v': out += '\v'; ++i; continue;
            case '\n': ++i; continue;                 // line continuation
            case '\r': ++i; if (i < s.size() && s[i] == '\n') ++i; continue;
            default: out += e; ++i; continue;         // \' \" \\ \<other>
        }
        out += s[i];  // malformed escape: keep the backslash
    }
    return out;
}

// Re-encode a decoded value as a double-quoted JS string literal.
std::string encodeString(const std::string& v) {
    std::string out;
    out.reserve(v.size() + 8);
    out += '"';
    for (unsigned char c : v) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\v': out += "\\v"; break;
            case 0: out += "\\x00"; break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    static const char* hexd = "0123456789abcdef";
                    out += "\\x";
                    out += hexd[c >> 4];
                    out += hexd[c & 0x0f];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
    return out;
}

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

// Index of the next/previous non-comment token (for regex detection + patterns).
int nextCode(const std::vector<Tok>& t, int i) {
    for (; i < static_cast<int>(t.size()); ++i)
        if (t[i].kind != K::Comment) return i;
    return -1;
}
int prevCode(const std::vector<Tok>& t, int i) {
    for (--i; i >= 0; --i)
        if (t[i].kind != K::Comment) return i;
    return -1;
}

bool regexKeyword(const std::string& s) {
    return s == "return" || s == "typeof" || s == "instanceof" || s == "in" ||
           s == "of" || s == "new" || s == "delete" || s == "void" ||
           s == "throw" || s == "case" || s == "else" || s == "do" ||
           s == "yield" || s == "await";
}

bool regexAllowed(const std::vector<Tok>& out) {
    const int p = prevCode(out, static_cast<int>(out.size()));
    if (p < 0) return true;
    const Tok& t = out[p];
    if (t.kind == K::Ident) return regexKeyword(t.raw);
    if (t.kind == K::Str || t.kind == K::Num || t.kind == K::Regex ||
        t.kind == K::Template)
        return false;
    if (t.kind == K::Punct)
        return t.raw != ")" && t.raw != "]" && t.raw != "++" && t.raw != "--";
    return true;  // comments never reach here
}

// Longest-match operators, longest first.
const char* const kOps[] = {
    ">>>=",                                  // 4
    "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "?" "?" "=", "...",  // 3
    "==", "!=", "<=", ">=", "&&", "||", "??", "+=", "-=", "*=", "/=", "%=",
    "&=", "|=", "^=", "<<", ">>", "=>", "**", "++", "--", "?.",            // 2
};

// Scan a template literal starting at s[i] == '`'; returns index AFTER it.
std::size_t skipTemplate(const std::string& s, std::size_t i) {
    const std::size_t n = s.size();
    ++i;
    while (i < n) {
        if (s[i] == '\\') { i += 2; continue; }
        if (s[i] == '`') { ++i; break; }
        if (s[i] == '$' && i + 1 < n && s[i + 1] == '{') {
            i += 2;
            int depth = 1;
            while (i < n && depth > 0) {
                const char c = s[i];
                if (c == '\\') { i += 2; continue; }
                if (c == '\'' || c == '"') {
                    const char q = c;
                    ++i;
                    while (i < n && s[i] != q) {
                        if (s[i] == '\\') ++i;
                        ++i;
                    }
                    if (i < n) ++i;
                    continue;
                }
                if (c == '`') { i = skipTemplate(s, i); continue; }
                if (c == '{') ++depth;
                else if (c == '}') --depth;
                ++i;
            }
            continue;
        }
        ++i;
    }
    return i;
}

std::vector<Tok> lex(const std::string& s, DeobfStats* st) {
    std::vector<Tok> out;
    const std::size_t n = s.size();
    std::size_t i = 0;
    auto push = [&](K k, std::size_t from, std::size_t to) {
        Tok t;
        t.kind = k;
        t.raw = s.substr(from, to - from);
        out.push_back(std::move(t));
    };
    while (i < n) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
            c == '\v' || c == '\0') {
            ++i;
            continue;
        }
        const std::size_t start = i;
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {           // line comment
            while (i < n && s[i] != '\n') ++i;
            push(K::Comment, start, i);
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {           // block comment
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) ++i;
            i = (i + 1 < n) ? i + 2 : n;
            push(K::Comment, start, i);
            continue;
        }
        if (c == '/' && regexAllowed(out)) {                      // regex literal
            ++i;
            bool inClass = false;
            bool closed = false;
            while (i < n) {
                const char d = s[i];
                if (d == '\\') { i += 2; continue; }
                if (d == '\n') break;                             // not a regex after all
                if (d == '[') inClass = true;
                else if (d == ']') inClass = false;
                else if (d == '/' && !inClass) { ++i; closed = true; break; }
                ++i;
            }
            if (closed) {
                while (i < n && wordChar(s[i])) ++i;               // flags
                push(K::Regex, start, i);
                continue;
            }
            // Bail out: treat as a plain '/' punctuator.
            i = start + 1;
            push(K::Punct, start, i);
            continue;
        }
        if (c == '"' || c == '\'') {                              // string
            const char q = c;
            ++i;
            while (i < n) {
                if (s[i] == '\\') { i += 2; continue; }
                if (s[i] == q) { ++i; break; }
                if (s[i] == '\n') break;                          // unterminated
                ++i;
            }
            Tok t;
            t.kind = K::Str;
            t.raw = s.substr(start, i - start);
            const std::string body = t.raw.substr(1, t.raw.size() >= 2 ? t.raw.size() - 2 : 0);
            std::size_t all = 0, obf = 0;
            t.val = decodeBody(body, &all, &obf);
            // Only re-encode strings carrying obfuscation-style escapes; plain
            // \n / \' stay exactly as written.
            if (obf > 0) {
                t.decoded = true;
                if (st) st->escapesDecoded += obf;
            }
            out.push_back(std::move(t));
            continue;
        }
        if (c == '`') {                                           // template
            i = skipTemplate(s, i);
            push(K::Template, start, i);
            continue;
        }
        if (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
            // .5
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            // number
            if ((c == '0') && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X' ||
                                             s[i + 1] == 'b' || s[i + 1] == 'B' ||
                                             s[i + 1] == 'o' || s[i + 1] == 'O')) {
                i += 2;
                while (i < n && (wordChar(s[i]))) ++i;
            } else {
                while (i < n && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
                if (i < n && s[i] == '.') {
                    ++i;
                    while (i < n && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
                }
                if (i < n && (s[i] == 'e' || s[i] == 'E')) {
                    const std::size_t save = i;
                    ++i;
                    if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
                    if (i < n && std::isdigit(static_cast<unsigned char>(s[i]))) {
                        while (i < n && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
                    } else {
                        i = save;
                    }
                }
            }
            if (i < n && s[i] == 'n') ++i;                        // BigInt suffix
            push(K::Num, start, i);
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$' ||
            static_cast<unsigned char>(c) >= 0x80) {
            // identifier
            ++i;
            while (i < n && (wordChar(s[i]) || static_cast<unsigned char>(s[i]) >= 0x80)) ++i;
            push(K::Ident, start, i);
            continue;
        }
        // operators / punctuation, longest match ('?.' only before a non-digit)
        bool matched = false;
        for (const char* op : kOps) {
            const std::size_t len = std::char_traits<char>::length(op);
            if (i + len <= n && s.compare(i, len, op) == 0) {
                if (std::string(op) == "?." && i + 2 < n &&
                    std::isdigit(static_cast<unsigned char>(s[i + 2])))
                    break;                                        // a ? .5 : b
                i += len;
                push(K::Punct, start, i);
                matched = true;
                break;
            }
        }
        if (matched) continue;
        // anything else (single byte): keep it verbatim
        i = start + 1;
        push(K::Punct, start, i);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Transforms — array / wrapper / base64 / concat folding
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Parse 0x / 0b / 0o / decimal (with `_` separators). Rejects fractions — array
// indices must be integers.
bool parseIntLit(const std::string& s, long long& out) {
    std::string t;
    t.reserve(s.size());
    for (char c : s)
        if (c != '_') t += c;
    if (t.empty()) return false;
    const char* p = t.c_str();
    int base = 10;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
        base = 16;
    } else if (t.size() > 2 && t[0] == '0' && (t[1] == 'b' || t[1] == 'B')) {
        p += 2; base = 2;
    } else if (t.size() > 2 && t[0] == '0' && (t[1] == 'o' || t[1] == 'O')) {
        p += 2; base = 8;
    } else if (t.size() > 1 && t[0] == '0' &&
               std::isdigit(static_cast<unsigned char>(t[1]))) {
        return false;  // legacy octal: skip rather than guess
    }
    char* end = nullptr;
    out = std::strtoll(p, &end, base);
    return end && *end == '\0';
}

// The value of a Str token (decoded or raw-as-written).
std::string strValue(const Tok& t) {
    if (t.decoded) return t.val;
    const std::string body =
        t.raw.size() >= 2 ? t.raw.substr(1, t.raw.size() - 2) : std::string();
    return decodeBody(body, nullptr, nullptr);
}

bool b64Decode(const std::string& in, std::string& out) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    s.reserve(in.size());
    for (char c : in)
        if (!std::isspace(static_cast<unsigned char>(c))) s += c;
    if (s.empty()) return false;
    std::size_t pad = 0;
    if (s.back() == '=') { ++pad; if (s.size() > 1 && s[s.size() - 2] == '=') ++pad; }
    const std::size_t bodyLen = s.size() - pad;
    if (bodyLen % 4 == 1) return false;
    out.clear();
    int val = 0, valb = -8;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '=') break;
        const char* f = std::strchr(kAlphabet, c);
        if (!f || !*f) {
            if (c == '=' ) break;
            return false;
        }
        val = (val << 6) + static_cast<int>(f - kAlphabet);
        valb += 6;
        if (valb >= 0) {
            out += static_cast<char>((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    return true;
}

bool unescapeConst(const std::string& in, std::string& out) {   // JS unescape()
    out.clear();
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') { out += in[i]; continue; }
        if (i + 2 >= in.size()) return false;
        const int h = hexVal(in[i + 1]) * 16 + hexVal(in[i + 2]);
        if (hexVal(in[i + 1]) < 0 || hexVal(in[i + 2]) < 0) return false;
        out += static_cast<char>(h);
        i += 2;
    }
    return true;
}

bool decodeUriConst(const std::string& in, std::string& out) {  // decodeURIComponent()
    // Valid only when EVERY % is part of a valid %XX escape — otherwise the real
    // call would throw, and we must not make that go away.
    out.clear();
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') { out += in[i]; continue; }
        if (i + 2 >= in.size()) return false;
        if (hexVal(in[i + 1]) < 0 || hexVal(in[i + 2]) < 0) return false;
        const int h = hexVal(in[i + 1]) * 16 + hexVal(in[i + 2]);
        if (h == 0) return false;   // a NUL byte would change program shape
        out += static_cast<char>(h);
        i += 2;
    }
    return out != in || in.find('%') == std::string::npos;
}

// ---------------------------------------------------------------------------
// The transforms
// ---------------------------------------------------------------------------

struct Arr {
    std::string name;
    std::vector<std::string> vals;
    int declB = -1, declE = -1;                       // token span
    std::vector<std::pair<int, int>> rotSpans;        // recognized rotation IIFEs
    bool decodable = true;
};

struct Wrap {
    int arrIdx = -1;
    std::vector<std::string> params;
    std::vector<Tok> expr;
    int spanB = -1, spanE = -1;
    std::string name;
};

struct Alias {
    std::string name;
    int arrIdx = -1;
    int spanB = -1, spanE = -1;
};

bool inSpan(int tok, int b, int e) { return b >= 0 && tok >= b && tok <= e; }

// Evaluate a wrapper expression like `i - k + 3` against literal arguments.
bool evalExpr(const std::vector<Tok>& expr, const std::vector<std::string>& params,
              const std::vector<long long>& args, long long& out) {
    bool have = false;
    char op = '+';
    for (const Tok& t : expr) {
        if (t.kind == K::Ident) {
            long long v = 0;
            auto it = std::find(params.begin(), params.end(), t.raw);
            if (it == params.end()) return false;
            v = args[static_cast<std::size_t>(it - params.begin())];
            out = have ? (op == '+' ? out + v : out - v) : (op == '-' ? -v : v);
            have = true;
        } else if (t.kind == K::Num) {
            long long v = 0;
            if (!parseIntLit(t.raw, v)) return false;
            out = have ? (op == '+' ? out + v : out - v) : (op == '-' ? -v : v);
            have = true;
        } else if (t.kind == K::Punct && (t.raw == "+" || t.raw == "-")) {
            op = t.raw[0];
        } else {
            return false;
        }
    }
    return have;
}

void runTransforms(std::vector<Tok>& toks, DeobfStats& st) {
    // Code-only positions (comments are invisible to every pattern below).
    std::vector<int> ci;
    ci.reserve(toks.size());
    for (int i = 0; i < static_cast<int>(toks.size()); ++i)
        if (toks[i].kind != K::Comment) ci.push_back(i);
    auto at = [&](int j) -> const Tok* {
        return (j >= 0 && j < static_cast<int>(ci.size())) ? &toks[ci[j]] : nullptr;
    };
    auto is = [&](int j, const char* s) {
        const Tok* t = at(j);
        return t && t->raw == s;
    };
    auto kindIs = [&](int j, K k) {
        const Tok* t = at(j);
        return t && t->kind == k;
    };

    std::vector<Arr> arr;
    std::vector<Alias> aliases;
    std::unordered_map<std::string, int> arrOf, aliasOf;
    std::unordered_map<std::string, int> wrapsByName;
    std::vector<Wrap> wraps;
    auto resolve = [&](const std::string& nm) -> int {
        auto a = arrOf.find(nm);
        if (a != arrOf.end()) return a->second;
        auto b = aliasOf.find(nm);
        if (b != aliasOf.end()) return b->second;
        return -1;
    };

    // -- pass 1: `var A = ['..','..'];` string-array declarations ------------
    for (int j = 0; j + 3 < static_cast<int>(ci.size()); ++j) {
        if (!is(j, "var") && !is(j, "let") && !is(j, "const")) continue;
        if (!kindIs(j + 1, K::Ident) || !is(j + 2, "=") || !is(j + 3, "[")) continue;
        int k = j + 4;
        std::vector<std::string> vals;
        bool ok = true;
        while (true) {
            const Tok* t = at(k);
            if (!t || t->kind != K::Str) { ok = false; break; }
            vals.push_back(strValue(*t));
            ++k;
            const Tok* n = at(k);
            if (n && n->raw == ",") { ++k; continue; }
            if (n && n->raw == "]") break;
            ok = false;
            break;
        }
        if (!ok || vals.empty()) continue;
        Arr a;
        a.name = at(j + 1)->raw;
        a.vals = std::move(vals);
        a.declB = ci[j];
        a.declE = ci[k];
        arrOf[a.name] = static_cast<int>(arr.size());
        arr.push_back(std::move(a));
        st.arraysFound++;
    }

    // -- pass 1b: `var ALIAS = A;` -------------------------------------------
    for (int j = 0; j + 4 < static_cast<int>(ci.size()); ++j) {
        if (!is(j, "var") && !is(j, "let") && !is(j, "const")) continue;
        if (!kindIs(j + 1, K::Ident) || !is(j + 2, "=") || !kindIs(j + 3, K::Ident))
            continue;
        if (!is(j + 4, ";")) continue;
        const int ai = resolve(at(j + 3)->raw);
        if (ai < 0) continue;
        const std::string nm = at(j + 1)->raw;
        if (arrOf.count(nm)) continue;
        Alias al;
        al.name = nm;
        al.arrIdx = ai;
        al.spanB = ci[j];
        al.spanE = ci[j + 4];
        aliasOf[nm] = ai;
        aliases.push_back(std::move(al));
    }

    // -- pass 1c: rotation IIFEs `(function(a,b){while(--b)a.push(a.shift())})(A,N)`
    for (int j = 0; j + 1 < static_cast<int>(ci.size()); ++j) {
        if (!(is(j, "(") && kindIs(j + 1, K::Ident) && is(j + 1, "function"))) continue;
        if (!is(j + 2, "(")) continue;
        std::vector<std::string> params;
        int k = j + 3;
        bool okP = true;
        while (true) {
            const Tok* t = at(k);
            if (!t) { okP = false; break; }
            if (t->raw == ")") break;
            if (t->kind != K::Ident) { okP = false; break; }
            params.push_back(t->raw);
            ++k;
            const Tok* n = at(k);
            if (n && n->raw == ",") { ++k; continue; }
            break;
        }
        if (!okP || params.size() < 2) continue;
        if (!is(k, ")") || !is(k + 1, "{")) continue;
        const int bodyOpen = k + 1;
        int depth = 1, c = bodyOpen + 1;
        while (c < static_cast<int>(ci.size()) && depth > 0) {
            if (is(c, "{")) ++depth;
            else if (is(c, "}")) --depth;
            ++c;
        }
        if (depth != 0) continue;
        const int bodyB = bodyOpen + 1, bodyE = c - 2;  // inside the braces
        // `) ( ARR , NUM )`
        if (!(is(c, ")") && is(c + 1, "(") && kindIs(c + 2, K::Ident) &&
              is(c + 3, ",") && kindIs(c + 4, K::Num) && is(c + 5, ")")))
            continue;
        const int ai = resolve(at(c + 2)->raw);
        if (ai < 0) continue;
        long long N = 0;
        if (!parseIntLit(at(c + 4)->raw, N)) continue;

        auto inBody = [&](int p, const char* s) {
            return p >= bodyB && p <= bodyE && is(p, s);
        };
        auto kindBody = [&](int p, K kk) {
            return p >= bodyB && p <= bodyE && kindIs(p, kk);
        };

        // The rotation loop: `.push(<arr>.shift())` or `['push'](['shift']())`.
        const std::string& pA = params[0];
        const std::string& pB = params[1];
        bool hasPush = false;
        for (int p = bodyB; p + 9 <= bodyE && !hasPush; ++p) {
            if (inBody(p, pA.c_str()) && inBody(p + 1, ".") && inBody(p + 2, "push") &&
                inBody(p + 3, "(") && inBody(p + 4, pA.c_str()) &&
                inBody(p + 5, ".") && inBody(p + 6, "shift") && inBody(p + 7, "(") &&
                inBody(p + 8, ")") && inBody(p + 9, ")")) {
                hasPush = true;
                break;
            }
            if (p + 11 <= bodyE && inBody(p, pA.c_str()) && inBody(p + 1, "[") &&
                kindBody(p + 2, K::Str) && strValue(toks[ci[p + 2]]) == "push" &&
                inBody(p + 3, "]") && inBody(p + 4, "(") &&
                inBody(p + 5, pA.c_str()) && inBody(p + 6, "[") &&
                kindBody(p + 7, K::Str) && strValue(toks[ci[p + 7]]) == "shift" &&
                inBody(p + 8, "]") && inBody(p + 9, "(") && inBody(p + 10, ")")) {
                hasPush = true;
                break;
            }
        }
        if (!hasPush) continue;

        // The iteration count.
        bool rec = false;
        long long iters = 0;
        for (int p = bodyB; p + 4 <= bodyE && !rec; ++p) {
            if (inBody(p, "while") && inBody(p + 1, "(") && inBody(p + 2, "--") &&
                inBody(p + 3, pB.c_str()) && inBody(p + 4, ")")) {
                rec = true;
                iters = N - 1;
            }
        }
        if (!rec) {  // wrapper form: `var F=function(D){while(--D)...}; F(--B+K)`
            for (int p = bodyB; p + 8 <= bodyE && !rec; ++p) {
                if (!(is(p, "var") || is(p, "let") || is(p, "const"))) continue;
                if (!(is(p + 2, "=") && is(p + 3, "function") && is(p + 4, "(")))
                    continue;
                if (!kindIs(p + 5, K::Ident) || !is(p + 6, ")") || !is(p + 7, "{"))
                    continue;
                const std::string D = at(p + 5)->raw;
                if (!(is(p + 8, "while") && is(p + 9, "(") && is(p + 10, "--") &&
                      is(p + 11, D.c_str()) && is(p + 12, ")")))
                    continue;
                const std::string F = at(p + 1)->raw;
                for (int q = p + 13; q + 2 <= bodyE && !rec; ++q) {
                    if (!inBody(q, F.c_str()) || !inBody(q + 1, "(")) continue;
                    const int k2 = q + 2;
                    if (kindBody(k2, K::Num)) {
                        long long v = 0;
                        if (parseIntLit(at(k2)->raw, v) && inBody(k2 + 1, ")")) {
                            rec = true;
                            iters = v - 1;
                        }
                        continue;
                    }
                    if ((inBody(k2, "--") || inBody(k2, "++")) &&
                        kindBody(k2 + 1, K::Ident) && at(k2 + 1)->raw == pB) {
                        long long v = N + (inBody(k2, "--") ? -1 : 1);
                        int k3 = k2 + 2;
                        if (inBody(k3, "+") || inBody(k3, "-")) {
                            long long lit = 0;
                            if (!kindBody(k3 + 1, K::Num) ||
                                !parseIntLit(at(k3 + 1)->raw, lit))
                                continue;
                            v += (at(k3)->raw == "+") ? lit : -lit;
                            k3 += 2;
                        }
                        if (inBody(k3, ")")) {
                            rec = true;
                            iters = v - 1;
                        }
                    }
                }
            }
        }
        if (!rec) continue;   // unrecognized rotation: refuse to decode this array

        if (iters < 0) iters = 0;
        if (!arr[ai].vals.empty()) {
            const long long sz = static_cast<long long>(arr[ai].vals.size());
            iters %= sz;
            if (iters > 0)
                std::rotate(arr[ai].vals.begin(),
                            arr[ai].vals.begin() + static_cast<std::ptrdiff_t>(iters),
                            arr[ai].vals.end());
        }
        arr[ai].rotSpans.emplace_back(ci[j], ci[c + 5]);
    }

    // -- pass 1d: wrappers `function W(i,k){return A[i-k];}` ------------------
    for (int j = 0; j + 3 < static_cast<int>(ci.size()); ++j) {
        if (!is(j, "function") || !kindIs(j + 1, K::Ident)) continue;
        if (!is(j + 2, "(")) continue;
        std::vector<std::string> params;
        int k = j + 3;
        bool okP = true;
        while (true) {
            const Tok* t = at(k);
            if (!t) { okP = false; break; }
            if (t->raw == ")") break;
            if (t->kind != K::Ident) { okP = false; break; }
            params.push_back(t->raw);
            ++k;
            if (at(k) && at(k)->raw == ",") { ++k; continue; }
            break;
        }
        if (!okP || params.empty() || !is(k, ")")) continue;
        if (!(is(k + 1, "{") && is(k + 2, "return") && kindIs(k + 3, K::Ident) &&
              is(k + 4, "[")))
            continue;
        const int ai = resolve(at(k + 3)->raw);
        if (ai < 0) continue;
        // expr: only params, integers and +/− until `]`
        int e = k + 5;
        std::vector<Tok> expr;
        bool okE = true;
        while (at(e) && at(e)->raw != "]") {
            const Tok& t = at(e) ? *at(e) : Tok{K::Punct, "", "", false};
            if (t.kind == K::Ident) {
                if (std::find(params.begin(), params.end(), t.raw) == params.end()) {
                    okE = false;
                    break;
                }
            } else if (t.kind == K::Num) {
                long long dummy = 0;
                if (!parseIntLit(t.raw, dummy)) { okE = false; break; }
            } else if (!(t.kind == K::Punct && (t.raw == "+" || t.raw == "-"))) {
                okE = false;
                break;
            }
            expr.push_back(t);
            ++e;
        }
        if (!okE || expr.empty() || !is(e, "]") || !is(e + 1, ";") || !is(e + 2, "}"))
            continue;
        Wrap w;
        w.arrIdx = ai;
        w.params = params;
        w.expr = std::move(expr);
        w.spanB = ci[j];
        w.spanE = ci[e + 2];
        w.name = at(j + 1)->raw;
        wrapsByName[w.name] = static_cast<int>(wraps.size());
        wraps.push_back(std::move(w));
    }

    // -- pass 1e: every other use of an array name must be provably a read ----
    static const std::unordered_set<std::string> kReadonly = {
        "length", "slice", "indexOf", "includes", "join", "at", "toString",
        "concat", "map", "filter", "find", "findIndex", "every", "some",
        "reduce", "reduceRight", "forEach", "keys", "values", "entries",
        "flat", "flatMap", "toLocaleString"};
    for (int i = 0; i < static_cast<int>(toks.size()); ++i) {
        if (toks[i].kind != K::Ident) continue;
        const int ai = resolve(toks[i].raw);
        if (ai < 0) continue;
        if (inSpan(i, arr[ai].declB, arr[ai].declE)) continue;
        bool exempt = false;
        for (const auto& sp : arr[ai].rotSpans)
            if (inSpan(i, sp.first, sp.second)) { exempt = true; break; }
        if (!exempt)
            for (const auto& al : aliases)
                if (al.arrIdx == ai && inSpan(i, al.spanB, al.spanE)) { exempt = true; break; }
        if (!exempt)
            for (const auto& w : wraps)
                if (w.arrIdx == ai && inSpan(i, w.spanB, w.spanE)) { exempt = true; break; }
        if (exempt) continue;

        const int j1 = nextCode(toks, i + 1);
        bool allowed = false;
        if (j1 >= 0 && toks[j1].raw == "[") {
            const int j2 = nextCode(toks, j1 + 1);
            const int j3 = j2 >= 0 ? nextCode(toks, j2 + 1) : -1;
            if (j2 >= 0 && toks[j2].kind == K::Num && j3 >= 0 && toks[j3].raw == "]") {
                const int j4 = nextCode(toks, j3 + 1);
                static const std::unordered_set<std::string> kAssign = {
                    "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
                    "<<=", ">>=", ">>>=", "++", "--"};
                allowed = j4 < 0 || !kAssign.count(toks[j4].raw);
            }
        } else if (j1 >= 0 && toks[j1].raw == ".") {
            const int j2 = nextCode(toks, j1 + 1);
            allowed = j2 >= 0 && toks[j2].kind == K::Ident &&
                      kReadonly.count(toks[j2].raw) > 0;
        } else if (j1 >= 0 && (toks[j1].raw == "of" || toks[j1].raw == "in" ||
                               toks[j1].raw == "===" || toks[j1].raw == "!==" ||
                               toks[j1].raw == "==" || toks[j1].raw == "!=" ||
                               toks[j1].raw == "??" || toks[j1].raw == "&&" ||
                               toks[j1].raw == "||" || toks[j1].raw == "?" ||
                               toks[j1].raw == "...")) {
            allowed = true;
        }
        if (!allowed) arr[ai].decodable = false;
    }

    // -- pass 2: rewrite constant reads, wrapper calls, atob/unescape calls ---
    std::vector<Tok> out;
    out.reserve(toks.size());
    auto makeStr = [](const std::string& v) {
        Tok s;
        s.kind = K::Str;
        s.val = v;
        s.decoded = true;
        s.raw = encodeString(v);
        return s;
    };
    int i = 0;
    while (i < static_cast<int>(toks.size())) {
        const Tok& t = toks[i];
        if (t.kind == K::Comment) { out.push_back(t); ++i; continue; }
        if (t.kind == K::Ident) {
            const int ai = resolve(t.raw);
            if (ai >= 0 && arr[ai].decodable) {
                const int j1 = nextCode(toks, i + 1);
                if (j1 >= 0 && toks[j1].raw == "[") {
                    const int j2 = nextCode(toks, j1 + 1);
                    const int j3 = j2 >= 0 ? nextCode(toks, j2 + 1) : -1;
                    long long idx = -1;
                    if (j2 >= 0 && toks[j2].kind == K::Num && j3 >= 0 &&
                        toks[j3].raw == "]" && parseIntLit(toks[j2].raw, idx) &&
                        idx >= 0 && static_cast<std::size_t>(idx) < arr[ai].vals.size()) {
                        out.push_back(makeStr(arr[ai].vals[static_cast<std::size_t>(idx)]));
                        st.stringsDecoded++;
                        i = j3 + 1;
                        continue;
                    }
                }
            }
            const auto wi = wrapsByName.find(t.raw);
            if (wi != wrapsByName.end()) {
                const Wrap& w = wraps[wi->second];
                const int j1 = nextCode(toks, i + 1);
                if (j1 >= 0 && toks[j1].raw == "(") {
                    std::vector<long long> args;
                    int j = nextCode(toks, j1 + 1);
                    bool ok = true;
                    while (j >= 0 && toks[j].raw != ")") {
                        long long v = 0;
                        if (toks[j].kind != K::Num || !parseIntLit(toks[j].raw, v)) {
                            ok = false;
                            break;
                        }
                        args.push_back(v);
                        const int jn = nextCode(toks, j + 1);
                        if (jn >= 0 && toks[jn].raw == ",") {
                            j = nextCode(toks, jn + 1);
                            continue;
                        }
                        j = jn;
                        break;
                    }
                    long long val = 0;
                    if (ok && j >= 0 && toks[j].raw == ")" &&
                        args.size() == w.params.size() &&
                        evalExpr(w.expr, w.params, args, val) && val >= 0 &&
                        static_cast<std::size_t>(val) < arr[w.arrIdx].vals.size()) {
                        out.push_back(
                            makeStr(arr[w.arrIdx].vals[static_cast<std::size_t>(val)]));
                        st.stringsDecoded++;
                        i = j + 1;
                        continue;
                    }
                }
            }
            const bool isBuiltin = t.raw == "atob" || t.raw == "unescape" ||
                                   t.raw == "decodeURIComponent";
            const int jp = prevCode(toks, i - 1);
            if (isBuiltin && (jp < 0 || toks[jp].raw != ".")) {
                const int j1 = nextCode(toks, i + 1);
                const int j2 = j1 >= 0 ? nextCode(toks, j1 + 1) : -1;
                const int j3 = j2 >= 0 ? nextCode(toks, j2 + 1) : -1;
                if (j1 >= 0 && toks[j1].raw == "(" && j2 >= 0 &&
                    toks[j2].kind == K::Str && j3 >= 0 && toks[j3].raw == ")") {
                    const std::string in = strValue(toks[j2]);
                    std::string dec;
                    bool ok = false;
                    if (t.raw == "atob") ok = b64Decode(in, dec);
                    else if (t.raw == "unescape") ok = unescapeConst(in, dec);
                    else ok = decodeUriConst(in, dec);
                    if (ok) {
                        out.push_back(makeStr(dec));
                        st.base64Decoded++;
                        i = j3 + 1;
                        continue;
                    }
                }
            }
        }
        out.push_back(t);
        ++i;
    }

    // -- pass 3: fold "a"+"b" chains (index-adjacent only, so comments stay) -
    for (std::size_t p = 0; p + 2 < out.size();) {
        if (out[p].kind == K::Str && out[p + 1].kind == K::Punct &&
            out[p + 1].raw == "+" && out[p + 2].kind == K::Str) {
            Tok m = makeStr(strValue(out[p]) + strValue(out[p + 2]));
            out.erase(out.begin() + static_cast<std::ptrdiff_t>(p + 1),
                      out.begin() + static_cast<std::ptrdiff_t>(p + 3));
            out[p] = std::move(m);
            st.concatsFolded++;
            continue;   // stay: the merged literal may fold again
        }
        ++p;
    }

    toks.swap(out);
    st.lookedObfuscated = st.arraysFound > 0 || st.base64Decoded > 0 ||
                          st.escapesDecoded >= 4;
}

// ---------------------------------------------------------------------------
// Emitter — re-indent without ever entering a token
// ---------------------------------------------------------------------------

bool needSpace(const Tok& p, const Tok& n) {
    if (p.raw.empty() || n.raw.empty()) return false;
    if (p.kind == K::Comment) return true;
    const char a = p.raw.back();
    const char b = n.raw.front();
    if (wordChar(a) && wordChar(b)) return true;            // var x / 1 in x
    if (a == '+' && b == '+') return true;                  // a+ +b must not fuse
    if (a == '-' && b == '-') return true;
    if (std::isdigit(static_cast<unsigned char>(a)) && b == '.') return true;  // 1 .toString
    if (a == '/' && (b == '/' || b == '*')) return true;
    if (p.kind == K::Ident &&
        (n.kind == K::Str || n.kind == K::Template || n.kind == K::Regex))
        return true;                                        // return"x" -> return "x"
    if (p.kind == K::Ident && b == '{' &&
        (p.raw == "else" || p.raw == "do" || p.raw == "try"))
        return true;
    if (p.kind == K::Punct && p.raw == ")" && b == '{') return true;  // if(x) {
    return false;
}

std::string emit(const std::vector<Tok>& toks) {
    std::string o;
    int ind = 0, paren = 0, brack = 0;
    bool lineStart = true;
    bool forceNl = false;
    const Tok* prev = nullptr;
    for (const Tok& t : toks) {
        bool nl = forceNl;
        if (t.raw == "}") {
            nl = true;
            if (ind > 0) --ind;
        }
        if (t.kind == K::Ident &&
            (t.raw == "else" || t.raw == "catch" || t.raw == "finally" ||
             t.raw == "case" || t.raw == "default" || t.raw == "do"))
            nl = true;
        if (nl && !lineStart) {
            o += '\n';
            lineStart = true;
        } else if (!nl && !lineStart && prev && needSpace(*prev, t)) {
            o += ' ';
        }
        if (lineStart) o.append(static_cast<std::size_t>(ind) * 2, ' ');
        if (t.kind == K::Str && t.decoded) o += encodeString(t.val);
        else o += t.raw;
        lineStart = false;
        prev = &t;
        forceNl = false;

        if (t.kind == K::Comment) {
            forceNl = true;   // comments always end their line
        } else if (t.kind == K::Punct) {
            if (t.raw == "(") ++paren;
            else if (t.raw == ")") --paren;
            else if (t.raw == "[") ++brack;
            else if (t.raw == "]") --brack;
            else if (t.raw == "{") { ++ind; forceNl = true; }
            else if (t.raw == "}") forceNl = true;
            else if ((t.raw == ";" || t.raw == ",") && paren == 0 && brack == 0)
                forceNl = true;
        }
    }
    if (!lineStart) o += '\n';
    return o;
}

}  // namespace

std::string deobfuscate(std::string_view js, DeobfStats* stats) {
    DeobfStats local;
    DeobfStats& st = stats ? *stats : local;
    st = DeobfStats{};
    st.inBytes = js.size();
    const std::string src(js);
    std::vector<Tok> toks = lex(src, &st);
    runTransforms(toks, st);
    std::string out = emit(toks);
    st.outBytes = out.size();
    return out;
}

}  // namespace lucyn::net
