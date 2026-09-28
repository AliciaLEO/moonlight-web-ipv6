#!/usr/bin/env python3
"""The rate-control gate (G3, plan pipeline-video-d3d12-v2 section 5), read off
--native-bench CSVs.

usage: rate-report.py <fps> <file.csv> [<file.csv> ...]

For each file, over the pictures that came from a capture (the moving ones):
  - the bitrate over every 2 s window against the target (criterion: +-10 %);
    windows where the target moved, or where the screen was mostly still,
    are left out;
  - frame sizes against the budget (criterion: p95 <= 2x);
    "far over" = the in-house controller's strong overshoot, over 2.5x the
    budget and over the VBV (one frame's worth, a sixtieth of a second's at
    most). The budget is the encoder's own rate for the picture over fps
    (encoder_kbps, a column since 27/09/2026): when the content slows, the
    session scales the encoder's rate to the frames that really come
    (EffectiveCadence, never below 30 of them a second), so target / fps —
    all older CSVs have — is too small there, and a far-over count against it
    is said beside the true one when they differ (the N95's ramp, 8n.9);
  - how many pictures a step of the target takes to be followed (criterion: 3):
    down, the first at or under 1.25x the new budget; up, the first at 0.75x
    or more, or at QP 18 (the content needs no more). Counted from the first
    picture coded at the new rate: the one right after the step is stamped
    with the new target but was coded before the session passed it on;
  - the first moving picture after a still spell (?pause=), against its budget:
    the transition a frame-by-frame rate control gets wrong first;
  - the re-sends of a still screen: how many, what they cost, the QP reached;
  - host_total (present -> encoded), mean and p99, for the latency criterion.
"""
import csv
import sys


def pct(values, q):
    if not values:
        return float("nan")
    s = sorted(values)
    return s[min(len(s) - 1, int(q * (len(s) - 1) + 0.5))]


def mean(values):
    return sum(values) / len(values) if values else float("nan")


