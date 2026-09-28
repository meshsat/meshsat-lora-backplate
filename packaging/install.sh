#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Make this PinePhone (Pro) with the Pine64 LoRa back cover a Meshtastic node under systemd,
# from source. Run as root, from a checkout of meshsat-lora-backplate, after
# packaging/build-daemon.sh has built the daemon:
#
#   sudo sh packaging/install.sh --daemon ~/meshsat-firmware/.pio/build/meshsat-pinephone-pro/meshtasticd
#
# Idempotent: every step checks before it changes. What it does, in order: packages, the
# i2c-dev module and the bus permissions, the keyboard-case drivers out of the way, the
# system user, the daemon and the bench tools, the configuration, the node's identity if
# this user's bench node has one, Meshtastic's web client, a certificate, the units. The
# one-command way is the meshsat .deb from meshsat-linux; this script is what its
# postinst does, for a source install.
set -eu

DAEMON=""
WEB_TAR=""
WEB_VERSION=v2.7.2
WEB_SHA256=62657b85b4c24af4d44da2932b64143abdbf6e65a79fcb51fd0801d9540616e2
NO_WEB=0
while [ $# -gt 0 ]; do
    case "$1" in
        --daemon) DAEMON=$2; shift 2 ;;
        --web-tar) WEB_TAR=$2; shift 2 ;;
        --no-web) NO_WEB=1; shift ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo)" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CALLER=${SUDO_USER:-}
CALLER_HOME=$( [ -n "$CALLER" ] && getent passwd "$CALLER" | cut -d: -f6 || echo "")
[ -n "$DAEMON" ] || DAEMON=${CALLER_HOME:-$HOME}/meshsat-firmware/.pio/build/meshsat-pinephone-pro/meshtasticd
[ -x "$DAEMON" ] || { echo "no daemon binary at $DAEMON: build it with packaging/build-daemon.sh or give --daemon" >&2; exit 2; }

say() { printf '%s\n' "install: $*"; }

# 1. Packages
need=""
for p in i2c-tools openssl curl python3 python3-venv; do dpkg -s "$p" >/dev/null 2>&1 || need="$need $p"; done
if [ -n "$need" ]; then say "apt install$need"; apt-get install -y -q $need; fi

# 2. The bus: the module at boot and now, the group, the rule
install -m 0644 "$HERE/modules-load.d/meshsat-node.conf" /etc/modules-load.d/meshsat-node.conf
modprobe i2c-dev || true
getent group i2c >/dev/null || groupadd --system i2c
install -m 0644 "$HERE/udev/60-meshsat-node.rules" /etc/udev/rules.d/60-meshsat-node.rules
udevadm control --reload && udevadm trigger --subsystem-match=i2c-dev || true
install -m 0644 "$HERE/modprobe.d/meshsat-no-keyboard.conf" /etc/modprobe.d/meshsat-no-keyboard.conf
for m in pinephone_keyboard ip5xxx_power; do lsmod | grep -q "^$m " && modprobe -r "$m" || true; done

# 3. The daemon's user and state
getent passwd meshtasticd >/dev/null || useradd --system --home-dir /var/lib/meshtasticd --shell /usr/sbin/nologin --gid i2c meshtasticd 2>/dev/null \
    || useradd --system --home-dir /var/lib/meshtasticd --shell /usr/sbin/nologin meshtasticd
getent group meshtasticd >/dev/null || groupadd --system meshtasticd
usermod -g meshtasticd -G i2c meshtasticd
install -d -o meshtasticd -g meshtasticd -m 0750 /var/lib/meshtasticd

# 4. The daemon and the tools
install -d /usr/local/bin /usr/local/lib/meshsat/bin /usr/local/lib/meshsat/watchdog /usr/local/lib/meshsat/node-setup
say "daemon from $DAEMON"
install -m 0755 "$DAEMON" /usr/local/bin/meshtasticd
strip --strip-unneeded /usr/local/bin/meshtasticd 2>/dev/null || true
if [ ! -x "$REPO/build/lora-listen" ]; then
    say "building the bench tools (lora-listen for the watchdog's alive check)"
    (cd "$REPO" && cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build -j2 --target lora-listen bridge-selftest >/dev/null)
fi
install -m 0755 "$REPO/build/lora-listen" /usr/local/lib/meshsat/bin/lora-listen
[ -x "$REPO/build/bridge-selftest" ] && install -m 0755 "$REPO/build/bridge-selftest" /usr/local/lib/meshsat/bin/bridge-selftest
install -m 0644 "$HERE/watchdog/meshsat_radio_watch.py" /usr/local/lib/meshsat/watchdog/meshsat_radio_watch.py
install -m 0644 "$REPO/tools/node-setup/set-channels.py" /usr/local/lib/meshsat/node-setup/set-channels.py
sed 's#/usr/lib/meshsat/node-setup#/usr/local/lib/meshsat/node-setup#' "$HERE/bin/meshsat-node-channels" > /usr/local/bin/meshsat-node-channels
chmod 0755 /usr/local/bin/meshsat-node-channels

