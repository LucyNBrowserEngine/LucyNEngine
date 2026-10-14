// Checks that the net module and the profile Switcher stay consistent.
// Passes on BOTH build flavours (stock libcurl, and libcurl-impersonate).
#include <cstdio>
#include <cstring>
#include <thread>
#include <lucyn/net/http_client.h>

#if defined(__unix__) || defined(__APPLE__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace lucyn;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

#if defined(__unix__) || defined(__APPLE__)
static void testConcurrentGets(net::HttpClient& client) {
    int server = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(server >= 0);
    if (server < 0) return;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    CHECK(::listen(server, 2) == 0);
    socklen_t addrLen = sizeof(addr);
    CHECK(::getsockname(server, reinterpret_cast<sockaddr*>(&addr), &addrLen) == 0);
    const std::string url = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port)) + "/";

    std::thread serverThread([server] {
        const char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-Test: yes\r\nConnection: close\r\n\r\nok";
        for (int i = 0; i < 2; ++i) {
            int peer = ::accept(server, nullptr, nullptr);
            if (peer < 0) break;
            char request[2048];
            (void)::recv(peer, request, sizeof(request), 0);
            (void)::send(peer, response, sizeof(response) - 1, 0);
            ::close(peer);
        }
        ::close(server);
    });

    net::Response first, second;
    std::thread a([&] { first = client.get(url + "first"); });
    std::thread b([&] { second = client.get(url + "second"); });
    a.join();
    b.join();
    serverThread.join();

    CHECK(first.ok() && first.status == 200 && first.body == "ok");
    CHECK(second.ok() && second.status == 200 && second.body == "ok");
    auto hasTestHeader = [](const net::Response& r) {
        for (const auto& h : r.headers)
            if (h.first == "X-Test" && h.second == "yes") return true;
        return false;
    };
    CHECK(hasTestHeader(first));
    CHECK(hasTestHeader(second));
}
#endif

int main() {
    profile::Switcher sw("chrome");
    {
        net::HttpClient client(sw);
        std::string err;

        bool ok = sw.select("safari184", &err);
        if (net::HttpClient::impersonationBuiltIn()) {
            CHECK(ok);
            CHECK(client.impersonating());
            CHECK(sw.current().target == "safari184");
        } else {
            CHECK(!ok);                                      // stock curl must refuse, not pretend
            CHECK(!err.empty());
            CHECK(!client.impersonating());
            CHECK(sw.current().target == "chrome146");       // identity unchanged
        }

        std::string before = sw.current().target;
        CHECK(!sw.select("definitely_not_a_browser", &err)); // the library says no, in both flavours
        CHECK(sw.current().target == before);

    #if defined(__unix__) || defined(__APPLE__)
        // Concurrent fetches used to share one easy handle while libcurl still
        // retained pointers into another call's stack-local Response.
        testConcurrentGets(client);
    #endif
    }
    // client is gone: its validator must be gone too
    std::string err;
    CHECK(sw.select("firefox133", &err));

    std::puts(failures ? "FAILED" : "net module: all tests passed");
    return failures ? 1 : 0;
}
