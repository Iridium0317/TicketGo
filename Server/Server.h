#pragma once
// =============================================================================
// Server.h  –  Modernised C++17
//
// Architecture overview
// ─────────────────────
//   Main thread
//     └─ epoll event loop (EPOLLET edge-triggered, O(1) dispatch)
//          ├─ New connection  → BitmapAllocator::allocate() → EPOLL_CTL_ADD
//          └─ Data ready      → parse, build Task, push to work queue
//
//   Worker threads  ×  WORKER_THREAD_COUNT
//     └─ Pop Task from queue → doTask() → send() reply to fd
//
// Key design decisions
// ─────────────────────
//  * select()         → epoll (Linux scalable I/O multiplexing)
//  * client_socket_[30] fixed array + O(n) scan
//                     → two-level BitmapAllocator (65,536 slots) giving
//                        near-O(1) alloc/free via __builtin_ctzll
//  * 5 detached threads sharing same mutex (no .join on shutdown)
//                     → WORKER_THREAD_COUNT joinable threads with a
//                        stop_flag_ for clean shutdown
//  * All sockets set O_NONBLOCK so the event loop never blocks on I/O
//
// =============================================================================
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <deque>
#include <vector>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include "../Entity/Task.h"
#include "../Entity/Train.h"
#include "../Entity/Ticket.h"

// ── Compile-time constants (replaces #define) ─────────────────────────────
inline constexpr int    SERVER_PORT          = 8888;
inline constexpr int    MAX_BUFFER           = 2048;
inline constexpr int    MAX_EVENTS           = 64;    // epoll_wait batch size
inline constexpr int    WORKER_THREAD_COUNT  = 4;
inline constexpr int    LISTEN_BACKLOG       = 128;

// ── Bitmap Session Allocator ──────────────────────────────────────────────
// Manages up to CAPACITY concurrent client slots with a two-level bitmap:
//   words_[w]   bit b set  ⇔  slot w*64+b is free
//   summary_[s] bit b set  ⇔  words_[s*64+b] has at least one free slot
// allocate(): scan ≤ kSummaryWords summary words, then two __builtin_ctzll
//             calls → constant work independent of how many slots are used.
// free():     O(1) bitwise OR on both levels.
//
// The class is NOT thread-safe on its own; callers must hold a lock.
class BitmapAllocator {
public:
    static constexpr int CAPACITY = 65536;

    BitmapAllocator() noexcept {
        words_.fill(~uint64_t{0});
        summary_.fill(~uint64_t{0});
    }

    // Returns slot index [0, CAPACITY) or -1 if full.
    int allocate() noexcept {
        for (int s = 0; s < kSummaryWords; ++s) {
            if (summary_[s] == 0) continue;
            const int w   = s * 64 + __builtin_ctzll(summary_[s]);
            const int bit = __builtin_ctzll(words_[w]);   // index of lowest 1-bit
            words_[w] &= ~(uint64_t{1} << bit);           // mark as used
            if (words_[w] == 0)                           // word now full
                summary_[s] &= ~(uint64_t{1} << (w % 64));
            return w * 64 + bit;
        }
        return -1;
    }

    // Marks slot as free.  Undefined behaviour if slot was not allocated.
    void free(int slot) noexcept {
        const int w = slot / 64;
        words_[w]        |= (uint64_t{1} << (slot % 64));
        summary_[w / 64] |= (uint64_t{1} << (w % 64));
    }

    bool full() const noexcept {
        for (uint64_t s : summary_) if (s != 0) return false;
        return true;
    }

private:
    static constexpr int kWords        = CAPACITY / 64;   // 1024
    static constexpr int kSummaryWords = kWords / 64;     // 16

    std::array<uint64_t, kWords>        words_;
    std::array<uint64_t, kSummaryWords> summary_;
};

// ── Server ────────────────────────────────────────────────────────────────
class Server {
public:
    explicit Server(std::deque<Train> trains);
    ~Server();

    // Returns 0 on success, non-zero on fatal error.
    int initServer();

    // Enters the epoll event loop.  Blocks until stop() is called.
    void run();

    // Signals the event loop and worker threads to stop.
    void stop();

private:
    // ── Network helpers ──────────────────────────────────────────────────
    // Sets fd to O_NONBLOCK; returns false on error.
    static bool setNonBlocking(int fd) noexcept;

    // Registers fd with epoll for EPOLLIN | EPOLLET.
    bool epollAdd(int fd) noexcept;

    // Removes fd from epoll and closes it.
    void epollRemove(int fd) noexcept;

    // Called from the event loop when the listen socket fires.
    void handleNewConnection();

    // Called from the event loop when a client socket has data.
    // Returns false if the connection should be dropped.
    bool handleClientData(int fd);

    // ── Worker-pool helpers ──────────────────────────────────────────────
    void workerLoop();

    void pushTask(int sd, std::string content);

    // ── Data members ─────────────────────────────────────────────────────
    int listen_fd_{-1};
    int epoll_fd_{-1};
    sockaddr_in address_{};

    // Per-fd slot bookkeeping: fd → slot index (for bitmap free on disconnect).
    // Direct-indexed by fd; heap-allocated because it is too large for the
    // stack-resident Server object.  Leaves headroom above CAPACITY for the
    // listen / epoll / stdio descriptors.
    static constexpr int FD_MAP_SIZE = BitmapAllocator::CAPACITY + 1024;
    std::vector<int> fd_to_slot_;  // -1 = untracked

    BitmapAllocator slot_alloc_;
    // client_fd_[slot] = fd  (needed to iterate / close on shutdown)
    std::vector<int> client_fd_;
    // Mutex guarding slot_alloc_, fd_to_slot_, client_fd_.
    std::mutex conn_mutex_;

    // ── Task queue ───────────────────────────────────────────────────────
    std::queue<Task>        task_queue_;
    std::mutex              queue_mutex_;
    std::condition_variable queue_cv_;

    // ── Shared business data ─────────────────────────────────────────────
    std::deque<Train>    trains_;
    std::vector<Ticket>  tickets_;
    std::mutex           tickets_mutex_;   // guards tickets_ vector

    // ── Worker threads ───────────────────────────────────────────────────
    std::array<std::thread, WORKER_THREAD_COUNT> workers_;
    std::atomic<bool> stop_flag_{false};
};
