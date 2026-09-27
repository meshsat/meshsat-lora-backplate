# The back cover, as verified on the phone

Bench of 27 September 2026: a PinePhone Pro running Mobian (Debian 13, kernel 6.12-rockchip, Tow-Boot 2021.10 in SPI) with the Pine64 LoRa back cover, a T-Deck Plus and a T-Beam as Meshtastic nodes on EU_868 LongFast. Everything below was observed on that bench unless marked *from source*.

## Where the cover is

| | Observed |
|---|---|
| Bus | `/dev/i2c-5` on Mobian's 6.12-rockchip kernel. The pogo bus is the RK3399 controller the keyboard driver also probes (`pinephone-keyboard 5-0015`). Other kernels number it differently; find it with `i2cdetect -l`. |
| Address | `0x28`, visible in `sudo i2cdetect -y 5` as soon as the cover is seated, with the phone running. Hot-plugging the cover is fine. |
| Power | From the phone's battery through the pogo pins. The bridge keeps its state across a process kill; a stalled I2C transaction is reported to hang it until the cover is re-seated or the phone is powered off (puurpl, Feb 2026, not reproduced here). |
| Fit | The cover made for the PinePhone fits the Pro and makes contact. Press it home; the contact is known to be intermittent. |

The keyboard case uses the same pogo pins, so the two are never on together. Its driver probes the bus at boot and fails harmlessly when the LoRa cover is on; the bench phone blacklists `pinephone_keyboard` and `ip5xxx_power` (`/etc/modprobe.d/meshsat-no-keyboard.conf`) to keep the bus quiet.

## What the bridge is

An ATtiny84 running [tiny-i2c-spi](https://github.com/zschroeder6212/tiny-i2c-spi) (GPL-3.0, last commit June 2021). *From source*, confirmed by the sync below:

- **Command `0x01` + N bytes:** chip select low, the N bytes are clocked out over a software SPI (mode 0, MSB first), chip select high. Every byte the radio clocked back is appended to a **128-byte ring buffer** in the ATtiny.
- **Command `0x02` + 1 byte:** SPI mode, bit order and clock divider. Marked untested in the firmware. Not used.
- **An I2C read returns one byte** from the ring buffer and advances its read index. Reading several bytes in one transfer returns junk after the first. The write index and the read index are never resynchronised by the firmware, so a driver must read exactly as many bytes as it wrote, forever, and resynchronise once at start.
- **Nothing else.** The bridge has no command for reset, BUSY, DIO1 or any GPIO. The SX1262's NRESET is wired to the ATtiny's own reset pin, so the radio cannot be reset without reprogramming the bridge. DIO1 reaches an ATtiny pin the firmware ignores. The pogo INT pin is not connected to the radio (R42 unfitted on v1.0 boards) and on the Pro it cannot be claimed as a GPIO without killing the I2C bus (puurpl, Feb 2026).

Consequences for any driver: BUSY is replaced by a delay or by polling the radio's status over SPI, DIO1 by polling `GetIrqStatus`, reset by a cold-start `SetSleep`. A full 255-byte Meshtastic frame needs a 257-byte transfer, which does not fit one I2C write to the ATtiny: `WriteBuffer` and `ReadBuffer` must be chunked by offset.

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

## Running the demo on the phone

Packages: `i2c-tools git cmake g++ make`. Then `sh tools/jf002-demo/build.sh` and:

```
sudo i2cdetect -y 5                       # expect 28
sleep 3600 | sudo ~/pinedio/pinedio-lora-driver/build/apps/pinephone-communicator/pinephone-communicator /dev/i2c-5
```

Hold stdin open with the `sleep`: at end of file the demo's chat loop sends an empty frame every 100 ms until killed. Stop it before the sleep ends, with `pkill -TERM` on the binary. The bridge answered normally after such a stop on this bench.

Bench phone settings that make this repeatable: `i2c-dev` in `/etc/modules-load.d/`, a udev rule giving `/dev/i2c-*` to group `i2c` with mode 0660, the user in that group, and the sleep and suspend targets masked so the phone stays reachable over SSH.

## Not yet verified

- BUSY wiring on this board revision (the 25 April 2021 schematic routes it to ATtiny PB2, the 2 April one does not).
- The ATtiny's clock (fuses imply 1 MHz, the firmware assumes 8 MHz) and the resulting I2C throughput.
- Range, packet loss and any packet longer than one I2C write.
- Whether the phone can cut the cover's supply, which would be the only way to reset the radio.
