// lucyn/core/assets.h — what counts as an "asset" worth keeping.
//
// LucyN is a reverse-engineering browser: everything the site serves that is
// CODE or a DOCUMENT gets kept (Moonbug body + a file in nothing-data/artifacts),
// while images, fonts and media only show up as a capture row with headers and a
// byte count. This header is the single definition of that line — cef_host uses
// it to decide whether to buffer a response body at all, the controller uses it
// to decide whether to save a file.
#pragma once
#include <string>

namespace lucyn::core {

// True for data:/blob:/about: pseudo-URLs — nothing to capture there.
bool isPseudoUrl(const std::string& url);

// Text/code on the wire: .js/.html/.txt/.css/.json/... by extension, or a
// text-ish Content-Type when the URL has no useful extension. Also the answer
// to "may the engine buffer this response body?" (cef_host filters on it, so
// images and video are never copied around).
bool isTextAsset(const std::string& url, const std::string& contentType);

// The script subset (.js/.mjs/.cjs or a JavaScript Content-Type): these get the
// deobfuscation pass, which is what produces the second view on disk.
bool isScriptAsset(const std::string& url, const std::string& contentType);

// Extension (WITH the leading dot, lowercase) to save this asset under: first
// from the URL path, else derived from the Content-Type (".json", ".html", ...).
// "" when neither says — callers then fall back to ".txt".
std::string assetExtension(const std::string& url, const std::string& contentType);

}  // namespace lucyn::core
