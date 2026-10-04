# Network latency findings

A running record of what has been observed about the network's share of
MoonlightWeb's latency: the WebRTC DataChannel (SCTP over DTLS over UDP, through
libdatachannel and usrsctp), its buffers and congestion window, the browser's
receive side, Wi-Fi against Ethernet. It keeps the raw numbers, how they were
measured, what was tried and failed and why, and the open questions, so that a
later study can start from here without the plans or the chat that produced
them.

Entries are dated (DD/MM/YYYY). The newest work is appended at the end of each
section; nothing is rewritten after the fact except to mark it superseded.
Times are local (Europe/Paris).

Sources: the plan « Wi-Fi : la vidéo qui attend dans SCTP »
(`wifi-sctp-descente.md`, outside the repo), its bench `scripts/bench/wifi/`
(README holds the tables), the Ultra LAN POC (`docs/design/ultra-lan-poc.md`
§6), the native host's design (`docs/design/native-capture-encoder.md`) and
bench (`docs/bench-native-host.md`).

## 1. The transport, as it is

What the numbers below were measured against (code as of 04/10/2026).

- **Video** goes over a WebRTC DataChannel, **ordered**, with
  `maxPacketLifeTime` 500 ms (`DataChannelRelay::createDataChannels`,
  `kVideoFrameLifetimeMs`; PR-SCTP timed reliability). Unordered was tried and
  removed: every reordering looked to the client like a hole in `frameId`,
  which invalidated the reference and asked for an IDR (the comment in
  `createDataChannels` tells it).
- Each encoded frame is cut by the host into **messages of up to 16,000 bytes
  of payload + a 17-byte header** (`FrameSender::buildFragments`), each sent
  as one DataChannel message. usrsctp cuts each message into DATA chunks of
  about one MTU: libdatachannel's MTU is fixed at 1,280 bytes, so a 16 KB
  message is ~13 packets.
- **The input channel** (id 2, reliable, ordered, JSON) shares the same SCTP
  association: pongs, cursor shapes, rumble, clipboard, link stats. The host's
  messages on it wait behind the video (see §3, W0/W1).
- **libdatachannel's settings** (`SctpTransport::SetSettings`, unless the
  host overrides them): send buffer 1 MiB sysctl (but see the 256 KiB
  finding below), receive buffer 1 MiB, initial cwnd 10 MTU, **max burst 10
  MTU**, congestion control RFC 2581 (module 0), delayed SACK 20 ms, RTO min
  200 ms, max chunks on queue 10 K, local max message size 256 KiB.
- **The host's own send path**: a sender thread (`FrameSender`) with a queue
  of at most 2 frames (a third evicts the oldest delta); `bufferedAmount` (what
  libdatachannel holds after usrsctp refused it) watched by `SendBacklog`: a
  delta is dropped only after the buffer stayed above its "drained" floor
  (50 ms of bitrate, 8-48 KB) for **250 ms** (`kToleranceMs`). After a drop,
  without named drops or ride-out, the relay closes an "awaiting IDR" gate and
  drops every delta until a keyframe (`gatedDelta`).
- **The native host's rate governor** (`encode::RateGovernor`): cuts 20 % on
  overuse (receiver's one-way-delay rise ≥ 30 ms, min-based; frame gaps;
  sender evictions; since 03/10/2026 SCTP retransmissions ≥ 3 ‰ in a report
  window on the Windows host), holds 2 s, raises after 3 s quiet. The client
  sends a `linkstats` report every 500 ms.
- **The browser** (Chrome) runs its own SCTP stack (dcsctp) behind a UDP
  socket whose receive buffer is small (see W1 bis: a 64 KB sink reproduces
  its drops).

## 2. Tools and method

- **Click → flag** (`frontend/js/stream/LatencyProbe.js`, host
  `backend/src/LatencyFlag.cpp`): the client clicks, the host raises a flag on
  screen, the client times until it sees it. The whole loop the player feels.
  Split by `inputstamp` into **up** (client → host injection), **rest**
  (everything after).
- **Content age** (`scripts/bench/content-age/`, `scroll.html?band=time`): the
  age of what is on screen, read from a time band. ⚠️ The probe that reads the
  band delayed the frames it read by 4-13 ms on the main thread until
  `b83f3dac` (03/10, moved to a worker); absolute ages before that are high by
  about that much (`f69ed657`, POC §6.1).
- **Per-frame log** (`frontend/js/stream/FrameLog.js`, POC U0.2 `ea3d313c`):
  every drawn frame's capture → arrival → decode → draw on the host's clock
  (via ping/pong). `e2e` = capture → draw; `down` = capture → last chunk
  arrived.
- **Relay frame log** (`relaylog=1`, `backend/src/streaming/RelayFrameLog.h`,
  `5ce2228c`): each video frame's way through the relay — capture on the
  host's steady clock, the decision (sent, backlog drop, gated, evicted), first
  and last fragment handed to the DataChannel, `bufferedAmount` before and
  after, usrsctp's retransmission counters, SCTP's smoothed RTT.
- **`flagpath.py`** joins the three, click by click, into legs that add up to
  the measured click: up, inject, raise (flag raised), toCap (to the capture
  that shows it), encode, send (sender queue), **net** (last fragment into the
  DataChannel → last chunk arrived on the client), decode, draw, detect.
  **inSctp** = net − srtt/2: the part of `net` spent before leaving usrsctp
  (an estimate: it assumes the air takes half the SCTP RTT).
- **usrsctp's global counters** (`mw::sctp::readCounters`,
  `SctpCounters.cpp`; process-wide, one session per worker): sent,
  retransmitted (fast, T3), fast-retransmit inside a recovery, SACKs, sends
  held by the window (`sctps_send_cwnd_avoid`), windows trimmed to max burst
  (`sctps_maxburstqueued`). The session ends with `SCTP this session: …` and
  `SCTP window this session: …`.
- **Kernel drops on the Mac client**: `netstat -s -p udp`, "dropped due to
  full socket buffers", read before and after each pass by `series.py`.
- **`udp_ref.py`**: a bare UDP ping to an echo on the client beside a pass,
  and video-shaped UDP bursts to a sink with a chosen receive buffer.
- **Series**: `scripts/bench/wifi/series.py` (one client's passes, alternated
  rounds), `report.py` (one row per pass), outputs in `bench-out/wifi/` and
  `bench-out/content-age/` (not in the repo).
