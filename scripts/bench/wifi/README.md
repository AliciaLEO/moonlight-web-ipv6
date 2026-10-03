# Wi-Fi bench — `scripts/bench/wifi/`

Click → flag series against DualRTX's native host, to judge what Wi-Fi costs
on the way down. Plan « Wi-Fi : la vidéo qui attend dans SCTP », born of T7 of
the radios plan (03/10/2026): on Wi-Fi a click goes up in 2 to 8 ms, but the
video, the flag's picture and the host's small messages wait in the host's SCTP
send queue (0.2 to 0.6 % of chunks retransmitted; none on Ethernet), while a
bare UDP ping on the same Wi-Fi does its round trip in 4.7 ms.

Nothing here ships. The passes are `../content-age/` passes (`local_matrix.py`,
`pass.py`, `age.py`); this folder drives them on the remote clients and reads
them back.

| tool | what it does |
|---|---|
| `series.py` | one client's phase: its Chrome up, its DevTools port tunnelled, stale pairings cleared, then each round × content as one `local_matrix.py` run (`--tuning` for host keys), the client down |
| `report.py` | the table of a prefix: click → flag and its split, the content's age, each frame's age, SCTP retransmissions, frames thrown away, `rep/m`, bitrate, the small messages' round trip, the UDP ping |
| `flagpath.py` | where each click's flag frame waited, from the relay's frame log, the client's and the flag's time (W1) |
| `udp_ref.py` | a bare UDP ping to an echo on the client (`--udp` in a series starts the echo on the Mac); video-shaped bursts to a sink (`--burst`) |
| `stop.ps1` | stops a series whatever it does, puts the virtual display's settings file back and turns the display off |
| `vdd-reset.ps1` | the virtual display left primary by a killed pass: a short `--dev` start resets it |
| `memwatch.ps1` | stops the series when DualRTX runs short of commit |
| `mac-prep.sh`, `mac-restore.sh` | Léo's games and auto-clicker stopped for a bench, then exactly those reopened |
| `cdpcall.mjs` | one DevTools call on a client (`Storage.clearDataForOrigin`…) |

```bash
ssh mw-mac bash -s < mac-prep.sh                       # say until when the Mac is kept
python series.py mac --prefix w0 --contents clk,g80 --rounds 2 --udp
python series.py n95 --prefix w0 --contents clk,g80 --rounds 2
python series.py um  --prefix w0 --contents clk,g80 --rounds 2
ssh mw-mac bash -s < mac-restore.sh
python report.py w0                                    # --passes for one row per pass
```

## Where the flag's frame waited (W1)

```bash
python series.py mac --prefix w1 --contents clk --rounds 2 --tuning relaylog=1 --udp --burst 45:120:20
python flagpath.py --prefix w1          # a row a pass; --clicks for every click
```

- `relaylog=1` (a link key of the native host, like `sctpcc=`) has the relay
  keep each video frame's way (`backend/src/streaming/RelayFrameLog.h`): its
  capture on the host's steady clock, the decision (sent, dropped by the
  backlog, gated awaiting a keyframe, evicted), the sender's first and last
  fragment into the DataChannel, `bufferedAmount` before and after, usrsctp's
  retransmission counters and SCTP's round trip. `local_matrix.py` copies it
  beside the pass (`<tag>.relay.csv`). The log ends with the session's line
  `frame log: …` (what became of the frames, and how long `bufferedAmount`
  held more than 1, 2 and 4 frames) and `SCTP window this session: …`.
- `pass.py` keeps the client's per-frame log of the clicks' minute
  (`<tag>.clicks.frames.csv`) and the page's time origin; the host's
  `[LatencyFlag] … shown at steady N us` dates each flag.
- `flagpath.py` joins the three, click by click: up, injection, flag raised,
  until the capture of the frame that showed it (`toCap`, with the frames in
  between that did not go out: `skipped`), encode, the sender's queue, `net`
  (libdatachannel's queue, usrsctp's, the air, a wait behind a retransmitted
  chunk), decode, draw, detection. The legs add up to the measured click.
- `--burst MBPS:FPS:SECS` sends video-shaped UDP bursts to the Mac before the
  passes, with no stream: what the radio loses, and what it only delivers out
  of order (SCTP repairs both with a retransmission).

## The kernel's drops, and pacing (W1 bis, W2 A)

