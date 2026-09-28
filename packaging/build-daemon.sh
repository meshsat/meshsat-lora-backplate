#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build meshtasticd for the PinePhone's LoRa back cover, on the phone itself.
#
#   sh packaging/build-daemon.sh            # ~/meshsat-firmware at the pinned commit, env meshsat-pinephone-pro
#
# The MeshSat fork of the Meshtastic firmware carries the back cover as a radio bus
# (`spidev: pinedio-i2c`), so stock meshtasticd cannot be used. PlatformIO builds it natively
# for the phone's own architecture; a full build took 31 minutes on a PinePhone Pro, an
# incremental one 1 to 18. Cross-building on another machine is not set up.
set -eu
FIRMWARE=${MESHSAT_FIRMWARE_DIR:-$HOME/meshsat-firmware}
COMMIT=${MESHSAT_FIRMWARE_COMMIT:-6777ecc53657b3f5a6684a78e4f906f6e12ee042}
ENV=meshsat-pinephone-pro
VENV=${MESHSAT_PIO_VENV:-$HOME/pio-venv}

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1 (apt install $2)" >&2; exit 1; }; }
need git git; need python3 python3; need g++ g++; need cmake cmake

if [ ! -d "$FIRMWARE/.git" ]; then
    git clone https://github.com/meshsat/meshsat-firmware.git "$FIRMWARE"
fi
git -C "$FIRMWARE" fetch --quiet origin
git -C "$FIRMWARE" checkout --quiet "$COMMIT"
git -C "$FIRMWARE" submodule update --init --recursive --quiet

if [ ! -x "$VENV/bin/platformio" ]; then
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install --quiet --disable-pip-version-check platformio
fi

echo "build start $(date '+%F %T'), commit $COMMIT, env $ENV"
cd "$FIRMWARE"
"$VENV/bin/platformio" run -e "$ENV"
BIN="$FIRMWARE/.pio/build/$ENV/meshtasticd"
echo "build end $(date '+%F %T')"
ls -la "$BIN"
sha256sum "$BIN"
echo "next: sudo sh packaging/install.sh --daemon $BIN"
