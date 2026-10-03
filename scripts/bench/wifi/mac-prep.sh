#!/bin/bash
# The Mac (mw-mac) for a bench: Léo's games and his auto-clicker stopped (Bruno's
# go, 29/09 and 03/10/2026: they may stop during a bench, reopened after), and
# caffeinate for 2 h. What was running is written down first, for
# mac-restore.sh to reopen exactly that. Discord is left as it is.
#   ssh mw-mac bash -s < mac-prep.sh
L=~/mw-c925/bench-apps.txt
mkdir -p ~/mw-c925
running() { ps -axo args= | grep -E "$1" | grep -v grep | head -1; }
: > "$L"
[ -n "$(running '[E]xoPanda.app/Contents/MacOS/ExoPanda')" ] && echo exopanda >> "$L"
[ -n "$(running '[R]oblox.app/Contents/MacOS/Roblox')" ] && echo roblox >> "$L"
[ -n "$(running 'WarThunder.app/Contents/MacOS/[a]ces')" ] && echo warthunder >> "$L"
[ -n "$(running '[O]PAutoClicker.app/Contents/MacOS/OPAutoClicker')" ] && echo autoclicker >> "$L"
echo "== running before: $(tr '\n' ' ' < "$L")"

stop() {  # $1 = a piece of the executable's path; $2 = the signal
  for p in $(pgrep -f "$1"); do
    echo "$2 $p $(ps -o comm= -p "$p" | sed 's#.*/##')"
    kill "-$2" "$p"
  done
}
stop "OPAutoClicker.app/Contents/MacOS/OPAutoClicker" TERM
stop "/Applications/ExoPanda Recorder.app/Contents/MacOS/exotask-mac" TERM
stop "/Applications/ExoPanda.app/Contents/MacOS/ExoPanda" TERM
# Roblox's menu bar helper relaunches its player: the helper first.
stop "/Applications/Roblox.app/Contents/MacOS/RobloxMenuBar" TERM
sleep 2
stop "/Applications/Roblox.app/Contents/MacOS/Roblox" TERM
stop "/Applications/WarThunderLauncher.app/Contents/WarThunder.app/Contents/MacOS/aces" TERM
sleep 6
stop "/Applications/Roblox.app/Contents/MacOS/" KILL

nohup caffeinate -i -s -t 7200 >/dev/null 2>&1 &
echo "caffeinate $!"
sleep 2
echo "== left:"
ps -axo pid=,args= | grep -i -E '[O]PAutoClicker|[e]xopanda|[e]xotask|[R]oblox|/[a]ces' | cut -c1-120 || true
