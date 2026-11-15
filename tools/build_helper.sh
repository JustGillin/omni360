#!/bin/sh
# Builds Omni360 Helper for a release: Windows without a console window, and
# Linux. The files land in Release/dist/helper/, to attach beside the XEX.
#
# macOS isn't built here: the tray library needs cgo there, so it has to be
# built on a Mac (go build -o Omni360Helper ./helper).
set -e
cd "$(dirname "$0")/../helper"
out=../Release/dist/helper
mkdir -p "$out"

GOOS=windows GOARCH=amd64 go build -trimpath -ldflags "-s -w -H=windowsgui" -o "$out/Omni360Helper.exe" .
GOOS=linux GOARCH=amd64 go build -trimpath -ldflags "-s -w" -o "$out/Omni360Helper-linux" .

ls -la "$out"
