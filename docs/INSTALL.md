# The phone as a node: install

A PinePhone or PinePhone Pro with the Pine64 LoRa back cover, running Mobian (Debian 13) or another Debian-based system with systemd, becomes a Meshtastic node in two ways.

## The one-command way

The `meshsat` package from [meshsat-linux](https://github.com/meshsat/meshsat-linux) installs everything at once: the daemon, its configuration, Meshtastic's web client, the watchdog, the MeshSat Bridge and the MeshSat app for the phone's screen. One command, then the app's icon is in the app grid:

```
sudo apt install ./meshsat_<version>_arm64.deb
```

That page says where to get the file. The rest of this page is the source route, which is also what the package does under the hood.

## From source

What you need on the phone: `git cmake g++ make python3 python3-venv i2c-tools openssl curl`, about 3 GB of free space for the firmware build, and an hour.

1. **Check the cover.** `sudo i2cdetect -y 5` (`-y 2` on the original PinePhone) shows `28`: the bridge in the cover answers. Nothing at 0x28 means the cover is not seated; press it home until it clicks.
2. **Build the daemon**, on the phone, from the MeshSat fork of the Meshtastic firmware. Stock `meshtasticd` does not know the cover:
   ```
   git clone https://github.com/meshsat/meshsat-lora-backplate.git
   cd meshsat-lora-backplate
   sh packaging/build-daemon.sh
   ```
   A full build took 31 minutes on a PinePhone Pro. The script prints the binary's path and its sha256.
3. **Install.** As root, from the same checkout:
   ```
   sudo sh packaging/install.sh --daemon ~/meshsat-firmware/.pio/build/meshsat-pinephone-pro/meshtasticd
   ```
   It installs the daemon under `/usr/local/bin`, the configuration under `/etc/meshtasticd`, Meshtastic's web client under `/usr/share/meshtasticd/web`, the watchdog and the bench tools under `/usr/local/lib/meshsat`, the `i2c-dev` module and the bus permissions, and starts `meshtasticd.service` and `meshsat-radio-watch.timer`. It keeps the keyboard case's drivers off the bus (`/etc/modprobe.d/meshsat-no-keyboard.conf`; delete it to use the case again). Run it again after a change: every step checks before it changes.
4. **Give the node its channels.** A new node has a region and no channel. Put a Meshtastic channel URL (from the app's share dialog: it carries the keys, so keep it out of chat logs and shell histories) in a file, then:
   ```
   cat channel-url.txt | meshsat-node-channels --owner my-phone --short MYPH --role CLIENT_MUTE
   ```
   The first run makes a private Python environment for the Meshtastic client library (needs the network once). A node that was run on the bench before keeps its keys and channels: the install copies `~/.portduino/default/prefs` of the user who ran the install.
5. **Look.** `sudo journalctl -fu meshtasticd` shows the daemon; the line to look for is `Final Tx power: 0 dBm`. Meshtastic's web client is at `https://localhost:9443/` in the phone's browser, with the self-signed certificate the install made (accept it once; without a certificate the daemon serves plain `http://` on the same port). It is laid out for a desktop; the phone app is in meshsat-linux. Other nodes see the phone as the name you gave it.

## What the node does, and does not do

| | |
|---|---|
| Receives | Everything on its channels, at any length: proven at -96 dBm from a T-Deck in the same flat. |
| Sends | At **0 dBm, 1 mW**, the one power the radio is qualified at: texts of 111 to 123 bytes were accepted by every receiver whose log covered them and read as text (28 September 2026). Range at that power was not measured: expect a house or a street, not the kilometres of an ordinary node. |
| Above 0 dBm | Not qualified. From 5 dBm up, the frames a node really sends arrived damaged on the bench (frames of 126 bytes or more: 0 of 22). `SX126X_MAX_POWER` in `/etc/meshtasticd/config.d/lora-pinedio-backcover.yaml` is the cap. The suspected cause is the board's plain crystal, not a TCXO, drifting as the amplifier heats: the receivers saw its frequency move with what it had just sent. That this is what damages the frames is not proven. A TCXO on the board's empty footprint is the usual remedy; it has not been tried here. |
| After sending | Deaf for 5 to 28 seconds (short frames from 11 s again, long ones later). So every broadcast goes out three times, other nodes see the copies as duplicates, and the ack for a direct message is usually missed. |
| Direct messages | To a node whose announcement the phone has heard. Before that the daemon refuses them (`PKI_SEND_FAIL_PUBLIC_KEY`). |
| Unattended | The radio can stop answering, and only a hand resets it: take the cover off and press it back on. The watchdog then starts the node again by itself, see below. Until a board with a reset line exists, do not rely on the node while nobody is near it. |

Every number comes from `docs/data/` and [`docs/BACKPLATE.md`](BACKPLATE.md).

## When the radio stops answering

The daemon prints `LoRa back cover does not answer on /dev/i2c-5` and exits; systemd tries five times in 200 s and stops. `meshsat-radio-watch` (a timer, every minute) then:

- writes what it means to `/run/meshsat-node/status` and to the journal (`journalctl -u meshsat-radio-watch`): *the radio in the back cover does not answer: take the cover off and press it back on until it clicks*, or *the back cover does not answer on the bus: check that it is seated*;
- shows a desktop notification if `notify-send` is installed;
- asks the radio once a minute whether it answers again (`lora-listen --seconds 1`, only while the daemon is stopped: the two must never share the bus), and starts the daemon when it does.

While the daemon runs, the watchdog only counts: more than three recoveries or twenty refused packets in ten minutes are noted as *unstable* in the status file.

`sudo /usr/local/lib/meshsat/bin/bridge-selftest /dev/i2c-5` checks a cover without a radio library, transmitting nothing; stop the daemon first (`sudo systemctl stop meshtasticd`).

## What is installed where

| Path | What |
|---|---|
| `/usr/local/bin/meshtasticd` | the daemon, built from meshsat-firmware `6777ecc`, env `meshsat-pinephone-pro` |
| `/etc/meshtasticd/config.yaml`, `config.d/lora-pinedio-backcover.yaml` | the configuration; the cap at 0 dBm lives in the second |
| `/etc/meshtasticd/ssl/` | a self-signed certificate, since the daemon cannot make its own on Linux |
| `/var/lib/meshtasticd/.portduino/default/prefs` | the node's keys, channels and node database (user `meshtasticd`, group `i2c`) |
| `/usr/share/meshtasticd/web/` | Meshtastic's web client v2.7.2, uncompressed |
| `/usr/local/lib/meshsat/watchdog/meshsat_radio_watch.py` | the watchdog |
| `/usr/local/lib/meshsat/bin/lora-listen`, `bridge-selftest` | the alive check and the self-test |
| `/usr/local/lib/meshsat/node-setup/set-channels.py`, `/usr/local/bin/meshsat-node-channels` | the channel setup |
| `/etc/systemd/system/meshtasticd.service`, `meshsat-radio-watch.{service,timer}` | the units |
| `/etc/modules-load.d/meshsat-node.conf`, `/etc/udev/rules.d/60-meshsat-node.rules`, `/etc/modprobe.d/meshsat-no-keyboard.conf` | the bus |
