#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ProjectHax LLC

"""Flood UDP packets over loopback to generate NET_RX/NET_TX softirq load.
Expect: softirq rates in Load & I/O -> Softirqs, and softirq evidence on hitches."""
import socket, sys, threading, time

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 20
threads = int(sys.argv[2]) if len(sys.argv) > 2 else 4
rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.bind(("127.0.0.1", 0))
rx.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 22)
port = rx.getsockname()[1]
stop = time.monotonic() + seconds

def drain():
    rx.settimeout(0.5)
    while time.monotonic() < stop:
        try:
            rx.recv(65536)
        except socket.timeout:
            pass

def send():
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    payload = b"x" * 1200
    while time.monotonic() < stop:
        for _ in range(1000):
            tx.sendto(payload, ("127.0.0.1", port))

print(f"Flooding 127.0.0.1:{port} with {threads} senders for {seconds:.0f} s")
workers = [threading.Thread(target=drain)] + [threading.Thread(target=send) for _ in range(threads)]
for w in workers:
    w.start()
for w in workers:
    w.join()
