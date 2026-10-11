#!/usr/bin/env python3
# =============================================================================
# load_test.py  –  End-to-end TCP load test for ticketgo_server
#
# Phase 1  Open --conns connections (ramped, --ramp at a time) and keep ALL
#          of them open.  A connection only counts as established once the
#          server has answered a first request on it – a bare connect() is not
#          enough, because the kernel completes the TCP handshake before the
#          server ever calls accept().
# Phase 2  After every client process has finished phase 1 (barrier), each
#          connection sends --rounds query requests back-to-back
#          (send → wait for the full reply → next), so all connections are
#          active concurrently.  Every reply is validated.
#
# Latency is measured client-side and includes Python/asyncio overhead, so
# the reported percentiles are an upper bound on server latency.
#
# Usage (server already running on 127.0.0.1:8888):
#   ulimit -n 200000
#   python3 benchmark/load_test.py --conns 10000 --rounds 20 --procs 4
# =============================================================================
import argparse
import asyncio
import multiprocessing as mp
import random
import time

SEP = "-" * 40 + "\n"
SEP_BYTES = SEP.encode()
# Station counts of the default fleet in server_main.cc::createTrains().
TRAIN_STATIONS = [11, 13, 15, 12]


def random_query():
    train = random.randrange(len(TRAIN_STATIONS))
    n = TRAIN_STATIONS[train]
    frm = random.randrange(0, n - 2)
    to = random.randrange(frm + 1, n - 1)
    return f"1 {train} {frm} {to} 0".encode()


async def request(reader, writer, timeout):
    """Sends one query and reads until the reply's closing separator."""
    writer.write(random_query())
    await writer.drain()
    buf = b""
    while not (buf.endswith(SEP_BYTES) and buf.count(SEP_BYTES) >= 2):
        chunk = await asyncio.wait_for(reader.read(4096), timeout)
        if not chunk:
            raise ConnectionError("server closed connection")
        buf += chunk
    if "剩余".encode() not in buf:
        raise ValueError(f"unexpected reply: {buf[:80]!r}")


async def run_client(conns, rounds, ramp, timeout, barrier):
    # ── Phase 1: establish and hold all connections ─────────────────────────
    sem = asyncio.Semaphore(ramp)

    async def open_one():
        async with sem:
            try:
                r, w = await asyncio.wait_for(
                    asyncio.open_connection("127.0.0.1", 8888), timeout)
                await request(r, w, timeout)
                return r, w
            except Exception:
                return None

    opened = await asyncio.gather(*(open_one() for _ in range(conns)))
    live = [c for c in opened if c is not None]

    # Block this process until every client process holds its connections.
    barrier.wait()

    # ── Phase 2: all connections issue requests concurrently ────────────────
    latencies = []
    failed_conns = 0
    failed_reqs = 0

    async def drive(r, w):
        nonlocal failed_conns, failed_reqs
        ok = True
        for _ in range(rounds):
            t0 = time.perf_counter()
            try:
                await request(r, w, timeout)
                latencies.append(time.perf_counter() - t0)
            except Exception:
                failed_reqs += 1
                ok = False
                break
        if not ok:
            failed_conns += 1

    t0 = time.perf_counter()
    await asyncio.gather(*(drive(r, w) for r, w in live))
    elapsed = time.perf_counter() - t0

    for _, w in live:
        w.close()
    return {
        "attempted": conns,
        "established": len(live),
        "failed_conns": failed_conns,
        "failed_reqs": failed_reqs,
        "latencies": latencies,
        "elapsed": elapsed,
    }


def worker(conns, rounds, ramp, timeout, barrier, out):
    out.put(asyncio.run(run_client(conns, rounds, ramp, timeout, barrier)))


def pct(sorted_vals, p):
    if not sorted_vals:
        return float("nan")
    idx = min(len(sorted_vals) - 1, max(0, int(len(sorted_vals) * p / 100.0 + 0.5) - 1))
    return sorted_vals[idx]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--conns", type=int, default=1000, help="total connections")
    ap.add_argument("--rounds", type=int, default=20, help="requests per connection")
    ap.add_argument("--procs", type=int, default=4, help="client processes")
    ap.add_argument("--ramp", type=int, default=200,
                    help="max in-flight connects per process")
    ap.add_argument("--timeout", type=float, default=30.0, help="per-op timeout (s)")
    args = ap.parse_args()

    barrier = mp.Barrier(args.procs)
    out = mp.Queue()
    share = [args.conns // args.procs + (i < args.conns % args.procs)
             for i in range(args.procs)]
    procs = [mp.Process(target=worker,
                        args=(n, args.rounds, args.ramp, args.timeout, barrier, out))
             for n in share]
    for p in procs:
        p.start()
    results = [out.get() for _ in procs]
    for p in procs:
        p.join()

    attempted = sum(r["attempted"] for r in results)
    established = sum(r["established"] for r in results)
    failed_conns = sum(r["failed_conns"] for r in results)
    failed_reqs = sum(r["failed_reqs"] for r in results)
    lat = sorted(x for r in results for x in r["latencies"])
    elapsed = max(r["elapsed"] for r in results)

    print(f"connections  attempted={attempted}  established+held={established}  "
          f"fully served={established - failed_conns}")
    print(f"requests     ok={len(lat)}  failed={failed_reqs}  "
          f"in {elapsed:.2f}s  →  {len(lat) / elapsed:,.0f} req/s")
    print(f"latency ms   p50={pct(lat, 50) * 1e3:.2f}  p99={pct(lat, 99) * 1e3:.2f}  "
          f"p999={pct(lat, 99.9) * 1e3:.2f}  max={lat[-1] * 1e3 if lat else float('nan'):.2f}")


if __name__ == "__main__":
    main()
