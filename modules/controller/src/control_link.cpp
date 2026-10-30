// modules/controller/src/control_link.cpp — see control_link.h for the idea.
//
// One engine, many drivers: the engine keeps its JSON loop, but the input is a
// queue fed by (a) its own stdin and (b) every script that connects to the
// loopback endpoint advertised in <nothing-data>/engine.json. Replies are
// routed back to the channel the command arrived on, so a script attached to a
// running engine gets its own private answer stream while the terminal keeps
// working at the same time.
#include <lucyn/controller/control_link.h>

#include <lucyn/core/paths.h>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <streambuf>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#define LUCYN_SOCK SOCKET
#define LUCYN_BAD_SOCK INVALID_SOCKET
#define LUCYN_SHUT_RDWR SD_BOTH
inline int lucynCloseSock(SOCKET s) { return closesocket(s); }
inline int lucynSockErr() { return WSAGetLastError(); }
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#define LUCYN_SOCK int
#define LUCYN_BAD_SOCK (-1)
#define LUCYN_SHUT_RDWR SHUT_RDWR
inline int lucynCloseSock(int s) { return ::close(s); }
inline int lucynSockErr() { return errno; }
#endif

// poll(), not select(): select() waiting for a WRITABLE fd (connect completion)
// demonstrably never fires on some sandboxes — loopback connect + select timed
// out for 5s while poll() reported POLLOUT in under a millisecond, same socket.
// poll() works everywhere, including WSAPoll on Windows.
#ifdef _WIN32
using LucynPollFd = WSAPOLLFD;
inline int lucynPoll(LucynPollFd* fds, int n, int timeoutMs) {
    return WSAPoll(fds, static_cast<ULONG>(n), timeoutMs);
}
#else
using LucynPollFd = struct pollfd;
inline int lucynPoll(LucynPollFd* fds, int n, int timeoutMs) {
    return ::poll(fds, static_cast<nfds_t>(n), timeoutMs);
}
#endif

namespace lucyn::controller {
namespace {

void winsockInit() {
#ifdef _WIN32
    static const bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)ok;
#endif
}

long currentPid() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

// Is the process with this pid still with us?
bool pidAlive(long pid) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    CloseHandle(h);
    return true;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

// Does something accept TCP connections on 127.0.0.1:port right now?
bool portIsAlive(int port, int timeoutMs) {
    winsockInit();
    LUCYN_SOCK s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == LUCYN_BAD_SOCK) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    // Non-blocking connect + select = a bounded timeout on every platform.
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = ::fcntl(s, F_GETFL, 0);
    ::fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif

    bool ok = false;
    const bool dbg = std::getenv("LUCYN_DEBUG_ATTACH") != nullptr;
    const int crc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    const int cErrno = errno;
    if (crc == 0) {
        ok = true;
    } else {
        LucynPollFd pfd{};
        pfd.fd = s;
        pfd.events = POLLOUT;
        const int rc = lucynPoll(&pfd, 1, timeoutMs);
        const int pErrno = errno;
        int err = 0;
#ifdef _WIN32
        int len = sizeof(err);
#else
        socklen_t len = sizeof(err);
#endif
        const int gsr = ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        ok = rc > 0 && (pfd.revents & POLLOUT) != 0 && gsr == 0 && err == 0;
        if (dbg)
            std::fprintf(stderr,
                         "[attach-dbg] port %d: connect=%d errno=%d, poll=%d errno=%d revents=0x%x, "
                         "getsockopt=%d so_error=%d\n",
                         port, crc, cErrno, rc, pErrno, pfd.revents, gsr, err);
    }
    lucynCloseSock(s);
    return ok;
}

// Naive "\"key\":<number>" reader — enough for our own descriptor file.
bool jsonNumber(const std::string& json, const char* key, long& out) {
    const std::string needle = std::string("\"") + key + "\":";
    size_t p = json.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p >= json.size()) return false;
    char* end = nullptr;
    const long v = std::strtol(json.c_str() + p, &end, 10);
    if (end == json.c_str() + p) return false;
    out = v;
    return true;
}

