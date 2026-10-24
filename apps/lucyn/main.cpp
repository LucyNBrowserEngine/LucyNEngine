// apps/lucyn/main.cpp — the LucyN headless engine binary.
//
// Usage:
//   lucyn                          # JSON command loop on stdin/stdout
//   lucyn --settle 8000            # JS settle timeout in ms
//   lucyn --console                # echo page console to stderr
//   lucyn --no-adblock             # disable built-in request filtering
//   lucyn --adblock-list <file>    # load additional ABP/uBO rules
//   lucyn --moonbug                # Moonbug on: raw :2005, NULLBYTE-signed :2006, dashboard :6969 (127.0.0.1 only)
//   lucyn --dashboard 8080         # just the dashboard, on a port of your choice (also --raw N, --signed N; 0 = any free port)
//   lucyn --events                 # also stream {"event":"request"...} capture lines on stdout
//   lucyn --download <url>         # ONE-SHOT: save <url> into nothing-data/downloads/, print the saved path, exit 0 (1 on failure)
//        [--name f] [--overwrite] [--max-bytes N] [--referer R]
//   lucyn --version                # print version and exit
//   lucyn --help
//
// The binary reads newline-delimited JSON commands from stdin and writes
// newline-delimited JSON replies to stdout. See controller.h for the full
// command reference.
//
// This is what the nothing-browser JS client spawns as a child process.
// CEF (Chromium Embedded Framework) — the engine from Session 19 on. Only used
// here for the subprocess check; the controller does not talk to CEF yet.
#include <lucyn/cef_host/browser_host.h>
#include <lucyn/controller/controller.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

// ---- one-shot download (--download) --------------------------------------------------------------------------------
// Feeds ONE `download` command (then `quit`) to a normal Controller, so the flag and the NDJSON command can never
// disagree. stdout gets exactly one line on success (the saved path); everything else goes to stderr.
static std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", (unsigned)c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

// Value of a string field in one reply line (handles the escapes our own replies use).
static std::string jsonField(const std::string& line, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    size_t p = line.find(needle);
    if (p == std::string::npos) return {};
    std::string out;
    for (p += needle.size(); p < line.size() && line[p] != '"'; ++p) {
        if (line[p] == '\\' && p + 1 < line.size()) {
            ++p;
            switch (line[p]) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': if (p + 4 < line.size()) { out += (char)std::strtol(line.substr(p + 1, 4).c_str(), nullptr, 16); p += 4; } break;
                default:  out += line[p]; break;
            }
        } else out += line[p];
    }
    return out;
}

static bool jsonStringAt(const std::string& json, size_t& pos, std::string& out) {
    if (pos >= json.size() || json[pos] != '"') return false;
    out.clear();
    for (++pos; pos < json.size(); ++pos) {
        char c = json[pos];
        if (c == '"') { ++pos; return true; }
        if (c != '\\') { out += c; continue; }
        if (++pos >= json.size()) return false;
        switch (json[pos]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            default: return false;
        }
    }
    return false;
}

// The runtime config is JSON, while the API spec remains the C++ embedding surface.
// This executable reads the startup settings it owns today: search.*.
static bool configString(const std::string& json, const std::string& objectName,
                         const std::string& keyName, std::string& value, std::string& error) {
    const size_t keyObject = json.find("\"" + objectName + "\"");
    if (keyObject == std::string::npos) return true;
    const size_t object = json.find('{', keyObject + objectName.size() + 2);
    if (object == std::string::npos) { error = "config field '" + objectName + "' must be an object"; return false; }
    const size_t end = json.find('}', object + 1);
    const size_t key = json.find("\"" + keyName + "\"", object + 1);
    if (key == std::string::npos || (end != std::string::npos && key > end)) return true;
    const size_t colon = json.find(':', key + keyName.size() + 2);
    if (colon == std::string::npos) { error = "config field '" + objectName + "." + keyName + "' is missing ':'"; return false; }
    size_t pos = json.find_first_not_of(" \t\r\n", colon + 1);
    if (pos == std::string::npos || !jsonStringAt(json, pos, value)) {
        error = "config field '" + objectName + "." + keyName + "' must be a string";
        return false;
    }
    return true;
}

static bool loadConfig(const std::string& path, lucyn::controller::ControllerOptions& opts, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "cannot read config file: " + path; return false; }
    std::ostringstream bytes;
    bytes << file.rdbuf();
    const std::string json = bytes.str();

    if (!configString(json, "search", "default", opts.searchDefault, error) ||
        !configString(json, "search", "searxng_url", opts.searxngUrl, error)) return false;
    if (opts.searchDefault.empty()) opts.searchDefault = "brave";
    return true;
}

