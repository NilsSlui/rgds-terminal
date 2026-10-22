#!/bin/sh
# Builds the app and zips it as dist/rgds-terminal.zip, laid out so its contents
# can be copied straight into Roms/APPS on the SD card.
set -e
cd "$(dirname "$0")/.."
scripts/build.sh
mkdir -p dist
rm -f dist/rgds-terminal.zip
(cd app && zip -qr ../dist/rgds-terminal.zip Terminal.sh Imgs/Terminal.png terminal \
    -x 'terminal/settings.conf' 'terminal/log.txt')
echo "Created dist/rgds-terminal.zip"
