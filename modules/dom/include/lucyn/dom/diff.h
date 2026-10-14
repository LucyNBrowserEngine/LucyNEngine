// lucyn/dom/diff.h - "what is in the new HTML that was not in the old one?"
// Works on HTML strings so it compares like with like (both sides are parsed the same way).
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace lucyn::dom {

struct DiffEntry {
    std::string key;      // tag name, or element signature like  div#app.card.big
    int         before = 0;
    int         after  = 0;
};

struct HtmlDiff {
    std::vector<DiffEntry> tags;       // per tag name: how many before / after (changed only)
    std::vector<DiffEntry> elements;   // per signature tag#id.class...: changed only
    int beforeElements = 0, afterElements = 0;

    bool empty() const { return tags.empty() && elements.empty(); }
    // Tags that exist after but never existed before.
    std::vector<std::string> newTags() const;
    // Tags that existed before and are gone now.
    std::vector<std::string> goneTags() const;
};

HtmlDiff diffHtml(std::string_view beforeHtml, std::string_view afterHtml);

// Human-readable report ("+ div#app (0 -> 1)", "- noscript (1 -> 0)"). Never empty.
std::string formatDiff(const HtmlDiff& d);

}  // namespace lucyn::dom
