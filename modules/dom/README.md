# lucyn::dom

HTML -> a tree you can read **and change**. Gumbo does the parsing; nothing outside this module sees Gumbo types.

```
cmake -S modules/dom -B build && cmake --build build && ctest --test-dir build
./build/lucyn-dom --tree page.html
./build/lucyn-dom -q "a[href]" -a href page.html
./build/lucyn-dom --diff before.html after.html        # new / gone tags and elements
```

| Read | |
|---|---|
| `parseHtml(html)` | Never fails (HTML5 recovery). |
| `querySelector(All)` | tag, `#id`, `.class`, `[attr]`, `[attr=v]`, compounds, descendant ` `, child `>`, comma lists, `*`. |
| `outerHTML / innerHTML / textContent`, `title()`, `getElementById`, navigation (`firstChild`, `nextSibling` ...) | |

| Change (all return `false` + message instead of throwing) | |
|---|---|
| `createElement / createTextNode / createComment` | New node starts detached, owned by the Document. |
| `appendChild / insertBefore / replaceChild` | Moves the node if it already sits somewhere (like the real DOM). Refuses cycles, text-with-children, other-document nodes. |
| `removeChild / remove` | Detaches; the node stays alive (pointer stays valid). |
| `setAttr / removeAttr / setTextContent / setInnerHTML / cloneNode / importNode` | `setInnerHTML` parses in the element's context (`<tr>` inside a table works; `<script>`/`<style>`/`<textarea>`/`<title>` take raw text). |
| `version()`, `onMutation(fn)` | Change counter and a mutation hook: the base for MutationObserver. |
| `diffHtml(a, b)` / `formatDiff` | Tag counts and element signatures (`div#app.a.b`) that changed. |

Ownership: every node is owned by its parent or by the Document's detached pool, so `Node*` never dangles until the Document dies or `purgeDetached()` is called.
Deep trees (100 000 levels) are handled without recursion for build, clone and free. Known gap: parsing and `outerHTML` still recurse.
