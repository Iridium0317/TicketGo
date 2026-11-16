// =============================================================================
// latency_client.cc  –  closed-loop latency / throughput client (Linux, epoll)
//
// Standalone: does not touch the server or the other benchmarks.
//
// N connections are spread over T threads.  Every connection first does one
// handshake query (a connection counts as established only once the server has
// answered it), then all threads start together and each connection repeatedly
// does: send one query -> wait for the full reply -> record latency -> repeat.
// Latency is measured from just before send() to the moment the last byte of
// the reply has been read, with steady_clock, in C++ (no interpreter overhead).
//
// Because the loop is closed, with N connections always in flight the latency
// is mostly queueing time (Little's law: N / throughput), not service time.
// Run with a small --conns to see latency at low load.
//
// Build:  g++ -std=c++17 -O3 -pthread benchmark/latency_client.cc -o latency_client
// Run  :  ./latency_client --conns 1000 --threads 4 --seconds 10   (server on :8888)
// =============================================================================
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

// Station counts of the default fleet in server_main.cc::createTrains().
static const int kTrainStations[] = {11, 13, 15, 12};

// A query reply is exactly three '\n'-terminated lines: SEP, text, SEP.
static const char* kRemaining = "\xE5\x89\xA9\xE4\xBD\x99";  // "剩余"
static bool reply_complete(const std::string& b) {
    return std::count(b.begin(), b.end(), '\n') >= 3 && b.back() == '\n';
}
static bool reply_valid(const std::string& b) {
    return b.find(kRemaining) != std::string::npos;
}

static std::string random_query(std::mt19937& rng) {
    const int train = static_cast<int>(rng() % 4);
    const int n = kTrainStations[train];
    const int frm = static_cast<int>(rng() % static_cast<unsigned>(n - 2));
    const int to = frm + 1 + static_cast<int>(rng() % static_cast<unsigned>(n - 2 - frm));
    return "1 " + std::to_string(train) + " " + std::to_string(frm) + " " +
           std::to_string(to) + " 0\n";
}

struct Conn {
    int fd = -1;
    std::string buf;
    std::string req;
    Clock::time_point sent;
};

struct ThreadResult {
    int established = 0;
    long long failed = 0;
    std::vector<double> lat_us;
};

static std::atomic<int> g_ready{0};
static std::atomic<bool> g_go{false};
static std::atomic<bool> g_stop{false};

