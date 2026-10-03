"""The reference: a bare UDP flow beside the stream, on the same Wi-Fi (radios
plan T7, step 4, 03/10/2026).

Moonlight's inputs ride their own UDP (ENet), not SCTP behind the video. If a
bare UDP round trip to the client is as slow and as spread as the input
channel's, the cost is the radio's, not SCTP's. On 03/10 it was not: 4.7 ms
(p99 13.8) on the Mac's Wi-Fi while the stream ran at 240 fps.

    client:  python3 udp_ref.py echo [port]                 # answers every datagram
    host:    python3 udp_ref.py ping <client> [port] [hz] [secs] [out.json]

The ping side sends <hz> small datagrams a second (seq, send time) and keeps
each round trip; a datagram not back within 2 s is lost. series.py runs it in a
thread for a whole pass, stopped from outside (`stop`).
"""
import json
import socket
import statistics
import struct
import sys
import time


def echo(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", port))
    print("echo on", port, flush=True)
    while True:
        data, addr = s.recvfrom(2048)
        s.sendto(data, addr)


def ping(host, port, hz, secs, out, stop=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setblocking(False)
    period = 1.0 / hz
    sent = {}
    rtts = []
    seq = 0
    t0 = time.perf_counter()
    start_wall = time.time()
    nxt = t0
    end = t0 + secs
    while True:
        now = time.perf_counter()
        if stop is not None and stop() and now < end:
            end = now
        if now >= end + 2:
            break
        if now >= nxt and now < end:
            sent[seq] = now
            s.sendto(struct.pack("!Id", seq, now) + b"\0" * 40, (host, port))
            seq += 1
            nxt += period
        try:
            while True:
                data, _ = s.recvfrom(2048)
                k, ts = struct.unpack("!Id", data[:12])
                if k in sent:
                    rtts.append((round((sent.pop(k) - t0) * 1000, 1),
                                 round((time.perf_counter() - ts) * 1000, 3)))
        except (BlockingIOError, ConnectionResetError):
            # Windows reports an ICMP port unreachable (no echo yet) on the
            # next receive.
            pass
        time.sleep(0.0005)
    v = sorted(r for _, r in rtts)
    q = lambda p: v[min(len(v) - 1, int(p * len(v)))] if v else None  # noqa: E731
    summary = {"sent": seq, "answered": len(v), "lost": seq - len(v),
               "median": q(0.5), "p90": q(0.9), "p99": q(0.99), "max": v[-1] if v else None,
               "mean": round(statistics.mean(v), 2) if v else None}
    print("udp %d/s %d s: %d/%d back, rtt median %s ms p90 %s p99 %s max %s" % (
        hz, round(time.perf_counter() - t0 - 2), summary["answered"], seq, summary["median"],
        summary["p90"], summary["p99"], summary["max"]), flush=True)
    if out:
        with open(out, "w") as f:
            json.dump({"summary": summary, "startWall": start_wall, "samples": rtts}, f)
    return summary


if __name__ == "__main__":
    if sys.argv[1] == "echo":
        echo(int(sys.argv[2]) if len(sys.argv) > 2 else 47998)
    else:
        a = sys.argv[2:]
        ping(a[0], int(a[1]) if len(a) > 1 else 47998, int(a[2]) if len(a) > 2 else 50,
             float(a[3]) if len(a) > 3 else 20, a[4] if len(a) > 4 else "")
