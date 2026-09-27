#!/bin/sh
# Starts PumpkinOS on postmarketOS. Run it from the PumpkinOS directory.
cd "$(dirname "$0")"
export LD_LIBRARY_PATH=./bin
exec ./pumpkin -d 1 -f pumpkin.log -s libscriptlua ./script/pumpkin_pmos.lua