- **Bench machines**: host DualRTX (Arc A380 renders the virtual display at
  240 Hz; RTX and an AMD iGPU also available). Clients: Mac M1 (Wi-Fi,
  120 Hz), N95 mini-PC (Wi-Fi, 60 Hz), UM790Pro under Windows (Ethernet 1 GbE,
  120 Hz, the witness), iPhone (Safari, Wi-Fi, by hand).
- Unless said, "two rounds" means the variants alternated A B A B (or A B C A
  B C), one pass each, the page scrolling at 240 fps, 60 clicks a pass; tables
  give the mean of the rounds.

## 3. Findings, by date

### 06/08/2026 — Periodic stutter on a Mac: AWDL, not the stream

Micro-stutters seen only on the Mac (Wi-Fi 5, host on Ethernet) came from
AirDrop/AWDL. `ping -i 0.1` Mac → host: one spike every 5 packets exactly, a
~500 ms period, 28 → 74 ms amplitude growing ~4 ms a cycle, on a 4.6 ms base.
AirDrop off → no stutter. Moonlight-Qt and Parsec stuttered the same way.
Lesson: a metronomic disturbance is the radio leaving the channel; congestion
is random. Ask for the ping before touching the code.

### 17/09/2026 — Corporate Wi-Fi: a freeze the stream never caught up

Heavy lag on a corporate Wi-Fi/firewall (Mac, iPhone; the same iPhone on 5G:
no freeze in 3 min). The stats showed ~20 ms latency, blind to the transport's
queue.
- After a timeout usrsctp drops to cwnd = 1 MTU, ssthresh = max(cwnd/2,
  4 MTU), then +1 MTU per RTT: the host's buffer (304 KB visible + 256 KB in
  usrsctp) drained at ~2 Mbit/s for 2 s on a 20 Mbit/s link.
- The video channel was ordered with `maxRetransmits 3`: everything in flight
  at the freeze was retransmitted in order when the link came back, before the
  keyframe; the client decoded second-old frames.
- The IDR cooldown doubled at each keyframe our own guard dropped.
- Fixed (`509bd9c2`, `9bb268d6`, `dfe4e0ee`, `92772341`, `80d73021`):
  `maxPacketLifeTime` 500 ms instead of a retransmission count, keyframe asked
  when the buffer drains, fast raise to the proven rate, "link freezes" line.
- A second blind spot: usrsctp's fixed 256 KiB hid 1 s of video at 2 Mbit/s
  (iPhone capture: "LINK QUEUE 933 ms, freezes 0"). "Fixed" by `fe359154`:
  send buffer = 100 ms of the set bitrate, 64-256 KiB. **⚠️ That fix never
  took effect: see 03/10/2026, the 256 KiB SO_SNDBUF.**

### 25/09/2026 — A click tail shaped like a T3 timeout

One click in ten ~205 ms late, the shape of a T3 retransmission timeout at
usrsctp's 200 ms minimum RTO: a lone flag frame whose last packet is lost has
nothing behind it to trigger a fast retransmit. Bench knobs added, never
settings: `MW_SCTP_RTO_MIN_MS`, `MW_SCTP_SACK_DELAY_MS` (`applySctpSettings`).
Not followed up with an A/B recorded here.

### 01-03/10/2026 — Loss lab and congestion modules (plan Punktfunk, A0)

`4815aabd`: bench keys `loss=` (video messages thrown away before SCTP),
`burst=`, `sctpcc=0..3` (usrsctp congestion control: RFC 2581, HSTCP, H-TCP,
RTCC), `flood=` (useless traffic on channel 3). RTCC (`sctpcc=3`), A/B
alternated on the N95 in Wi-Fi: retransmissions halved, click unchanged (88-94
against 88-100 ms). Set aside.

### 03/10/2026 — T7 (radios plan): the click's surplus is on the way down

| Client | Click → flag | Up | Rest | SCTP retransmissions |
|---|---|---|---|---|
| Mac M1, Wi-Fi, page at 240 | 71.4 | 2.6 | 69.2 | 0.6 % |
| N95, Wi-Fi, page at 240 | 88-100 | 4-8 | 80-90 | 0.2-0.5 % |
| UM790Pro Windows, Ethernet | 34.6 | 1.6 | 32.6 | 0 |
| Still screen (all) | 27-37 | 1.5-2.6 | 25-35 | ~0 |

(ms.) The click goes up fast; a bare UDP ping on the same Wi-Fi does its round
trip in 4.7 ms (p99 13.8). The host's small messages on the input channel took
12-37 ms at the median, 387 ms at p90 on the Mac. `bufferedAmount` reached
142 KB. Hypothesis then: Wi-Fi loses or reorders chunks, usrsctp's loss-based
window brakes, everything waits behind.

### 03/10/2026 — W0: the baseline (10:25-11:08)

Host Arc, Auto with detection, two rounds per cell (`report.py w0`), ms:

| Client | Content | Click | Up | Rest | Shown age | Frame age med / mean / p90 | Retr. | Messages (p90) | UDP ping (p99) |
|---|---|---|---|---|---|---|---|---|---|
| Mac M1, Wi-Fi | page at 240 | 71.1 | 3.3 | 67.1 | 37.9 | 20.7 / 33.1 / 63.2 | 0.72 % | 34.6 (225) | 4.6 (13.5) |
| | game 75-83 | 57.3 | 2.3 | 54.7 | 47.9 | 37.5 / 41.7 / 69.7 | 0.71 % | 68.7 (455) | 5.1 (12.3) |
| N95, Wi-Fi | page at 240 | 99.4 | 5.6 | 90.7 | 56.6 | 35.0 / 47.6 / 76.4 | 0.57 % | 34.7 (269) | |
| | game 75-83 | 93.5 | 5.9 | 85.1 | 54.7 | 38.5 / 47.9 / 84.1 | 0.21 % | 25.6 (67) | |
| UM790Pro, Ethernet | page at 240 | 33.6 | 1.7 | 31.9 | 24.0 | 12.6 / 13.0 / 17.2 | 0 | 4.1 (9) | |
| | game 75-83 | 34.2 | 1.6 | 31.8 | 28.2 | 16.6 / 17.5 / 25.6 | 0 | 5.2 (14) | |

