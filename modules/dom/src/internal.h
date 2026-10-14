// dom-private helpers, not installed.
#pragma once
#include <lucyn/dom/dom.h>

namespace lucyn::dom {
// Parses `html` as if it were the content of <ctx>. Returned nodes have no parent;
// their ownerDocument still points at a dead scratch Document, so the caller MUST re-tag them.
std::vector<std::unique_ptr<Node>> parseFragment(const std::string& ctx, std::string_view html);
}
