#!/usr/bin/env python3
"""C5 probe: drive gamescope's pointer through its EIS socket with the host's libei (sender).

    ei_probe.py <socket> abs <x> <y>      absolute motion, in the device region's coordinates
    ei_probe.py <socket> rel <dx> <dy>    relative motion
    ei_probe.py <socket> info             list the seats and devices gamescope offers
"""
import ctypes
import select
import sys
import time

ei = ctypes.CDLL("libei.so.1")
P = ctypes.c_void_p


def fn(name, res, *args):
    f = getattr(ei, name)
    f.restype = res
    f.argtypes = list(args)
    return f


ei_new_sender = fn("ei_new_sender", P, P)
ei_configure_name = fn("ei_configure_name", None, P, ctypes.c_char_p)
ei_setup_backend_socket = fn("ei_setup_backend_socket", ctypes.c_int, P, ctypes.c_char_p)
ei_get_fd = fn("ei_get_fd", ctypes.c_int, P)
ei_dispatch = fn("ei_dispatch", None, P)
ei_get_event = fn("ei_get_event", P, P)
ei_event_get_type = fn("ei_event_get_type", ctypes.c_int, P)
ei_event_get_seat = fn("ei_event_get_seat", P, P)
ei_event_get_device = fn("ei_event_get_device", P, P)
ei_event_unref = fn("ei_event_unref", P, P)
ei_seat_get_name = fn("ei_seat_get_name", ctypes.c_char_p, P)
ei_device_ref = fn("ei_device_ref", P, P)
ei_device_get_name = fn("ei_device_get_name", ctypes.c_char_p, P)
ei_device_has_capability = fn("ei_device_has_capability", ctypes.c_bool, P, ctypes.c_int)
ei_device_get_region = fn("ei_device_get_region", P, P, ctypes.c_size_t)
ei_region_get_x = fn("ei_region_get_x", ctypes.c_uint32, P)
ei_region_get_y = fn("ei_region_get_y", ctypes.c_uint32, P)
ei_region_get_width = fn("ei_region_get_width", ctypes.c_uint32, P)
ei_region_get_height = fn("ei_region_get_height", ctypes.c_uint32, P)
ei_device_start_emulating = fn("ei_device_start_emulating", None, P, ctypes.c_uint32)
ei_device_stop_emulating = fn("ei_device_stop_emulating", None, P)
ei_device_pointer_motion = fn("ei_device_pointer_motion", None, P, ctypes.c_double, ctypes.c_double)
ei_device_pointer_motion_absolute = fn("ei_device_pointer_motion_absolute", None, P,
                                       ctypes.c_double, ctypes.c_double)
ei_device_frame = fn("ei_device_frame", None, P, ctypes.c_uint64)
ei_now = fn("ei_now", ctypes.c_uint64, P)
ei_seat_bind_capabilities = ei.ei_seat_bind_capabilities  # variadic, 0-terminated
ei_seat_bind_capabilities.restype = None

CAP = {"pointer": 1, "pointer_absolute": 2, "keyboard": 4, "touch": 8, "scroll": 16, "button": 32}
CONNECT, DISCONNECT, SEAT_ADDED, SEAT_REMOVED, DEVICE_ADDED, DEVICE_REMOVED, DEVICE_PAUSED, \
    DEVICE_RESUMED = range(1, 9)


def main():
    sock, mode = sys.argv[1], sys.argv[2]
    ctx = ei_new_sender(None)
    ei_configure_name(ctx, b"mw-c5-probe")
    if ei_setup_backend_socket(ctx, sock.encode()) != 0:
        raise SystemExit("cannot connect to " + sock)
    fd = ei_get_fd(ctx)
    devices, resumed, seq, done = [], set(), 1, False
    end = time.time() + 5
    while time.time() < end and not done:
        select.select([fd], [], [], 0.2)
        ei_dispatch(ctx)
        while True:
            ev = ei_get_event(ctx)
            if not ev:
                break
            t = ei_event_get_type(ev)
            if t == CONNECT:
                print("connected")
            elif t == DISCONNECT:
                print("disconnected")
                done = True
            elif t == SEAT_ADDED:
                seat = ei_event_get_seat(ev)
                print("seat", ei_seat_get_name(seat).decode())
                ei_seat_bind_capabilities(P(seat), ctypes.c_int(1), ctypes.c_int(2), ctypes.c_int(4),
                                          ctypes.c_int(16), ctypes.c_int(32), ctypes.c_int(0))
            elif t == DEVICE_ADDED:
                dev = ei_device_ref(ei_event_get_device(ev))
                caps = [k for k, v in CAP.items() if ei_device_has_capability(dev, v)]
                reg = ei_device_get_region(dev, 0)
                region = (ei_region_get_x(reg), ei_region_get_y(reg), ei_region_get_width(reg),
                          ei_region_get_height(reg)) if reg else None
                print("device", ei_device_get_name(dev).decode(), caps, "region", region)
                devices.append((dev, caps))
            elif t == DEVICE_RESUMED:
                resumed.add(ei_event_get_device(ev))
            ei_event_unref(ev)
        if mode == "info":
            continue
        for dev, caps in devices:
            want = "pointer_absolute" if mode == "abs" else "pointer"
            if want in caps and dev in resumed:
                a, b = float(sys.argv[3]), float(sys.argv[4])
                ei_device_start_emulating(dev, seq)
                seq += 1
                if mode == "abs":
                    ei_device_pointer_motion_absolute(dev, a, b)
                else:
                    ei_device_pointer_motion(dev, a, b)
                ei_device_frame(dev, ei_now(ctx))
                ei_device_stop_emulating(dev)
                ei_dispatch(ctx)
                print("sent", mode, a, b)
                done = True
                break
    time.sleep(0.2)
    ei_dispatch(ctx)


if __name__ == "__main__":
    main()
