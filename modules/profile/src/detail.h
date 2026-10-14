// Private helpers shared by the profile module's .cpp files. Not installed.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace lucyn::profile::detail {

inline std::string jsonQuote(std::string_view s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    o += buf;
                } else {
                    o += c;
                }
        }
    }
    return o + "\"";
}

// Minimal reader for ONE flat JSON object whose values are strings, numbers,
// true/false/null, or arrays of strings. That is all an identity file contains,
// so we do not need (or want) a full JSON library inside this module.
struct FlatJson {
    std::map<std::string, std::string>              scalars;   // strings (unescaped) and raw number/bool tokens
    std::map<std::string, std::vector<std::string>> arrays;
};

inline bool parseFlatJson(std::string_view s, FlatJson& out, std::string* err) {
    size_t i = 0;
    auto fail = [&](const char* m) {
        if (err) *err = std::string(m) + " at offset " + std::to_string(i);
        return false;
    };
    auto ws = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i; };
    auto str = [&](std::string& o) -> bool {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        o.clear();
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { o += c; continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case '"': o += '"';  break;
                case '\\': o += '\\'; break;
                case '/': o += '/';  break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    unsigned v = static_cast<unsigned>(std::strtoul(std::string(s.substr(i, 4)).c_str(), nullptr, 16));
                    i += 4;
                    o += v < 0x80 ? static_cast<char>(v) : '?';
                    break;
                }
                default: return false;
            }
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    };

    ws();
    if (i >= s.size() || s[i] != '{') return fail("expected '{'");
    ++i;
    ws();
    if (i < s.size() && s[i] == '}') return true;

    for (;;) {
        ws();
        std::string key;
        if (!str(key)) return fail("expected a quoted key");
        ws();
        if (i >= s.size() || s[i] != ':') return fail("expected ':'");
        ++i;
        ws();
        if (i >= s.size()) return fail("unexpected end");

        if (s[i] == '"') {
            std::string v;
            if (!str(v)) return fail("bad string");
            out.scalars[key] = v;
        } else if (s[i] == '[') {
            ++i;
            std::vector<std::string> arr;
            ws();
            if (i < s.size() && s[i] == ']') {
                ++i;
            } else {
                for (;;) {
                    ws();
                    std::string v;
                    if (!str(v)) return fail("expected a string in array");
                    arr.push_back(v);
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == ']') { ++i; break; }
                    return fail("expected ',' or ']'");
                }
            }
            out.arrays[key] = arr;
        } else {
            size_t st = i;
            while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ' ' && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') ++i;
            if (i == st) return fail("bad value");
            out.scalars[key] = std::string(s.substr(st, i - st));
        }

        ws();
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') return true;
        return fail("expected ',' or '}'");
    }
}

}  // namespace lucyn::profile::detail
