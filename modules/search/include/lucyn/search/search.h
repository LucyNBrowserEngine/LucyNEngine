// lucyn/search/search.h - provider registry and URL builder for ordinary web searches.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace lucyn::search {

// Per-search options. Every field is optional; unset options are ignored. An engine that
// declares no parameter template for an option ignores that option too, so a caller can
// always set everything and let each engine pick up what it understands.
struct Options {
    std::optional<std::string> region;         // "us", "de", "jp"
    std::optional<int>         safeSearch;     // 0 = off, 1 = moderate, 2 = strict
    std::optional<std::string> language;       // "en", "de"
    std::optional<int>         resultsPerPage; // 10, 20, 50, 100
};

struct Engine {
    std::string id;
    std::string name;
    // Must contain one `{query}` placeholder. Empty means the provider needs configuration.
    std::string queryTemplate;
    // The remaining fields have defaults so a 3-field aggregate like {"id", "name", "tmpl"}
    // (used by callers such as the controller) stays valid and warning-free.
    // Optional short keyword for URL-bar shortcuts ("g" -> Google). Empty = none.
    std::string keyword = "";
    // Per-option query-parameter templates. Empty = engine does not support that option.
    // Placeholders: {safe} (0..2), {region}, {language}, {count}. A template without the
    // matching placeholder is appended verbatim when the option is set (a fixed flag like "kp=1").
    std::string safeSearchParam = "";
    std::string regionParam     = "";
    std::string languageParam   = "";
    std::string countParam      = "";
};

// Reverse-mapped result of parseUrl: which engine produced a search URL, and the query.
struct ParsedSearch {
    std::string engineId;
    std::string query;   // percent-decoded
};

// Registry of search providers.
// NOT thread-safe: callers that share one across threads must synchronize around the mutating
// calls (add/setDefault/load*). Read-only lookups (find/engines/url/parseUrl) are safe only when
// no concurrent writer is running.
class Registry {
public:
    Registry();

    // Adds or replaces a provider with the same id. A non-empty template must contain
    // exactly one `{query}` placeholder; an empty template is allowed for configurable engines.
    bool add(Engine engine);
    std::optional<Engine> find(const std::string& id) const;
    std::optional<Engine> findByKeyword(const std::string& keyword) const;
    std::vector<Engine> engines() const;

    // Explicit engine, no options.
    std::optional<std::string> url(const std::string& id, const std::string& query,
                                   std::string* error = nullptr) const;
    // Explicit engine with options.
    std::optional<std::string> url(const std::string& id, const std::string& query,
                                   const Options& opts, std::string* error = nullptr) const;
    // Default engine (see defaultId/setDefault).
    std::optional<std::string> url(const std::string& query, std::string* error = nullptr) const;

    // The engine used when no id is given ("brave" out of the box).
    bool setDefault(const std::string& id);   // false if id unknown
    const std::string& defaultId() const;

    // Reverse lookup: which engine produced this search URL, and what was the query.
    std::optional<ParsedSearch> parseUrl(const std::string& url) const;

    // Persistence. loadFromJson also backs the CLI's --config override file:
    //   { "default": "brave", "engines": [ { "id": "searxng", ... } ] }
    // load* merges into the current set (matching ids are replaced); save/toJson serialize everything.
    bool loadFromJson(const std::string& json, std::string* error = nullptr);
    bool loadFromFile(const std::string& path, std::string* error = nullptr);
    bool saveToFile(const std::string& path, std::string* error = nullptr) const;
    std::string toJson() const;

private:
    std::vector<Engine> engines_;
    std::string defaultId_ = "brave";
};

}  // namespace lucyn::search
