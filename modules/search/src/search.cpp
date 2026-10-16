#include <lucyn/search/search.h>

#include <algorithm>
#include <utility>

namespace lucyn::search {
namespace {

std::string encodeQuery(const std::string& query) {
    static const char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char c : query) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += static_cast<char>(c);
        } else {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 0x0f];
        }
    }
    return encoded;
}

}  // namespace

Registry::Registry() {
    add({"brave", "Brave Search", "https://search.brave.com/search?q={query}"});
    add({"searxng", "SearXNG", ""});
    add({"google", "Google", "https://www.google.com/search?q={query}"});
    add({"duckduckgo", "DuckDuckGo", "https://duckduckgo.com/?q={query}"});
    add({"duckduckgo-lite", "DuckDuckGo Lite (no JavaScript)", "https://lite.duckduckgo.com/lite/?q={query}"});
    add({"bing", "Bing", "https://www.bing.com/search?q={query}"});
    add({"ecosia", "Ecosia", "https://www.ecosia.org/search?q={query}"});
    add({"startpage", "Startpage", "https://www.startpage.com/sp/search?q={query}"});
    add({"mojeek", "Mojeek", "https://www.mojeek.com/search?q={query}"});
    add({"qwant", "Qwant", "https://www.qwant.com/?q={query}"});
    add({"swisscows", "Swisscows", "https://swisscows.com/en/web?query={query}"});
}

bool Registry::add(Engine engine) {
    if (engine.id.empty() || engine.name.empty()) return false;
    if (!engine.queryTemplate.empty()) {
        const size_t first = engine.queryTemplate.find("{query}");
        if (first == std::string::npos || engine.queryTemplate.find("{query}", first + 7) != std::string::npos)
            return false;
    }
    auto it = std::find_if(engines_.begin(), engines_.end(), [&](const Engine& e) { return e.id == engine.id; });
    if (it == engines_.end()) engines_.push_back(std::move(engine));
    else *it = std::move(engine);
    return true;
}

std::optional<Engine> Registry::find(const std::string& id) const {
    auto it = std::find_if(engines_.begin(), engines_.end(), [&](const Engine& e) { return e.id == id; });
    if (it == engines_.end()) return std::nullopt;
    return *it;
}

std::vector<Engine> Registry::engines() const { return engines_; }

std::optional<std::string> Registry::url(const std::string& id, const std::string& query, std::string* error) const {
    auto engine = find(id);
    if (!engine) {
        if (error) *error = "unknown search engine: " + id;
        return std::nullopt;
    }
    if (engine->queryTemplate.empty()) {
        if (error) *error = "search engine '" + id + "' needs a configured search URL";
        return std::nullopt;
    }
    std::string result = engine->queryTemplate;
    const size_t slot = result.find("{query}");
    result.replace(slot, 7, encodeQuery(query));
    return result;
}

}  // namespace lucyn::search
