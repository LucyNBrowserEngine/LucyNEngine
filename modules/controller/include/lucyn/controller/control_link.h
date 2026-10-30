// lucyn/controller/control_link.h — how scripts find and hook into a running engine.
//
// Every `lucyn` that runs its JSON command loop (headless or headful, not the
// one-shot --download) binds a loopback TCP endpoint and advertises it in
//
//     <nothing-data>/engine.json   {"pid":123,"port":45678,"host":"127.0.0.1"}
//
// The nothing-browser client reads that file BEFORE spawning anything: if an
// engine is already alive, it connects to the port and drives that engine over
// the socket instead of starting a second Chromium (which is what used to end
// in "broken pipe" / zygote errors). The engine's own stdin/stdout keeps
// working too — commands from stdin and from every attached script are merged
// into one loop, and each reply goes back to whoever asked.
#pragma once
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>

namespace lucyn::controller {

// Absolute path of the descriptor file (<dataRoot>/engine.json).
std::string engineDescriptorPath();

struct RunningEngine {
    long pid = 0;
    int port = 0;
};

// The engine advertised in engine.json, but ONLY if it is actually alive:
// the pid must still exist and something must accept connections on the port.
// A stale descriptor from a dead engine yields nullopt.
std::optional<RunningEngine> findRunningEngine();

class ControlLink {
public:
    // Bind 127.0.0.1 (ephemeral port), publish engine.json, start the stdin
    // reader and the accept loop. nullptr when the socket cannot be set up —
    // the caller then falls back to plain stdin/stdout (old behaviour).
    static std::unique_ptr<ControlLink> start();

    // Stops accepting, disconnects clients, removes engine.json. A detached
    // stdin reader may outlive this object; it just finds `stopping` set.
    ~ControlLink();
    ControlLink(const ControlLink&) = delete;
    ControlLink& operator=(const ControlLink&) = delete;

    // The streams to hand to ControllerOptions: input comes from stdin AND from
    // every attached script; output is routed to the channel the command
    // arrived on (0 = stdout).
    std::istream& in();
    std::ostream& out();

    // True while at least one script is attached (listen mode uses this to
    // decide whether unprompted event lines may go to the wire).
    bool hasAttachedClients() const;

    int port() const;

private:
    struct Impl;
    explicit ControlLink(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> p_;
};

}  // namespace lucyn::controller
