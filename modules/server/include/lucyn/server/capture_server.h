// lucyn/server/capture_server.h - serves Moonbug's captures: raw stream, NULLBYTE-signed stream, dashboard.
//
//   Website traffic -> Moonbug (captures) -> NULLBYTE (signs) -> TinyElysia (serves) -> Dashboard
//
// Three independent listeners, each optional, each on 127.0.0.1 only:
//
//   activate(2005)          raw stream     ws  /stream     one JSON capture per frame
//                                          GET /captures?limit=N   history, JSON array
//   alsoUseNullbyte(2006)   signed stream  ws  /stream     one NULLBYTE envelope per frame
//                                          GET /captures?limit=N   history, array of envelopes
//                                          POST /verify    body = an envelope -> {"ok":..,"error":..}
//   serveDashboard(6969)    dashboard      GET /           home_dashboard.html (embedded in the binary)
//                                          ws  /stream     envelopes (the page verifies seals through /api/verify)
//                                          GET /api/captures?limit=N, POST /api/verify, GET /api/status
//
// Mirrors the README: engine.moonbug.activate(2005); engine.moonbug.alsouse.nullbyte(2006);
// engine.moonbug.expose("home_dashboard.html").
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace lucyn::net      { class Moonbug; }
namespace lucyn::security { class Nullbyte; }

namespace lucyn::server {

class CaptureServer {
public:
    // `mb` and `nb` must outlive the server.
    CaptureServer(net::Moonbug& mb, const security::Nullbyte& nb);
    ~CaptureServer();
    CaptureServer(const CaptureServer&) = delete;
    CaptureServer& operator=(const CaptureServer&) = delete;

    // port 0 = any free port (read it back with the *Port() getters). False + *err when the port is taken.
    bool activate(std::uint16_t port, std::string* err = nullptr);          // raw stream
    bool alsoUseNullbyte(std::uint16_t port, std::string* err = nullptr);   // signed stream
    bool serveDashboard(std::uint16_t port, std::string* err = nullptr);    // dashboard + its API

    // Serve this HTML file instead of the embedded dashboard (re-read on every request, so you can edit it
    // live). Falls back to the embedded page, with a header saying so, if the file is unreadable.
    void expose(const std::string& htmlPath);

    std::uint16_t rawPort() const;         // 0 = not running
    std::uint16_t signedPort() const;
    std::uint16_t dashboardPort() const;

    void stop();

    static std::string embeddedDashboard();
    static constexpr const char* kVersion = "0.5.0-m5";

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace lucyn::server
