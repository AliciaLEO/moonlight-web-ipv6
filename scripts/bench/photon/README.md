# Click → photon without a camera (POC Ultra U0.4)

The same measure for any streaming client, MoonlightWeb or Steam Remote Play
(HEVC or PyroWave): a click on the client, timed until its answer shows on the
client's own screen.

- `flag-window.ps1`, on the **host**: a window over a whole screen that flips
  black ↔ white on every mouse button press. Optional `-Log` writes each
  flip's host time.
- `click-photon.ps1`, on the **client**: finds the streaming client's window by
  process (`streaming_client` for Steam, `chrome` with `-Title` for
  MoonlightWeb), clicks at its centre (SendInput) and reads that pixel back
  from the composed desktop until it flips. 60 clicks, 700 ms apart by
  default; median, p90, misses, every sample in `-Out <json>`.

What a sample includes: the click's way up, the flip on the host, capture,
encode, network, decode, the client's present and the DWM's composition, plus
up to one pixel read (~1 ms). It leaves out the screen's scan-out and
response, the same for every client on one screen. GDI reads the DWM's last
composed frame: right for a windowed client, unproven for an exclusive
full-screen one. Keep the client windowed.

## Steam Remote Play (U0.4)

1. Both machines: Steam → Settings → Interface → Client Beta Participation →
   "Steam Beta Update", restart Steam. The same Steam account on both.
2. Host: add `flag-window.ps1` as a non-Steam game. Target `powershell.exe`,
   launch options `-NoProfile -ExecutionPolicy Bypass -File "<path>\flag-window.ps1"`.
3. Client: Settings → Remote Play → Advanced Client Options → Pyrowave on
   (off for HEVC), and "Display performance information" for Steam's own
   readings. Stream the flag "game" from the client's library.
4. Client: `powershell -NoProfile -File click-photon.ps1 -Process streaming_client -Out steam-pyrowave-1.json`,
   mouse untouched. Alternate HEVC and PyroWave, two runs each.
5. For MoonlightWeb on the same pair: stream the host's screen with the flag
   window on it, then `click-photon.ps1 -Process chrome -Title MoonlightWeb`.

The plan's gate: if native PyroWave does not beat Steam's HEVC by 2 ms or
more on NVIDIA, a browser port will not.