static int runDownload(lucyn::controller::ControllerOptions opts, const std::string& url, const std::string& name,
                       bool overwrite, long long maxBytes, const std::string& referer) {
    std::string cmd = "{\"v\":1,\"id\":1,\"cmd\":\"download\",\"url\":\"" + jsonEscape(url) + "\"";
    if (!name.empty())    cmd += ",\"name\":\"" + jsonEscape(name) + "\"";
    if (overwrite)        cmd += ",\"overwrite\":true";
    if (maxBytes > 0)     cmd += ",\"maxBytes\":" + std::to_string(maxBytes);
    if (!referer.empty()) cmd += ",\"referer\":\"" + jsonEscape(referer) + "\"";
    cmd += "}\n{\"v\":1,\"id\":2,\"cmd\":\"quit\"}\n";

    std::istringstream in(cmd);
    std::ostringstream out;
    opts.in  = &in;
    opts.out = &out;
    opts.onEvent = [](const std::string& line) { std::fprintf(stderr, "[lucyn] %s\n", line.c_str()); };
    std::fprintf(stderr, "[lucyn] downloading %s\n", url.c_str());
    {
        lucyn::controller::Controller ctrl(opts);
        ctrl.run();
    }

    std::istringstream replies(out.str());
    std::string line;
    while (std::getline(replies, line)) {
        if (line.rfind("{\"id\":1,", 0) != 0) continue;           // skip event lines
        if (line.find("\"ok\":true") != std::string::npos) {
            std::fprintf(stderr, "[lucyn] saved %s\n", line.c_str());
            std::printf("%s\n", jsonField(line, "path").c_str());
            return 0;
        }
        std::fprintf(stderr, "lucyn: download failed: %s\n", jsonField(line, "error").c_str());
        return 1;
    }
    std::fprintf(stderr, "lucyn: download failed: no reply from the engine\n");
    return 1;
}

static void usage(const char* argv0) {
    std::fprintf(stdout,
        "usage: %s [options]\n"
        "\n"
        "options:\n"
        "  --settle  <ms>     JS settle timeout in ms (default: 5000)\n"
        "  --console          echo page console.* to stderr\n"
        "  --no-adblock       disable request filtering (enabled by default)\n"
        "  --adblock-list <f> load additional EasyList/uBO-format rules\n"
        "  --moonbug          start every Moonbug listener on its default port:\n"
        "                     raw stream 2005, NULLBYTE-signed stream 2006, dashboard 6969\n"
        "  --raw <port>       raw capture stream (ws://127.0.0.1:<port>/stream); 0 = any free port\n"
        "  --signed <port>    NULLBYTE-signed capture stream\n"
        "  --dashboard <port> capture dashboard (http://127.0.0.1:<port>/)\n"
        "  --dashboard-file <f>  serve this HTML instead of the built-in dashboard\n"
        "  --events           write a request event per capture to stdout (the nothing-browser client uses this)\n"
        "  --config <file>    read engine config JSON (search.default, search.searxng_url)\n"
        "  --download <url>   one-shot: save <url> into nothing-data/downloads/, print the saved path, exit\n"
        "                     (0 = saved, 1 = failed). Extras: --name <file>  --overwrite  --max-bytes <n>  --referer <url>\n"
        "  --version          print version and exit\n"
        "  --help             print this message and exit\n"
        "\n"
        "reads JSON commands from stdin, writes JSON replies to stdout.\n"
        "see the nothing-browser JS client for a high-level API.\n",
        argv0
    );
}

