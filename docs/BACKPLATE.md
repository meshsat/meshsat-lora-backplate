# The back cover, as verified on the phone

Bench of 27 September 2026: a PinePhone Pro running Mobian (Debian 13, kernel 6.12-rockchip, Tow-Boot 2021.10 in SPI) with the Pine64 LoRa back cover, and a T-Deck Plus, a T-Beam and a T-Deck Pro as Meshtastic nodes on EU_868 LongFast. Everything below was observed on that bench unless marked *from source*.

## Where the cover is

| | Observed |
|---|---|
| Bus | `/dev/i2c-5` on Mobian's 6.12-rockchip kernel. The pogo bus is the RK3399 controller the keyboard driver also probes (`pinephone-keyboard 5-0015`). Other kernels number it differently; find it with `i2cdetect -l`. |
| Address | `0x28`, visible in `sudo i2cdetect -y 5` as soon as the cover is seated, with the phone running. Hot-plugging the cover is fine. |
| Power | From the phone's battery through the pogo pins. The bridge keeps its state across a process kill; a stalled I2C transaction is reported to hang it until the cover is re-seated or the phone is powered off (puurpl, Feb 2026, not reproduced here). |
| Fit | The cover made for the PinePhone fits the Pro and makes contact. Press it home; the contact is known to be intermittent. |

The keyboard case uses the same pogo pins, so the two are never on together. Its driver probes the bus at boot and fails harmlessly when the LoRa cover is on; the bench phone blacklists `pinephone_keyboard` and `ip5xxx_power` (`/etc/modprobe.d/meshsat-no-keyboard.conf`) to keep the bus quiet.

## What the bridge is

