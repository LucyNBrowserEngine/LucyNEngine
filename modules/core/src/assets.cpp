// lucyn/core/assets.cpp — see assets.h for the idea.
#include "lucyn/core/assets.h"

#include <algorithm>
#include <cctype>

namespace lucyn::core {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// URL path without query/fragment, lowercased.
std::string lowerPath(const std::string& url) {
    std::size_t end = url.size();
    if (const std::size_t q = url.find_first_of("?#"); q != std::string::npos) end = q;
    return lower(url.substr(0, end));
}

// Lowercased Content-Type without parameters ("text/html; charset=utf-8" -> "text/html").
std::string mediaType(const std::string& contentType) {
    std::size_t end = contentType.find(';');
    if (end == std::string::npos) end = contentType.size();
    return lower(contentType.substr(0, end));
}

// Extension after the last '.' in the final path segment, lowercased, no dot.
std::string urlExt(const std::string& url) {
    const std::string path = lowerPath(url);
    std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) slash = 0;
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 == path.size()) return "";
    if (dot < slash) return "";
    return path.substr(dot + 1);
}

bool inSet(const std::string& ext, const char* const* set, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (ext == set[i]) return true;
    return false;
}

// The files a site serves that are worth reading: code and documents.
const char* const kTextExts[] = {"js",  "mjs", "cjs", "html", "htm", "txt", "css", "json",
                                 "map", "xml", "svg", "md",   "csv", "ts",  "tsx", "jsx",
                                 "webmanifest", "yml", "yaml"};
const char* const kScriptExts[] = {"js", "mjs", "cjs"};

bool textMediaType(const std::string& mt) {
    if (mt.empty()) return false;
    if (mt.rfind("text/", 0) == 0) return true;   // text/html, text/plain, text/css, ...
    return mt == "application/json" || mt == "application/manifest+json" ||
           mt == "application/javascript" || mt == "application/x-javascript" ||
           mt == "application/ecmascript" || mt == "application/xml" ||
           mt == "application/xhtml+xml" || mt == "application/rss+xml" ||
           mt == "image/svg+xml" || mt.find("json") != std::string::npos ||
           mt.find("javascript") != std::string::npos;
}

bool scriptMediaType(const std::string& mt) {
    return mt.find("javascript") != std::string::npos ||
           mt.find("ecmascript") != std::string::npos;
}

}  // namespace

bool isPseudoUrl(const std::string& url) {
    return url.rfind("data:", 0) == 0 || url.rfind("blob:", 0) == 0 ||
           url.rfind("about:", 0) == 0 || url.rfind("chrome:", 0) == 0;
}

bool isTextAsset(const std::string& url, const std::string& contentType) {
    if (isPseudoUrl(url)) return false;
    const std::string ext = urlExt(url);
    if (!ext.empty() &&
        inSet(ext, kTextExts, sizeof(kTextExts) / sizeof(kTextExts[0])))
        return true;
    return textMediaType(mediaType(contentType));
}

bool isScriptAsset(const std::string& url, const std::string& contentType) {
    if (isPseudoUrl(url)) return false;
    const std::string ext = urlExt(url);
    if (!ext.empty() &&
        inSet(ext, kScriptExts, sizeof(kScriptExts) / sizeof(kScriptExts[0])))
        return true;
    // An extensionless endpoint serving JavaScript (bundles, /api/..., ?module)
    // counts — but only when the type says so; a .json body is not a script.
    if (!ext.empty() && ext != "json") return false;
    return scriptMediaType(mediaType(contentType));
}

std::string assetExtension(const std::string& url, const std::string& contentType) {
    const std::string ext = urlExt(url);
    if (!ext.empty() &&
        inSet(ext, kTextExts, sizeof(kTextExts) / sizeof(kTextExts[0])))
        return "." + ext;
    const std::string mt = mediaType(contentType);
    if (mt == "application/json" || mt.find("json") != std::string::npos) return ".json";
    if (scriptMediaType(mt)) return ".js";
    if (mt == "text/html" || mt == "application/xhtml+xml") return ".html";
    if (mt == "text/css") return ".css";
    if (mt == "application/xml" || mt == "text/xml") return ".xml";
    if (mt == "image/svg+xml") return ".svg";
    if (mt.rfind("text/", 0) == 0) return ".txt";
    return "";
}

}  // namespace lucyn::core
