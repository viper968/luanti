#!/usr/bin/env python3
"""Measure UDP round-trip time and loss against the board test's echo server.

Usage: python3 udp_ping.py <board-ip> [port] [count]
Sends packets sized like typical Luanti traffic, one every 20 ms.
"""
import socket
import statistics
import sys
import time

host = sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__)
port = int(sys.argv[2]) if len(sys.argv) > 2 else 30000
count = int(sys.argv[3]) if len(sys.argv) > 3 else 500

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(0.5)
rtts = []
sizes = (64, 256, 512)  # small player/position packets up to a full MTU-safe chunk

for i in range(count):
    payload = i.to_bytes(4, "big") + bytes(sizes[i % len(sizes)] - 4)
    t0 = time.perf_counter()
    sock.sendto(payload, (host, port))
    try:
        while True:
            data, _ = sock.recvfrom(2048)
            if data[:4] == payload[:4]:
                rtts.append((time.perf_counter() - t0) * 1000)
                break
    except socket.timeout:
        pass
    time.sleep(0.02)

lost = count - len(rtts)
print(f"sent {count}, received {len(rtts)}, loss {100 * lost / count:.1f}%")
if rtts:
    rtts.sort()
    print(f"rtt ms: min {rtts[0]:.1f}  median {statistics.median(rtts):.1f}  "
          f"p95 {rtts[int(len(rtts) * 0.95) - 1]:.1f}  max {rtts[-1]:.1f}")
