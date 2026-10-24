# lucyn::search

A small registry that turns a query into a provider URL. It does not scrape results or use paid APIs; LucyN visits the provider's ordinary search page through its existing network and browser pipeline.

The default registry includes Brave, Google, DuckDuckGo, Bing, Ecosia, Startpage, Mojeek, Qwant, Swisscows, and SearXNG. SearXNG is a self-hostable metasearch engine rather than one canonical website, so configure an instance URL before selecting it.

Add or override a provider with a template containing one `{query}` slot:

```cpp
lucyn::search::Registry providers;
providers.add({"my-search", "My Search", "https://search.example/?term={query}"});
auto url = providers.url("my-search", "LucyN browser");
```

## Options

Real engines accept more than a query. `Options` adds safe search, region, language, and results-per-page; each engine picks up only the options it declares a parameter template for, and unsupported options are ignored:

```cpp
lucyn::search::Options o;
o.region = "us"; o.language = "en"; o.resultsPerPage = 100; o.safeSearch = 2;
auto url = providers.url("google", "LucyN browser", o);   // ...&gl=us&hl=en&num=100
```

Per-engine templates use the placeholders `{safe}`, `{region}`, `{language}`, and `{count}`. A template without its placeholder is appended as a fixed flag when the option is set (DuckDuckGo's `kp=1` for safe search, for example). Engines that use a value vocabulary the option can't express leave that template empty.

## Default engine and keywords

`url(query)` uses the default engine (Brave out of the box). Pick another with `setDefault`:

```cpp
providers.setDefault("google");          // false if the id is unknown
auto url = providers.url("LucyN");       // Google
```

Each engine may carry a short keyword for URL-bar shortcuts; look one up with `findByKeyword`:

```cpp
auto google = providers.findByKeyword("g");
```

## Reverse lookup

`parseUrl` maps a search URL back to `(engine, query)` — useful for a URL bar that wants to show "Search Google for: …" instead of the raw URL:

```cpp
if (auto hit = providers.parseUrl(address))
    show("Search " + hit->engineId + " for: " + hit->query);
```

## Persistence

User-added engines survive a restart. `saveToFile`/`loadFromFile` round-trip the whole registry as JSON (`toJson`/`loadFromJson` expose the same data in memory). Loading merges into the current set, replacing any engine with a matching id, and can set the default:

```json
{
  "default": "brave",
  "engines": [
    { "id": "searxng", "name": "SearXNG", "queryTemplate": "https://my-instance.example/search?q={query}" }
  ]
}
```

## Notes

- Input queries are expected to be UTF-8; each byte is percent-encoded independently (correct for UTF-8 URLs).
- `Registry` is not thread-safe. Callers sharing one across threads must synchronize around the mutating calls (`add`, `setDefault`, `load*`).

`Registry` has no dependencies and builds standalone with `cmake -S modules/search -B build-search && cmake --build build-search && ctest --test-dir build-search`.
