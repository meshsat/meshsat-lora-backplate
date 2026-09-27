# The back cover, as verified on the phone

Bench of 27 September 2026: a PinePhone Pro running Mobian (Debian 13, kernel 6.12-rockchip, Tow-Boot 2021.10 in SPI) with the Pine64 LoRa back cover, and a T-Deck Plus, a T-Beam and a T-Deck Pro as Meshtastic nodes on EU_868 LongFast. Everything below was observed on that bench unless marked *from source*. Every frame behind the transmit figures is in [`data/`](data/), one line each with its verdict; the sender-side record of the first transmit session was lost, and what is said of that session is marked as reconstructed.

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

**State on 27 September 2026: not solved, and nothing in this section is a qualified setting.** A frame of 32 bytes was accepted every time, 6 of 6. From a rested radio at 5 dBm and above, no frame of 126 bytes or more was accepted, 0 of 22, with preambles of 160 to 320 symbols; the daemon's own frames are 90 to 176 bytes. At 0 dBm and below, 126 bytes were accepted 5 of 5 and longer frames 2 of 12. An earlier version of this page said a long preamble fixes this. It does not: see "What went wrong with the first conclusion" below.

### How it is measured

`lora-ping` sends frames with a Meshtastic header and a random payload, on a channel hash no node uses, hop limit 0. A T-Deck Pro about two metres away writes its firmware log to a file, and `tools/bench/verdict.py` gives every frame exactly one verdict from that log:

| Verdict | What it takes |
|---|---|
| accepted | A complete `Lora RX` line with the frame's packet id and length, at the time the frame was on the air. The receiver's checksum passed. **The content is not compared.** |
| refused | A complete `Ignore received packet due to error=-7` line with the frame's id. The id in such a line comes from a damaged frame and is tentative. |
| no verdict | The capture was not running, the receiver was sending at that moment, or the log contradicts itself. Such a frame counts neither for the radio nor against it. |

One receiver, one evening, one unit, the phone not fixed in place. The counts are small: 4 accepted out of 4 still allows a true rate as low as 47 %, and none out of 12 allows one as high as 22 % (one-sided, 95 %). None of this is a reliability figure.

### From a rested radio

Single frames and pairs, at least 14 s after the previous transmission. Accepted / judged.

Preamble of 160 to 320 symbols:

| Frame length | -9 dBm | 0 dBm | 5 dBm | 10 dBm | 14 dBm | 22 dBm |
|---|---:|---:|---:|---:|---:|---:|
| 32 bytes | | | | | 2 / 2 | |
| 76 bytes | | | | | 0 / 2 | |
| 126 bytes | 1 / 1 | 4 / 4 | 0 / 1 | 0 / 1 | 0 / 4 | 0 / 2 |
| 192 bytes | 2 / 4 | 0 / 4 | 0 / 1 | 0 / 1 | 0 / 12 | |
| 237 bytes | 0 / 2 | 0 / 2 | | | | |

Preamble of 16 symbols, Meshtastic's usual:

| Frame length | 0 dBm | 10 dBm | 14 dBm | 22 dBm |
|---|---:|---:|---:|---:|
| 32 bytes | | | 2 / 2 | 2 / 2 |
| 76 bytes | | 2 / 2 | 0 / 2 | |
| 126 bytes | 0 / 1 | | | |
| 192 bytes | 0 / 1 | | | |

Thirteen more frames have no verdict: twelve went out while the capture was not running, one while the receiver itself was sending. At 14 dBm and 192 bytes the preamble was 160, 200, 256 and 320 symbols (7, 1, 2 and 2 frames judged).

### Frames in a row

| Run, frames about 5 s apart | Verdicts in order |
|---|---|
| Ten frames of 126 bytes, 22 dBm, preamble 200 | refused, refused, refused, refused, then six accepted |
| Straight after it, two frames of 126 bytes, 22 dBm, preamble 16 | refused, refused |
| Eight frames of 126 bytes, -9 dBm, preamble 200 | all eight accepted |
| Straight after it, one frame of 192 bytes, 14 dBm, preamble 160 | refused |
| Four frames of 192 bytes, 0 dBm, preamble 200, 15 s of receive between them | refused, refused, accepted, accepted |

The last run was interrupted after its fourth frame and its own output was lost. What is known of it is what the receiver logged.