- Wi-Fi's cost is in the tail: the median frame age on the Mac is barely
  above Ethernet's, the mean and p90 are not.
- 13-20 link freezes a pass on the Mac (longest 1.2-3.3 s), 1-3 on the N95,
  none on Ethernet. Frames dropped by the host: Mac 45-109/min, N95 30-39.
  `bufferedAmount` up to 1.3 MB on the Mac.
- The gate: after 250 ms of full queue the relay drops a delta; the Arc has no
  named drops (oneVPL, see below) and the Mac cannot decode through a hole, so
  it awaits a keyframe and drops every delta until then (`gatedDelta`,
  231-322 a pass on the Mac at T7).

### 03/10/2026 — W1: where the flag's frame waits (13:15-14:15)

`relaylog=1`, `flagpath.py`, ~230 clicks per client, medians in ms:

| Leg | Mac M1, Wi-Fi | N95, Wi-Fi | UM790Pro, Ethernet |
|---|---|---|---|
| **Click → flag** | **63.8** (mean 69.8) | **91.9** (99.1) | **36.8** (38.6) |
| up | 2.7 | 3.4 | 1.8 |
| injected → flag raised | 11.0 | 10.3 | 10.4 |
| raised → capture showing it | 4.3 | 8.3 | 4.7 |
| encode | 4.0 | 3.7 | 4.2 |
| sender queue | 0.1 | 0.2 | 0.1 |
| **net** (into DataChannel → arrived) | **26.6** | **34.2** | **6.9** |
| … of which above srtt/2 (`inSctp`) | 23.2 | 24.6 | 4.5 |
| SCTP srtt | 6 | 16 | 4 |
| decode | 5.5 | 3.7 | 5.0 |
| draw | 7.3 | 16.0 | 3.4 |

- The flag's frame waits inside usrsctp before leaving. The Mac's surplus over
  Ethernet (+27 ms) is +20 in `net`, of which +19 before leaving usrsctp; the
  air (srtt/2) takes +1.
- 650-760 "losses inside a recovery" a pass on the Mac, 120-130 on the N95, 0
  on Ethernet. `bufferedAmount` was empty for the flag's frame in 82 % of Mac
  clicks: the wait is in usrsctp's buffer, invisible to the host.
- Keyframe gate: 0.9 % of Mac clicks (143 ms each). Head-of-line blocking
  behind a retransmitted chunk: 9 % of Mac clicks, +8 ms of `net`, ~1 ms on
  average.
- **The radio alone loses nothing**: UDP bursts host → Mac, 45 Mbit/s at 120
  then 240 packets/s, 20 s each, no stream: 185,000 datagrams, 0 lost, 0
  reordered.
- `sctps_slowpath_sack` is useless for counting gap SACKs: with PR-SCTP almost
  every SACK takes the slow path. "Sends held by the window" are high
  everywhere, Ethernet included (28-31 K a pass).

### 03/10/2026 — W1 bis: the "losses" are the browser's full UDP socket (14:40-14:52)

Mac's kernel counter "dropped due to full socket buffers", read around each
pass; UDP bursts host → a sink on the Mac, 45 Mbit/s in packets of 120
frames/s, 15 s:

| Run | Kernel drops | SCTP retransmissions | Wait in usrsctp | Click | Messages (p90) |
|---|---|---|---|---|---|
| UDP burst, sink buffer 4 MB | 0 (1 lost of 70,200) | — | — | — | — |
| UDP burst, 256 KB | 0 (1 lost) | — | — | — | — |
| UDP burst, 64 KB | **1,073** (1.5 %) | — | — | — | — |
| Stream, page at 240, Auto (~35 Mbit/s) | **1,855** | 3,120 (0.60 %) | 27.7 ms | 74.0 ms | 53 (444) |
| Stream, game, Auto (~41 Mbit/s) | **2,303** | 3,960 (0.66 %) | 20.5 ms | 58.6 ms | 25 (338) |
| Stream, page at 240, **20 Mbit/s fixed** | **213** | 279 (0.15 %) | 9.1 ms | **48.5 ms** | 16.5 (18) |

- What SCTP repairs on the Mac's Wi-Fi is, for the most part, datagrams the
  Mac's kernel throws away because the receive buffer of Chrome's UDP socket
  overflows. Wi-Fi delivers in aggregates; a 64 KB buffer reproduces it, 256 KB
  does not.
- Each drop cuts usrsctp's window, and the frames wait. At a lower bitrate the
  drops fall ninefold, the wait in usrsctp from 28 to 9 ms, the click from 74
  to 48.5 ms.
- N95 (Windows client): "received errors" for UDP at 0 since boot, IPv4 and
  IPv6. Whether Windows counts a full socket there is unknown: inconclusive.
- Not investigated: Chrome's own counters (`webrtc-internals`), whether Chrome
  sets SO_RCVBUF on its SCTP socket and to what.

### 03/10/2026 — W2 A: pacing the host's sends (failed, kept as a bench key)

`pace=<n>` (`b2b52486`, `SendPacer.h`): a frame's chunks handed to SCTP at n
times the stream's bitrate, 16 KB at a time, with a high-resolution waitable
timer. Mac in Wi-Fi, two rounds:

| | Click | Kernel drops | Retr. | Frame age med / p90 | Dropped /min | `bufferedAmount` > 1 frame |
|---|---|---|---|---|---|---|
| no pacing | 74.9 ms | 2,115 | 0.74 % | 20.6 / 51 | 110 | 6.0 % |
| `pace=4` | 85.0 ms | 2,540 | 0.79 % | 23.8 / 60 | 75 | 6.8 % |
| `pace=2` | 79.7 ms | 2,065 | 0.70 % | 15.5 / 24 | 48 | 2.8 % |

- **Why it failed**: pacing does not bring the drops down. Chrome's socket
  overflows when Chrome reads late (at 42 Mbit/s a 64 KB buffer holds
  ~12 ms), not under the host's bursts. Only a lower bitrate did.
- N95: no measurable effect (0.40/0.33 % retransmissions without, 0.39/0.24 %
  with). Local witness: +0.6-0.8 ms of send time.
- `pace=2` halved the p90 of a frame's age with no click gain; combined later
  with B it added nothing.

### 03/10/2026 — W2 B: the rate follows SCTP's retransmissions (kept: the Windows default)

