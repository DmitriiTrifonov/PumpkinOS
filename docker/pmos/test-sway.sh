#!/bin/sh
# Runs the PumpkinOS build under a headless sway compositor (as used by
# sxmo-de-sway) and saves a screenshot and the logs.
# Usage: docker/pmos/test-sway.sh [output dir] (default: docker/pmos/out)
# Environment:
#   RESOLUTION=2160x1080   headless output size (Pixel 3a XL, landscape)
#   SECONDS_TO_RUN=15      how long PumpkinOS runs before the screenshot
#   LAUNCHER=pumpkin_pmos.sh  startup script in the PumpkinOS root
#   SCALE=1                output scale (sxmo usually sets 2 or more on phones)
#   CLICKS="x,y ..."       left clicks after the first screenshot, in output
#                          pixels; screenshot-after.png is taken after them
#   PLATFORM, ALPINE_VERSION  same as in build.sh

set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(mkdir -p "${1:-$ROOT/docker/pmos/out}" && cd "${1:-$ROOT/docker/pmos/out}" && pwd)
PLATFORM=${PLATFORM:-linux/arm64}
ALPINE_VERSION=${ALPINE_VERSION:-3.24}
IMAGE=pumpkinos-pmos-build:$ALPINE_VERSION-$(echo "$PLATFORM" | tr / -)

# sway has the cap_sys_nice file capability, it cannot be executed without SYS_NICE
docker run --rm --platform "$PLATFORM" --cap-add SYS_NICE \
  -e RESOLUTION="${RESOLUTION:-2160x1080}" -e SECONDS_TO_RUN="${SECONDS_TO_RUN:-15}" \
  -e LAUNCHER="${LAUNCHER:-pumpkin_pmos.sh}" -e SCALE="${SCALE:-1}" -e CLICKS="$CLICKS" \
  -v "$ROOT":/src/PumpkinOS -v "$OUT":/out "$IMAGE" sh -c '
apk add -q sway grim font-dejavu wlr-protocols >/dev/null 2>&1
if [ -n "$CLICKS" ]; then
  make -s -C /src/PumpkinOS/docker/pmos/vclick >/dev/null || exit 1
fi
export XDG_RUNTIME_DIR=/tmp/xdg
mkdir -p -m 700 $XDG_RUNTIME_DIR
echo "output HEADLESS-1 resolution $RESOLUTION scale $SCALE" > /tmp/sway.conf
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman sway -c /tmp/sway.conf > /out/sway.log 2>&1 &
sleep 3
export WAYLAND_DISPLAY=$(ls $XDG_RUNTIME_DIR | grep -E "^wayland-[0-9]+$" | head -1)
[ -n "$WAYLAND_DISPLAY" ] || { echo "sway failed to start, see sway.log"; exit 1; }
cd /src/PumpkinOS
rm -f pumpkin.log
sh ./$LAUNCHER &
PID=$!
sleep $SECONDS_TO_RUN
grim /out/screenshot.png
if [ -n "$CLICKS" ]; then
  /src/PumpkinOS/docker/pmos/vclick/vclick ${RESOLUTION%x*} ${RESOLUTION#*x} $CLICKS
  sleep 3
  grim /out/screenshot-after.png
fi
if kill -0 $PID 2>/dev/null; then echo "PumpkinOS is running"; R=0; else echo "PumpkinOS exited early"; R=1; fi
kill $PID 2>/dev/null; sleep 2
cp pumpkin.log /out/
echo "errors in pumpkin.log: $(grep -c " E " /out/pumpkin.log)"
exit $R
'
echo "Output in $OUT"