- `series.py` reads the Mac's `netstat -s -p udp` counter "dropped due to full
  socket buffers" before and after each pass and each burst: on 03/10/2026,
  1,855 and 2,303 drops in passes at Auto's bitrate, against 3,120 and 3,960
  SCTP retransmissions. A burst can name the sink's receive buffer
  (`--burst 45:120:15:64` for 64 KB, like a browser's): 64 KB lost 1.5 %,
  256 KB nothing.
- `pace=<n>` (a link key, with `paceburst=<KB>`) has the host hand a frame's
  chunks to SCTP at n times the stream's bitrate at most, 16 KB at a time
  (`backend/src/streaming/SendPacer.h`). The session ends with
  `bench pacing this session: …`, read by `report.py` (`pace%`, `pmax`).

```bash
python series.py mac --prefix w2a0 --contents clk --tuning relaylog=1 --udp        # today
python series.py mac --prefix w2a4 --contents clk --tuning relaylog=1,pace=4 --udp
```

**Pacing's verdict (W2 A, 03/10/2026): not for the product, the key stays.**
On the Mac in Wi-Fi, two alternated rounds each (the page at 240):

| | click → flag | kernel drops | SCTP retr. | frame age median / p90 | dropped /min |
|---|---|---|---|---|---|
| no pacing | 74.9 ms | 2,115 | 0.74 % | 20.6 / 51 ms | 110 |
| `pace=4` | 85.0 ms | 2,540 | 0.79 % | 23.8 / 60 ms | 75 |
| `pace=2` | 79.7 ms | 2,065 | 0.70 % | 15.5 / 24 ms | 48 |

Pacing does not bring the drops down: Chrome's socket overflows when Chrome
reads late (at 42 Mbit/s a 64 KB buffer holds ~12 ms), not under the host's
bursts. Only a lower bitrate did (213 drops at 20 Mbit/s): that is W2 B, the
rate governor cutting on SCTP's retransmissions (`retrcut=`). On the N95,
pacing changed nothing measurable.

## The rate that follows SCTP, and the picture held (W2 B, W2 C)

- `retrcut=<‰>` (W2 B): the native host's rate governor also cuts when SCTP
  retransmits at least that many chunks in a thousand over a report window
  (`report.py`: `gRetr`, `gMin`). On the Mac, 03/10/2026: click 73.6 → 62.6 ms,
  kernel drops 1,016 → 366, a frame's age p90 86 → 29 ms, 23 → 20 Mbit/s. On
  Ethernet (UM790Pro, `w2b-um-run.sh`): 39.8 → 39.0 ms, SCTP sending 1 to 8
  chunks again a session; one cut, 8 retransmits in one report window, 40 → 32
  Mbit/s for 3.5 s. **The Windows host's default since `55dd9cde`** (3; the
  baseline of any later A/B carries it, `retrcut=0` takes it off).
- usrsctp's send buffer has always been **256 KiB**: libdatachannel raises
  `SO_SNDBUF` to its largest message right after the sysctl, so the relay's
  "100 ms of bitrate" never took. `sctpbuf=<KB>` (W2 C) really sets it, by
  lowering the largest message with it; what usrsctp cannot take then waits in
  `bufferedAmount`.
- `linkhold=<ms>` (W2 C, Windows host): once the video channel's
  `bufferedAmount` has stayed above zero that long, the capture loop holds its
  picture unencoded and sends the freshest once it drained: no frame thrown
  away, no keyframe asked for. The session ends with `[native] link hold: …`
  (`hold/s`). Not at the first byte: on Ethernet, with `sctpbuf=48`, 44 % of
  frames overflow usrsctp for a millisecond or two; holding on that took the
  stream from 223 to 87 fps (03/10/2026), the held frames coming out bigger.

```bash
bash ../../../bench-out/wifi/w2b-um-run.sh                  # Ethernet witness (C=1: also linkhold)
python series.py mac --prefix w2c --name machold --contents clk --tuning relaylog=1,sctpbuf=48,linkhold=4 --udp
```

## Clients

