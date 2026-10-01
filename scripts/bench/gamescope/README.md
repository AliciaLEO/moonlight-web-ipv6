# gamescope headless — `scripts/bench/gamescope/`

A lab for plan « Idées Punktfunk », chapter C, phase C5, then chapter G: what a headless gamescope
gives — an app's own screen at the client's size and rate — how its input is reached, and a gamescope
for a bench machine on Ubuntu 24.04, which ships none. The product's side is
`backend/native-host/src/capture/linux/GamescopeSession.h`. Results: `docs/bench-native-host.md`
§8s.13 (the probe) and §8s.14 (the product).

| file | what it does |
|---|---|
| `Dockerfile` | Arch Linux with a current gamescope (3.16.31 on 01/10/2026), RADV, `vkcube`, `xdotool` |
| `gamescope_headless.sh` | starts gamescope `--backend headless` in that container on the host's GPU, its PipeWire stream on the session's PipeWire, its EIS socket in a host directory; finds the node, counts its frames, stops |
| `ei_probe.py` | the host's libei as a sender on gamescope's EIS socket: lists its devices, sends one absolute or relative pointer motion |
| `build_ubuntu2404.sh` | gamescope built on Ubuntu 24.04 into `~/.local/opt/gamescope`, linked as `~/.local/bin/gamescope` — where the product looks |

## Run

On the Linux desktop, as its user, inside the graphical session:

    docker build -t mw-c5-gamescope:3 scripts/bench/gamescope
    scripts/bench/gamescope/gamescope_headless.sh start 2560 1440 240
    scripts/bench/gamescope/gamescope_headless.sh count 8 2560 1440 240
    python3 scripts/bench/gamescope/ei_probe.py /tmp/mw-c5-xdg/gamescope-0-ei info
    python3 scripts/bench/gamescope/ei_probe.py /tmp/mw-c5-xdg/gamescope-0-ei rel 100 50
    docker exec -e DISPLAY=:0 mw-c5 xdotool getmouselocation
    scripts/bench/gamescope/gamescope_headless.sh stop

`count` needs `../mutter/pw_vcount` built (see that folder's README); `ei_probe.py`
needs `libei.so.1` on the host (Ubuntu 24.04: `libei1`).

The product on a bench host: `build_ubuntu2404.sh`, then the DEV restarted with
`MW_GAMESCOPE_APP="vkcube --wsi xcb"` (an app in Steam's place on the Steam Big Picture card, no
sign-in needed) and `MW_GAMESCOPE_LINGER_S=45` (the session's wait after the last stream, 600 s
otherwise).

## Traps

- **Ubuntu has no usable gamescope headless**: 24.04 has none, 25.04 has
  3.16.1, which stops on a wlroots assertion (`wlr_linux_dmabuf_v1.c:532`,
  an empty format table) on a Radeon 780M, 26.04 has 3.16.20. Punktfunk asks
  for 3.16.22.
- **A gamescope built on 24.04 keeps its WSI layer out of the Vulkan loader**:
  the layer carries a libwayland of its own beside the app's, and vkcube froze.
- **Xwayland turns away the clients of an unnamed uid** ("Authorization
  required"): the container's uid must have a passwd entry.
- gamescope quits when its app quits: a client that cannot connect ends
  the whole container — and, in the product, the stream.
- Headless, gamescope reads no evdev or uinput device: input goes through
  its EIS socket only. An absolute position is counted from the focused
  window, and the pointer stays inside it.
- After an absolute move gamescope says its pointer is hidden
  (`GAMESCOPE_CURSOR_VISIBLE_FEEDBACK` 0), after a relative one shown: its own
  screen's business, not the picture's.
- One Steam per user: a second one hands its command line to the first and
  quits. A desktop Steam is asked to quit through `steam-runtime-steam-remote`
  (its HOME); the snap's launcher took `-shutdown` and did nothing.
