#!/bin/bash
# Terminal: a shell on the top screen with a touch keyboard on the bottom screen.

progdir="$(cd "$(dirname "$0")" && pwd)/terminal"

export XDG_RUNTIME_DIR=/var/run
export WAYLAND_DISPLAY=wayland-0
export SDL_VIDEODRIVER=wayland

# Make sure the bottom screen is on and its touch panel is enabled.
if [ "$(cat /sys/class/anbernic_misc/lcdoff 2>/dev/null)" != "0" ]; then
	echo 0 > /sys/class/anbernic_misc/lcdoff
	/mnt/vendor/bin/bright.dge
fi
old_tp="$(cat /sys/class/anbernic_misc/tpctrl 2>/dev/null)"
echo 0 > /sys/class/anbernic_misc/tpctrl

"$progdir/dsterm" > "$progdir/log.txt" 2>&1

[ -n "$old_tp" ] && echo "$old_tp" > /sys/class/anbernic_misc/tpctrl