// Write one full buffer to a socket; SIGPIPE-proof on Linux (MSG_NOSIGNAL),
// on macOS the socket got SO_NOSIGPIPE at accept time.
bool sendAll(LUCYN_SOCK s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        auto n = ::send(s, data.data() + off, static_cast<int>(data.size() - off), flags);
        if (n <= 0) {
            if (n < 0) {
#ifdef _WIN32
                if (WSAGetLastError() == WSAEINTR) continue;
#else
                if (errno == EINTR) continue;
#endif
            }
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Descriptor
// ---------------------------------------------------------------------------

std::string engineDescriptorPath() {
    const std::string root = core::dataRoot();
    if (root.empty()) return {};
    return root + "/engine.json";
}

std::optional<RunningEngine> findRunningEngine() {
    const std::string path = engineDescriptorPath();
    const bool dbg = std::getenv("LUCYN_DEBUG_ATTACH") != nullptr;
    if (path.empty()) {
        if (dbg) std::fprintf(stderr, "[attach-dbg] no descriptor path\n");
        return std::nullopt;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (dbg) std::fprintf(stderr, "[attach-dbg] no descriptor at %s\n", path.c_str());
        return std::nullopt;
    }
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    long pid = 0, port = 0;
    if (!jsonNumber(json, "pid", pid) || !jsonNumber(json, "port", port)) {
        if (dbg) std::fprintf(stderr, "[attach-dbg] unparseable descriptor: %s\n", json.c_str());
        return std::nullopt;
    }
    if (port <= 0 || port > 65535) return std::nullopt;
    // Both checks matter: a crashed engine leaves a stale file whose port some
    // other process may have picked up, and a live pid on a dead port happens
    // after a hard kill. Only "pid alive AND port accepting" = a real engine.
    if (!pidAlive(pid)) {
        if (dbg) std::fprintf(stderr, "[attach-dbg] pid %ld is not alive\n", pid);
        return std::nullopt;
    }
    if (!portIsAlive(static_cast<int>(port), 750)) {
        if (dbg) std::fprintf(stderr, "[attach-dbg] port %ld is not accepting\n", port);
        return std::nullopt;
    }
    if (dbg) std::fprintf(stderr, "[attach-dbg] found engine pid=%ld port=%ld\n", pid, port);
    return RunningEngine{pid, static_cast<int>(port)};
}

// ---------------------------------------------------------------------------
// ControlLink::Impl
// ---------------------------------------------------------------------------

struct ControlLink::Impl : std::enable_shared_from_this<ControlLink::Impl> {
    using traits = std::char_traits<char>;

    // A channel id: 0 = the engine's own stdin/stdout, otherwise the socket
    // (intptr_t because a Windows SOCKET is not an int).
    using Chan = std::intptr_t;

    struct Item {
        Chan src = 0;
        bool eof = false;
        std::string line;
    };

    // ---- shared queue + routing state (all guarded by mu) ----
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Item> q;
    std::set<LUCYN_SOCK> allFds;  // every accepted socket; closed exactly once by the dtor
    std::set<LUCYN_SOCK> active;  // still connected
    bool stopping = false;
    Chan target = 0;  // channel the next reply goes to: 0 = stdout, else the socket

    LUCYN_SOCK listenFd = LUCYN_BAD_SOCK;
    int portNo = 0;
    std::string descPath;

    std::thread listener;
    std::vector<std::thread> readers;
    std::mutex readersMu;

    // ---- streams handed to the Controller ----
    struct InBuf : std::streambuf {
        std::shared_ptr<Impl> self;
        std::string cur;
        explicit InBuf(std::shared_ptr<Impl> s) : self(std::move(s)) {}
        int_type underflow() override {
            for (;;) {
                Item it;
                {
                    std::unique_lock<std::mutex> lk(self->mu);
                    self->cv.wait(lk, [&] { return self->stopping || !self->q.empty(); });
                    if (self->stopping) return traits::eof();
                    if (self->q.empty()) continue;
                    it = std::move(self->q.front());
                    self->q.pop_front();
                }
                if (it.eof) {
                    // stdin closed. A headful engine keeps serving attached
                    // scripts; only with nobody attached does the loop end.
                    std::lock_guard<std::mutex> lk(self->mu);
                    if (self->stopping) return traits::eof();
                    if (self->active.empty()) return traits::eof();
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lk(self->mu);
                    self->target = it.src;
                }

                cur = std::move(it.line);
                cur.push_back('\n');
                setg(cur.data(), cur.data(), cur.data() + cur.size());
                return traits::to_int_type(cur[0]);
            }
        }
    };

    struct OutBuf : std::streambuf {
        std::shared_ptr<Impl> self;
        std::ostream* realOut;  // the engine's stdout
        std::string buf;
        OutBuf(std::shared_ptr<Impl> s, std::ostream* out) : self(std::move(s)), realOut(out) {}

        int_type overflow(int_type ch) override {
            if (traits::eq_int_type(ch, traits::eof())) return traits::not_eof(ch);
            buf.push_back(traits::to_char_type(ch));
            return ch;
        }
        std::streamsize xsputn(const char* p, std::streamsize n) override {
            buf.append(p, static_cast<size_t>(n));
            return n;
        }
        int sync() override { flush(); return 0; }

        void flush() {
            if (buf.empty()) return;
            const std::string data = std::exchange(buf, std::string{});
            Chan t;
            {
                std::lock_guard<std::mutex> lk(self->mu);
                if (self->stopping) return;
                t = self->target;
            }
            if (t == 0) {
                realOut->write(data.data(), static_cast<std::streamsize>(data.size()));
                realOut->flush();
                return;
            }
            if (!sendAll(static_cast<LUCYN_SOCK>(t), data)) {
                // Peer is gone mid-write: stop routing to it, drop the rest.
                std::lock_guard<std::mutex> lk(self->mu);
                self->active.erase(static_cast<LUCYN_SOCK>(t));
                if (self->target == t) self->target = 0;
            }

        }
    };

    std::unique_ptr<InBuf> inBuf;
    std::unique_ptr<OutBuf> outBuf;
    std::unique_ptr<std::istream> inStream;
    std::unique_ptr<std::ostream> outStream;

    bool init(std::ostream* realOut) {
        winsockInit();
        if (!bindListen()) return false;
        if (!publish()) {
            lucynCloseSock(listenFd);
            listenFd = LUCYN_BAD_SOCK;
            return false;
        }
        inBuf = std::make_unique<InBuf>(shared_from_this());
        outBuf = std::make_unique<OutBuf>(shared_from_this(), realOut);
        inStream = std::make_unique<std::istream>(inBuf.get());
        outStream = std::make_unique<std::ostream>(outBuf.get());

        listener = std::thread([self = shared_from_this()] { self->listenLoop(); });
        // Detached on purpose: getline(stdin) cannot be woken up, so this one
        // thread may outlive the ControlLink. It only ever pushes into the
        // (shared) queue, sees `stopping` and leaves. main() exits right after.
        std::thread([self = shared_from_this()] { self->stdinLoop(); }).detach();
        return true;
    }

    bool bindListen() {
        listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd == LUCYN_BAD_SOCK) return false;
        int yes = 1;
        ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;  // ephemeral — the port goes into engine.json
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 ONLY
        if (::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            lucynCloseSock(listenFd);
            listenFd = LUCYN_BAD_SOCK;
            return false;
        }
        if (::listen(listenFd, 4) != 0) {
            lucynCloseSock(listenFd);
            listenFd = LUCYN_BAD_SOCK;
            return false;
        }
        sockaddr_in got{};
#ifdef _WIN32
        int len = sizeof(got);
#else
        socklen_t len = sizeof(got);
#endif
        if (::getsockname(listenFd, reinterpret_cast<sockaddr*>(&got), &len) == 0)
            portNo = ntohs(got.sin_port);
        return portNo > 0;
    }

    bool publish() {
        const std::string root = core::dataRoot();
        if (root.empty()) return false;
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        descPath = root + "/engine.json";
        std::ofstream f(descPath, std::ios::trunc);
        if (!f) return false;
        f << "{\"pid\":" << currentPid() << ",\"port\":" << portNo << ",\"host\":\"127.0.0.1\"}\n";
        return static_cast<bool>(f);
    }

    void listenLoop() {
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mu);
                if (stopping) return;
            }
            LucynPollFd pfd{};
            pfd.fd = listenFd;
            pfd.events = POLLIN;
            const int rc = lucynPoll(&pfd, 1, 200);   // wake ~5x/s to re-check `stopping`
            if (rc <= 0) continue;
            LUCYN_SOCK c = ::accept(listenFd, nullptr, nullptr);
            if (c == LUCYN_BAD_SOCK) continue;
#ifdef SO_NOSIGPIPE
            int on = 1;
            ::setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&on), sizeof(on));
