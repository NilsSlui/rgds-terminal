#!/bin/sh
# Builds the app and zips it as dist/rgds-terminal.zip, laid out so its contents
# can be copied straight into Roms/APPS on the SD card. The licence texts go in
# the terminal folder.
set -e
cd "$(dirname "$0")/.."
scripts/build.sh
rm -rf dist
mkdir -p dist/stage
cp -R app/Terminal.sh app/Imgs app/terminal dist/stage/
rm -f dist/stage/terminal/settings.conf dist/stage/terminal/log.txt
cp LICENSE dist/stage/terminal/LICENSE
{
    echo "rgds-terminal includes libvterm 0.3.3 (https://www.leonerd.org.uk/code/libvterm/)."
    echo
    cat third_party/libvterm-0.3.3/LICENSE
} > dist/stage/terminal/THIRD_PARTY_LICENSES.txt
(cd dist/stage && zip -qr ../rgds-terminal.zip Terminal.sh Imgs terminal -x '*.DS_Store')
rm -rf dist/stage
echo "Created dist/rgds-terminal.zip"
