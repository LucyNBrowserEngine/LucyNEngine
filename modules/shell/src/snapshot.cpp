#include <lucyn/shell/snapshot.h>
#include <lucyn/core/paths.h>
#include <lucyn/shell/url.h>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace lucyn::shell {

namespace {
constexpr size_t kMaxLabels = 64;

std::string stamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

bool writeFile(const fs::path& p, const std::string& data, std::string* err) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (out) out.write(data.data(), (std::streamsize)data.size());
    if (!out) { if (err) *err = "cannot write " + p.string(); return false; }
    return true;
}
}  // namespace

Snapshots::Snapshots(bool toDisk, std::string pageUrl) : toDisk_(toDisk), url_(std::move(pageUrl)) {}

bool Snapshots::validLabel(const std::string& l) {
    if (l.empty() || l.size() > 64) return false;
    for (char c : l) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool Snapshots::ensureDir(std::string* err) {
    if (!runDir_.empty()) return true;
    std::string base = core::dataDir("snapshots/" + hostKey(url_));
    if (base.empty()) { if (err) *err = "cannot resolve the snapshot folder"; return false; }
    std::string name = stamp();
    fs::path dir = fs::path(base) / name;
    std::error_code ec;
    for (int n = 2; fs::exists(dir, ec) && n < 1000; ++n) dir = fs::path(base) / (name + "-" + std::to_string(n));
    if (!fs::create_directories(dir, ec) && !fs::is_directory(dir, ec)) {
        if (err) *err = "cannot create " + dir.string() + ": " + ec.message();
        return false;
    }
    runDir_ = dir.string();
    return true;
}

bool Snapshots::take(const std::string& label, std::string html, std::string* err) {
    if (!validLabel(label)) { if (err) *err = "bad snapshot label '" + label + "' (letters, digits, - and _ only)"; return false; }
    size_t idx = order_.size();
    for (size_t i = 0; i < order_.size(); ++i) if (order_[i] == label) { idx = i; break; }
    if (idx == order_.size() && order_.size() >= kMaxLabels) { if (err) *err = "too many snapshots in one run"; return false; }

    bool wrote = true;
    if (toDisk_) wrote = ensureDir(err) && writeFile(fs::path(runDir_) / (label + ".html"), html, err);
    // Memory is updated either way: a full disk must not lose the snapshot for the diff.
    if (idx == order_.size()) { order_.push_back(label); html_.push_back(std::move(html)); }
    else html_[idx] = std::move(html);
    return wrote;
}

bool Snapshots::saveNote(const std::string& fileName, const std::string& text, std::string* err) {
    if (!toDisk_) return true;
    if (fileName.empty() || fileName.find_first_of("/\\") != std::string::npos || fileName.find("..") != std::string::npos) {
        if (err) *err = "bad file name";
        return false;
    }
    return ensureDir(err) && writeFile(fs::path(runDir_) / fileName, text, err);
}

const std::string* Snapshots::get(const std::string& label) const {
    for (size_t i = 0; i < order_.size(); ++i) if (order_[i] == label) return &html_[i];
    return nullptr;
}

std::optional<dom::HtmlDiff> Snapshots::diff(const std::string& a, const std::string& b) const {
    const std::string* x = get(a);
    const std::string* y = get(b);
    if (!x || !y) return std::nullopt;
    return dom::diffHtml(*x, *y);
}

}  // namespace lucyn::shell