| name | machine | link | DevTools |
|---|---|---|---|
| `mac` | Mac M1 (`mw-mac`, Léo's), 120 Hz | Wi-Fi | 9422 → 9222, `~/mw-c925/mac-chrome.sh` |
| `n95` | N95 (`mw-intel`), 60 Hz | Wi-Fi | 9423 → 9232, `ua-n95-chrome.ps1`, one frame in 10 read |
| `um` | UM790Pro under Windows (`mw-um790win`), 120 Hz | Ethernet: the witness | 9424 → 9222, `um790-chrome.ps1` |
| `lx` | UM790Pro under Ubuntu | Ethernet | 9425 → 9222 through WSL |
| `loc` | Chrome on DualRTX's AMD screen | local | the kiosk, 9353 |

## Contents

| name | what streams | clicks |
|---|---|---|
| `clk` | the page at the display's rate (240 on the virtual display), Auto with detection | 60, then the way up 50/s for 20 s |
| `g80` | the page at a game's rate, 75-83 fps drawn at random | the same |
| `still` | a still desktop: almost no video coming down | 40 |
| `f60` | the page streamed at 60 fps | 40 |

## Reading it

- **click** is the whole loop the player feels. **up** is the way up (host's
  stamp on the clock both ends share), **rest** everything after the flag:
  capture, encode, the way down, decode, draw.
- **shown** is the age of what is on screen, from the band of
  `scroll.html?band=time`. The frames the probe reads wait about 12 ms more
  before their draw (U0.2 cross-check, `docs/design/ultra-lan-poc.md` §6.1):
  **e2e**, every frame's capture → draw from the per-frame log, does not carry
  that, and its first leg **down** (capture → last chunk arrived) holds the
  encode, the host's send queue and the air.
- **retr%** and **T3** are the host's SCTP counters for the session;
  **drop/m** the frames the relay threw away before SCTP; **buf** the largest
  `bufferedAmount` it logged.
- **msg** is a small message's round trip on the input channel (the host's
  messages wait behind the video); **udp** the bare UDP ping beside it.

## The baseline (W0, 03/10/2026, 10:25-11:08)

DualRTX's Arc renders the virtual display at 240 Hz; Auto with detection; two
rounds per cell, alternated; `report.py w0`. Milliseconds, means of the rounds.

| client | content | click | up | rest | shown | e2e median / mean / p90 | retr. | msg (p90) | UDP (p99) |
|---|---|---|---|---|---|---|---|---|---|
| Mac M1, Wi-Fi | page at 240 | 71.1 | 3.3 | 67.1 | 37.9 | 20.7 / 33.1 / 63.2 | 0.72 % | 34.6 (225) | 4.6 (13.5) |
| | game 75-83 | 57.3 | 2.3 | 54.7 | 47.9 | 37.5 / 41.7 / 69.7 | 0.71 % | 68.7 (455) | 5.1 (12.3) |
| N95, Wi-Fi | page at 240 | 99.4 | 5.6 | 90.7 | 56.6 | 35.0 / 47.6 / 76.4 | 0.57 % | 34.7 (269) | |
| | game 75-83 | 93.5 | 5.9 | 85.1 | 54.7 | 38.5 / 47.9 / 84.1 | 0.21 % | 25.6 (67) | |
| UM790Pro, Ethernet | page at 240 | 33.6 | 1.7 | 31.9 | 24.0 | 12.6 / 13.0 / 17.2 | 0 | 4.1 (9) | |
| | game 75-83 | 34.2 | 1.6 | 31.8 | 28.2 | 16.6 / 17.5 / 25.6 | 0 | 5.2 (14) | |

The overlay counted 13 to 20 link freezes a pass on the Mac (the longest 1.2
to 3.3 s), 1 to 3 on the N95, none on Ethernet.

## Traps (03/10/2026)

- One `--dev` per PC, on **8080/8443**. Each pass kills and relaunches it:
  check that nothing holds 8443/18443 first (`Get-NetTCPConnection`; an
  elevated `--dev` is invisible to the scripts) and tell the sessions that use
  it.
- Build in `build\`: it has its firewall rule. Another build opens the
  « Windows Security » prompt **at 0,0 of the primary screen — the captured
  virtual display**, over the band: every read is invalid (`block`) until it is
  answered. Never answer « Cancel », which writes Block rules.
- A pairing cookie from another port of the same IP gets the client refused
  (`pairing verification failed`, 1008) and the stream falls back to the
  WebSocket: `series.py` clears it.
- A virtual display left primary after a killed pass: `vdd-reset.ps1`.
- DualRTX runs short of memory: `memwatch.ps1` beside a series.
- Encoder ≠ decoder, HAGS on, never « maximum performance ».
