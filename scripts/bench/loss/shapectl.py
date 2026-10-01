"""Talk to mwshaper.py (the host-side link shaper) on the loopback.

    py shapectl.py <command ...>      e.g. stats | cut 500 | rate 4000 5000 200 | clear | quit

As a module: ctl("cut 500") -> the shaper's JSON reply, as a dict.
"""
import json
import socket
import sys

PORT = 47250


def ctl(line, port=PORT, timeout=5.0):
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        f = s.makefile("rw", encoding="utf-8", newline="\n")
        f.write(line.strip() + "\n")
        f.flush()
        reply = f.readline()
    return json.loads(reply) if reply else {"ok": False, "err": "no reply"}


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    print(json.dumps(ctl(" ".join(sys.argv[1:]))))