# 5. Configuration (kept if already there)
install -d /etc/meshtasticd/config.d
[ -f /etc/meshtasticd/config.yaml ] || install -m 0644 "$HERE/meshtasticd/config.yaml" /etc/meshtasticd/config.yaml
[ -f /etc/meshtasticd/config.d/lora-pinedio-backcover.yaml ] || install -m 0644 "$HERE/meshtasticd/config.d/lora-pinedio-backcover.yaml" /etc/meshtasticd/config.d/lora-pinedio-backcover.yaml

# 6. The node's identity: a bench node run by the calling user keeps its keys and channels
if [ -n "$CALLER_HOME" ] && [ -d "$CALLER_HOME/.portduino/default/prefs" ] && [ ! -d /var/lib/meshtasticd/.portduino/default/prefs ]; then
    say "keeping the node identity found in $CALLER_HOME/.portduino/default/prefs"
    install -d -o meshtasticd -g meshtasticd /var/lib/meshtasticd/.portduino/default
    cp -a "$CALLER_HOME/.portduino/default/prefs" /var/lib/meshtasticd/.portduino/default/prefs
    chown -R meshtasticd:meshtasticd /var/lib/meshtasticd/.portduino
fi

# 7. Meshtastic's web client, served by the daemon: uncompressed, because the Linux
#    web server serves the path it is asked for
if [ "$NO_WEB" -eq 0 ] && [ ! -f /usr/share/meshtasticd/web/index.html ]; then
    tmp=$(mktemp -d)
    if [ -n "$WEB_TAR" ]; then cp "$WEB_TAR" "$tmp/build.tar"; else
        say "downloading Meshtastic web client $WEB_VERSION"
        curl -sSL -o "$tmp/build.tar" "https://github.com/meshtastic/web/releases/download/$WEB_VERSION/build.tar"
    fi
    echo "$WEB_SHA256  $tmp/build.tar" | sha256sum -c --quiet || { echo "web client bundle: checksum mismatch" >&2; exit 1; }
    install -d /usr/share/meshtasticd/web
    tar xf "$tmp/build.tar" -C /usr/share/meshtasticd/web
    find /usr/share/meshtasticd/web -name '*.gz' -exec gunzip -f {} \;
    chown -R meshtasticd:meshtasticd /usr/share/meshtasticd
    rm -rf "$tmp"
fi

# 8. A certificate: the daemon cannot make its own on Linux
if [ ! -f /etc/meshtasticd/ssl/certificate.pem ]; then
    install -d -o meshtasticd -g meshtasticd -m 0750 /etc/meshtasticd/ssl
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=meshtastic.local/O=MeshSat" \
        -keyout /etc/meshtasticd/ssl/private_key.pem -out /etc/meshtasticd/ssl/certificate.pem 2>/dev/null
    chown meshtasticd:meshtasticd /etc/meshtasticd/ssl/*.pem
    chmod 0600 /etc/meshtasticd/ssl/private_key.pem
fi

# 9. A bench daemon started by hand gives way
if [ -n "$CALLER_HOME" ] && [ -f "$CALLER_HOME/pinedio/daemon/run.sh" ]; then
    touch "$CALLER_HOME/pinedio/daemon/stop"; chown "$CALLER" "$CALLER_HOME/pinedio/daemon/stop" 2>/dev/null || true
    pkill -TERM -x meshtasticd 2>/dev/null || true
fi

# 10. The units: paths of a source install, then enable and start
sed 's#/usr/bin/meshtasticd#/usr/local/bin/meshtasticd#' "$HERE/systemd/meshtasticd.service" > /etc/systemd/system/meshtasticd.service
sed 's#/usr/lib/meshsat/watchdog#/usr/local/lib/meshsat/watchdog#' "$HERE/systemd/meshsat-radio-watch.service" > /etc/systemd/system/meshsat-radio-watch.service
install -m 0644 "$HERE/systemd/meshsat-radio-watch.timer" /etc/systemd/system/meshsat-radio-watch.timer
systemctl daemon-reload
systemctl enable --now meshtasticd.service meshsat-radio-watch.timer >/dev/null
sleep 3
say "meshtasticd is $(systemctl is-active meshtasticd); web client at http://localhost:9443/ ; log: journalctl -fu meshtasticd"
if [ ! -d /var/lib/meshtasticd/.portduino/default/prefs ]; then
    say "a new node: give it its channels with  cat channel-url.txt | meshsat-node-channels --owner <name> --short <ABCD> --role CLIENT_MUTE"
fi
