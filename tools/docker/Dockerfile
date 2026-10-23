# The Linux tools the build needs that macOS does not have: a GCC for MIPS
# with its binutils (the runtime, the boot ROM, the C library and the SoC's
# test programs), meson and ninja (the C library), and Verilator as a
# fallback for a host without one. Build it once:
#   docker build -t mirlo64-tools tools/docker
# then run any command of the README in it with tools/docker/run.sh.
FROM debian:trixie-slim
RUN apt-get update -q && apt-get install -y -q --no-install-recommends \
        gcc-mips-linux-gnu binutils-mips-linux-gnu \
        gcc g++ make meson ninja-build python3 python3-pip git ca-certificates \
        verilator \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