int main(int argc, char** argv) {
    // CEF is multi-process: Chromium re-executes this binary as a renderer/gpu/
    // utility subprocess. If we ARE a subprocess, hand control straight to CEF
    // and exit with its return code — this check must run before ANYTHING else
    // (no logging, no file I/O, no option parsing) or subprocesses hang/crash.
    // cef_host::executeCefProcess passes our LucynApp so renderer subprocesses get
    // the eval/V8 hook (a raw CefExecuteProcess(nullptr) would leave eval broken).
    // Only when it returns -1 are we the browser process.
    const int cef_subprocess_exit = lucyn::cef_host::executeCefProcess(argc, argv);
    if (cef_subprocess_exit >= 0) return cef_subprocess_exit;

    lucyn::controller::ControllerOptions opts;
    opts.settleTimeoutMs = 5000;
    std::string dlUrl, dlName, dlReferer;
    std::string configPath;
    bool dlOverwrite = false;
    long long dlMax = 0;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            usage(argv[0]);
            return 0;
        } else if (a == "--version") {
            std::puts("lucyn 0.7.0-m7");
            return 0;
        } else if (a == "--settle" && i + 1 < argc) {
            opts.settleTimeoutMs = std::atoi(argv[++i]);
            if (opts.settleTimeoutMs <= 0) opts.settleTimeoutMs = 5000;
        } else if (a == "--console") {
            opts.onConsole = [](const std::string& level, const std::string& line) {
                std::fprintf(stderr, "console.%s: %s\n", level.c_str(), line.c_str());
            };
        } else if (a == "--no-adblock") {
            opts.adblockEnabled = false;
        } else if (a == "--adblock-list" && i + 1 < argc) {
            opts.adblockFilterListPath = argv[++i];
        } else if (a == "--moonbug") {
            if (opts.moonbugRawPort < 0) opts.moonbugRawPort = 2005;
            if (opts.nullbytePort   < 0) opts.nullbytePort   = 2006;
            if (opts.dashboardPort  < 0) opts.dashboardPort  = 6969;
        } else if (a == "--raw" && i + 1 < argc) {
            opts.moonbugRawPort = std::atoi(argv[++i]);
        } else if (a == "--signed" && i + 1 < argc) {
            opts.nullbytePort = std::atoi(argv[++i]);
        } else if (a == "--dashboard" && i + 1 < argc) {
            opts.dashboardPort = std::atoi(argv[++i]);
        } else if (a == "--dashboard-file" && i + 1 < argc) {
            opts.dashboardFile = argv[++i];
        } else if (a == "--events") {
            opts.captureEvents = true;
        } else if (a == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (a == "--download" && i + 1 < argc) {
            dlUrl = argv[++i];
        } else if (a == "--name" && i + 1 < argc) {
            dlName = argv[++i];
        } else if (a == "--overwrite") {
            dlOverwrite = true;
        } else if (a == "--max-bytes" && i + 1 < argc) {
            dlMax = std::atoll(argv[++i]);
        } else if (a == "--referer" && i + 1 < argc) {
            dlReferer = argv[++i];
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            usage(argv[0]);
            return 1;
        }
    }

    // (the --download path runs further below, after initCef; this only catches the
    // download-only flags when no --download <url> was given at all)
    if (dlUrl.empty() && (!dlName.empty() || dlOverwrite || dlMax > 0 || !dlReferer.empty())) {
        std::fprintf(stderr, "--name/--overwrite/--max-bytes/--referer only make sense together with --download <url>\n");
        return 1;
    }

    if (!configPath.empty()) {
        std::string error;
        if (!loadConfig(configPath, opts, error)) {
            std::fprintf(stderr, "lucyn: %s\n", error.c_str());
            return 1;
        }
    }

    // Tell the human where the listeners ended up, on STDERR (stdout is the JSON protocol channel).
    auto prev = opts.onEvent;
    opts.onEvent = [prev](const std::string& line) {
        if (prev) prev(line);
        if (line.rfind("{\"event\":\"moonbug\"", 0) == 0)
            std::fprintf(stderr, "[lucyn] moonbug up: %s\n", line.c_str());
    };
    // Session 19: initialise Chromium BEFORE any command runs — the one-shot
    // `--download` path fetches through Chromium's network stack now too (the
    // subprocess check already ran at the top of main). On failure we keep going;
    // engine commands then report "no browser instance (CEF not initialised)".
    if (!lucyn::cef_host::initCef(argc, argv))
        std::fprintf(stderr, "[lucyn] CEF init failed; engine commands will fail\n");

    // One-shot --download: run that single command, then stop CEF and exit.
    if (!dlUrl.empty()) {
        const int dlRc = runDownload(opts, dlUrl, dlName, dlOverwrite, dlMax, dlReferer);
        lucyn::cef_host::shutdownCef();
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(dlRc);
    }

    int rc = 0;
    {
        lucyn::controller::Controller ctrl(opts);
        rc = ctrl.run();
    }
    // The Controller (and its BrowserInstance) is gone now; stop CEF's UI thread
    // cleanly before the immediate _Exit below.
    lucyn::cef_host::shutdownCef();
    // Flush the protocol channel and leave immediately (skip static destructors; nothing
    // is left to wait for — the engine has no window and no background threads to join).
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(rc);
}
