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

Loss or disorder (plan Wi-Fi W1): SCTP fast-retransmits a chunk when three
SACKs report it missing, whether it was lost or only overtaken. A one-way flow
shaped like the video tells the two apart on the radio itself:

    client:  python3 udp_ref.py sink [port]                 # counts what arrives
    host:    python3 udp_ref.py burst <client> [port] [mbps] [fps] [secs] [out.json]

Each "frame" is <mbps>/<fps> worth of 1,200-byte datagrams sent back to back;
the sink counts what never came and what came after a later one (how far
behind, in datagrams and in ms), and answers a report at the end.
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


def sink(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    s.bind(("0.0.0.0", port))
    print("sink on", port, flush=True)
    st = None
    while True:
        data, addr = s.recvfrom(4096)
        now = time.perf_counter()
        if data[:6] == b"RESET!":
            st = {"got": 0, "maxSeq": -1, "late": 0, "lateBy": [], "lateMs": [], "dup": 0,
                  "seen": set(), "at": {}}
            continue
        if data[:6] == b"REPORT":
            if st is None:
                s.sendto(b"{}", addr)
                continue
            n = int(data[6:].decode() or 0)
            lb, lm = sorted(st["lateBy"]), sorted(st["lateMs"])
            pick = lambda v, p: v[min(len(v) - 1, int(p * len(v)))] if v else None  # noqa: E731
            rep = {"sent": n, "got": st["got"], "lost": n - len(st["seen"]), "late": st["late"],
                   "dup": st["dup"], "lateByMedian": pick(lb, 0.5),
                   "lateByMax": lb[-1] if lb else None, "lateMsMedian": pick(lm, 0.5),
                   "lateMsMax": lm[-1] if lm else None}
            s.sendto(json.dumps(rep).encode(), addr)
            continue
        if st is None or len(data) < 4:
            continue
        seq = struct.unpack("!I", data[:4])[0]
        st["got"] += 1
        if seq in st["seen"]:
            st["dup"] += 1
            continue
        st["seen"].add(seq)
        st["at"][seq] = now
        if seq < st["maxSeq"]:
            # Overtaken: how many datagrams, and how long after the one that
            # overtook it.
            st["late"] += 1
            st["lateBy"].append(st["maxSeq"] - seq)
            st["lateMs"].append(round((now - st["at"][st["maxSeq"]]) * 1000, 3))
        else:
            st["maxSeq"] = seq


def burst(host, port, mbps, fps, secs, out):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    per = max(1, int(mbps * 1e6 / 8 / fps / 1200))
    pad = bytes(1200 - 4)
    s.sendto(b"RESET!", (host, port))
    time.sleep(0.2)
    seq = 0
    t0 = time.perf_counter()
    nxt = t0
    while time.perf_counter() - t0 < secs:
        for _ in range(per):
            s.sendto(struct.pack("!I", seq) + pad, (host, port))
            seq += 1
        nxt += 1.0 / fps
        while time.perf_counter() < nxt:
            time.sleep(0.0005)
    time.sleep(1.0)
    rep = {}
    for _ in range(3):
        try:
            s.sendto(b"REPORT" + str(seq).encode(), (host, port))
            rep = json.loads(s.recvfrom(65536)[0].decode())
            break
        except (socket.timeout, ValueError):
            continue
    rep.update({"mbps": mbps, "fps": fps, "secs": secs, "perFrame": per})
    print("burst %s Mbit/s at %s fps (%d datagrams a frame) for %s s: %s sent, %s lost, %s late "
          "(by %s datagrams median, %s max; %s ms median, %s max), %s duplicated" % (
              mbps, fps, per, secs, rep.get("sent"), rep.get("lost"), rep.get("late"),
              rep.get("lateByMedian"), rep.get("lateByMax"), rep.get("lateMsMedian"),
              rep.get("lateMsMax"), rep.get("dup")), flush=True)
    if out:
        with open(out, "w") as f:
            json.dump(rep, f)
    return rep


if __name__ == "__main__":
    if sys.argv[1] == "echo":
        echo(int(sys.argv[2]) if len(sys.argv) > 2 else 47998)
    elif sys.argv[1] == "sink":
        sink(int(sys.argv[2]) if len(sys.argv) > 2 else 47999)
    elif sys.argv[1] == "burst":
        a = sys.argv[2:]
        burst(a[0], int(a[1]) if len(a) > 1 else 47999, float(a[2]) if len(a) > 2 else 40,
              float(a[3]) if len(a) > 3 else 120, float(a[4]) if len(a) > 4 else 20,
              a[5] if len(a) > 5 else "")
    else:
        a = sys.argv[2:]
        ping(a[0], int(a[1]) if len(a) > 1 else 47998, int(a[2]) if len(a) > 2 else 50,
             float(a[3]) if len(a) > 3 else 20, a[4] if len(a) > 4 else "")
