#!/usr/bin/env python3
# =============================================================================
# framing_test.py  –  Commands must survive TCP's lack of message boundaries
#
# TCP is a byte stream: two commands sent back-to-back can arrive in one
# read(), and one command can arrive split across two.  The server must
# split the stream on '\n' and answer every command, in order.  Cases:
#   pipelined   two queries in one send          → two replies
#   mixed       "2\n3\n" in one send             → train list, then my tickets
#   split       "1 0 0" … " 3 0\n" in two sends  → one query reply, no error
#   crlf        a command ending in "\r\n"       → answered normally
#   order       "3\n1 0 0 1 1\n3\n" in one send  → empty list, purchase, list
#               with the new ticket, in that order (repeated --rounds times)
#   too long    2,000 bytes with no '\n'         → server drops the connection
#
# Usage (fresh server running on 127.0.0.1:8888):
#   python3 benchmark/framing_test.py --rounds 30
# =============================================================================
import argparse
import re
import socket
import sys
import time

HOST, PORT = "127.0.0.1", 8888
REMAINING = "剩余"
TRAIN_LIST = "[0] (0)"
NO_TICKETS = "您当前没有订单！"
BOUGHT = "预定成功"
FAILED = "操作失败"


def collect(sock, idle=0.3):
    """Replies carry no length, so read until the server goes quiet."""
    sock.settimeout(idle)
    buf = b""
    try:
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        pass
    return buf.decode(errors="replace")


def exchange(*chunks, pause=0.2):
    """Sends each chunk separately (pausing between them) on a new connection."""
    with socket.create_connection((HOST, PORT)) as s:
        for i, c in enumerate(chunks):
            s.sendall(c.encode())
            if i + 1 < len(chunks):
                time.sleep(pause)
        return collect(s)


def check(name, ok, detail):
    print(f"{'PASS' if ok else 'FAIL'}  {name:<10} {detail}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=30)
    args = ap.parse_args()
    results = []

    out = exchange("1 0 0 3 0\n1 0 0 5 0\n")
    n = out.count(REMAINING)
    results.append(check("pipelined", n == 2, f"replies={n} (want 2)"))

    out = exchange("2\n3\n")
    i, j = out.find(TRAIN_LIST), out.find(NO_TICKETS)
    results.append(check("mixed", 0 <= i < j,
                         f"train list at {i}, my tickets at {j} (want both, in order)"))

    out = exchange("1 0 0", " 3 0\n")
    ok = out.count(REMAINING) == 1 and FAILED not in out and NO_TICKETS not in out
    results.append(check("split", ok,
                         f"replies={out.count(REMAINING)} error={FAILED in out} "
                         f"stray-cmd-3={NO_TICKETS in out}"))

    out = exchange("1 0 0 3 0\r\n")
    results.append(check("crlf", out.count(REMAINING) == 1 and FAILED not in out,
                         f"replies={out.count(REMAINING)}"))

    bad = 0
    for r in range(args.rounds):
        out = exchange("3\n1 0 0 1 1\n3\n")
        m = re.search(r"订单号：(\d+)", out)
        empty, bought = out.find(NO_TICKETS), out.find(BOUGHT)
        listed = out.find(f"[{m.group(1)}]  车次") if m else -1
        if not (0 <= empty < bought < listed):
            bad += 1
            if bad <= 3:
                print(f"      round {r}: empty={empty} bought={bought} listed={listed}")
    results.append(check("order", bad == 0, f"{args.rounds - bad}/{args.rounds} rounds in order"))

    with socket.create_connection((HOST, PORT)) as s:
        s.sendall(b"1" * 2000)
        s.settimeout(2.0)
        try:
            dropped = s.recv(65536) == b""
        except ConnectionResetError:   # closed with our bytes unread → RST
            dropped = True
        except socket.timeout:
            dropped = False
    results.append(check("too long", dropped, "connection closed by server" if dropped
                         else "connection still open"))

    failed = results.count(False)
    print(f"{len(results) - failed}/{len(results)} cases passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
