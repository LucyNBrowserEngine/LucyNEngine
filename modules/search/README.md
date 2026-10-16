# lucyn::search

A small registry that turns a query into a provider URL. It does not scrape results or use paid APIs; LucyN visits the provider's ordinary search page through its existing network and browser pipeline.

The default registry includes Brave, Google, DuckDuckGo, Bing, Ecosia, Startpage, Mojeek, Qwant, Swisscows, and SearXNG. SearXNG is a self-hostable metasearch engine rather than one canonical website, so configure an instance URL before selecting it.

Add or override a provider with a template containing one `{query}` slot:

```cpp
lucyn::search::Registry providers;
providers.add({"my-search", "My Search", "https://search.example/?term={query}"});
auto url = providers.url("my-search", "LucyN browser");
```

`Registry` has no dependencies and builds standalone with `cmake -S modules/search -B build-search`.
