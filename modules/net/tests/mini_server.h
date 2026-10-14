// mini_server.h - a tiny scripted HTTP server on loopback, for tests. Header-only.
// Records every request and answers through a handler, so tests can assert on what a client actually SENT.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <lucyn/net/moonbug.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace lucyn::testsupport {
using lucyn::net::HeaderList;

struct Req {
    std::string method, path, body;
    HeaderList headers;
    std::string header(const std::string& name) const {
        for (auto& h : headers) {
            if (h.first.size() != name.size()) continue;
            bool eq = true;
            for (size_t i = 0; i < name.size(); ++i)
                if (std::tolower((unsigned char)h.first[i]) != std::tolower((unsigned char)name[i])) { eq = false; break; }
            if (eq) return h.second;
        }
        return "";
    }
    bool has(const std::string& name) const {
        for (auto& h : headers) {
            std::string a = h.first, b = name;
            std::transform(a.begin(), a.end(), a.begin(), ::tolower);
            std::transform(b.begin(), b.end(), b.begin(), ::tolower);
            if (a == b) return true;
        }
        return false;
    }
};
struct Rep {
    int status = 200;
    std::string body = "ok";
    HeaderList headers;
    int delayMs = 0;
};

class MiniServer {
public:
    explicit MiniServer(std::function<Rep(const Req&)> h) : handler_(std::move(h)) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        ::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::listen(fd_, 16);
        socklen_t len = sizeof(a);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        th_ = std::thread([this] { loop(); });
    }
    ~MiniServer() { stop_ = true; th_.join(); ::close(fd_); }
    std::string url(const std::string& path = "/") const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
    std::vector<Req> log() { std::lock_guard<std::mutex> lk(mu_); return log_; }
    Req last() { auto l = log(); return l.empty() ? Req{} : l.back(); }

private:
    void loop() {
        while (!stop_) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0) continue;
            int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0) continue;
            timeval tv{5, 0};
            ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            serve(c);
            ::close(c);
        }
    }
    void serve(int c) {
        std::string buf;
        char tmp[8192];
        size_t headEnd = std::string::npos;
        while ((headEnd = buf.find("\r\n\r\n")) == std::string::npos) {
            ssize_t n = ::recv(c, tmp, sizeof(tmp), 0);
            if (n <= 0) return;
            buf.append(tmp, static_cast<size_t>(n));
        }
        Req r;
        std::string head = buf.substr(0, headEnd);
        size_t le = head.find("\r\n");
        std::string line = head.substr(0, le);
        size_t s1 = line.find(' '), s2 = line.find(' ', s1 + 1);
        r.method = line.substr(0, s1);
        r.path = line.substr(s1 + 1, s2 - s1 - 1);
        size_t pos = le == std::string::npos ? head.size() : le + 2;
        while (pos < head.size()) {
            size_t e = head.find("\r\n", pos);
            if (e == std::string::npos) e = head.size();
            std::string h = head.substr(pos, e - pos);
            size_t co = h.find(':');
            if (co != std::string::npos) {
                size_t v = co + 1;
                while (v < h.size() && h[v] == ' ') ++v;
                r.headers.emplace_back(h.substr(0, co), h.substr(v));
            }
            pos = e + 2;
        }
        size_t want = static_cast<size_t>(std::atol(r.header("Content-Length").c_str()));
        std::string body = buf.substr(headEnd + 4);
        while (body.size() < want) {
            ssize_t n = ::recv(c, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            body.append(tmp, static_cast<size_t>(n));
        }
        r.body = body.substr(0, want);
        { std::lock_guard<std::mutex> lk(mu_); log_.push_back(r); }

        Rep rep = handler_(r);
        if (rep.delayMs) std::this_thread::sleep_for(std::chrono::milliseconds(rep.delayMs));
        std::string out = "HTTP/1.1 " + std::to_string(rep.status) + (rep.status == 200 ? " OK" : " X") + "\r\n";
        for (auto& h : rep.headers) out += h.first + ": " + h.second + "\r\n";
        out += "Content-Length: " + std::to_string(rep.body.size()) + "\r\nConnection: close\r\n\r\n";
        if (r.method != "HEAD") out += rep.body;
        size_t sent = 0;
        while (sent < out.size()) {
            ssize_t n = ::send(c, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
    }

    std::function<Rep(const Req&)> handler_;
    int fd_ = -1, port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread th_;
    std::mutex mu_;
    std::vector<Req> log_;
};


}  // namespace lucyn::testsupport
