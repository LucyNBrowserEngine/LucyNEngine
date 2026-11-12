// lucyn/core/overrides.cpp — see overrides.h for the idea.
#include "lucyn/core/overrides.h"
#include "lucyn/core/assets.h"
#include "lucyn/core/paths.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace lucyn::core {
namespace {

std::string sanitizePart(std::string v, std::size_t maxLen) {
    for (char& c : v) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) c = '_';
    }
    if (v.size() > maxLen) v.resize(maxLen);
    return v;
}

void parseUrl(const std::string& url, std::string& host, std::string& path) {
    if (const std::size_t s = url.find("://"); s != std::string::npos) {
        const std::size_t h = s + 3;
        const std::size_t e = url.find('/', h);
        host = (e == std::string::npos) ? url.substr(h) : url.substr(h, e - h);
        path = (e == std::string::npos) ? std::string() : url.substr(e);
    } else {
        path = url;
    }
    if (const std::size_t q = path.find_first_of("?#"); q != std::string::npos) {
        path = path.substr(0, q);
    }
}

std::string deobfVariant(const std::string& filename) {
    const std::size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos) {
        return filename + ".deobf.js";
    }
    return filename.substr(0, dot) + ".deobf" + filename.substr(dot);
}

}  // namespace

std::string assetFilename(const std::string& url, const std::string& contentType) {
    std::string host, path;
    parseUrl(url, host, path);

    host = sanitizePart(host, 60);
    std::string base = sanitizePart(path.empty() ? std::string() : path.substr(path.find_last_of('/') + 1), 80);
    if (base.empty()) base = "index";
    if (base.find('.') == std::string::npos) {
        const std::string ext = assetExtension(url, contentType);
        base += ext.empty() ? ".txt" : ext;
    }
    return host.empty() ? base : (host + "__" + base);
}

std::string mimeTypeForPath(const std::string& path) {
    std::string ext;
    const std::size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        ext = path.substr(dot + 1);
        for (char& c : ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (ext == "js" || ext == "mjs" || ext == "cjs") return "application/javascript";
    if (ext == "html" || ext == "htm") return "text/html";
    if (ext == "css") return "text/css";
    if (ext == "json" || ext == "map") return "application/json";
    if (ext == "wasm") return "application/wasm";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "xml") return "application/xml";
    if (ext == "txt" || ext == "md") return "text/plain";
    return "text/plain";
}

std::string findOverrideFile(const std::string& url, const OverrideOptions& opts) {
    if (!opts.enabled || isPseudoUrl(url)) return "";

    const std::string dir = opts.customDir.empty() ? dataDir("overrides") : opts.customDir;
    std::error_code ec;

    const std::string flatName = assetFilename(url);
    const std::string deobfName = deobfVariant(flatName);

    if (!dir.empty() && fs::exists(dir, ec)) {
        // 1. Flat match in overrides: overrides/host__file.ext
        fs::path p1 = fs::path(dir) / flatName;
        if (fs::is_regular_file(p1, ec)) return p1.string();

        // 2. Deobf match in overrides: overrides/host__file.deobf.ext
        fs::path p2 = fs::path(dir) / deobfName;
        if (fs::is_regular_file(p2, ec)) return p2.string();

        // 3. Hierarchical match in overrides: overrides/host/path/file.ext
        std::string host, path;
        parseUrl(url, host, path);
        while (!path.empty() && path.front() == '/') path.erase(0, 1);
        if (!host.empty() && !path.empty()) {
            fs::path p3 = fs::path(dir) / host / path;
            if (fs::is_regular_file(p3, ec)) return p3.string();
        }
    }

    // 4. Auto-deobfuscation from artifacts/ folder if requested
    if (opts.autoDeobf) {
        const std::string artDir = dataDir("artifacts");
        if (!artDir.empty() && fs::exists(artDir, ec)) {
            fs::path p4 = fs::path(artDir) / deobfName;
            if (fs::is_regular_file(p4, ec)) return p4.string();
        }
    }

    return "";
}

bool saveOverride(const std::string& url, const std::string& content, const OverrideOptions& opts) {
    const std::string dir = opts.customDir.empty() ? dataDir("overrides") : opts.customDir;
    if (dir.empty()) return false;

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return false;

    const std::string filename = assetFilename(url);
    fs::path p = fs::path(dir) / filename;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return f.good();
}

bool clearOverride(const std::string& url, const OverrideOptions& opts) {
    const std::string dir = opts.customDir.empty() ? dataDir("overrides") : opts.customDir;
    if (dir.empty()) return false;

    std::error_code ec;
    if (!fs::exists(dir, ec)) return false;

    const std::string filename = assetFilename(url);
    fs::path p = fs::path(dir) / filename;
    bool removed = fs::remove(p, ec);

    // Also attempt removing deobf-named override if present
    const std::string deobfName = deobfVariant(filename);
    if (fs::remove(fs::path(dir) / deobfName, ec)) {
        removed = true;
    }

    return removed;
}

std::vector<OverrideEntry> listOverrides(const OverrideOptions& opts) {
    std::vector<OverrideEntry> out;
    const std::string dir = opts.customDir.empty() ? dataDir("overrides") : opts.customDir;
    if (dir.empty()) return out;

    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return out;

    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec)) {
            OverrideEntry e;
            e.fullPath = entry.path().string();
            e.name = fs::relative(entry.path(), dir, ec).string();
            e.size = entry.file_size(ec);
            out.push_back(std::move(e));
        }
    }
    std::sort(out.begin(), out.end(), [](const OverrideEntry& a, const OverrideEntry& b) {
        return a.name < b.name;
    });
    return out;
}

}  // namespace lucyn::core

