// lucyn::logger - engine diagnostics without third-party dependencies.
#pragma once

#include <iosfwd>
#include <mutex>
#include <string>

namespace lucyn::logger {

// The threshold follows severity order. SUCCESS and BUILD are informational
// categories. FATAL records a fatal condition but does not terminate the
// process; the owning layer decides how to recover or exit.
enum class Level { debug, info, success, warning, error, fatal, build };
enum class Threshold { debug, info, warning, error, fatal, off };

// Defaults to DEBUG unless NDEBUG is defined, then WARN. Color is enabled only
// for a terminal, and is disabled by NO_COLOR. Output is serialized to stderr.
class Logger {
public:
    explicit Logger(Threshold threshold = defaultThreshold());

    void setThreshold(Threshold threshold);
    Threshold threshold() const;
    bool enabled(Level level) const;
    void log(Level level, const std::string& message);

    void debug(const std::string& message)   { log(Level::debug, message); }
    void info(const std::string& message)    { log(Level::info, message); }
    void success(const std::string& message) { log(Level::success, message); }
    void warn(const std::string& message)    { log(Level::warning, message); }
    void error(const std::string& message)   { log(Level::error, message); }
    void fatal(const std::string& message)   { log(Level::fatal, message); }
    void build(const std::string& message)   { log(Level::build, message); }

    static constexpr Threshold defaultThreshold() {
#ifdef NDEBUG
        return Threshold::warning;
#else
        return Threshold::debug;
#endif
    }

private:
    mutable std::mutex mutex_;
    Threshold threshold_;
};

// Process-wide convenience logger. Applications needing isolated settings can
// construct Logger directly.
Logger& defaultLogger();

}  // namespace lucyn::logger
