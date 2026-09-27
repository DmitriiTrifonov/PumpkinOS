# Build environment for PumpkinOS on postmarketOS (Alpine Linux / musl).
# postmarketOS v26.06 is based on Alpine 3.24. Change ALPINE_VERSION to
# match the Alpine base of the postmarketOS release installed on the device.
ARG ALPINE_VERSION=3.24
FROM alpine:${ALPINE_VERSION}

RUN apk add --no-cache \
    build-base \
    make \
    git \
    bash \
    coreutils \
    sdl2-dev \
    mesa-dev \
    wayland-dev \
    wayland-protocols \
    libxkbcommon-dev \
    libdecor-dev \
    alsa-lib-dev \
    pipewire-dev \
    pulseaudio-dev \
    unicorn-dev

WORKDIR /src/PumpkinOS/src
