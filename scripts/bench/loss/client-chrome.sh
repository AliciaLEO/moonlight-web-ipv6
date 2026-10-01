#!/bin/bash
# The measuring Chrome on a Linux client (the UM790Pro), in the logged-in user's
# Wayland session, its own profile, DevTools on the loopback (port $1, default
# 9222). Replaces a running one of the same profile.
#
# --password-store=basic is not optional: a fresh profile under an automatic
# login waits for the GNOME keyring (locked) before it reads its cookies, and
# no page ever loads — Page.navigate hangs (28/09 and 01/10/2026).
PORT=${1:-9222}
export XDG_RUNTIME_DIR=/run/user/$(id -u)
export WAYLAND_DISPLAY=$(basename "$(ls $XDG_RUNTIME_DIR/wayland-[0-9] 2>/dev/null | head -1)")
export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus
PROFILE=$HOME/.mw-loss-chrome
# Not pkill -f: over SSH it matches the shell running this very line.
for p in $(pgrep -x chrome); do
    tr '\0' ' ' < /proc/$p/cmdline | grep -q "user-data-dir=$PROFILE" && kill "$p"
done
sleep 2
setsid -f google-chrome-stable --user-data-dir=$PROFILE --no-first-run --no-default-browser-check \
    --ozone-platform=wayland --ignore-certificate-errors --remote-debugging-port=$PORT \
    --start-maximized --autoplay-policy=no-user-gesture-required --disable-infobars \
    --disable-search-engine-choice-screen --disable-sync --password-store=basic \
    --disable-features=CalculateNativeWinOcclusion,LocalNetworkAccessChecks,LocalNetworkAccessChecksWebSockets,LocalNetworkAccessChecksWebRTC \
    --disable-backgrounding-occluded-windows --disable-renderer-backgrounding \
    about:blank >/dev/null 2>&1
for i in $(seq 1 30); do
    curl -s --max-time 1 http://127.0.0.1:$PORT/json/version >/dev/null && break
    sleep 0.5
done
curl -s --max-time 2 http://127.0.0.1:$PORT/json/version | grep -E '"Browser"'