#endif
            {
                std::lock_guard<std::mutex> lk(mu);
                if (stopping) {
                    lucynCloseSock(c);
                    return;
                }
                allFds.insert(c);
                active.insert(c);
            }
            std::lock_guard<std::mutex> rl(readersMu);
            readers.emplace_back([self = shared_from_this(), c] { self->clientLoop(c); });
        }
    }

    void clientLoop(LUCYN_SOCK c) {
        char buf[4096];
        std::string pending;
        for (;;) {
            const int n = ::recv(c, buf, static_cast<int>(sizeof(buf)), 0);
            if (n <= 0) break;
            pending.append(buf, static_cast<size_t>(n));
            size_t p;
            while ((p = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, p);
                pending.erase(0, p + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    if (stopping) return;
                    q.push_back(Item{static_cast<Chan>(c), false, std::move(line)});
                }
                cv.notify_one();
            }
        }
        // Peer closed: stop routing replies there, but keep the fd for the dtor.
        {
            std::lock_guard<std::mutex> lk(mu);
            active.erase(c);
            if (target == static_cast<Chan>(c)) target = 0;
        }
        cv.notify_all();
    }

    void stdinLoop() {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            {
                std::lock_guard<std::mutex> lk(mu);
                if (stopping) return;
                q.push_back(Item{0, false, std::move(line)});
            }
            cv.notify_one();
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            if (stopping) return;
            q.push_back(Item{0, true, std::string{}});
        }
        cv.notify_one();
    }

    void teardown() {
        std::vector<LUCYN_SOCK> toShutdown;
        {
            std::lock_guard<std::mutex> lk(mu);
            stopping = true;
            toShutdown.assign(active.begin(), active.end());
        }
        cv.notify_all();
        for (LUCYN_SOCK fd : toShutdown) ::shutdown(fd, LUCYN_SHUT_RDWR);
        if (listener.joinable()) listener.join();
        {
            std::lock_guard<std::mutex> rl(readersMu);
            for (std::thread& t : readers)
                if (t.joinable()) t.join();
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            for (LUCYN_SOCK fd : allFds) lucynCloseSock(fd);
            allFds.clear();
            active.clear();
        }
        if (listenFd != LUCYN_BAD_SOCK) {
            lucynCloseSock(listenFd);
            listenFd = LUCYN_BAD_SOCK;
        }
        if (!descPath.empty()) {
            std::error_code ec;
            std::filesystem::remove(descPath, ec);
            descPath.clear();
        }
    }
};

// ---------------------------------------------------------------------------
// ControlLink
// ---------------------------------------------------------------------------

ControlLink::ControlLink(std::shared_ptr<Impl> impl) : p_(std::move(impl)) {}

ControlLink::~ControlLink() {
    if (p_) p_->teardown();
}

std::unique_ptr<ControlLink> ControlLink::start() {
    auto impl = std::make_shared<Impl>();
    if (!impl->init(&std::cout)) return nullptr;
    return std::unique_ptr<ControlLink>(new ControlLink(std::move(impl)));
}

std::istream& ControlLink::in() { return *p_->inStream; }
std::ostream& ControlLink::out() { return *p_->outStream; }

bool ControlLink::hasAttachedClients() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return !p_->active.empty();
}

int ControlLink::port() const { return p_->portNo; }

}  // namespace lucyn::controller
