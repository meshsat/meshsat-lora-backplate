#!/bin/sh
# Build JF002's PineDio demo for the PinePhone back cover, patched to listen on
# Meshtastic's EU_868 LongFast settings (869.525 MHz, SF11, BW 250 kHz, CR 4/5,
# sync word 0x2B, preamble 16). Milestone A1 of MESHSAT-1379: proves the ATtiny84
# bridge answers and the radio hears the T-Beam before any Meshtastic code exists.
#
# Run on the phone (needs git, cmake >= 3.21, g++, i2c-tools):
#   sh tools/jf002-demo/build.sh [workdir]
#   i2cdetect -l                      # PinePhone: i2c-2. Pro: the pogo bus (ff140000.i2c) is i2c-5 on
#                                     # Megi 6.19, i2c-4 on 6.12; on postmarketOS it may be disabled in the DTB
#   sudo i2cdetect -y <bus>           # 0x28 is the back cover; reseat the cover if it is missing
#   sleep 3600 | sudo <workdir>/pinedio-lora-driver/build/apps/pinephone-communicator/pinephone-communicator /dev/i2c-<bus>
# Keep stdin open with the sleep: at end of file the demo transmits an empty frame every 100 ms.
# Stop it with pkill -TERM before the sleep ends. i2cdetect lives in /usr/sbin and needs root.
# Verified on Mobian 6.12 on 27 Sep 2026: bus i2c-5, 0x28, sync after 26 bytes, T-Deck received.
#
# JF002's driver is LGPL-3.0, SudoMaker's SX126x library LGPL-3.0; neither is vendored here.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=${1:-"$HOME/pinedio"}
mkdir -p "$work" && cd "$work"
[ -d pinedio-lora-driver ] || git clone --depth 1 https://codeberg.org/JF002/pinedio-lora-driver.git
cd pinedio-lora-driver
# .gitmodules points at an ssh URL; fetch the submodule over https instead
if [ ! -f libs/sx126x_driver/SX126x.cpp ]; then
  rm -rf libs/sx126x_driver
  git clone --depth 1 https://github.com/SudoMaker/sx126x_driver.git libs/sx126x_driver
fi
if git apply --check "$here/longfast.patch" 2>/dev/null; then
  git apply "$here/longfast.patch"
elif git apply --reverse --check "$here/longfast.patch" 2>/dev/null; then
  echo "patch already applied"
else
  echo "longfast.patch does not apply to this checkout" >&2; exit 1
fi
mkdir -p build && cd build
cmake -DBUILD_FOR_PINEPHONE=1 -DBUILD_FOR_USB=0 .. >/dev/null
make pinephone-communicator
echo
echo "built: $PWD/apps/pinephone-communicator/pinephone-communicator"
echo "find the bus: i2cdetect -l, then sudo i2cdetect -y <bus> (expect 28), then run the binary with /dev/i2c-<bus>"
