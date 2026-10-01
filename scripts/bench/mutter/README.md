# Mutter's own virtual monitors — `scripts/bench/mutter/`

A lab probe for plan « Idées Punktfunk », chapter C, phase C1: what GNOME's
compositor gives through its own D-Bus API (`org.gnome.Mutter.ScreenCast`
`RecordVirtual`, under `RemoteDesktop`), the route gnome-remote-desktop and
Punktfunk take, with no portal and no dialog. Nothing here ships. The results
are in `docs/bench-native-host.md` §8s.

| file | what it does |
|---|---|
| `mutter_virtual.py` | makes N virtual monitors, counts what each delivers, reads what DisplayConfig says of them, can lay them out, move the pointer over them, lock the session; one JSON line per event, a `summary` at the end |
| `pw_vcount.c` | a plain PipeWire consumer of one node: negotiates the size and rate it is told, shared memory or DMA-BUF, header, cursor and damage metadata; one JSON line a second (frames, cursor-only buffers, cursor metadata, longest gap, two pixels); `dump=` writes one frame as a PPM |
| `anim.py` | a GTK 4 window full screen (or maximized) on one monitor, redrawn by GSK at every tick of its frame clock: something new at every refresh |

## Build and run

On the GNOME machine, inside the user's graphical session (`XDG_RUNTIME_DIR`,
`DBUS_SESSION_BUS_ADDRESS`, `WAYLAND_DISPLAY`), from binaries with **no file
capabilities** (sd-bus and glibc hide the session from them):

    gcc -O2 -o pw_vcount pw_vcount.c $(pkg-config --cflags --libs libpipewire-0.3)
    python3 mutter_virtual.py --size 1920x1080 --maxfps 240 --wiggle --seconds 8
    python3 mutter_virtual.py --size 1280x720 --count 2 --anim none
    python3 mutter_virtual.py --layout primary --anim maximized --wiggle

Needs `python3-gi` with GTK 4 and Graphene, and `libpipewire-0.3-dev` to
build the counter (PipeWire 0.3.48 of Ubuntu 22.04 and later).

- `--pw-opts` passes the counter its options: `dmabuf` (linear or implicit
  modifier, as the product offers), `size=pin|range`, `rate=max|range|product`
  — `max` pins `maxFramerate`, `product` asks only `framerate`, as
  `PortalCapture` did until C0 bis.
- `--hz N` also passes `modes` (one mode, that size at N Hz).
- `--wiggle` moves the pointer over the first monitor through RemoteDesktop:
  GNOME opens a new window on the monitor under the pointer, so it comes
  before the animation.
- `--lock-at S` locks the session after S seconds (`loginctl`) and unlocks it
  at the end.

## Traps

- **Mutter makes the monitor when a consumer negotiates** the stream's
  format, not at `RecordVirtual`: its size and its refresh are the negotiated
  size and **`maxFramerate`** (GNOME 42, 46 and 48 alike). A free
  `maxFramerate` settles on Mutter's default, 60.
- PipeWire 1.0 reads a number in `target.object` as an object serial: name
  the node by its id through `pw_stream_connect`.
- Mutter 46 and later ask room for a **384×384** cursor in the metadata; a
  consumer whose range stops below it gets no cursor metadata at all.
- A buffer flagged corrupted with no pixels is a cursor-only update.
- A full-screen window on GNOME 46 does not reach a shared-memory consumer;
  a window opened while the pointer is elsewhere opens on that other monitor.
- **Locking the session ends every screencast session** (the virtual monitor
  goes, nothing comes back at unlock) and refuses new ones ("Session creation
  inhibited"); after an unlock the physical screens may stay powered off —
  `DisplayConfig.PowerSaveMode = 0` wakes them.
- ⛔ **Never switch physical monitors off** (`--layout exclusive`) on a
  machine whose screens hang off a second GPU: on the UM790Pro (GNOME 46, the
  M27Q on the GTX 1050) gnome-shell crashed when Mutter switched them back on,
  and the session went down with every program in it.
