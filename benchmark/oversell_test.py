#!/usr/bin/env python3
# =============================================================================
# oversell_test.py  –  Concurrent purchase test for ticketgo_server
#
# --clients connections (spread over --procs processes) wait on a barrier,
# then each sends --attempts purchase requests back-to-back:
#   * --hot of them buy the same full-route ticket on train 0, so every
#     client competes for the same 100 seats;
#   * the rest buy random overlapping trips on trains 1–3.
#
# Every purchase is recorded client-side.  Afterwards the test replays the
# successful purchases through a model of Train's seat rules and checks:
#   1. no oversell   – no segment and no selling cap went below zero;
#   2. no lost/duplicated update – for every (train, from, to) the server's
#      remaining count equals the model's;
#   3. no undersell  – seats only decrease (no refunds here), so every trip
#      that was ever rejected must be sold out at the end;
#   4. the hot trip sold exactly its limit (min of capacity and selling cap).
#
# Must be run against a freshly started server (it assumes full inventory).
#
# Usage (server already running on 127.0.0.1:8888):
#   python3 benchmark/oversell_test.py --clients 50 --attempts 200 --procs 4
# =============================================================================
import argparse
import asyncio
import multiprocessing as mp
import random
import socket
import sys

SEP_BYTES = ("-" * 40 + "\n").encode()
FAIL_BYTES = "请重试。\n".encode()
# Default fleet in server_main.cc::createTrains(): (capacity, station count).
CAPACITY = 100
TRAIN_STATIONS = [11, 13, 15, 12]
HOT_TRAIN = 0


def cap_index(n, frm, to):
    """Index of the selling cap a trip draws from (mirrors Train.cc)."""
    return to - 1 if frm > n - 2 - to else frm


