#include <lucyn/shell/host.h>
#include <exception>

namespace lucyn::shell {

bool NativeHooks::add(const std::string& name, HookFn fn) {
    if (name.empty() || !fn || hooks_.count(name)) return false;
    hooks_[name] = std::move(fn);
    return true;
}

std::vector<std::string> NativeHooks::names() const {
    std::vector<std::string> v;
    for (auto& [k, _] : hooks_) v.push_back(k);
    return v;
}

HookResult NativeHooks::call(const std::string& name, const std::vector<std::string>& args) const {
    auto it = hooks_.find(name);
    if (it == hooks_.end()) return {false, "no such hook: " + name};
    try {
        return it->second(args);
    } catch (const std::exception& e) {
        return {false, std::string("hook '") + name + "' failed: " + e.what()};
    } catch (...) {
        return {false, "hook '" + name + "' failed"};
    }
}

}  // namespace lucyn::shell