`retrcut=<‰>` (`e0324f4e`): the relay adds to each link report the share of
SCTP chunks sent again since the previous one (`LinkFeedback::retransPermille`);
at the threshold or above the governor cuts 20 %, and calls the link quiet
only under half of it. Why it was needed: the governor's delay rise is a
minimum over its window, and on the Mac a few frames always arrive fast while
the rest wait ~20 ms in usrsctp — the rise never reached 30 ms.

N95, Wi-Fi (18:03-18:32, two rounds): click 109.2 → 100.2 ms (p90 177 → 144),
`net` of the flag 40.7 → 34.8, all frames' way down 27.8 → 22.0 ms, received
11.0 → 10.6 Mbit/s, retransmissions 0.31 → 0.32 %.

Mac, Wi-Fi (22:29-22:47, two rounds):

| | Click (p90) | Kernel drops | Retr. | Frame age med / p90 | Messages p90 | Dropped /min | Received |
|---|---|---|---|---|---|---|---|
| without | 73.6 ms (112) | 1,016 | 0.62 % | 20.3 / 86 | ~330 ms | 182 | 23.1 Mbit/s |
| `retrcut=3` | **62.6 ms (86)** | **366** | 0.27 % | 14.8 / **29** | **35 ms** | **34** | 20.0 Mbit/s |
| `retrcut=3,pace=2` (1 pass) | 61.5 ms (86) | 513 | 0.34 % | 15.7 / 72 | 37 ms | 35 | 17.5 Mbit/s |

The governor cut 5-10 times a pass and came back 2-6 times to the last good
rate: it held ~20-30 Mbit/s of the 60 set. ~18 ms remained in usrsctp
(`inSctp` 17.9 against 4.5 on Ethernet).

Ethernet witness (UM790Pro, 23:25-23:46, two rounds): click 37.1 / 42.4 ms
without, 41.3 / 36.7 with (means 39.8 / 39.0). SCTP sent 0-8 chunks again in a
whole session. One cut: all 8 retransmissions of a session fell in one 500 ms
report window (at 30 Mbit/s, 3 ‰ is ~5 chunks), 40 → 32 Mbit/s for 3.5 s.
Judged acceptable; a floor in chunks would not have filtered it (8), "two
windows in a row" would, but would change what was measured on the Mac.

**Decision (Bruno, 03/10)**: the Windows host's default, `55dd9cde`
(`RateGovernor::kRetransCutPermille` = 3; `retrcut=0` turns it off). Linux and
macOS hosts unchanged; GameStream relays have no governor.

### 03/10/2026 — usrsctp's send buffer has always been 256 KiB

Read in libdatachannel (`src/impl/sctptransport.cpp`, the `SctpTransport`
constructor): after reading the `sctp_sendspace` sysctl, it raises
`SO_SNDBUF` to its largest message, `Configuration::maxMessageSize`
(`DEFAULT_LOCAL_MAX_MESSAGE_SIZE`, 256 KiB unset). usrsctp's `SO_SNDBUF`
setsockopt is `sbreserve(&so->so_snd, …)` (`user_socket.c`). So every session
has had **256 KiB, whatever the bitrate**: `SendBacklog::sendBufferBytesFor`
(100 ms of bitrate, 64-256 KiB, `fe359154`, 17/09) never took. At 20 Mbit/s
that hides ~100 ms, at 2 Mbit/s a second. The comment in `applySctpSettings`
says so since `dc7f9c72`; the product is unchanged. The only way down is a
smaller `maxMessageSize` on the peer connection, which also caps what either
side may send in one message (both ways: the clipboard can reach 256 KB).

usrsctp's send buffer holds both data not yet sent and data sent but not yet
acked (`total_output_queue_size` against `SCTP_SB_LIMIT_SND`), so it caps the
data in flight too.

### 03/10-04/10/2026 — W2 C: a small usrsctp buffer and the picture held (failed, kept as bench keys)

`sctpbuf=<KB>` (`dc7f9c72`): the real usrsctp buffer, through
`maxMessageSize`. `linkhold=<ms>` (`a511bdd3`; Windows host): the capture loop
holds its picture unencoded once the video channel's `bufferedAmount` has
stayed above zero that long, and sends the freshest once it drained — the
`cadence=host-guarded` hold, asked of the relay through
`Session::setLinkBusyProbe`. No frame thrown away, so no keyframe.

Ethernet (UM790Pro, 23:32-23:54):
- First version, holding at the first byte (`linkhold=1` as a switch), with
  `sctpbuf=48`: **87 fps instead of 223**. 44 % of frames overflowed usrsctp
  for a millisecond or two; held frames came out bigger (34 KB median against
  14) and overflowed again. Throughput unchanged (29 Mbit/s).
- Holding after a 4 ms backlog: 233 fps at 48 KiB, 225 at 32; frame age
  11.2 / 10.8 ms (unchanged); `net` 7.8 / 7.3 ms (unchanged).

Mac, Wi-Fi (04/10 23:56-00:17, two rounds, B already the default):

| | Click (p90) | Frame age med / p90 | fps | Repeats /min | Kernel drops | Retr. | Received | Held /s |
|---|---|---|---|---|---|---|---|---|
| B alone | **63.6 ms (78)** | 15.1 / 26 | 120 | 1,141 | 472 | 0.36 % | 21 Mbit/s | — |
| `sctpbuf=48,linkhold=4` | 83.1 ms (134) | 25.7 / 79 | 31 | 4,524 | 0 | 0 | 25 Mbit/s | 107 |
| `sctpbuf=32,linkhold=4` | 77.7 ms (141) | 30.8 / 81 | 31 | 4,765 | 0 | 0 | 17 Mbit/s | 78 |

- **Why it failed**: the ~18 ms "inside usrsctp" is not a queue of frames that
  could be cut. A byte stays ~16 ms in usrsctp's buffer between hand-over and
  ack on this Wi-Fi, even with no loss at all, so the buffer caps the
  throughput at about buffer / 16 ms (48 KiB → ~25 Mbit/s, 32 KiB →
  ~17 Mbit/s). With no loss B never cuts, the encoder aims at 30+ Mbit/s, the
  excess spills into `bufferedAmount` (up to 560 KB) and the hold turns it
  into lost frame rate. `inSctp` rose to 25-26 ms instead of falling.
