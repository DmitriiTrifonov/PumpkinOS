#!/bin/sh
# Builds PumpkinOS for postmarketOS inside an Alpine container.
# Usage: docker/pmos/build.sh [make arguments...]
# Environment:
#   PLATFORM=linux/amd64   build for x86_64 devices (default linux/arm64)
#   ALPINE_VERSION=3.24    Alpine base of the target postmarketOS release
#   TYPE=release           optimized build (default); TYPE=debug for a debug build.
#                          Run "build.sh clean" when switching, objects are not
#                          rebuilt when only the flags change.

set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PLATFORM=${PLATFORM:-linux/arm64}
ALPINE_VERSION=${ALPINE_VERSION:-3.24}
IMAGE=pumpkinos-pmos-build:$ALPINE_VERSION-$(echo "$PLATFORM" | tr / -)

docker build --platform "$PLATFORM" --build-arg ALPINE_VERSION="$ALPINE_VERSION" -t "$IMAGE" "$ROOT/docker/pmos"
docker run --rm --platform "$PLATFORM" -v "$ROOT":/src/PumpkinOS "$IMAGE" make TYPE="${TYPE:-release}" "$@"