def initial_caps(n):
    return [CAPACITY * n // ((n - (i + 1)) * (i + 1)) for i in range(n - 1)]


def random_trip(rng):
    train = rng.randrange(1, len(TRAIN_STATIONS))
    n = TRAIN_STATIONS[train]
    frm = rng.randrange(0, n - 1)
    to = rng.randrange(frm + 1, n)
    return train, frm, to


async def read_reply(reader, timeout):
    buf = b""
    while True:
        chunk = await asyncio.wait_for(reader.read(4096), timeout)
        if not chunk:
            raise ConnectionError("server closed connection")
        buf += chunk
        if buf.endswith(FAIL_BYTES):
            return buf
        if buf.endswith(SEP_BYTES) and buf.count(SEP_BYTES) >= 4:
            return buf


async def run_client(n_clients, attempts, hot, seed, timeout, barrier):
    conns = [await asyncio.open_connection("127.0.0.1", 8888)
             for _ in range(n_clients)]
    barrier.wait()

    async def drive(idx, r, w):
        rng = random.Random(seed * 100003 + idx)
        log = []
        for _ in range(attempts):
            if rng.random() < hot:
                trip = (HOT_TRAIN, 0, TRAIN_STATIONS[HOT_TRAIN] - 1)
            else:
                trip = random_trip(rng)
            w.write(f"1 {trip[0]} {trip[1]} {trip[2]} 1".encode())
            await w.drain()
            reply = await read_reply(r, timeout)
            if "预定成功".encode() in reply:
                log.append((trip, True))
            elif "余票不足".encode() in reply:
                log.append((trip, False))
            else:
                raise ValueError(f"unexpected reply: {reply[:80]!r}")
        return log

    logs = await asyncio.gather(*(drive(i, r, w) for i, (r, w) in enumerate(conns)))
    for _, w in conns:
        w.close()
    return [e for log in logs for e in log]


def worker(n_clients, attempts, hot, seed, timeout, barrier, out):
    out.put(asyncio.run(run_client(n_clients, attempts, hot, seed, timeout, barrier)))


def query(sock, train, frm, to):
    sock.sendall(f"1 {train} {frm} {to} 0".encode())
    buf = b""
    while not (buf.endswith(SEP_BYTES) and buf.count(SEP_BYTES) >= 2):
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("server closed connection")
        buf += chunk
    text = buf.decode()
    return int(text.split("剩余：")[1].split("\n")[0])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--clients", type=int, default=50, help="concurrent clients")
    ap.add_argument("--attempts", type=int, default=200, help="purchases per client")
    ap.add_argument("--procs", type=int, default=4, help="client processes")
    ap.add_argument("--hot", type=float, default=0.3,
                    help="fraction of purchases aimed at the hot trip")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=30.0, help="per-op timeout (s)")
    args = ap.parse_args()

    barrier = mp.Barrier(args.procs)
    out = mp.Queue()
    share = [args.clients // args.procs + (i < args.clients % args.procs)
             for i in range(args.procs)]
    procs = [mp.Process(target=worker,
                        args=(n, args.attempts, args.hot, args.seed * 1000 + i,
                              args.timeout, barrier, out))
             for i, n in enumerate(share)]
    for p in procs:
        p.start()
    log = [e for _ in procs for e in out.get()]
    for p in procs:
        p.join()

    # ── Replay successful purchases through the model ───────────────────────
    seats = [[CAPACITY] * (n - 1) for n in TRAIN_STATIONS]
    caps = [initial_caps(n) for n in TRAIN_STATIONS]
    for (train, frm, to), ok in log:
        if ok:
            for i in range(frm, to):
                seats[train][i] -= 1
            caps[train][cap_index(TRAIN_STATIONS[train], frm, to)] -= 1

    def model_remaining(train, frm, to):
        n = TRAIN_STATIONS[train]
        c = caps[train][cap_index(n, frm, to)]
        segs = seats[train][frm:to]
        if c <= 0 or min(segs) <= 0:
            return 0
        return min(min(segs), c)

    errors = []
    oversold = sum(1 for t in seats for s in t if s < 0) + \
               sum(1 for t in caps for c in t if c < 0)
    if oversold:
        errors.append(f"oversell: {oversold} segments/caps below zero")

    mismatched = 0
    with socket.create_connection(("127.0.0.1", 8888)) as sock:
        for train, n in enumerate(TRAIN_STATIONS):
            for frm in range(n - 1):
                for to in range(frm + 1, n):
                    if query(sock, train, frm, to) != model_remaining(train, frm, to):
                        mismatched += 1
    if mismatched:
        errors.append(f"lost/duplicated update: {mismatched} trips disagree with model")

    rejected = {trip for trip, ok in log if not ok}
    undersold = sum(1 for t in rejected if model_remaining(*t) > 0)
    if undersold:
        errors.append(f"undersell: {undersold} rejected trips still have seats")

    n_hot = TRAIN_STATIONS[HOT_TRAIN]
    hot_trip = (HOT_TRAIN, 0, n_hot - 1)
    hot_limit = min(CAPACITY, initial_caps(n_hot)[cap_index(n_hot, 0, n_hot - 1)])
    hot_tries = sum(1 for trip, _ in log if trip == hot_trip)
    hot_sold = sum(1 for trip, ok in log if ok and trip == hot_trip)
    if hot_sold != hot_limit:
        errors.append(f"hot trip sold {hot_sold}, expected {hot_limit}")

    sold = sum(1 for _, ok in log if ok)
    print(f"clients={args.clients}  purchase attempts={len(log)}  "
          f"sold={sold}  rejected={len(log) - sold}")
    print(f"hot trip     attempts={hot_tries}  sold={hot_sold} (limit {hot_limit})")
    print(f"checked      {sum(n * (n - 1) // 2 for n in TRAIN_STATIONS)} trips "
          f"against the model")
    if errors:
        for e in errors:
            print("FAIL  " + e)
        sys.exit(1)
    print("PASS  no oversell, no undersell, server state matches model")


if __name__ == "__main__":
    main()