An ATtiny84 running [tiny-i2c-spi](https://github.com/zschroeder6212/tiny-i2c-spi) (GPL-3.0, last commit June 2021). From its source, confirmed on the bench:

- **Command `0x01` + N bytes:** chip select low, the N bytes are clocked out over a software SPI (mode 0, MSB first), chip select high. Every byte the radio clocked back is appended to a **128-byte ring buffer** in the ATtiny. One I2C write takes 128 bytes, the command byte included, so a frame is at most 127 bytes. The self-test carried 127 without error.
- **Command `0x02` + 1 byte:** SPI mode, bit order and clock divider. Marked untested in the firmware. Not used.
- **An I2C read returns one byte** from the ring buffer and advances its read index. The write index and the read index are never resynchronised by the firmware, so a driver must read exactly as many bytes as it wrote, forever, and resynchronise once at start.
- **Nothing else.** The bridge has no command for reset, BUSY, DIO1 or any GPIO. The SX1262's NRESET is wired to the ATtiny's own reset pin. DIO1 reaches an ATtiny pin the firmware ignores. The pogo INT pin is not connected to the radio (R42 unfitted on v1.0 boards) and on the Pro it cannot be claimed as a GPIO without killing the I2C bus (puurpl, Feb 2026, not tried here).

## How the bridge behaves in time

Measured on the bench phone, I2C at 100 kHz:

| | Measured |
|---|---|
| An I2C write of N bytes | 0.1 ms per byte, returns at once |
| The first read after a write | N x 3.5 ms: this is when the frame is clocked out |
| Every further read | 0.5 ms |
| Two writes back to back | the second waits N x 3.5 ms for the first frame |
| A frame written and read back | about 4 ms per byte, whatever its length |

**The bridge does not clock a frame out when it is written.** It learns that a write is over when the next I2C transaction starts, clocks the frame out then, and holds that transaction up until it is done. So a command reaches the radio when the read after it begins, and a driver that writes and walks away has not sent anything yet. Reading every reply in full, straight after the write, is what makes a command happen now and what tells when it is over.

**3.5 ms per byte is the firmware's debug output.** tiny-i2c-spi is built with `DEBUG` defined and prints every byte it clocks back as hex on a software serial port at 9600 baud: three characters, 3.1 ms. Batching the reads into one ioctl changes nothing. Only an ATtiny reflashed without `DEBUG` would be faster, by a factor of fifty or more; that needs the ISP pads inside the cover and has not been done here.

What this costs a driver:

| | Bytes | Time |
|---|---|---|
| Ask the radio's status, one byte | 1 | 6 ms |
| Fetch the interrupt flags | 4 | 16 to 19 ms |
| Fetch a received frame of 50 bytes, with its status and signal report | 69 | 290 ms |
| Set up a receive | 33 | 150 ms |
| Carry a full 255-byte buffer to the radio and back | 520 | 2.0 s |

**The status byte is there from the first byte.** While the opcode of a status request is clocked, the radio already answers with its status, top bit set (`0xA2` for `0x22`). A one-byte frame is therefore enough to ask whether anything moved, and the four-byte interrupt query is only needed when it did.

**Sleep.** With LoRa as its packet type the radio keeps packet type, frequency and sync word across a warm sleep (`SetSleep 0x04`) and drops the receiver gain register, as Semtech documents. A cold sleep (`SetSleep 0x00`) drops everything and is the only reset there is. Any frame wakes the radio, and the frame that does is lost on it.

Consequences for any driver: BUSY is replaced by a settle delay counted from the end of the reply, DIO1 by polling, reset by a cold sleep. `WriteBuffer` and `ReadBuffer` longer than one frame are cut up by offset. `src/PineDioBridge.*` in this repository does all of this, and `test/sim` is a bridge and a radio that behave as measured here.

## Start-up sync, as run

JF002's driver synchronises the ring buffer like this, and on this phone it converged after **26 bytes**:

1. `SetStandby` (`0x80 0x00`) and `SetBufferBaseAddress` (`0x8F 0x00 0x00`) through the bridge.
2. `WriteBuffer` of the pattern `10 20 30 40 50 AA 55 00 FF` at offset 0, then `ReadBuffer` of 9 bytes from offset 0.
3. Single-byte I2C reads until the pattern is seen in order, at most 256 reads.

After that the read index trails the write index by a known amount and stays there as long as every N-byte write is followed by exactly N reads.

## Receiving Meshtastic

JF002's demo as published listens on 868.000 MHz, SF12, 500 kHz, inverted IQ and hears nothing from a Meshtastic node. With the patch in `tools/jf002-demo/` it listens on EU_868 LongFast, which on this bench received the T-Deck:

| Setting | Value | Where it comes from |
|---|---|---|
| Frequency | 869.525 MHz | EU_868 spans 869.4 to 869.65 MHz, one 250 kHz slot, centre 869.4 + 0.125 |
| Modulation | SF11, BW 250 kHz, CR 4/5 | LongFast preset |
| Sync word | 0x2B, registers 0x0740/0x0741 = `24 B4` | Meshtastic's private sync word as RadioLib writes it |
| Preamble | 16 symbols | Meshtastic default |
| Header, CRC, IQ | explicit, CRC on, standard IQ | Meshtastic default |

The first packet, 25 bytes, a text sent from the T-Deck Plus on channel `i9603`:

```
ff ff ff ff  1c 8f ca 27  39 a6 d9 a2  63  fe  00  1c  99 c7 42 d9 a7 e5 cb 54 67
dest=broadcast  sender=!27ca8f1c  id=0xa2d9a639  flags=0x63 (hop limit 3, hop start 3)
channel hash=0xfe  next hop=0x00  relay=0x1c  then 9 bytes of AES-CTR payload
```

The demo polls the radio's IRQ status every 100 ms and reads the frame from the radio's buffer through the bridge. Nothing was transmitted: the demo sends only when a line is typed on its stdin.

## Checking a back cover

`bridge-selftest` needs no radio library and transmits nothing. It syncs the bridge, reads the radio's identity (`SX1261 V2D 2D02` on this unit, which is what an SX1262 says), carries buffers of every awkward length both ways, finds the largest frame the bridge takes, goes through sleep, wake and the reset stand-in, and times the polls. On the bench phone every check passes, with no I2C retry and no error in some 2700 bytes.

```
cmake -B build && cmake --build build -j4
build/test_bridge && build/test_radiolib      # the simulated back cover, no hardware
build/bridge-selftest /dev/i2c-5              # the real one
build/lora-listen /dev/i2c-5 --seconds 600    # RadioLib's driver over the bridge, receive only
```

## Receiving through RadioLib

`lora-listen` is RadioLib's unmodified SX1262 driver over `PineDioBridgeHal`. In forty minutes on the bench it received what the two nodes in the room sent of their own accord:

```
18:52:35   50 bytes  RSSI -50.0 dBm  SNR 5.75 dB  read in 347.9 ms
          to !ffffffff  from !27ca8f1c  id 0xff06463c  hop limit 3  hop start 3  channel 0xfe  relay 0x1c  payload 34
18:58:06  170 bytes  RSSI -56.0 dBm  SNR 6.00 dB  read in 822.0 ms
          to !ffffffff  from !8b04a69e  id 0x83d8e516  hop limit 3  hop start 3  channel 0xce  relay 0x9e  payload 154
```

The first is the T-Deck, the second the T-Beam. The second is longer than one bridge transaction, so its payload was fetched from the radio in two frames with the offset moved along, and the radio's checksum over the whole packet held. Reading a packet takes about 4.8 ms per byte of it, during which the radio is not listening.

## Transmitting

`lora-ping` sends Meshtastic-shaped frames of a chosen length and power. A T-Deck Pro two metres away, its firmware log read over USB, said of every frame whether it arrived intact or with a checksum error (`error=-7`). A frame that failed was still heard, and rejected for its checksum.

| Preamble | Power | Frames that arrived intact |
|---|---|---|
| 16 symbols, Meshtastic's usual | 10 dBm | up to about 76 bytes |
| 16 symbols | 22 dBm | up to 32 bytes |
| 16 symbols | 0 dBm | 116 and 122 bytes did not |
| 16 symbols, coding rate 4/8 | 22 dBm | 76 and 126 bytes did not |
| 96 symbols | 22 dBm | 126 bytes did not |
| 128 and 160 symbols | 22 dBm | 126 bytes did |
| 200 symbols | 10 and 22 dBm | every length tried, up to 216 bytes |

**The back cover needs a long preamble to send anything longer than a short frame.** The radio runs on a plain crystal, not a TCXO. What fits the numbers is that the crystal drifts while the transmitter starts, that the receiver locks at the end of the preamble, and that with a long preamble the drift is over by then. Nobody has measured the frequency itself. Less power helps a little and does not cure it; a stronger coding rate does not help; keeping the bridge silent while the frame is on the air (`--quiet-tx`) changes nothing, so the bridge's traffic is not the cause.

A receiver accepts a preamble of any length, so the other nodes need no change. The price is time on air: 160 symbols at SF11 and 250 kHz are 1.3 s for every packet, against 0.13 s for 16.

This matters more than it looks, because firmware 2.8 signs every broadcast and the signature is 66 bytes. The same short text that leaves a T-Deck as a frame of 30 bytes leaves the daemon as one of 106, over the limit of the usual preamble.

## The daemon

Meshtastic's daemon, built from the MeshSat firmware with `env:meshsat-pinephone-pro`, drove the radio through the bridge for the first time on 27 September 2026. With the usual preamble and continuous receive:

- it received and decoded a text from the T-Deck Pro;
- its traceroute to the T-Deck Pro came back after 1.3 s, with the signal reports of both ends in it: a full round trip;
- its own texts reached the T-Deck Pro damaged, which is what led to the table above. Traceroutes are short and got through.

A node that never had a region makes its keys when it gets one, and takes its node number from them. Over the bridge that took 108 s. `tools/node-setup/set-channels.py` sets the region first, waits for the node to come back under its new number, and only then sets the rest.

**The long preamble is for sending only.** Meshtastic hands its preamble length to RadioLib's duty-cycled receive, which sleeps the radio for as long as the preamble allows. With 16 symbols that is no sleep at all. With 160 it is 1.2 s of sleep in every 1.25 s, in which the radio misses its neighbours, whose preamble is still 16 symbols, and in which every poll through the bridge wakes it and reads rubbish. The first build with the long preamble did exactly that: in 90 s it reported 15 packets, all of them bad, among them its own last transmission read back from the radio's buffer, and it restarted the radio four times. Behind this bridge the radio has to receive continuously.

## When the radio stops answering

At the end of those 90 s the radio stopped obeying commands, and it has to be said plainly: **software could not bring it back.**

| | Observed |
|---|---|
| The bridge | Answers at 0x28 and clocks frames out in the usual 3.5 ms per byte. |
| The radio | Answers every frame with the same bytes, `f0 a0` and then more status-like bytes, whatever the command. It returns no register and no buffer content, so the start-up sync never finds its pattern. |
| In a long frame | The status bytes walk through standby, oscillator, synthesizer and receive within some 50 ms, as if the radio ran a receive cycle of its own on every chip-select edge. |
| Wake, `SetStandby`, cold `SetSleep` | No effect, at any spacing between 0 and 130 ms. |
| Rebooting the phone | No effect. The ATtiny's ring buffer still held the bytes from before the reboot: the cover's supply is not cut by a reboot, and the kernel offers no regulator for it. |

The ATtiny's firmware has one more command, `0x02`, which sets its SPI mode, bit order and clock. It is marked untested in the source and, as written, can only ever select LSB first. `0x02` is also the radio's `ClearIrqStatus` opcode, so a frame that lost its leading `0x01` on the way would reconfigure the bridge instead of reaching the radio. That was checked and is not what happened here: the radio does not answer bit-reversed commands either, and its status bytes read correctly in the normal bit order.

## Running the demo on the phone

Packages: `i2c-tools git cmake g++ make`. Then `sh tools/jf002-demo/build.sh` and:

```
sudo i2cdetect -y 5                       # expect 28
sleep 3600 | sudo ~/pinedio/pinedio-lora-driver/build/apps/pinephone-communicator/pinephone-communicator /dev/i2c-5
```

Hold stdin open with the `sleep`: at end of file the demo's chat loop sends an empty frame every 100 ms until killed. Stop it before the sleep ends, with `pkill -TERM` on the binary. The bridge answered normally after such a stop on this bench.

Bench phone settings that make this repeatable: `i2c-dev` in `/etc/modules-load.d/`, a udev rule giving `/dev/i2c-*` to group `i2c` with mode 0660, the user in that group, and the sleep and suspend targets masked so the phone stays reachable over SSH.

## Not yet verified

- BUSY wiring on this board revision (the 25 April 2021 schematic routes it to ATtiny PB2, the 2 April one does not). No firmware reads it either way.
- Whether the debug serial output is really what takes the time: it fits the numbers and the source, but nobody has put a probe on the ATtiny's pin.
- Range and packet loss. Every packet so far came from a node in the same room.
- What state the radio was in when it stopped answering, and whether taking the cover off and putting it back is enough to reset it. A reboot is not, and the phone has no switch for the cover's supply.
- The daemon with the long preamble and continuous receive, on the air.
- The crystal's drift itself. The preamble lengths are measured, the explanation is not.
