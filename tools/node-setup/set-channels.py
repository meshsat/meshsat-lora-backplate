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

    # A node that never had a region makes its keys when it gets one, and takes its number
    # from them. On a slow radio link that is a minute or two, and everything addressed to
    # the old number afterwards is refused. So the region goes first, alone.
    interface = connect(args.host)
    if interface.localNode.localConfig.lora.region != region:
        lora = interface.localNode.localConfig.lora
        lora.region = region
        if args.no_tx:
            lora.tx_enabled = False
        interface.localNode.writeConfig("lora")
        print(f"region set to {args.region}, waiting for the node to settle", flush=True)
        time.sleep(5)
        interface.close()
        settled = False
        for _ in range(90):
            time.sleep(4)
            try:
                interface = meshtastic.tcp_interface.TCPInterface(args.host)
            except Exception:
                continue
            if interface.localNode.localConfig.lora.region == region:
                settled = True
                break
            interface.close()
        if not settled:
            raise SystemExit("the node did not come back with the region set")

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
    time.sleep(5)
    interface.close()

    # Believe nothing that was not read back.
    time.sleep(10)
    interface = connect(args.host)
    node = interface.localNode
    problems = []
    if node.localConfig.lora.region != region:
        problems.append("region")
    if role is not None and node.localConfig.device.role != role:
        problems.append("role")
    if args.tz and node.localConfig.device.tzdef != args.tz:
        problems.append("time zone")
    if args.no_tx and node.localConfig.lora.tx_enabled:
        problems.append("transmitter still on")
    me = interface.getMyUser() or {}
    if args.owner and me.get("longName") != args.owner:
        problems.append("name")
    if args.short and me.get("shortName") != args.short:
        problems.append("short name")
    for index, settings in enumerate(wanted.settings):
        have = node.channels[index].settings
        if have.name != settings.name or have.psk != settings.psk:
            problems.append(f"channel {index}")
    lora = node.localConfig.lora
    number = interface.myInfo.my_node_num if interface.myInfo else 0
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
    print(f"node !{number:08x}: " + said)
    if problems:
        print("NOT APPLIED: " + ", ".join(problems), file=sys.stderr)
        return 1
    print("read back and verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
