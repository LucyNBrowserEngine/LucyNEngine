// lucyn/core/overrides.h — local file overrides for intercepted network assets.
//
// Allows reverse-engineers to override any script, document, or API response
// in the live browser by placing files in nothing-data/overrides/.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace lucyn::core {

struct OverrideOptions {
    bool enabled = true;
    bool autoDeobf = false;       // If true, also checks nothing-data/artifacts/*__*.deobf.js
    std::string customDir;        // Empty = nothing-data/overrides/
};

// Given a URL, compute the canonical sanitized artifact/override base name:
// e.g. "https://example.com/static/js/app.123.js?v=2" -> "example.com__app.123.js"
std::string assetFilename(const std::string& url, const std::string& contentType = "");

// Determine the MIME type for an asset given its file path or extension:
// e.g. "example.com__app.js" -> "application/javascript"
std::string mimeTypeForPath(const std::string& path);

// Find an existing override file on disk for `url`.
// Checks candidates in priority order:
// 1. <overridesDir>/<host>__<base>
// 2. <overridesDir>/<host>__<base_stem>.deobf.<ext>
// 3. <overridesDir>/<host>/<url_path>
// 4. If opts.autoDeobf is true: <artifactsDir>/<host>__<base_stem>.deobf.js
// Returns the absolute path to the file if found, or empty string if not found.
std::string findOverrideFile(const std::string& url, const OverrideOptions& opts = {});

// Save an override on disk for `url` with `content`.
// Writes to <overridesDir>/<host>__<base>. Returns true on success.
bool saveOverride(const std::string& url, const std::string& content, const OverrideOptions& opts = {});

// Remove the override file for `url` if one exists in <overridesDir>. Returns true if removed.
bool clearOverride(const std::string& url, const OverrideOptions& opts = {});

// Entry in the list of overrides
struct OverrideEntry {
    std::string name;       // relative name, e.g. "example.com__app.js"
    std::string fullPath;   // absolute path on disk
    std::size_t size = 0;   // bytes
};

// List all files currently in the overrides directory.
std::vector<OverrideEntry> listOverrides(const OverrideOptions& opts = {});

}  // namespace lucyn::core

