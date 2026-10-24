#include <lucyn/search/search.h>

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    lucyn::search::Registry providers;

    // Split args into positionals and option pairs, so --config/--searxng can appear anywhere.
    std::vector<std::string> positional;
    std::string configPath, instance;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc)       configPath = argv[++i];
        else if (a == "--searxng" && i + 1 < argc) instance = argv[++i];
        else                                       positional.push_back(a);
    }

    // A JSON config file can override any engine (and the default) before we build a URL.
    if (!configPath.empty()) {
        std::string err;
        if (!providers.loadFromFile(configPath, &err)) {
            std::cerr << "config: " << err << '\n';
            return 1;
        }
    }
    if (!instance.empty()) {
        while (!instance.empty() && instance.back() == '/') instance.pop_back();
        if (instance.size() >= 7 && instance.compare(instance.size() - 7, 7, "/search") == 0)
            providers.add({"searxng", "SearXNG", instance + "?q={query}"});
        else
            providers.add({"searxng", "SearXNG", instance + "/search?q={query}"});
    }

    if (positional.empty() || positional[0] == "list") {
        for (const auto& engine : providers.engines()) {
            std::cout << engine.id << "\t" << engine.name;
            if (!engine.keyword.empty()) std::cout << "\tkeyword:" << engine.keyword;
            if (engine.queryTemplate.empty()) std::cout << "\t(configure instance)";
            if (engine.id == providers.defaultId()) std::cout << "\t(default)";
            std::cout << '\n';
        }
        return 0;
    }

    const std::string id = positional[0];
    std::string query;
    for (size_t i = 1; i < positional.size(); ++i) {
        if (!query.empty()) query += ' ';
        query += positional[i];
    }
    std::string error;
    auto url = providers.url(id, query, &error);
    if (!url) { std::cerr << error << '\n'; return 1; }
    std::cout << *url << '\n';
    return 0;
}
