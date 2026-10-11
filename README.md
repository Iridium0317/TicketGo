# TicketGo (Modern C++17)

A multithreaded train-ticketing TCP server for Linux.

This is a modernised and extended version of an open-source C++ TicketGo course project. The segment-inventory data model and the client/server protocol come from the original. This version contributes:

- the move from `select` to edge-triggered `epoll`;
- thread-safe inventory using a reader–writer lock and re-checking availability under the lock;
- the bitmap connection allocator;
- C++17 rewrites and bug fixes;
- the benchmarks and the load test below.

- **Networking**: Reactor model on edge-triggered `epoll` with non-blocking sockets. The I/O thread hands decoded requests to a fixed worker pool through a `mutex` + `condition_variable` queue. A two-level bitmap allocator assigns connection slots, up to 65,536 of them.
- **Segment inventory**: a ticket from station *a* to *b* occupies every segment in `[a, b)`. Each purchase re-checks and reserves all of its segments inside one per-train critical section, so tickets are never oversold.
- **Locking**: each train has a `std::shared_mutex`. Queries take a shared lock; purchases and refunds take an exclusive lock. The train lock and the ticket-ledger lock are never held at the same time, so they cannot deadlock.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

`ticketgo_server` needs Linux (`epoll`). `ticketgo_client` and `ticketgo_bench` also build on macOS.

## Run

```bash
./build/ticketgo_server        # listens on :8888
./build/ticketgo_client        # interactive client
```

## Benchmarks

### End-to-end load test

`benchmark/load_test.py` opens N connections and keeps all of them open. Once every connection is established, each one sends query requests back-to-back. Every reply is validated. A connection counts as established only after the server answers a request on it, because a successful `connect()` alone does not prove the server accepted the connection.

```bash
ulimit -n 200000
./build/ticketgo_server >/dev/null &
python3 benchmark/load_test.py --conns 10000 --rounds 100 --procs 8
```

Results below are from a Docker `gcc:13` container with 8 vCPUs, running on an Apple Silicon Mac. Each run sends 100 requests per connection.

| Concurrent connections | Runs | Failed requests | Throughput (req/s) |
|---|---|---|---|
| 1,000  | 6 | 0 | 128K – 141K |
| 10,000 | 6 | 0 | 46K – 70K |
| 20,000 | 3 | 0 | 44K – 62K |

The load generator is the bottleneck in these runs. When the server and client were pinned to separate 4-core sets, the Python client used all 4 of its cores, while the server used about 1.2 cores at 10K connections. The throughput figures are therefore lower bounds for the server. Latencies are measured on the client and include asyncio scheduling, so they are not reported here as server latency.

Profiling with `perf` at 10K connections shows the reactor thread spends about 60% of its CPU in `pushTask` → `pthread_cond_signal` → futex wake-ups of worker threads. This is the hand-off cost of the single-reactor + worker-pool design. A one-loop-per-thread design would remove it.

### Microbenchmarks

```bash
./build/ticketgo_bench
```

- **RW-lock vs mutex**: 4 readers and 1 writer, 90% reads. `std::shared_mutex` gives about 7× the throughput of `std::mutex` on Linux (2.29M vs 0.33M ops/s). The read critical section is deliberately lengthened in this benchmark, so the speedup in the real server is smaller.
- **Slot allocator**: bitmap allocation with `__builtin_ctzll` is about 10–20× faster than a linear scan over 64 slots.
