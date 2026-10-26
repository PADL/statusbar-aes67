# Debian builder image for statusbar exported packages.
#
# DEBIAN_VERSION selects the base image; the container-build scripts pass
# it through and tag the built image accordingly.
ARG DEBIAN_VERSION=trixie
FROM docker.io/library/debian:${DEBIAN_VERSION}

# STATUSBAR_TOOLCHAIN selects which C++ compiler the image is provisioned for.
# The container-build scripts pass it through and include it in the image tag.
#
#   clang (default) — Debian's clang + libc++, which every Debian base carries.
#   gcc             — additionally installs g++-16 for the C++26 build and
#                     points the unversioned gcc/g++ at it via
#                     update-alternatives, so a toolchain file that looks for
#                     plain `g++` on PATH finds the right one. Needs a base
#                     that ships g++-16: trixie stops at g++-14, so the scripts
#                     default DEBIAN_VERSION to forky for this toolchain.
#
# clang is installed either way: the XDP filter in statusbar-core is built with
# `clang -target bpf`, and GCC has no BPF backend, so a gcc build still needs
# clang present for that one object.
ARG STATUSBAR_TOOLCHAIN=clang

RUN apt-get update && apt-get install -y --no-install-recommends \
    clang clang-tools lld \
    libc++-dev libc++abi-dev libclang-rt-dev \
    cmake ninja-build ccache pkg-config \
    dpkg-dev file ca-certificates \
    python3 python3-jsonschema \
    libasound2-dev libbpf-dev libxdp-dev libelf-dev zlib1g-dev \
    && if [ "$STATUSBAR_TOOLCHAIN" = "gcc" ]; then \
         apt-get install -y --no-install-recommends g++-16 \
         && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-16 100 \
              --slave /usr/bin/g++ g++ /usr/bin/g++-16 ; \
       fi \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
