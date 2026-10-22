#!/bin/sh
# Captures README screenshots from the device over SSH.
#
#   scripts/screenshot.sh NAME 'COMMAND\r' SECONDS user@host
#
# Starts the installed app, types COMMAND into the shell (\r is Enter), waits
# SECONDS (4 is usually enough), then saves both screens to docs/screenshots/:
#   NAME-top.png, NAME-bottom.png and NAME.png (both screens stacked like the device).
# The device needs Python with Pillow, which the stock firmware ships.
set -e
cd "$(dirname "$0")/.."
name=$1
input=$2
secs=${3:-4}
host=${4:?usage: scripts/screenshot.sh NAME COMMAND SECONDS user@host}
out=docs/screenshots
tmp=/tmp/rgds-terminal-shots
mkdir -p "$out"

ssh "$host" "rm -rf $tmp && mkdir -p $tmp && cd /mnt/mmc/Roms/APPS &&
  XDG_RUNTIME_DIR=/var/run WAYLAND_DISPLAY=wayland-0 SDL_VIDEODRIVER=wayland \
  DSTERM_SNAPSHOT=$tmp DSTERM_SNAPSHOT_AT=$secs DSTERM_INPUT='$input' \
  timeout $((secs + 3)) terminal/dsterm >/dev/null 2>&1 || true"

ssh "$host" "cd $tmp && python3 -" <<'EOF'
from PIL import Image, ImageDraw

top = Image.open('top.bmp').convert('RGB')
bottom = Image.open('bottom.bmp').convert('RGB')
top.save('top.png')
bottom.save('bottom.png')

# Both screens in a simple device frame, at half size for the README.
w, h = top.size
pad, hinge = 36, 40
half = h + 2 * pad  # height of each half of the device
body = Image.new('RGBA', (w + 2 * pad, 2 * half + hinge), (0, 0, 0, 0))
d = ImageDraw.Draw(body)
d.rounded_rectangle([0, 0, body.width - 1, half - 1], 48, fill=(42, 47, 55))
d.rectangle([pad * 3, half, body.width - pad * 3, half + hinge - 1], fill=(30, 34, 40))
d.rounded_rectangle([0, half + hinge, body.width - 1, body.height - 1], 48, fill=(42, 47, 55))
body.paste(top, (pad, pad))
body.paste(bottom, (pad, half + hinge + pad))
body.resize((body.width // 2, body.height // 2), Image.LANCZOS).save('device.png')
EOF

scp -q "$host:$tmp/top.png" "$out/$name-top.png"
scp -q "$host:$tmp/bottom.png" "$out/$name-bottom.png"
scp -q "$host:$tmp/device.png" "$out/$name.png"
ssh "$host" "rm -rf $tmp"
echo "Saved $out/$name.png, $name-top.png and $name-bottom.png"