### What the receiver says about the frequency

The T-Deck Pro logs a frequency offset for every packet it accepts. It is the receiver's own estimate, not a calibrated measurement, it exists only for accepted packets, and it tells how far a transmitter moved between two frames, not within one.

| Transmitter | Offset seen by the same receiver |
|---|---|
| T-Deck Plus | 239 to 247 Hz, five packets over three hours |
| T-Beam | 658 Hz, one packet |
| Back cover at 0 dBm and below, after a quarter of an hour without sending | 4254 to 4300 Hz |
| Back cover, ten frames at 22 dBm | rising to 5461 Hz; from the fifth frame on, the first accepted, by 48, 37, 33, 23 and 23 Hz from frame to frame |
| Back cover, eight frames at -9 dBm | 4413 Hz rising to 4603 Hz, by 66, 39, 25, 21, 14, 14 and 12 Hz from frame to frame |
| Back cover, earlier session, frames in a steady row at full power | 5709 to 5748 Hz, moving 0 to 8 Hz from frame to frame, once 21 Hz |

So the other nodes stand still and the back cover's transmitter moves by more than 1 ppm with what it has just been doing. Semtech's datasheet for the SX1261/2 (revision 1.2, section 4.1.2) gives the drift a LoRa receiver tolerates over one packet, without low data rate optimisation, as bandwidth / (3 x 2^SF): 40.7 Hz at SF11 and 250 kHz, which is 0.047 ppm at this frequency. Semtech's application note AN1200.37 describes a crystal next to a power amplifier drifting from its heat, and recommends a TCXO or thermal relief for it. This cover has a crystal.

**What is established and what is not.** Established: the frequency moves with the transmit history, and long frames are refused while it moves quickly. Not established: that this movement is what damages the frames, how it divides between the amplifier's heat, the supply and the oscillator starting, and what happens inside one frame. No frequency, temperature or supply voltage was measured directly.

### The power that is asked for and the power that arrives

| Asked for | Frames heard | Median RSSI at the receiver | Amplifier setting RadioLib chooses (duty cycle, hpMax, value given to SetTxParams) |
|---:|---:|---:|---|
| -9 dBm | 14 | -84 dBm | 2, 2, -5 |
| 0 dBm | 16 | -75 dBm | 2, 1, 11 |
| 5 dBm | 2 | -70 dBm | 2, 2, 11 |
| 10 dBm | 4 | -61 dBm | 1, 2, 22 |
| 14 dBm | 25 | -59 dBm | 1, 4, 20 |
| 22 dBm | 16 | -54 dBm | 4, 7, 22 |

RadioLib does not scale one amplifier setting: by default it takes a different amplifier configuration for every dBm from a table meant to save current, in the tool and in the daemon alike. The steps at the receiver are uneven (9 dB more from 5 to 10 dBm, 2 dB more from 10 to 14). Whether that is this table on this board, the supply, or the receiver's reading is not known. The reported signal-to-noise ratio does not tell an accepted frame from a refused one: the median is 4.75 dB for both.

### What went wrong with the first conclusion

Between 19:58 and 20:12 the same tool sent about seventy frames in dense succession. After nine minutes of that, 19 out of 20 frames of 76 to 216 bytes were accepted that the reconstruction assigns to preambles of 128, 160 and 200 symbols, and the frames of 126 bytes and more that it assigns to 16 and 96 symbols were not. The conclusion written here that evening was that a long preamble lets the crystal settle. The experiment was confounded: the radio had been transmitting a third of the time for minutes, and the receiver's offsets show its frequency had stopped moving by then. After a power cycle and from a rested radio the same settings failed. The sender-side record of that session was lost with a temporary directory; what is said about it here is reconstructed from the receiver's log.

What stands of it: with the radio in a steady state, a preamble of 16 symbols was refused where one of 128 or more was accepted.

## The daemon

Meshtastic's daemon, built from the MeshSat firmware with `env:meshsat-pinephone-pro`, drove the radio through the bridge for the first time on 27 September 2026.

What it sent, from its own log, 16 bytes of header included:

| Packet | On the air | Verdict at the T-Deck Pro |
|---|---:|---|
| Traceroute request and its routing packet | 22 and 29 bytes | No firmware log was kept at that moment. A Meshtastic client on the T-Deck Pro showed both packets decoded, and the answer came back to the phone after 1.3 s |
| Text of 2 characters, broadcast, sent three times | 90 bytes | refused, all three |
| Text of 66 characters, broadcast, sent three times | 154 bytes | refused, all three |
| Two more broadcasts, sent three times each | 132 and 145 bytes | no firmware log was kept; the client showed neither |
| Node announcement, preamble 160, 14 dBm | 176 bytes | refused |

Those were sent with 16 symbols of preamble at 22 dBm, the announcement excepted. A broadcast text of N characters is N + 88 bytes on the air, because firmware 2.8 signs every broadcast it originates and the signature takes 66 bytes. A text to one node is not signed.

What is proven with the daemon: it received and decoded a text from the T-Deck Pro, and its traceroute went there and back. **A text sent by the daemon has not been read on another node.**

A node that never had a region makes its keys when it gets one, and takes its node number from them. Over the bridge that took 108 s; how that time divides between the processor, the entropy and the bridge was not measured. `tools/node-setup/set-channels.py` sets the region first, waits for the node to come back under its new number, and only then sets the rest.

**The preamble the daemon sends with must not be the one it receives with.** Meshtastic hands its preamble length to RadioLib's duty-cycled receive, which sleeps the radio for as long as that preamble allows. With 16 symbols that is no sleep at all. With 160 it is 1.18 s of sleep in every 1.25 s, in which the radio misses its neighbours, whose preamble is still 16 symbols, and in which every poll through the bridge wakes it and reads rubbish. The first build with the long preamble did exactly that: in 90 s it reported 15 packets, all of them bad, among them its own last transmission read back from the radio's buffer, and it restarted the radio four times. The build after it receives continuously. It ran for 100 s without a bad packet or a restart; no neighbour sent anything in those 100 s, so that it receives is not proven again yet.

**Before it sends, the daemon stops the crystal.** From the source: it leaves receive for standby, scans the channel, falls back to standby and loads the frame. RadioLib's standby is the one on the RC oscillator unless told otherwise, and loading a frame of 176 bytes through this bridge takes about a second. The crystal therefore starts again at the beginning of every transmission. On a board with a real SPI bus the same steps take a millisecond. Whether this matters for the frames is not tested.

## When the radio stops answering

At the end of those 90 s the radio stopped obeying commands, and it has to be said plainly: **software could not bring it back, and why it happened is not known.**

| | Observed |
|---|---|
| The bridge | Answers at 0x28 and clocks frames out in the usual 3.5 ms per byte. |
| The radio | Answers every frame with the same bytes, `f0 a0` and then more status-like bytes, whatever the command. It returns no register and no buffer content, so the start-up sync never finds its pattern. Which state of the radio this is cannot be told from those bytes. |
| Wake, `SetStandby`, cold `SetSleep` | No command ever returned a register or the buffer. |
| Rebooting the phone | No effect. The ATtiny's ring buffer still held the bytes from before the reboot: the cover's supply is not cut by a reboot, and the kernel offers no regulator for it. |
| Taking the cover off and pressing it back on | The radio answered again. The cover needed a second, firm press before the bridge was seen at all. |

The ATtiny's firmware has one more command, `0x02`, which sets its SPI mode, bit order and clock. It is marked untested in the source and, as written, can only ever select LSB first. `0x02` is also the radio's `ClearIrqStatus` opcode, so a frame that lost its leading `0x01` on the way would reconfigure the bridge instead of reaching the radio. One test was made for it: commands sent bit-reversed got no answer either. That speaks against a changed bit order alone. A changed clock mode was not tested, so this cause is not excluded.

**What follows for any use without a person nearby:** a radio that only a hand can reset. A reset line driven by a reflashed ATtiny, or a supply the phone can switch, would be the remedy; neither exists, and the board's reset wiring has not been verified on this unit.

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
- What damages the long frames. The receiver's offsets fit a frequency that moves with heat; nothing was measured inside a frame, and the supply was not measured at all.
- Whether keeping the crystal running before a transmission changes anything.
- The content of a frame the back cover sent. Every verdict so far is a checksum.
- The daemon that receives continuously, receiving.
- What state the radio was in when it stopped answering.
- The settle delays that stand in for BUSY, in every state of the radio. Semtech's datasheet has BUSY rise while the radio handles an interrupt of its own, which no delay counted from a command covers.
