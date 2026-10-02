# The content-age bench: how old is what the client shows?

The overlay's end-to-end starts when the host captured a frame. It cannot see
how long the picture waited before that — for the compositor of the host's
display, or for a present the cadence gate skipped. That wait is what a stream
at the host's own rate is meant to remove (plan `framerate-hote`, design §33).
So the content carries its own time.

```bash
# --dev instance on 18080/18443; the client on another screen and another GPU
set MW_BENCH_LOCAL_PORTS=18080,18443
set MW_BENCH_CLIENT_POS=secondary
set MW_BENCH_DEBUG_PORT=9353   # only if another Chrome already holds the kiosk's 9333
python pass.py --tag d1-auto --secs 30            # a whole self-stream pass
python age.py summary ../../../bench-out/content-age/*.json
```

- `scroll.html?band=time` codes the host's steady clock into a band across the
  top of its screen at every frame: 40 blocks, each a hundredth of the width
  (32 bits of time in 10 µs units, 8 of check), with the complement under it.
  Nothing is drawn until `age.py calibrate` has given the page the offset
  between its `performance.now()` and the host's clock, over CDP.
- The client (`frontend/js/stream/ContentAgeProbe.js`, `mwContentAge` in the
  console) copies that strip out of each decoded frame, dates the draw of the
  same frame, and puts it on the host's clock with an estimate made from the
  ping/pong, whose pong carries the host's time.
- `age.py run --client host:port --tag …` drives a client anywhere: open an SSH
  tunnel to its debugging port first.

## What a pass gives

| field | what it is |
|---|---|
| content age | client draw − the page's frame time: the whole chain, page to glass |
| capture age | client draw − the frame's present on the host (`backendTs`): the overlay's end-to-end, on this estimate |
| before capture | present − page frame time: the host's own share — rendering, composition, the gate |

`before capture` rests on the host's clocks alone; the other two also on the
client's estimate. A client on the host's own machine reads the same counter
(QueryPerformanceCounter), and `age.py` then checks the estimate against it.

## Checked on 29/09/2026 (DualRTX, Arc screen at 60 Hz, client on the AMD screen)

- The estimate stood 0.08 ms off the host's clock; the page's calibration was
  good to 0.3 ms.
- 1,800 bands read out of 1,800 decoded frames, none invalid.
- Content 70.8 ms, capture 20.2 ms (the overlay said 16–19), before capture
  50.7 ms — three frames of the 60 Hz display between the page's frame and
  its present, the same on two passes.

A first version read the band back off the canvas: 13–14 ms of the main
thread per read on the AMD iGPU, whatever the rate. `VideoFrame.copyTo` of the
strip costs the main thread nothing measurable.

## "Auto" with detection: the UA bench (plan POC Ultra, Phase UA; design §33.10)

The detection (`frontend/js/stream/CadenceStepper.js`) steps a native host's
stream above the client's rate (twice it, then the virtual display's 240 Hz)
and keeps a step only when what is shown gets younger. Its gate:

- within 10 s of the content moving, it reaches the better of today's Auto and
  `cadence=host-guarded` on each client: the high step on the UM790Pro on
  Ethernet and on the Mac (3 to 6 ms younger expected), the client's own rate on
  the N95 and on the local iGPU (never more than 1 ms worse than today);
- no worse click → flag, no more repeated pictures a minute (`rep/m`);
- the page at a game's 49-53 frames a second starts no trial at all.

**The matrix.** Two passes per cell, alternated (`--repeat 2`); every mode with
the same `--settle 14`, so the windows start at the same moment.

| | |
|---|---|
| hosts | DualRTX, the product's virtual display at 240 Hz (`--rates 0`) rendered in turn by the Arc, the iGPU AMD and the RTX (`--vdd-gpu`) |
| clients | UM790Pro under Windows, Ethernet, 120 Hz (`um790-chrome.ps1`, tunnel 9424); Mac M1, 120 Hz; N95, Wi-Fi, 60 Hz (`n95-chrome.ps1`, tunnel 9423, `--every 10`); the local iGPU AMD, for tuning only |
| modes | `client` (today's Auto), `detect` (`--autostep`), `host-guarded` |
| contents | the page at the display's rate; `--game-fps 75-83`; `--game-fps 49-53` |
| clicks | `--clicks 60` on the page at the display's rate, `client` against `detect` (`latency_flag_enabled` in the instance's settings.json) |

```bash
set MW_BENCH_LOCAL_PORTS=18080,18443
python local_matrix.py --prefix ua-um-arc --rates 0 --cadences client,detect,host-guarded \
    --repeat 2 --settle 14 --client-port 9424 --client-url https://<DualRTX>:18443/ \
    --vdd-gpu "Intel(R) Arc(TM) A380 Graphics"
python local_matrix.py --prefix ua-um-arc-g80 --game-fps 75-83 ...   # same, a game's rate
python age.py table ua-um-arc
```

**What to read.** `shown` and `p99` (the age of what is on screen), `rep/m`
(refreshes that showed no new picture, per minute), `step` (where the detection
ended: its base when no step held), `kept` (seconds after the content moved
when the last step was kept), `trips` (times its net brought the stream back).
Each pass's JSON has the detection's decisions (`stepper.events`, with
`sinceContentS`), and its `.host.txt` the host's lines: `cadence step … refused`
and, at the end, `cadence steps: … asked, … applied, … refused; … s above the
client's rate`.

**Before a series.** The UM790Pro boots Ubuntu by default (dual boot):
Windows only with Bruno's go. `local_matrix.py` kills and relaunches the
`--dev` instance of DualRTX for each pass: say so to the sessions that use it.
Encoder and decoder on different GPUs, HAGS on, no "maximum performance" mode,
no switch to another GPU; screens listed before and after (the script does it,
and puts the virtual display's settings file back); series run when Bruno is
not at his desk — the RTX's screen once left the desktop during the switches.
