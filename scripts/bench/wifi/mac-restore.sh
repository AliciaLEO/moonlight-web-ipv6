#!/bin/bash
# The Mac back to Léo after a bench: the bench Chrome closed, caffeinate stopped,
# and what mac-prep.sh found running reopened (all of it if it wrote nothing).
#   ssh mw-mac bash -s < mac-restore.sh
L=~/mw-c925/bench-apps.txt
echo "== bench chrome"; bash ~/mw-c925/mac-chrome.sh stop
echo "== caffeinate"
pids=$(ps -axo pid=,args= | awk '/[c]affeinate -i -s -t 7200/ {print $1}')
if [ -n "$pids" ]; then kill $pids && echo "stopped $pids"; else echo "none running"; fi
apps=$(cat "$L" 2>/dev/null)
[ -z "$apps" ] && apps="exopanda roblox warthunder autoclicker"
echo "== apps: $(echo $apps)"
for a in $apps; do
  case $a in
    exopanda) open -a /Applications/ExoPanda.app && echo "ExoPanda opened" ;;
    roblox) open /Applications/Roblox.app/Contents/MacOS/RobloxMenuBar.app && echo "RobloxMenuBar opened" ;;
    warthunder) open /Applications/WarThunderLauncher.app/Contents/WarThunder.app && echo "WarThunder opened" ;;
    autoclicker) open ~/Downloads/OPAutoClicker.app && echo "OPAutoClicker opened" ;;
  esac
done
rm -f "$L"
sleep 8
ps -axo args= | grep -E '[E]xoPanda.app/Contents/MacOS/ExoPanda|[R]obloxMenuBar.app/Contents/MacOS|WarThunder.app/Contents/MacOS/[a]ces|[O]PAutoClicker' | cut -c1-110
