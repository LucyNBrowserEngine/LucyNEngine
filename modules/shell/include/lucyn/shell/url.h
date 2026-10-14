// lucyn/shell/url.h - just enough URL handling for hoisting <link>/<base> out of a site.
#pragma once
#include <string>

namespace lucyn::dom { class Document; }

namespace lucyn::shell {

// RFC 3986-style resolution of `ref` against `base` (absolute, //host, /path, ../x, ?q, #f).
// data:, javascript:, mailto: and other absolute schemes come back untouched.
std::string resolveUrl(const std::string& base, const std::string& ref);

// "https://host:8080" for http(s)/ws URLs, "" when there is no authority (file:, data:, junk).
std::string originOf(const std::string& url);

// A folder-safe name for a page's site: "example.com", "example.com_8080", "local" for file:/unknown.
std::string hostKey(const std::string& url);

// Where this page's favicon lives: first <link rel~=icon> (relative to <base href> and the page URL),
// then apple-touch-icon, then <origin>/favicon.ico. "" when the page has no origin and no link.
std::string faviconUrl(const dom::Document& doc, const std::string& pageUrl);

}  // namespace lucyn::shell