- **0 kernel drops** at 48 and 32 KiB: with that little in flight, Chrome's
  64 KB socket never overflows. Confirms W1 bis (the drops come from what is in
  flight when Chrome reads late), at too high a price.

### 03/10/2026 — Ultra LAN POC (U0.2, U0.3): usrsctp holds the video at 120 fps

From the POC session (`docs/design/ultra-lan-poc.md` §6). Host DualRTX;
D = Auto with detection (the product), U = 120 fps asked, virtual display at
240 Hz, tearing. `relaylog=1` + `flagpath.py`; p90 of `inSctp` over every frame
of the clicks' minute:

| Client | Link | D | U (120 fps) | Ref. |
|---|---|---|---|---|
| N95 | Wi-Fi | 35-54 ms | 186-232 ms (`net` p90 204-263) | POC §6.3, `8fb7c380` |
| UM790Pro | Ethernet 1 GbE | 5-12 ms | 5-12 ms (`net` p90 7-15) | `0ac463e7` |
| Mac M1 | Wi-Fi | 50-67 ms | 55-79 ms (`net` p90 57-84) | `923c7c1b` |

- N95 in U: the air (half the SRTT) takes 24-28 ms at p90, no SCTP
  retransmission, the decoder adds ~15 ms (2-3 → 18-22 ms on the flag's
  frame). The congestion window holds the video at 120 fps, as in W1. Effects
  (20 passes, 15:57-16:50, `94990337`): link cuts of 3-16 s, rate down from 7
  to 5 Mbit/s, 41-82 fps delivered, shown age 130-320 ms (D: 45-58), more than
  one click in two without a flag (D: 83-94 ms). The detection in D tried
  116 fps and went back to 58 (decode queue filling, capture → paint
  > 189 ms). AV1 in U: 0.9-1.1 s decode, cuts up to 31 s.
- Mac holds 120 fps without dropping out: cuts frequent but short (7-14 a
  pass, longest 1.2-3.5 s), click 61-75 ms in D and U alike.
- UM790Pro on Ethernet: no cut, 24-47 Mbit/s; the detection climbs to
  230-240 fps and stays.
- iPhone (Safari, Wi-Fi, 21:26-21:37, `3b04ba8b`): network 4.5-17 ms (40 once,
  during a 368 ms freeze), 1-2 freezes of 0.33-0.73 s a pass. **"Frames
  dropped (jitter)" 38-47 % in both modes while "frames lost (network)" is
  0.00 %** — maybe the counter under Safari: to check. On iOS the `--dev` at
  its LAN address fails: Safari does not extend the page's certificate
  exception to the signalling WebSocket.
- A pass's end-of-pass "Measured" line read 42 ms while the pass median was
  315 ms: it is a sliding window. Use the per-frame log for pass statistics.

### 04/10/2026 — W2.5: usrsctp's max burst (a clear gain on the Mac)

