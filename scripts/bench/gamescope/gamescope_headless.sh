#!/bin/bash
# Plan « Idées Punktfunk », C5: gamescope headless in a container, on this machine's GPU, its
# PipeWire stream on the user's session PipeWire and its EIS socket in a host directory.
#
#   gamescope_headless.sh start [W H HZ]   app: $APP (default vkcube on X11), image: $IMG
#   gamescope_headless.sh node             the stream's PipeWire node id
#   gamescope_headless.sh count [S W H HZ] frames a second for S seconds (../mutter/pw_vcount)
#   gamescope_headless.sh logs | stop
#
# Run as the desktop's user (uid 1000 here), in the user's graphical session. $RENDER and $CARD
# name the GPU's nodes; the container gets the host's render and video groups.
set -u
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
IMG=${IMG:-mw-c5-gamescope:3}
RENDER=${RENDER:-/dev/dri/renderD128}
CARD=${CARD:-/dev/dri/card1}
XDG=${XDG:-/tmp/mw-c5-xdg}
HERE=$(cd "$(dirname "$0")" && pwd)

node() {
    pw-cli ls Node 2>/dev/null |
        awk '/^[[:space:]]*id [0-9]+,/{id=$2; sub(",","",id)} /node.name = "gamescope"/{print id}' | head -1
}

case "${1:-}" in
start)
    W=${2:-1920}; H=${3:-1080}; HZ=${4:-120}
    docker rm -f mw-c5 >/dev/null 2>&1
    mkdir -p "$XDG" && chmod 700 "$XDG"
    # shellcheck disable=SC2086
    docker run -d --name mw-c5 \
        --device "$RENDER" --device "$CARD" \
        --user "$(id -u):$(id -g)" \
        --group-add "$(getent group render | cut -d: -f3)" --group-add "$(getent group video | cut -d: -f3)" \
        -v "$XDG":/tmp/xdg -v "$XDG_RUNTIME_DIR/pipewire-0":/tmp/xdg/pipewire-0 \
        -e XDG_RUNTIME_DIR=/tmp/xdg -e HOME=/home/mw \
        --entrypoint gamescope "$IMG" \
        --backend headless -W "$W" -H "$H" -w "$W" -h "$H" -r "$HZ" -- ${APP:-vkcube --wsi xcb} >/dev/null
    sleep 6
    docker ps --filter name=mw-c5 --format '{{.Names}} {{.Status}}'
    echo "EIS socket: $XDG/gamescope-0-ei"
    ;;
node) node ;;
count)
    N=$(node)
    [ -n "$N" ] || { echo "no gamescope node"; exit 1; }
    timeout $((${2:-8} + 4)) "$HERE/../mutter/pw_vcount" "$N" 0 "${3:-1920}" "${4:-1080}" "${5:-120}" dmabuf
    ;;
logs) docker logs --tail "${2:-30}" mw-c5 2>&1 ;;
stop) docker rm -f mw-c5 >/dev/null 2>&1 ;;
*) sed -n '2,12p' "$0"; exit 2 ;;
esac
