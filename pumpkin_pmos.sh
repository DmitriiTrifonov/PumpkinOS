#!/bin/sh
# Starts PumpkinOS on postmarketOS. Run it from the PumpkinOS directory.
cd "$(dirname "$0")"
export LD_LIBRARY_PATH=./bin
# Size of a PumpkinOS pixel in physical screen pixels. The window uses the
# physical resolution of the screen, so the picture is not scaled by the
# compositor. On a 2160x1080 screen, 2 gives a 1080x540 PumpkinOS screen.
export PUMPKIN_WAYLAND_ZOOM=${PUMPKIN_WAYLAND_ZOOM:-2}
exec ./pumpkin -d 1 -f pumpkin.log -s libscriptlua ./script/pumpkin_pmos.lua