`sctpburst=<n>` (`01717368`): usrsctp's `sctp_max_burst_default` sysctl, set
with libdatachannel's settings before the peer connection; libdatachannel's
own is 10 packets. Mac, Wi-Fi, 05:34-05:55, two alternated rounds, B already
the default (`build\` `01717368`). `inSctp` here is over every frame of the
clicks' minute (`inSAll`), the click's leg over the flags' frames:

| | Click (p90) | `net` of the flag | `inSctp` all frames | Frame age med / p90 | Repeats /min | Messages med / p90 | Kernel drops | Retr. | Trimmed to max burst |
|---|---|---|---|---|---|---|---|---|---|
| base (10) | 66.9 ms (98) | 20.5 ms | 15.5 ms | 14.7 / 25.4 | 1,169 | 24.5 / 37.3 | 422 | 0.31 % | 7,156-7,494 |
| `sctpburst=0` (no limit) | **58.5 ms (69)** | **14.2 ms** | **9.4 ms** | 12.2 / 17.5 | **417** | 23.6 / **25.4** | 547 | 0.42 % | 0 |
| `sctpburst=32` | 60.6 ms (76) | 20.6 ms | 11.8 ms | 13.4 / 21.5 | 780 | 24.4 / 35.7 | 409 | 0.29 % | 3,414-3,880 |

- **The max burst was a third of what remained in usrsctp.** With no limit,
  each frame spends ~6 ms less inside usrsctp, the click gains 8 ms at the
  median and ~30 ms at p90, the repeats fall to a third.
- "Sends held by the window" fell from ~19-21 K to ~3-4 K a pass: most of what
  that counter showed was the burst limit, not the congestion window.
- Kernel drops rose a little (422 → 547 on average, one pass at 639), and the
  retransmissions with them (0.31 → 0.42 %): a bigger burst lands on Chrome's
  socket at once. B absorbs it.
- Still ~9 ms in usrsctp against ~4.5 on Ethernet (W1): the SACK clock.

N95, Wi-Fi (10:01-10:30, two alternated rounds, same build): **no gain**.

| | Click (p90) | `net` / `inSctp` of the flag | `inSctp` all frames | Frame age med / p90 | Messages med | T3 |
|---|---|---|---|---|---|---|
| base (10) | 80.0 / 87.0 ms (146 / 152) | 22.4 / 14.8 ms | 12.0 ms | 24.6 / 86, 31.6 / 202 | 26.4 ms | 0, 1 |
| `sctpburst=0` | 80.2 / 95.5 ms (125 / 166) | 26.5 / 19.4 ms | 9.6 / 14.3 ms | 22.6 / 187, 19.9 / 76 | 17.6 ms | 1, 3 |

- At ~6 Mbit/s and 60 fps an N95 frame is ~10 packets, so a burst of 10
  rarely holds it back; the Mac's gain (120 fps, ~20 Mbit/s) does not carry
  over. The click moves within the N95's pass-to-pass noise (80-95 ms); the
  host's messages get faster (26 → 18 ms median).

Ethernet witness (UM790Pro, 10:44-11:05, two alternated rounds, same build,
with `sctpss=4` as a third arm): **no regression, a gain**.

| | Click | Frame age med / p90 | `inSctp` flag / all frames | fps | Repeats /min | Messages med / p90 | Sends held by the window |
|---|---|---|---|---|---|---|---|
| base (10) | 38.7 / 37.7 ms | 14.6 / 26.0, 11.4 / 16.2 | 4.1-4.8 / 2.8-3.3 ms | 120, 239 | 2,473, 462 | 5.6 / 14.5, 4.3 / 10.1 | ~29-30 K |
| `sctpburst=0` | 34.4 / 39.0 ms | 8.1 / 10.6, 8.7 / 10.8 | 2.1-2.7 / 0-0.7 ms | 239, 239 | 141, 198 | 3.3 / 6.3, 3.6 / 6.8 | 26 |
| `sctpss=4` | 36.8 / 38.7 ms | 9.9 / 13.5, 11.4 / 17.5 | 4.5-5.1 / 2.5-2.8 ms | 239, 239 | 621, 597 | 3.6 / 7.1, 4.5 / 11.6 | ~27 K |

- Even on Ethernet the burst limit cost 2-3 ms a frame inside usrsctp; with
  no limit almost nothing waits there. The first base pass stayed at 120 fps
  (Auto's own pick); the others ran at 239.
- **Decision (Bruno, 04/10 ~10:45): no max burst is the Windows native
  host's default, `2ef56bfe`** (`DataChannelRelay::kNativeSctpMaxBurst` = 0;
  `sctpburst=10` restores libdatachannel's). Native sessions now always hand
  the relay their link settings; GameStream relays and the Linux and macOS
  hosts keep 10.

### 04/10/2026 — W2.3: usrsctp's stream scheduler (no effect; one module breaks the association)

The host's small messages on the input channel (pongs, cursor, rumble) took
~25 ms round trip at the median and 34-53 at p90 on the Mac with B, against a
7 ms bare UDP ping. libdatachannel exposes no per-stream priority and keeps
the socket `SCTP_SS_VALUE` would need; the scheduler's module, though, is a
sysctl (`sctp_default_ss_module`) the socket copies when it is made.
`sctpss=<0..5>` (`0c21bd5a`) sets it right after the peer connection (whose
`usrsctp_init` resets the sysctls) and before the SCTP transport is made.
Mac, Wi-Fi, 06:00-06:16, two alternated rounds, B the default:

| | Click | Message round trip med / p90 |
|---|---|---|
| base (module 0) | 67.3 / 61.4 ms | 25.3 / 52.6, 24.5 / 33.9 |
| `sctpss=4` fair bandwidth (shortest pending message first) | 66.0 / 61.2 ms | 24.9 / 49.9, 24.6 / 38.9 |
| `sctpss=2` round robin by packet | — | — |

- **Fair bandwidth changes nothing**: the log confirms it was applied, and the
  messages kept their times. They do not wait in usrsctp's stream queues; the
  wait is elsewhere (in flight behind the video, the SACK clock). What did help
  them was W2.5's burst (p90 37 → 25 ms).
- **Round robin by packet breaks the association**, both rounds: the video
  channel opened, then the peer connection failed ~8 s later, with 98 and 360
  of ~5,200 chunks retransmitted and 1 T3 each. Never use module 2 against
  Chrome. Not investigated further (a guess: fragments of different streams'
  messages interleaved without I-DATA, which Chrome's dcsctp cannot
  reassemble).

### 04/10/2026 — POC Ultra: the iPad under RE9 (from session 9b)

Bruno's iPad, Safari, Wi-Fi, streaming the RTX's screen (NVENC) under RE9,
19:17-19:36; the `--dev` `ded56fc6`, joined through stream.dev, so with
`retrcut=3` and `sctpburst=0` as defaults (`24509762`, POC doc §6.3).
- Network leg of the latency detail: 13.8 ms (50 fps), 19.5 ms (62 fps),
  2.3 ms (26 fps). "Link queue": 11, 8, 3 ms. "Link freezes": 2 of 0.91 s,
  2 of 0.51 s, then none.
- **"Frames dropped (jitter)" 44-45 % at 50-62 fps against 2.7 % at 26 fps,
  with "Frames lost (network)" at 0.00 % everywhere** (iPhone, 03/10:
  38-47 %). The counter follows the received rate and the decoder (16-22 ms a
  frame at 50-62 fps, 8 ms at 26), not a loss on the link. To check in the
  counter's code.
- Safari on the iPad reported a refresh of 32 to 51 Hz with Low Power Mode
  off; Auto followed it (26 fps in pass 3) while the game presented ~65 fps.

### 04/10/2026 — B and W2.5 on the Linux native host (UM790Pro)

Host: the UM790Pro under Ubuntu 24.04, X11 session (the click's flag is X11
only on Linux), DEV `6ab77b3f` LAN only, KMS capture 1920×1080 at 60 Hz, the
bench page on its screen. Driven from DualRTX by `series.py --host um790pro`
(`8fd33518`, `1231e818`): the band is not read on a remote host. Two
alternated rounds per arm; Linux's own defaults: no retrcut, burst 10.

Without clicks (19:45-20:36; the `input` group was not yet granted):

| Client | Arm | Frame age med / p90 | `inSctp` all frames | Messages med | Retr. |
|---|---|---|---|---|---|
| N95, Wi-Fi | base | 18.4 / 23.9, 18.8 / 23.3 | 9.3 / 9.7 ms | 4.4-5.0 | 0-0.02 % |
| | `retrcut=3` | 17.8 / 22.3, 18.7 / 23.0 | 9.1 / 9.8 ms | 4.4-5.0 | 0 % |
| | `sctpburst=0` | 13.3 / 17.6, 11.9 / 15.8 | 2.9 / 2.5 ms | 3.2 | 0.03-0.12 % |
| | both | 13.1 / 17.6, 13.2 / 17.4 | 3.1 / 3.2 ms | 3.3 | 0.01-0.13 % |
| DualRTX Chrome, wired | base | 17.1 / 21.4, 21.0 / 35.5 | | 4.2, 8.3 | 0-0.01 % |
| | `retrcut=3` | 17.3 / 21.2, 21.6 / 37.9 | | 5.2, 8.7 | 0 % |
| | `sctpburst=0` | 11.9 / 18.1, 13.3 / 19.0 | | 3.3, 7.4 | 0-0.03 % |
| | both | 12.5 / 17.8, 12.9 / 18.5 | | 3.8, 7.5 | 0-0.01 % |

With clicks (20:38-21:01; bruno added to `input` for the series, removed after):

| Client | Arm | Click (p90) | `net` / `inSctp` of the flag | Frame age med |
|---|---|---|---|---|
| N95, Wi-Fi | base | 63.6 / 65.6 ms (86 / 92) | 13.9 / 11.0 ms | 20.6 / 21.0 |
| | `sctpburst=0` | 58.7 / 58.8 ms (80 / 81) | 6.1 / 2.6 ms | 14.4 / 14.7 |
| DualRTX Chrome, wired | base | 86.5 / 87.2 ms (126 / 107) | 20.3 / 17.1 ms | 20.9 / 21.3 |
| | `sctpburst=0` | 78.4 / 78.4 ms (111 / 101) | 10.9 / 7.9 ms | 13.5 / 12.8 |

- **`sctpburst=0` gains on the Linux host too**: ~6.5 ms less inside usrsctp
  per frame, a frame ~6 ms younger, the click 6-8 ms faster. A few more
  retransmissions on the N95 (to ~0.1 %) and, in one set, more frames
  dropped (71 → 128 a minute).
- **`retrcut=3` changes nothing there**: this link barely retransmits
  (0-0.1 %), unlike the Windows host to the Mac's Wi-Fi.
- On this host ~37 ms pass between the flag going up and the capture that
  shows it (`toCap`: KMS at 60 Hz plus the X11 flag window), against 3-4 ms on
  the Windows host: the largest share of a Linux click, and not the network.
- The wired client here is DualRTX's Chrome through its Hyper-V switch (2-3 ms
  with spikes to 25 ms to the UM790Pro): its clicks are slower than the N95's,
  so it only counts against its own base.
- **Decision (Bruno, 04/10 ~23:20): no max burst is the Linux native host's
  default too, `6a833826`** (`kNativeSctpMaxBurst` = 0 on Windows and Linux);
  `retrcut` stays off on Linux.

### 04/10/2026 — B and W2.5 on the macOS native host: started, not finished

Host: the Mac M1 Pro (Wi-Fi), DEV `0.3.1.g5de-dev` from the CI (`5de1e2af`),
ScreenCaptureKit, the bench page on its screen; client the N95 in Wi-Fi. Both
ends are on Wi-Fi here, so this is not the same link as the Windows host to
the Mac. Two passes only before the screen time Bruno granted ran out:

| Arm | Click (p90) | Frame age med / p90 | fps | Received | Dropped /min | Messages med |
|---|---|---|---|---|---|---|
| base | 70.5 ms (127) | 19.4 / 56.5 | 96 | 10.8 Mbit/s | 238 | 38.4 |
| `retrcut=3` | no click seen | 39.2 / 63.1 | 106 | 22.5 Mbit/s | 426 | 34.6 |

- In the `retrcut=3` pass the host saw and showed all 60 flags (its log) but
  the client found none: the flag sits at the bottom of the Mac's screen
  (792,1111 of 1800×1169), probably under the kiosk page that pass. Not
  understood yet.
- The following passes failed on the bench driver, not the host: it clicked
  a button labelled "Unlock", and the N95's Chrome is in French
  ("Déverrouiller"); fixed in `d1900763`. Still to rerun: all four arms, two
  rounds, on another Mac slot.


### 04/10/2026 — POC Ultra U0.4: Steam Remote Play, and the browser's present path (session ex-3b)

Steam Remote Play (client beta), host DualRTX streaming its primary screen,
the RTX's (NVENC for HEVC); client the UM790Pro under Windows on 1 GbE (780M
hardware decode, 1920×1080, automatic bitrate, quality modifier middle,
4:4:4 off), 22:10-23:28 (`e93407a7`, `9e1b8899`, POC doc §6.5). Method,
no camera (`scripts/bench/photon/`, `2e0a012b`): the host shows a full-screen
window that flips black/white on each mouse press, streamed as a non-Steam
game. The client injects a click at the stream window's centre and reads that
pixel back from its own composed desktop (GDI) until it flips. The sample
covers everything from the click's way up to the client's DWM composition,
leaving out the panel's scan-out. 60 clicks per pass, none missed; raw samples
in `bench-out/photon/*.json`.

| Pass | Codec | Low Latency Networking | Median | p90 | Min - max |
|---|---|---|---|---|---|
| 1 | HEVC | off | 49.9 ms | 58.9 ms | 32.6 - 67.2 |
| 2 | PyroWave | off | 42.7 ms | 59.0 ms | 32.7 - 66.6 |
| 3 | HEVC | off | 57.9 ms | 66.6 ms | 40.6 - 91.7 |
| 4 | PyroWave | off | 42.6 ms | 58.3 ms | 32.4 - 83.9 |
| 5 | HEVC | on | 58.6 ms | 83.2 ms | 41.3 - 375.8 |
| 6 | PyroWave | on | 49.7 ms | 59.1 ms | 32.9 - 92.3 |

- **Steam's "Low Latency Networking" makes both codecs worse** on 1 GbE:
  PyroWave +7 ms at the median; HEVC no better at the median, with a p90 of
  83 ms and one 376 ms outlier.
- PyroWave is steady from pass to pass (42.6-42.7 ms); Steam's HEVC moves by
  8 ms (49.9 → 57.9).
- **MoonlightWeb on the same pair, same tool** (23:22, `--dev` `ded56fc6`,
  the RTX screen in Auto, 120 fps, tearing, Chrome; defaults `retrcut=3`,
  `sctpburst=0`): median 58.2 ms, p90 75.0 ms (42-108), 60 of 60. A second pass
  is void (58 of 60 missed): most likely the bench's `latency_flag_enabled`
  overlay drew over the pixel read.
- **The browser's present path**: MoonlightWeb's own click → flag on this pair
  read 33-43 ms (U0.3, the flag read in the canvas at draw). Read on the
  composed desktop it is 58 ms. The 15-25 ms between them are Chrome's
  compositing and the DWM's, a leg the network and the codec do not touch and
  a native client such as Steam does not have.

## 4. The model so far (04/10/2026)

What the measurements support, in order of the path:

1. **The way up is not the problem** on Wi-Fi: 2-6 ms.
2. **The radio alone does not lose**: 0 of 185,000 UDP datagrams at 45 Mbit/s.
3. **The client's kernel does**: Chrome's UDP socket has a small receive buffer
   (a 64 KB sink reproduces it; 256 KB does not) and overflows when Chrome
   reads late while Wi-Fi delivers in aggregates. 1,000-2,300 drops a 2-minute
   pass at 35-42 Mbit/s, ~200-470 at 20 Mbit/s, 0 when less than ~48 KB can be
   in flight.
4. **SCTP reads each drop as congestion**: fast retransmit, the window halves,
   the frames behind wait in usrsctp's (256 KiB, invisible) buffer.
5. **Lowering the bitrate when SCTP retransmits** (B) removes most of it:
   −11 ms at the click on the Mac, the video's tail ÷3, the host's messages
   from ~330 to 35 ms at p90.
6. **What remains (~18 ms of `inSctp` on the Mac with B, 4.5 on Ethernet)
   is not a queue of frames.** A byte stays ~16 ms in usrsctp between
   hand-over and ack even without loss. Candidates: the SACK clock
   (Wi-Fi aggregation, the client's delayed SACK) and usrsctp's max burst of
   10 packets per send opportunity (a frame of 20-35 packets needs 2-4 SACK
   round trips: 8-9 ms each in Wi-Fi, 3-4 on Ethernet).
7. **The max burst was ~6 of those ~15 ms** (W2.5, 04/10): with no limit a
   frame spends ~9 ms in usrsctp on the Mac, the click gains 8 ms at the
   median and ~30 at p90; on Ethernet the wait there falls to almost nothing.
   The Windows native host's default since `2ef56bfe`. What remains on the
   Mac is the SACK clock of the Wi-Fi link.

## 5. What was tried and failed

| When | What | Result | Why |
|---|---|---|---|
| before 09/2026 | Unordered video channel | removed | reordering looked like holes, each asked for an IDR |
| 17/09 | Send buffer sized at 100 ms of bitrate | never took | libdatachannel raises it to 256 KiB (found 03/10) |
| 01-03/10 | RTCC congestion module (`sctpcc=3`) | no click gain | retransmissions halved, the wait did not move |
| 03/10 | Pacing the host's chunks (`pace=`) | no gain, `pace=4` worse | the socket overflows when Chrome reads late, not under the host's bursts |
| 03-04/10 | Small usrsctp buffer + picture held (`sctpbuf=`, `linkhold=`) | worse: fps ÷4, +14-20 ms click | the buffer caps throughput at ~buffer / 16 ms; the wait is in-flight time, not a queue |
| 04/10 | usrsctp's fair-bandwidth stream scheduler for the host's messages (`sctpss=4`) | no effect | the messages do not wait in usrsctp's stream queues |
| 04/10 | usrsctp's round-robin-by-packet scheduler (`sctpss=2`) | breaks the association in ~8 s | not investigated; never against Chrome |
| 29/09 | Named drops on oneVPL (Arc) | forbidden | a long-term-reference repair during an intra-refresh wave hangs Intel's HEVC encoder (bench §8n.30) |

## 6. Open questions

- What sets the ~16 ms a byte stays unacked on the Mac's Wi-Fi with no loss:
  the client's SACK policy (dcsctp), Wi-Fi aggregation, usrsctp's max burst?
  W2.5 took ~6 ms off with the burst; ~9 ms remain against ~4.5 on Ethernet.
- Whether the slightly higher kernel drops with no max burst (422 → 547 a
  pass on the Mac) or the N95's extra T3 timeouts (1 and 3 against 0 and 1)
  matter in the field. Ethernet gained (04/10).
- Whether GameStream sessions (Sunshine through the same relay) and the
  Linux and macOS native hosts gain the same from `sctpburst=0`: not measured,
  so not changed.
- What receive buffer Chrome gives its UDP socket on macOS and on Windows, and
  whether a page can influence it (it cannot directly). Whether Windows counts
  a full-socket drop anywhere (the N95 showed 0 "received errors").
- Whether the N95's SCTP losses are the same mechanism (pacing changed nothing
  there; no kernel counter).
- Why "two windows in a row" or a minimum count was not needed for
  `retrcut=3`: one false cut in four Ethernet passes; worth watching in the
  field.
- Safari's "frames dropped (jitter)" at 38-47 % with no network loss.
- The host's messages on the input channel still wait behind the video
  (~25 ms median, ~25 p90 with B and `sctpburst=0` on the Mac, against a 7 ms
  UDP ping). Not in usrsctp's stream queues (W2.3). In flight behind video
  chunks, at the AP or in the client's reassembly? A separate association or
  an unordered small-message channel would tell.
- RTO minimum (200 ms) and the lone-frame T3 tail (25/09): never A/B'd.
- DSCP/WMM marking of the video, from the host (W2 item 5 of the plan): not
  tried.

## 7. Knobs (bench keys, off by default unless said)

Link keys go in `MW_NATIVE_TUNING` / `--tuning` of `local_matrix.py`
(`backend/src/streaming/NativeBench.cpp`):

| Key | What | Since |
|---|---|---|
| `relaylog=1` | the relay's per-frame CSV | `5ce2228c` |
| `loss=`, `burst=` | video messages thrown away before SCTP | `4815aabd` |
| `sctpcc=0..3` | usrsctp congestion module | `4815aabd` |
| `flood=`, `floodsize=`, `floodchannel=` | useless traffic on channel 3 | |
| `pace=<n>`, `paceburst=<KB>` | pacing of a frame's chunks | `b2b52486` |
| `retrcut=<‰>` | governor cuts on SCTP retransmissions; **3 by default on Windows**, 0 off | `e0324f4e`, `55dd9cde` |
| `sctpbuf=<KB>` | usrsctp's real send buffer (via `maxMessageSize`) | `dc7f9c72` |
| `linkhold=<ms>` | hold the picture after a backlog that long (Windows) | `dc7f9c72`, `a511bdd3` |
| `sctpburst=<n>` | usrsctp's max burst in packets, 0 no limit; **0 by default on the Windows native host**, 10 elsewhere | `01717368`, `2ef56bfe` |
| `sctpss=0..5` | usrsctp's stream scheduler (4 = fair bandwidth) | `0c21bd5a` |
| `namedrops=0\|1` | name the relay's dropped delta to the encoder | `25bf8c48` |

Environment: `MW_SCTP_RTO_MIN_MS`, `MW_SCTP_SACK_DELAY_MS`.
