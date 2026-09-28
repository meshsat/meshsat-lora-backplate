# Frames sent on the bench

One line for every frame the phone sent of which a record survives, judged by `tools/bench/verdict.py` from the firmware log of the receiving node, a T-Deck Pro about two metres away.

| File | What is in it |
|---|---|
| `2026-09-27-frames.csv` | 110 frames of 27 September 2026: 94 sent by `lora-ping`, 16 sendings by the daemon. 31 accepted, 54 refused with a checksum error, 25 without a verdict |
| `2026-09-27-first-session-receiver-side.csv` | 70 frames of the first transmit session, 19:58 to 20:12, as the receiver logged them. The sender-side record of that session was lost, so the settings of these frames are not in the file; `docs/BACKPLATE.md` says which settings the order of the experiments assigns to them |
| `2026-09-28-b-against-c.csv` | 16 frames of 176 bytes at 0 dBm, 00:41 to 01:37 on 28 September 2026: eight blocks of one frame loaded with the crystal running (B) and one loaded the daemon's way, on the RC oscillator (C), judged at two receivers. `verdict_A`/`verdict_B` are the two receivers, `arm` the load state, `mode_loaded` the chip mode read back after the load (3 = standby with the crystal, 2 = standby RC); `void` marks a frame whose read-back was not its arm's |
| `2026-09-28-daemon-frames.csv` | The 11 frames the daemon sent between 02:51 and 03:44 on 28 September 2026 at 0 dBm: two node announcements and three texts of three copies each, one row per copy (`copy`), with the verdict at both receivers and, in `decoded_A`, the text receiver A read out of the first copy |
| `2026-09-28-deaf-window.csv` | Every frame the phone's radio reported or missed after a transmission of its own that night: the seconds since its own transmission ended (`since_own_tx_s`), the frame's length where known, and whether it was accepted, refused with a checksum error, or not reported although receiver A heard it |

## Reading a line

- **verdict**: `accepted` needs a complete `Lora RX` line of the receiver with the frame's packet id and length, at the time the frame was on the air. `crc_failed` is a complete line refusing the packet with error -7. `capture_invalid` and `ambiguous` are no verdict: the capture was not running at that time, or the receiver was sending itself.
- **content**: `not_checked` everywhere. A verdict is a checksum; what the frame carried was not compared.
- **identity_checked**: `id, sender, length` where the sender side kept the sender; `id, length; the sender is the receiver's` where it did not (the tool's output had been filtered before it was saved) and the sender was taken from the receiver's own line, which proves nothing about it. 79 of the 110 frames of the first file are of the second kind: the 75 tool frames of 22:25 to 23:00 and the 4 of the interrupted run. `id, sender, length, relay` (the daemon's frames of 28 September): the daemon sends a packet again under the same id when it hears no rebroadcast, and neighbours rebroadcast it under the same id, so a copy counts only with the sender's own relay byte, and each copy is judged in its own time.
- **state_before**: what the radio was doing before the frame was loaded. It decides whether the crystal was running.
- **offset_hz**: the receiver's own estimate of the frequency offset, for accepted frames only. Not a calibrated measurement.
- **pa_***: the amplifier configuration RadioLib 7.7.1 takes from its table for the power asked for.
- **receiver_airtime_ms**: the time on air the receiver sums for the length it read, with its own preamble of 16 symbols. For a refused frame it is the only trace of its length.
- The four frames named `x1` belong to a run that was interrupted. The tool's output was lost; ids, sender and times are the receiver's.

The raw logs are not published: they hold the traffic of other nodes.
