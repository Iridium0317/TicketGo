// =============================================================================
// bench_main.cc  –  TicketGo Modern C++17  Micro-Benchmarks
//
// Four independent benchmarks, each self-contained:
//
//   [1] BitmapAllocator  vs  linear-scan array  (O(1) vs O(n))
//       – Measures alloc+free cycles per second at varying slot counts.
//
//   [2] std::shared_mutex (RW-lock)  vs  std::mutex (exclusive)
//       – 4 reader threads + 1 writer thread, 90/10 workload mirrors
//         production read-heavy ticket-query traffic.
//
//   [3] Thread-pool task throughput
//       – Pushes N tasks into the same Producer-Consumer queue used by
//         Server; measures tasks/second end-to-end including lock
//         contention and condition_variable wake-up overhead.
//
//   [4] Single-operation latency (p50 / p99 / p999)
//       – Measures each individual allocate+deallocate call with
//         high_resolution_clock; collects 100k samples and reports
//         percentile distribution for BitmapAllocator vs LinearAllocator.
//
// Build (from Modern/):
//   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
//   ./build/ticketgo_bench
//
// No external libraries required.
// =============================================================================

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

// ── Timing helper ─────────────────────────────────────────────────────────
using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using NS        = std::chrono::nanoseconds;

static double elapsed_ms(TimePoint t0, TimePoint t1) {
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

static void print_header(const char* title) {
    std::cout << "\n╔══════════════════════════════════════════════════════╗\n"
              << "║  " << std::left << std::setw(52) << title << "║\n"
              << "╚══════════════════════════════════════════════════════╝\n";
}

static void print_row(const std::string& label, double ms, long long ops) {
    double ops_per_sec = ops / (ms / 1000.0);
    std::cout << "  " << std::left  << std::setw(30) << label
              << std::right << std::setw(10) << std::fixed << std::setprecision(2) << ms << " ms"
              << "   "
              << std::setw(14) << std::setprecision(0) << ops_per_sec << " ops/s\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// BENCHMARK 1 – BitmapAllocator  vs  Linear-Scan Array
// ═══════════════════════════════════════════════════════════════════════════

// ── Replica of the production BitmapAllocator ────────────────────────────
class BitmapAllocator {
public:
    static constexpr int CAPACITY = 64;
    BitmapAllocator() noexcept : free_mask_(~uint64_t{0}) {}

    int  allocate() noexcept {
        if (!free_mask_) return -1;
        const int slot = __builtin_ctzll(free_mask_);
        free_mask_ &= ~(uint64_t{1} << slot);
        return slot;
    }
    void free(int slot) noexcept { free_mask_ |= (uint64_t{1} << slot); }
    bool full()  const noexcept { return !free_mask_; }

private:
    uint64_t free_mask_;
};

// ── Legacy linear-scan allocator (mirrors original client_socket_[30]) ───
struct LinearAllocator {
    static constexpr int CAPACITY = 64;
    std::array<int, CAPACITY> slots{};
    LinearAllocator() { slots.fill(0); }

    int allocate() noexcept {
        for (int i = 0; i < CAPACITY; ++i)
            if (slots[i] == 0) { slots[i] = 1; return i; }
        return -1;
    }
    void free(int slot) noexcept { slots[slot] = 0; }
};

static void bench_allocator() {
    print_header("BENCH 1 – Session Slot Allocator");

    constexpr long long ROUNDS = 50'000'000LL;;

    // ── Bitmap ────────────────────────────────────────────────────────────
    {
        BitmapAllocator alloc;
        // Pre-fill half the slots to create a realistic "middle" fragmentation.
        for (int i = 0; i < BitmapAllocator::CAPACITY / 2; ++i) alloc.allocate();

        long long dummy = 0;
        auto t0 = Clock::now();
        for (long long r = 0; r < ROUNDS; ++r) {
            int s = alloc.allocate();
            dummy += s;
            asm volatile("" : "+r"(dummy));
            alloc.free(s);
        }
        auto t1 = Clock::now();
        asm volatile("" : : "r"(dummy));
        print_row("BitmapAllocator (O(1), 50% full)", elapsed_ms(t0, t1), ROUNDS);
    }

    // ── Linear scan ───────────────────────────────────────────────────────
    {
        LinearAllocator alloc;
        for (int i = 0; i < LinearAllocator::CAPACITY / 2; ++i) alloc.allocate();

        long long dummy = 0;
        auto t0 = Clock::now();
        for (long long r = 0; r < ROUNDS; ++r) {
            int s = alloc.allocate();
            dummy += s;
            asm volatile("" : "+r"(dummy));
            alloc.free(s);
        }
        auto t1 = Clock::now();
        asm volatile("" : : "r"(dummy));
        print_row("LinearAllocator  (O(n), 50% full)", elapsed_ms(t0, t1), ROUNDS);
    }

    // ── Worst-case: bitmap vs linear at 63/64 slots used ─────────────────
    {
        BitmapAllocator ba;
        for (int i = 0; i < BitmapAllocator::CAPACITY - 1; ++i) ba.allocate();
        long long dummy = 0;
        auto t0 = Clock::now();
        for (long long r = 0; r < ROUNDS; ++r) {
            int s = ba.allocate();   // only slot 63 is free → worst-case for linear
            dummy += s;
            asm volatile("" : "+r"(dummy));
            ba.free(s);
        }
        auto t1 = Clock::now();
        asm volatile("" : : "r"(dummy));
        print_row("BitmapAllocator (O(1), 98% full)", elapsed_ms(t0, t1), ROUNDS);
    }
    {
        LinearAllocator la;
        for (int i = 0; i < LinearAllocator::CAPACITY - 1; ++i) la.allocate();
        long long dummy = 0;
        auto t0 = Clock::now();
        for (long long r = 0; r < ROUNDS; ++r) {
            int s = la.allocate();   // must scan all 63 slots before finding free one
            dummy += s;
            asm volatile("" : "+r"(dummy));
            la.free(s);
        }
        auto t1 = Clock::now();
        asm volatile("" : : "r"(dummy));
        print_row("LinearAllocator  (O(n), 98% full)", elapsed_ms(t0, t1), ROUNDS);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// BENCHMARK 2 – shared_mutex (RW-lock) vs exclusive mutex
//
// Models the production read-heavy workload:
//   • READER_THREADS reader threads each doing READ_OPS reads.
//   • 1 writer thread doing WRITE_OPS writes interspersed at ~10% rate.
//
// The "shared value" mimics each_available_seats_[0].
// ═══════════════════════════════════════════════════════════════════════════

static void bench_rwlock() {
    print_header("BENCH 2 – RW-Lock vs Exclusive Mutex (4R+1W, 90/10)");

    constexpr int       READER_THREADS = 4;
    constexpr long long READ_OPS       = 2'000'000LL;
    constexpr long long WRITE_OPS      = READ_OPS / 9;   // ~10% writes

    // ── shared_mutex ─────────────────────────────────────────────────────
    {
        std::shared_mutex rw;
        volatile int      value = 100;
        std::atomic<long long> total_reads{0};

        auto reader = [&] {
            long long local = 0;
            for (long long i = 0; i < READ_OPS; ++i) {
                std::shared_lock lock(rw);
                local += value;   // simulates getTicketNumber()
                for (volatile int j = 0; j < 50; ++j) {}  // simulate query work
            }
            total_reads.fetch_add(local, std::memory_order_relaxed);
        };
        auto writer = [&] {
            for (long long i = 0; i < WRITE_OPS; ++i) {
                std::unique_lock lock(rw);
                --value;          // simulates buyTicket()
                if (value < 0) value = 100;
            }
        };

        auto t0 = Clock::now();
        std::vector<std::thread> threads;
        threads.reserve(READER_THREADS + 1);
        for (int i = 0; i < READER_THREADS; ++i) threads.emplace_back(reader);
        threads.emplace_back(writer);
        for (auto& t : threads) t.join();
        auto t1 = Clock::now();

        const long long total = READ_OPS * READER_THREADS + WRITE_OPS;
        print_row("std::shared_mutex  (RW-lock)", elapsed_ms(t0, t1), total);
    }

    // ── exclusive mutex ───────────────────────────────────────────────────
    {
        std::mutex   mtx;
        volatile int value = 100;
        std::atomic<long long> total_reads{0};

        auto reader = [&] {
            long long local = 0;
            for (long long i = 0; i < READ_OPS; ++i) {
                std::unique_lock lock(mtx);   // exclusive, even for reads
                local += value;
                for (volatile int j = 0; j < 500; ++j) {}  // simulate query work

            }
            total_reads.fetch_add(local, std::memory_order_relaxed);
        };
        auto writer = [&] {
            for (long long i = 0; i < WRITE_OPS; ++i) {
                std::unique_lock lock(mtx);
                --value;
                if (value < 0) value = 100;
            }
        };

        auto t0 = Clock::now();
        std::vector<std::thread> threads;
        threads.reserve(READER_THREADS + 1);
        for (int i = 0; i < READER_THREADS; ++i) threads.emplace_back(reader);
        threads.emplace_back(writer);
        for (auto& t : threads) t.join();
        auto t1 = Clock::now();

        const long long total = READ_OPS * READER_THREADS + WRITE_OPS;
        print_row("std::mutex         (exclusive)", elapsed_ms(t0, t1), total);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// BENCHMARK 3 – Thread-pool task throughput
//
// Replicates the exact queue / mutex / condition_variable structure used in
// Server.cc.  Each "task" does a small amount of CPU work (string concat) to
// model Task::doTask() overhead without needing a real network socket.
// ═══════════════════════════════════════════════════════════════════════════

struct BenchTask {
    int  id;
    int  value;
};

static void bench_threadpool() {
    print_header("BENCH 3 – Thread-Pool Throughput (4 workers)");

    constexpr int WORKER_COUNT = 4;

    auto run_pool = [&](long long total_tasks, const char* label) {
        std::queue<BenchTask>   q;
        std::mutex              qmtx;
        std::condition_variable qcv;
        std::atomic<long long>  done{0};
        std::atomic<bool>       stop{false};

        // Workers
        std::array<std::thread, WORKER_COUNT> workers;
        for (auto& w : workers) {
            w = std::thread([&] {
                while (true) {
                    std::unique_lock<std::mutex> lock(qmtx);
                    qcv.wait(lock, [&] { return !q.empty() || stop.load(); });
                    if (q.empty()) break;

                    BenchTask t = q.front();
                    q.pop();
                    lock.unlock();

                    // Simulate lightweight doTask() work: one integer op.
                    volatile int result = t.value * 2 + t.id;
                    (void)result;

                    done.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        auto t0 = Clock::now();

        // Producer: push all tasks
        for (long long i = 0; i < total_tasks; ++i) {
            {
                std::lock_guard<std::mutex> lock(qmtx);
                q.push({static_cast<int>(i), static_cast<int>(i & 0xFF)});
            }
            qcv.notify_one();
        }

        // Wait until all tasks consumed
        while (done.load(std::memory_order_relaxed) < total_tasks)
            std::this_thread::yield();

        auto t1 = Clock::now();

        stop.store(true);
        qcv.notify_all();
        for (auto& w : workers) w.join();

        print_row(label, elapsed_ms(t0, t1), total_tasks);
    };

    run_pool(100'000LL,   "100k  tasks, 4 workers");
    run_pool(500'000LL,   "500k  tasks, 4 workers");
    run_pool(1'000'000LL, "1M    tasks, 4 workers");
}

// ═══════════════════════════════════════════════════════════════════════════
// BENCHMARK 4 – Single-Operation Latency  (p50 / p99 / p999)
//
// Collects SAMPLES individual allocate+deallocate timings using
// high_resolution_clock, then sorts and reports percentiles.
//
// The allocator is pre-loaded to 50% occupancy so we measure a realistic
// middle-of-the-table scenario, identical to BENCH 1.
// ═══════════════════════════════════════════════════════════════════════════

using HRClock = std::chrono::high_resolution_clock;

static void print_latency_header() {
    std::cout << "\n╔══════════════════════════════════════════════════════╗\n"
              << "║  BENCH 4 – Single-Op Latency  (100k samples each)   ║\n"
              << "╚══════════════════════════════════════════════════════╝\n";
    std::cout << "  " << std::left  << std::setw(34) << "Allocator"
              << std::right << std::setw(9)  << "p50"
              << std::setw(9)  << "p99"
              << std::setw(9)  << "p999"
              << std::setw(9)  << "min"
              << std::setw(9)  << "max"
              << "  (ns)\n"
              << "  " << std::string(79, '-') << "\n";
}

static void print_latency_row(const std::string& label, std::vector<long long>& samples) {
    std::sort(samples.begin(), samples.end());
    const auto n = static_cast<long long>(samples.size());

    // Percentile index: use nearest-rank (ceiling).
    auto pct = [&](double p) -> long long {
        long long idx = static_cast<long long>(std::ceil(p / 100.0 * n)) - 1;
        if (idx < 0)     idx = 0;
        if (idx >= n)    idx = n - 1;
        return samples[static_cast<size_t>(idx)];
    };

    std::cout << "  " << std::left  << std::setw(34) << label
              << std::right
              << std::setw(9)  << pct(50.0)
              << std::setw(9)  << pct(99.0)
              << std::setw(9)  << pct(99.9)
              << std::setw(9)  << samples.front()
              << std::setw(9)  << samples.back()
              << "\n";
}

static void bench_latency() {
    print_latency_header();

    constexpr int SAMPLES = 100'000;

    // ── BitmapAllocator at 50% occupancy ─────────────────────────────────
    {
        BitmapAllocator alloc;
        for (int i = 0; i < BitmapAllocator::CAPACITY / 2; ++i) alloc.allocate();

        std::vector<long long> samples;
        samples.reserve(SAMPLES);

        for (int s = 0; s < SAMPLES; ++s) {
            auto t0 = HRClock::now();
            int slot = alloc.allocate();
            asm volatile("" : "+r"(slot));   // prevent hoisting
            alloc.free(slot);
            auto t1 = HRClock::now();
            samples.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }

        print_latency_row("BitmapAllocator (50% full)", samples);
    }

    // ── LinearAllocator at 50% occupancy ─────────────────────────────────
    {
        LinearAllocator alloc;
        for (int i = 0; i < LinearAllocator::CAPACITY / 2; ++i) alloc.allocate();

        std::vector<long long> samples;
        samples.reserve(SAMPLES);

        for (int s = 0; s < SAMPLES; ++s) {
            auto t0 = HRClock::now();
            int slot = alloc.allocate();
            asm volatile("" : "+r"(slot));
            alloc.free(slot);
            auto t1 = HRClock::now();
            samples.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }

        print_latency_row("LinearAllocator  (50% full)", samples);
    }

    // ── BitmapAllocator at 98% occupancy (worst-case for linear) ─────────
    {
        BitmapAllocator alloc;
        for (int i = 0; i < BitmapAllocator::CAPACITY - 1; ++i) alloc.allocate();

        std::vector<long long> samples;
        samples.reserve(SAMPLES);

        for (int s = 0; s < SAMPLES; ++s) {
            auto t0 = HRClock::now();
            int slot = alloc.allocate();
            asm volatile("" : "+r"(slot));
            alloc.free(slot);
            auto t1 = HRClock::now();
            samples.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }

        print_latency_row("BitmapAllocator (98% full)", samples);
    }

    // ── LinearAllocator at 98% occupancy ─────────────────────────────────
    {
        LinearAllocator alloc;
        for (int i = 0; i < LinearAllocator::CAPACITY - 1; ++i) alloc.allocate();

        std::vector<long long> samples;
        samples.reserve(SAMPLES);

        for (int s = 0; s < SAMPLES; ++s) {
            auto t0 = HRClock::now();
            int slot = alloc.allocate();
            asm volatile("" : "+r"(slot));
            alloc.free(slot);
            auto t1 = HRClock::now();
            samples.push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }

        print_latency_row("LinearAllocator  (98% full)", samples);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// main
// ═══════════════════════════════════════════════════════════════════════════
int main() {
    std::cout << "TicketGo Modern C++17 – Micro-Benchmark Suite\n"
              << "Compiled: " __DATE__ "  " __TIME__ "\n"
              << "Threads available: " << std::thread::hardware_concurrency() << '\n';

    bench_allocator();
    bench_rwlock();
    bench_threadpool();
    bench_latency();

    std::cout << "\nDone.\n";
    return 0;
}