def report(path, fps):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        print(f"{path}: empty")
        return
    for r in rows:
        r["bytes"] = int(r["bytes"])
        r["qp"] = int(r["avg_qp"])
        r["captured"] = r["captured"] == "1"
        r["key"] = r["keyframe"] == "1"
        r["t"] = int(r["t3_encoded_us"])
        r["target"] = int(r["target_kbps"])
        held = int(r.get("encoder_kbps") or 0)
        r["held"] = held if held > 0 else r["target"]
        r["budget"] = r["held"] * 1000 / fps / 8  # bytes
        r["vbv"] = r["held"] * 1000 / min(fps, 60) / 8
        r["nominal"] = r["target"] * 1000 / fps / 8
        r["total"] = int(r["host_total_us"])
    has_held = any(int(r.get("encoder_kbps") or 0) > 0 for r in rows)

    start = rows[0]["t"]
    moving = [r for r in rows if r["captured"] and not r["key"] and r["t"] - start >= 1_000_000]
    name = path.replace("\\", "/").split("/")[-1]
    print(f"== {name}: {len(rows)} pictures, {sum(r['captured'] for r in rows)} captured, "
          f"pipeline {rows[0]['pipeline']}")
    print("   budget per picture: " + ("the encoder's own rate (encoder_kbps) / fps" if has_held
                                      else "target / fps (no encoder_kbps: too small where the "
                                           "content slowed)"))

    # 2 s windows.
    inside, windows, worst = 0, 0, []
    t = start + 1_000_000
    end = rows[-1]["t"]
    while t + 2_000_000 <= end:
        span = [r for r in rows if t <= r["t"] < t + 2_000_000]
        t += 2_000_000
        targets = {r["target"] for r in span}
        if len(targets) != 1:
            continue
        captured = sum(r["captured"] for r in span)
        if captured < fps * 2 * 0.5:
            continue
        rate = sum(r["bytes"] for r in span) * 8 / 2 / 1000  # kbps
        ratio = rate / targets.pop()
        windows += 1
        inside += abs(ratio - 1) <= 0.10
        worst.append(ratio)
    if windows:
        print(f"   rate/target over 2 s: {inside}/{windows} windows within 10 % "
              f"(min {min(worst):.2f}, max {max(worst):.2f})")
    else:
        print("   rate/target over 2 s: no window with a steady target and a moving screen")

    # Sizes against the budget.
    ratios = [r["bytes"] / r["budget"] for r in moving]
    far = [r for r in moving if r["bytes"] > 2.5 * r["budget"] and r["bytes"] > r["vbv"]]
    far_nominal = [r for r in moving if r["bytes"] > 2.5 * r["nominal"]
                   and r["bytes"] > r["nominal"] * fps / min(fps, 60)]
    beside = (f" ({len(far_nominal)} against target / fps)"
              if has_held and len(far_nominal) != len(far) else "")
    print(f"   size/budget (moving): mean {mean(ratios):.2f}, p95 {pct(ratios, 0.95):.2f}, "
          f"max {max(ratios, default=float('nan')):.2f}; far over: {len(far)}{beside}")
    qps = [r["qp"] for r in moving if r["qp"] >= 0]
    if qps:
        print(f"   QP (moving): mean {mean(qps):.1f}, p5 {pct(qps, 0.05)}, p95 {pct(qps, 0.95)}, "
              f"at 18: {sum(q == 18 for q in qps)}/{len(qps)}")

    # Steps of the target.
    steps = []
    for i in range(1, len(rows)):
        if rows[i]["target"] == rows[i - 1]["target"]:
            continue
        up = rows[i]["target"] > rows[i - 1]["target"]
        # From the first picture the encoder coded at a new rate; a rate it
        # never took (a driver that refuses the change) leaves the step as is.
        first = i
        if has_held:
            for j in range(i, min(len(rows), i + 30)):
                if rows[j]["target"] != rows[i]["target"]:
                    break
                if rows[j]["held"] != rows[i - 1]["held"]:
                    first = j
                    break
        taken = None
        k = 0
        for r in rows[first:]:
            if not r["captured"] or r["key"]:
                continue
            k += 1
            ok = (r["bytes"] >= 0.75 * r["budget"] or r["qp"] == 18) if up \
                else r["bytes"] <= 1.25 * r["budget"]
            if ok:
                taken = k
                break
            if k >= 30:
                break
        steps.append((up, taken))
    if steps:
        followed = [s for s in steps if s[1] is not None and s[1] <= 3]
        detail = " ".join(f"{'up' if u else 'down'}:{k if k is not None else '-'}" for u, k in steps)
        print(f"   steps followed in 3 pictures: {len(followed)}/{len(steps)}  ({detail})")

    # The first moving picture after a still spell.
    after, after_nominal = [], []
    last_captured = None
    for r in rows:
        if not r["captured"]:
            continue
        if last_captured is not None and r["t"] - last_captured["t"] >= 500_000 and not r["key"]:
            after.append(r["bytes"] / r["budget"])
            after_nominal.append(r["bytes"] / r["nominal"])
        last_captured = r
    if after:
        line = (f"   first picture after a still spell: {len(after)} spells, size/budget "
                f"{' '.join(f'{a:.1f}' for a in after[:12])}")
        if has_held and any(abs(a - b) > 0.05 for a, b in zip(after, after_nominal)):
            line += f" (against target / fps: {' '.join(f'{a:.1f}' for a in after_nominal[:12])})"
        print(line)

    # Re-sends.
    resent = [r for r in rows if not r["captured"]]
    if resent:
        qps = [r["qp"] for r in resent if r["qp"] >= 0]
        at18 = [r for r in resent if r["qp"] == 18]
        print(f"   re-sends: {len(resent)}, {sum(r['bytes'] for r in resent) // 1024} KB, "
              f"QP {min(qps) if qps else '-'}..{max(qps) if qps else '-'}; "
              f"at QP 18: {len(at18)}, {mean([r['bytes'] for r in at18]) / 1024 if at18 else 0:.1f} KB each")

    totals = [r["total"] / 1000 for r in rows if r["captured"]]
    print(f"   host_total ms (captured): mean {mean(totals):.2f}, p99 {pct(totals, 0.99):.2f}")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    fps = int(sys.argv[1])
    for path in sys.argv[2:]:
        report(path, fps)


if __name__ == "__main__":
    main()
