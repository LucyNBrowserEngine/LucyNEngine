// lucyn/shell/snapshot.h - labelled copies of a page ("before", "after", or anything a script asks for).
//
// On disk (unless disabled):
//     nothing-data/snapshots/<site>/<YYYYmmdd-HHMMSS>/before.html
//                                                     after.html
//                                                     diff.txt
#pragma once
#include <optional>
#include <string>
#include <vector>
#include <lucyn/dom/diff.h>

namespace lucyn::shell {

class Snapshots {
public:
    // toDisk=false keeps everything in memory.
    Snapshots(bool toDisk, std::string pageUrl);

    // Labels: letters, digits, '-' and '_' only (they become file names). Same label again overwrites.
    // At most 64 labels per run. Returns false and fills *err instead of throwing.
    bool take(const std::string& label, std::string html, std::string* err = nullptr);
    bool saveNote(const std::string& fileName, const std::string& text, std::string* err = nullptr);   // e.g. diff.txt

    const std::string* get(const std::string& label) const;
    const std::vector<std::string>& labels() const { return order_; }
    std::optional<dom::HtmlDiff> diff(const std::string& labelA, const std::string& labelB) const;

    // "" until the first file is written (or always, when toDisk is false).
    const std::string& runDir() const { return runDir_; }
    static bool validLabel(const std::string& l);

private:
    bool ensureDir(std::string* err);
    bool toDisk_;
    std::string url_, runDir_;
    std::vector<std::string> order_;
    std::vector<std::string> html_;
};

}  // namespace lucyn::shell
