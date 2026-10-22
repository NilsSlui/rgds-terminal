#!/bin/sh
# Builds app/terminal/dsterm for the device in an arm64 Debian container (Podman).
# Extra arguments are passed to make, e.g. scripts/build.sh clean
set -e
cd "$(dirname "$0")/.."
podman build --arch arm64 -q -t rgds-terminal-build -f build/Containerfile build >/dev/null
podman run --rm --arch arm64 -v "$PWD":/src rgds-terminal-build make "$@"
