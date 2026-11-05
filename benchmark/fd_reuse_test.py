#!/usr/bin/env python3
# =============================================================================
# fd_reuse_test.py  –  Ticket ownership must survive socket-fd reuse
#
# The kernel hands out the lowest free fd, so when client A disconnects and
# client B connects, B usually gets A's old fd.  If the server identified
# users by fd, B would inherit A's tickets.  Each round:
#   1. A connects, buys a ticket, disconnects;
#   2. B connects (typically on the same fd) and must
#        - see no tickets of its own   (cmd 3),
#        - fail to refund A's ticket   (cmd 4 <id>);
#   3. the ticket must still be unrefunded in the global list (cmd 5).
#
# Usage (server already running on 127.0.0.1:8888):
#   python3 benchmark/fd_reuse_test.py --rounds 20
# =============================================================================
import argparse
import re
import socket
import sys
import time

NO_TICKETS = "您当前没有订单！"


def request(sock, line, idle=0.2):
    """Send one command and collect the reply until the server goes quiet."""
    sock.sendall(line.encode())
    sock.settimeout(5.0)
    buf = sock.recv(65536)
    sock.settimeout(idle)
    try:
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        pass
    return buf.decode()


def connect():
    return socket.create_connection(("127.0.0.1", 8888))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=20)
    args = ap.parse_args()

    failures = 0
    for rnd in range(args.rounds):
        a = connect()
        reply = request(a, "1 1 0 1 1")
        m = re.search(r"订单号：(\d+)", reply)
        if not m:
            print(f"round {rnd}: purchase failed: {reply!r}")
            return 2
        ticket_id = m.group(1)
        a.close()
        time.sleep(0.1)   # let the server see the disconnect and close the fd

        b = connect()
        mine = request(b, "3")
        refund = request(b, f"4 {ticket_id}")
        b.close()

        c = connect()
        listing = request(c, "5")
        c.close()
        row = re.search(rf"\[{ticket_id}\][^\n]*是否退票： (\S+)", listing)

        problems = []
        if NO_TICKETS not in mine:
            problems.append("new client sees an earlier client's tickets")
        if "退票成功" in refund:
            problems.append("new client refunded an earlier client's ticket")
        if not row or row.group(1) != "否":
            problems.append("ticket no longer listed as unrefunded")
        if problems:
            failures += 1
            print(f"round {rnd} (ticket {ticket_id}): " + "; ".join(problems))

    print(f"{args.rounds} rounds, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