static void worker(int tid, int nconns, double warmup_s, ThreadResult* out) {
    std::mt19937 rng(static_cast<unsigned>(12345 + tid));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8888);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    std::vector<Conn> conns;
    // ── Phase 1: connect + handshake query (blocking) ───────────────────────
    for (int i = 0; i < nconns; ++i) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { ++out->failed; continue; }
        timeval tv{10, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd); ++out->failed; continue;
        }
        const int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        const std::string q = random_query(rng);
        std::string reply;
        char tmp[4096];
        bool ok = ::send(fd, q.data(), q.size(), 0) == static_cast<ssize_t>(q.size());
        while (ok && !reply_complete(reply)) {
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) { ok = false; break; }
            reply.append(tmp, static_cast<size_t>(n));
        }
        if (!ok || !reply_valid(reply)) { ::close(fd); ++out->failed; continue; }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        Conn c; c.fd = fd;
        conns.push_back(std::move(c));
    }
    out->established = static_cast<int>(conns.size());

    int ep = epoll_create1(0);
    for (size_t i = 0; i < conns.size(); ++i) {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.u32 = static_cast<uint32_t>(i);
        epoll_ctl(ep, EPOLL_CTL_ADD, conns[i].fd, &ev);
    }

    g_ready.fetch_add(1);
    while (!g_go.load()) std::this_thread::yield();

    const auto t_start = Clock::now();
    const auto t_measure = t_start + std::chrono::duration_cast<Clock::duration>(
                                         std::chrono::duration<double>(warmup_s));
    auto send_next = [&](Conn& c) {
        c.req = random_query(rng);
        c.buf.clear();
        c.sent = Clock::now();
        if (::send(c.fd, c.req.data(), c.req.size(), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(c.req.size())) {
            ++out->failed; ::close(c.fd); c.fd = -1;
        }
    };
    for (auto& c : conns) send_next(c);

    std::vector<epoll_event> evs(256);
    size_t alive = conns.size();
    char tmp[4096];
    while (alive > 0 && !g_stop.load()) {
        int n = epoll_wait(ep, evs.data(), static_cast<int>(evs.size()), 100);
        for (int k = 0; k < n; ++k) {
            Conn& c = conns[evs[static_cast<size_t>(k)].data.u32];
            if (c.fd < 0) continue;
            ssize_t r = ::recv(c.fd, tmp, sizeof(tmp), 0);
            if (r <= 0) {
                if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                ++out->failed; ::close(c.fd); c.fd = -1; --alive; continue;
            }
            c.buf.append(tmp, static_cast<size_t>(r));
            if (!reply_complete(c.buf)) continue;
            const auto now = Clock::now();
            if (!reply_valid(c.buf)) {
                ++out->failed; ::close(c.fd); c.fd = -1; --alive; continue;
            }
            if (c.sent >= t_measure)
                out->lat_us.push_back(
                    std::chrono::duration<double, std::micro>(now - c.sent).count());
            send_next(c);
            if (c.fd < 0) --alive;
        }
    }
    for (auto& c : conns) if (c.fd >= 0) ::close(c.fd);
    ::close(ep);
}

static double pct(const std::vector<double>& v, double p) {
    if (v.empty()) return 0;
    size_t idx = static_cast<size_t>(std::ceil(p / 100.0 * static_cast<double>(v.size())));
    if (idx > 0) --idx;
    return v[std::min(idx, v.size() - 1)];
}

int main(int argc, char** argv) {
    int conns = 1000, threads = 4;
    double seconds = 10, warmup = 2;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--conns")) conns = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--threads")) threads = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--seconds")) seconds = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--warmup")) warmup = atof(argv[i + 1]);
    }
    if (threads > conns) threads = conns;

    std::vector<ThreadResult> res(static_cast<size_t>(threads));
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        const int share = conns / threads + (t < conns % threads);
        ts.emplace_back(worker, t, share, warmup, &res[static_cast<size_t>(t)]);
    }
    while (g_ready.load() < threads) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto t0 = Clock::now();
    g_go.store(true);
    std::this_thread::sleep_for(std::chrono::duration<double>(warmup + seconds));
    g_stop.store(true);
    const auto t1 = Clock::now();
    for (auto& t : ts) t.join();

    std::vector<double> lat;
    int established = 0;
    long long failed = 0;
    for (auto& r : res) {
        established += r.established;
        failed += r.failed;
        lat.insert(lat.end(), r.lat_us.begin(), r.lat_us.end());
    }
    std::sort(lat.begin(), lat.end());
    const double measured = std::chrono::duration<double>(t1 - t0).count() - warmup;
    double mean = 0;
    for (double x : lat) mean += x;
    if (!lat.empty()) mean /= static_cast<double>(lat.size());

    std::printf("conns attempted=%d established=%d  threads=%d  warmup=%.1fs measured=%.1fs\n",
                conns, established, threads, warmup, measured);
    std::printf("requests ok=%zu failed=%lld  ->  %.0f req/s\n", lat.size(), failed,
                static_cast<double>(lat.size()) / measured);
    std::printf("latency us  mean=%.0f  p50=%.0f  p95=%.0f  p99=%.0f  p99.9=%.0f  max=%.0f\n",
                mean, pct(lat, 50), pct(lat, 95), pct(lat, 99), pct(lat, 99.9),
                lat.empty() ? 0.0 : lat.back());
    return 0;
}
