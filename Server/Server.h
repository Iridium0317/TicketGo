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
//                     → BitmapAllocator<64> giving O(1) alloc/free via
//                        __builtin_ctzll on a uint64_t bitmask
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
// Manages up to 64 concurrent client slots using a single uint64_t.
// allocate(): O(1) via __builtin_ctzll (count trailing zeros).
// free():     O(1) bitwise OR.
//
// The class is NOT thread-safe on its own; callers must hold a lock.
class BitmapAllocator {
public:
    static constexpr int CAPACITY = 64;

    BitmapAllocator() noexcept : free_mask_(~uint64_t{0}) {}

    // Returns slot index [0, CAPACITY) or -1 if full.
    int allocate() noexcept {
        if (free_mask_ == 0) return -1;
        const int slot = __builtin_ctzll(free_mask_);   // index of lowest 1-bit
        free_mask_ &= ~(uint64_t{1} << slot);           // mark as used
        return slot;
    }

    // Marks slot as free.  Undefined behaviour if slot was not allocated.
    void free(int slot) noexcept {
        free_mask_ |= (uint64_t{1} << slot);
    }

    bool full()  const noexcept { return free_mask_ == 0;       }
    bool empty() const noexcept { return free_mask_ == ~uint64_t{0}; }

private:
    uint64_t free_mask_;
};

// ── Server ────────────────────────────────────────────────────────────────
class Server {
public:
    explicit Server(std::vector<Train> trains);
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
    // Sized to RLIMIT_NOFILE default; direct-index into an array.
    static constexpr int FD_MAP_SIZE = 4096;
    std::array<int, FD_MAP_SIZE> fd_to_slot_{};  // -1 = untracked

    BitmapAllocator slot_alloc_;
    // client_fd_[slot] = fd  (needed to iterate / close on shutdown)
    std::array<int, BitmapAllocator::CAPACITY> client_fd_{};
    // Mutex guarding slot_alloc_, fd_to_slot_, client_fd_.
    std::mutex conn_mutex_;

    // ── Task queue ───────────────────────────────────────────────────────
    std::queue<Task>        task_queue_;
    std::mutex              queue_mutex_;
    std::condition_variable queue_cv_;

    // ── Shared business data ─────────────────────────────────────────────
    std::vector<Train>   trains_;
    std::vector<Ticket>  tickets_;
    std::mutex           tickets_mutex_;   // guards tickets_ vector

    // ── Worker threads ───────────────────────────────────────────────────
    std::array<std::thread, WORKER_THREAD_COUNT> workers_;
    std::atomic<bool> stop_flag_{false};
};
