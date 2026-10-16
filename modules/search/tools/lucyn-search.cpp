#include <lucyn/search/search.h>

#include <iostream>
#include <sstream>

int main(int argc, char** argv) {
    lucyn::search::Registry providers;
    if (argc == 2 && std::string(argv[1]) == "list") {
        for (const auto& engine : providers.engines())
            std::cout << engine.id << "\t" << engine.name << (engine.queryTemplate.empty() ? "\t(configure instance)" : "") << '\n';
        return 0;
    }
    if (argc < 3) {
        std::cerr << "usage: lucyn-search list | <engine> <query> [--searxng <instance-url>]\n";
        return 2;
    }

    const std::string id = argv[1];
    std::string query;
    std::string instance;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--searxng" && i + 1 < argc) {
            instance = argv[++i];
        } else {
            if (!query.empty()) query += ' ';
            query += argv[i];
        }
    }
    if (!instance.empty()) {
        while (!instance.empty() && instance.back() == '/') instance.pop_back();
        if (instance.size() >= 7 && instance.compare(instance.size() - 7, 7, "/search") == 0)
            providers.add({"searxng", "SearXNG", instance + "?q={query}"});
        else
            providers.add({"searxng", "SearXNG", instance + "/search?q={query}"});
    }
    std::string error;
    auto url = providers.url(id, query, &error);
    if (!url) { std::cerr << error << '\n'; return 1; }
    std::cout << *url << '\n';
    return 0;
}
