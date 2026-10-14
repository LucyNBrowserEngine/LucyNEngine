#include "../include/network_filter.h"
#include <algorithm>
#include <sstream>

namespace adblock {

bool check_is_regex(const std::string_view pattern) {
    for (char c : pattern)
        if (c == '*' || c == '^') return true;
    return false;
}

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, delim))
        parts.push_back(part);
    return parts;
}

static void parse_options(const std::string& options, NetworkFilter& f) {
    bool has_positive_type = false;
    auto set_type = [&](uint32_t bit, bool negated) {
        if (!negated && !has_positive_type) {
            f.mask &= ~FROM_ALL_TYPES;
            has_positive_type = true;
        }
        if (negated) f.mask &= ~bit;
        else f.mask |= bit;
    };
    auto opts = split(options, ',');
    for (auto& opt : opts) {
        bool negated = (!opt.empty() && opt[0] == '~');
        std::string name = negated ? opt.substr(1) : opt;
        std::string value;

        // Split on '='
        auto eq = name.find('=');
        if (eq != std::string::npos) {
            value = name.substr(eq + 1);
            name  = name.substr(0, eq);
        }

        // Content type options
        if      (name == "image")          set_type(FROM_IMAGE, negated);
        else if (name == "media")          set_type(FROM_MEDIA, negated);
        else if (name == "object")         set_type(FROM_OBJECT, negated);
        else if (name == "other")          set_type(FROM_OTHER, negated);
        else if (name == "ping")           set_type(FROM_PING, negated);
        else if (name == "script")         set_type(FROM_SCRIPT, negated);
        else if (name == "stylesheet")     set_type(FROM_STYLESHEET, negated);
        else if (name == "xmlhttprequest") set_type(FROM_XMLHTTPREQUEST, negated);
        else if (name == "font")           set_type(FROM_FONT, negated);
        else if (name == "websocket")      set_type(FROM_WEBSOCKET, negated);
        else if (name == "subdocument")    set_type(FROM_SUBDOCUMENT, negated);
        else if (name == "document")       set_type(FROM_DOCUMENT, negated);

        // Party options
        else if (name == "third-party")    negated ? (f.mask &= ~THIRD_PARTY)
                                                   : (f.mask |= THIRD_PARTY);
        else if (name == "first-party")    negated ? (f.mask &= ~FIRST_PARTY)
                                                   : (f.mask |= FIRST_PARTY);

        // Modifier options
        else if (name == "important")      f.mask |= IS_IMPORTANT;
        else if (name == "match-case")     f.mask |= MATCH_CASE;
        else if (name == "redirect"   ||
                 name == "csp"        ||
                 name == "removeparam")    f.modifier = value;

        // Domain options
        else if (name == "domain" || name == "from") {
            auto domains = split(value, '|');
            for (auto& d : domains) {
                bool neg = (!d.empty() && d[0] == '~');
                std::string domain = neg ? d.substr(1) : d;
                Hash h = sea_hash(domain);
                if (neg) f.opt_not_domains.push_back(h);
                else     f.opt_domains.push_back(h);
            }
        }
    }
}

bool NetworkFilter::parse(const std::string& line, NetworkFilter& f) {
    f.raw_line = line;
    f.mask = DEFAULT_MASK;

    if (line.empty()) return false;

    // Skip comments
    if (line[0] == '!' || line[0] == '#' ||
        line.substr(0, 9) == "[Adblock") return false;

    std::string rule = line;

    // Check for exception prefix @@
    if (rule.size() >= 2 && rule[0] == '@' && rule[1] == '@') {
        f.mask |= IS_EXCEPTION;
        rule = rule.substr(2);
    }

    // Find last $ for options (search in reverse)
    size_t dollar = std::string::npos;
    for (int i = (int)rule.size() - 1; i >= 0; i--) {
        if (rule[i] == '$') { dollar = i; break; }
    }

    std::string options;
    if (dollar != std::string::npos) {
        options = rule.substr(dollar + 1);
        rule    = rule.substr(0, dollar);
    }

    // Check for hostname anchor ||
    if (rule.size() >= 2 && rule[0] == '|' && rule[1] == '|') {
        f.mask |= IS_HOSTNAME_ANCHOR;
        rule = rule.substr(2);

        // Extract hostname (up to first /, ^, or *)
        size_t split_pos = rule.find_first_of("/^*");
        if (split_pos == std::string::npos) {
            f.hostname = rule;
            rule = "";
        } else {
            f.hostname = rule.substr(0, split_pos);
            rule = rule.substr(split_pos);
        }

        // Check for wildcard in hostname
        if (f.hostname.find('*') != std::string::npos)
            f.mask |= IS_HOSTNAME_REGEX;
    }
    // Left anchor |
    else if (!rule.empty() && rule[0] == '|') {
        f.mask |= IS_LEFT_ANCHOR;
        rule = rule.substr(1);
    }

    // Right anchor
    if (!rule.empty() && rule.back() == '|') {
        f.mask |= IS_RIGHT_ANCHOR;
        rule.pop_back();
    }

    // Strip leading/trailing wildcards
    if (!rule.empty() && rule.back() == '*') rule.pop_back();
    if (!rule.empty() && rule[0] == '*') {
        rule = rule.substr(1);
        f.mask &= ~IS_LEFT_ANCHOR;
    }

    // Check if pattern needs regex
    if (check_is_regex(rule)) f.mask |= IS_REGEX;

    f.pattern = rule;

    // Parse options
    if (!options.empty()) parse_options(options, f);

    return true;
}

} // namespace adblock 
