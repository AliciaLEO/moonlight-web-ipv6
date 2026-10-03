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
| `udp_ref.py` | a bare UDP ping to an echo on the client (`--udp` in a series starts the echo on the Mac) |
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
