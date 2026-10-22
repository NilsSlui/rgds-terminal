#!/bin/sh
# Copies the app into the device's Apps folder over SSH.
# Usage: scripts/deploy.sh user@host   (for example root@192.168.1.50)
set -e
cd "$(dirname "$0")/../app"
host=${1:?usage: scripts/deploy.sh user@host}
dest=/mnt/mmc/Roms/APPS
ssh "$host" "mkdir -p $dest/Imgs $dest/terminal"
scp -q Terminal.sh "$host:$dest/"
scp -q Imgs/Terminal.png "$host:$dest/Imgs/"
scp -q -r terminal/dsterm terminal/bashrc terminal/terminfo "$host:$dest/terminal/"
ssh "$host" "chmod +x $dest/Terminal.sh $dest/terminal/dsterm"
echo "Installed to $host:$dest"
