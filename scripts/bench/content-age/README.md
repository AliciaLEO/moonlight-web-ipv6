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
