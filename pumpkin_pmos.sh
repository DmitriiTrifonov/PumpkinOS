#!/bin/sh
# Starts PumpkinOS on postmarketOS. Run it from the PumpkinOS directory.
cd "$(dirname "$0")"
export LD_LIBRARY_PATH=./bin
# The PumpkinOS screen is PUMPKIN_WAYLAND_SCALE times the logical size of the
# output. With sxmo scale 3 on a 2160x1080 screen the logical size is 720x360,
# too small for the Launcher; 2 gives a 1440x720 PumpkinOS screen.
export PUMPKIN_WAYLAND_SCALE=${PUMPKIN_WAYLAND_SCALE:-2}
exec ./pumpkin -d 1 -f pumpkin.log -s libscriptlua ./script/pumpkin_pmos.lua
