// lucyn/search/search.h - provider registry and URL builder for ordinary web searches.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace lucyn::search {

struct Engine {
    std::string id;
    std::string name;
    // Must contain one `{query}` placeholder. Empty means the provider needs configuration.
    std::string queryTemplate;
};

class Registry {
public:
    Registry();

    // Adds or replaces a provider with the same id. A non-empty template must contain
    // exactly one `{query}` placeholder; an empty template is allowed for configurable engines.
    bool add(Engine engine);
    std::optional<Engine> find(const std::string& id) const;
    std::vector<Engine> engines() const;
    std::optional<std::string> url(const std::string& id, const std::string& query,
                                   std::string* error = nullptr) const;

private:
    std::vector<Engine> engines_;
};

}  // namespace lucyn::search
