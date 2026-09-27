#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Give a running meshtasticd the channels of a Meshtastic channel URL.

`meshtastic --seturl` also applies the radio settings the URL carries, transmit enable
included. This sets the region, takes the modem preset from the URL, writes the channels,
and leaves the rest of the radio settings alone. With --no-tx it switches the transmitter
off in the same step. Name, role and time zone can be set in the same step, so that a new
node restarts once and comes up as what it is meant to be.

The URL holds the channel keys. It is read from standard input so that it never appears
on a command line or in a shell history, and nothing of it is printed but channel names.

    ssh laptop cat channel-url.txt | set-channels.py --owner my-phone --short MYPH --role CLIENT_MUTE
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
    parser.add_argument("--owner", help="the node's long name")
    parser.add_argument("--short", help="the node's short name, four characters at most")
    parser.add_argument("--role", help="device role, for example CLIENT or CLIENT_MUTE")
    parser.add_argument("--tz", help="POSIX time zone, for example CET-1CEST,M3.5.0,M10.5.0/3")
    parser.add_argument("--hop-limit", type=int, help="hops a packet of ours may travel")
    args = parser.parse_args()
    if args.short and len(args.short) > 4:
        parser.error("--short takes four characters at most")
    role = config_pb2.Config.DeviceConfig.Role.Value(args.role) if args.role else None

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
    if args.hop_limit is not None:
        lora.hop_limit = args.hop_limit
    node.writeConfig("lora")

    if role is not None or args.tz:
        device = node.localConfig.device
        if role is not None:
            device.role = role
        if args.tz:
            device.tzdef = args.tz
        node.writeConfig("device")
    if args.owner or args.short:
        node.setOwner(long_name=args.owner, short_name=args.short)

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
    said = f"region {args.region}, preset {preset}, channels: {names}"
    if args.owner or args.short:
        said += f", name {args.owner or '(kept)'} / {args.short or '(kept)'}"
    if args.role:
        said += f", role {args.role}"
    if args.no_tx:
        said += ", transmitter off"
    print(said)
    return 0


if __name__ == "__main__":
    sys.exit(main())
