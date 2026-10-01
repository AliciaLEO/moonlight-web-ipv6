# gamescope headless — `scripts/bench/gamescope/`

A lab probe for plan « Idées Punktfunk », chapter C, phase C5: what a
headless gamescope gives — an app's own screen at the client's size and
rate — and how its input is reached. Nothing here ships. The results are in
`docs/bench-native-host.md` §8s.13.

| file | what it does |
|---|---|
| `Dockerfile` | Arch Linux with a current gamescope (3.16.31 on 01/10/2026), RADV, `vkcube`, `xdotool` |
| `gamescope_headless.sh` | starts gamescope `--backend headless` in that container on the host's GPU, its PipeWire stream on the session's PipeWire, its EIS socket in a host directory; finds the node, counts its frames, stops |
| `ei_probe.py` | the host's libei as a sender on gamescope's EIS socket: lists its devices, sends one absolute or relative pointer motion |

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

## Traps

- **Ubuntu has no usable gamescope headless**: 24.04 has none, 25.04 has
  3.16.1, which stops on a wlroots assertion (`wlr_linux_dmabuf_v1.c:532`,
  an empty format table) on a Radeon 780M. Punktfunk asks for 3.16.22.
- **Xwayland turns away the clients of an unnamed uid** ("Authorization
  required"): the container's uid must have a passwd entry.
- gamescope quits when its app quits: a client that cannot connect ends
  the whole container.
- Headless, gamescope reads no evdev or uinput device: input goes through
  its EIS socket only. An absolute position is counted from the focused
  window, and the pointer stays inside it.
