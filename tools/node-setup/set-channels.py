#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Give a running meshtasticd the channels of a Meshtastic channel URL.

`meshtastic --seturl` also applies the radio settings the URL carries, transmit enable
included. This sets the region, takes the modem preset from the URL, writes the channels,
and leaves the rest of the radio settings alone. With --no-tx it switches the transmitter
off in the same step.

The URL holds the channel keys. It is read from standard input so that it never appears
on a command line or in a shell history, and nothing of it is printed but channel names.

    ssh laptop cat channel-url.txt | set-channels.py --no-tx
"""
import argparse
import base64
import sys
import time

import meshtastic.tcp_interface
from meshtastic.protobuf import apponly_pb2, channel_pb2, config_pb2


def channel_set(url: str) -> apponly_pb2.ChannelSet:
    fragment = url.strip().split("#", 1)[-1].split("?", 1)[0]
    fragment = fragment.rsplit("/", 1)[-1]
    raw = base64.urlsafe_b64decode(fragment + "=" * (-len(fragment) % 4))
    parsed = apponly_pb2.ChannelSet()
    parsed.ParseFromString(raw)
    if not parsed.settings:
        raise ValueError("no channel in that URL")
    return parsed


def connect(host: str, tries: int = 30):
    last = None
    for _ in range(tries):
        try:
            return meshtastic.tcp_interface.TCPInterface(host)
        except Exception as error:  # the daemon restarts after a radio setting changed
            last = error
            time.sleep(2)
    raise SystemExit(f"no meshtasticd on {host}: {last}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--region", default="EU_868")
    parser.add_argument("--no-tx", action="store_true", help="switch the transmitter off as well")
    args = parser.parse_args()

    wanted = channel_set(sys.stdin.readline())
    region = config_pb2.Config.LoRaConfig.RegionCode.Value(args.region)

    interface = connect(args.host)
    node = interface.localNode
    node.beginSettingsTransaction()

    lora = node.localConfig.lora
    lora.region = region
    if wanted.HasField("lora_config") and wanted.lora_config.use_preset:
        lora.use_preset = True
        lora.modem_preset = wanted.lora_config.modem_preset
    if args.no_tx:
        lora.tx_enabled = False
    node.writeConfig("lora")

    for index in range(len(node.channels)):
        channel = channel_pb2.Channel()
        channel.index = index
        if index < len(wanted.settings):
            channel.role = channel_pb2.Channel.Role.PRIMARY if index == 0 else channel_pb2.Channel.Role.SECONDARY
            channel.settings.CopyFrom(wanted.settings[index])
        else:
            channel.role = channel_pb2.Channel.Role.DISABLED
        node.channels[index].CopyFrom(channel)
        node.writeChannel(index)

    node.commitSettingsTransaction()
    time.sleep(2)
    interface.close()

    names = ", ".join(s.name or "(default)" for s in wanted.settings)
    preset = config_pb2.Config.LoRaConfig.ModemPreset.Name(lora.modem_preset)
    print(f"region {args.region}, preset {preset}, channels: {names}" + (", transmitter off" if args.no_tx else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
