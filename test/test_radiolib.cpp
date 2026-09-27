// SPDX-License-Identifier: GPL-3.0-or-later
// RadioLib's own SX1262 driver, unmodified, over the bridge and the simulated back cover.
// What passes here is what the Meshtastic daemon will ask of the transport.
#include "PineDioBridgeHal.h"
#include "check.h"
#include "sequence.h"
#include "sim/SimBackplate.h"

#include <atomic>

using namespace meshsat::pinedio;
using sim::Backplate;
using sim::SimClock;

namespace
{
std::atomic<int> irqCount{0};
void onIrq()
{
    irqCount++;
}

struct Bench {
    SimClock clock;
    Backplate plate;
    Bridge bridge;
    PineDioBridgeHal hal;
    Module module;
    SX1262 radio;

    explicit Bench(unsigned seed, size_t frame = 120, uint32_t usPerSpiByte = 3450)
        : plate(clock, seed, 25, usPerSpiByte), bridge(plate, clock, config(frame)), hal(bridge, clock, false),
          module(&hal, Bridge::PinCs, Bridge::PinIrq, Bridge::PinReset, Bridge::PinBusy), radio(&module)
    {
    }
    static Config config(size_t frame)
    {
        Config c;
        c.maxSpiFrame = frame;
        return c;
    }
    /// True when, after one more reply has been fetched, nothing is left unread in the ring.
    bool level()
    {
        uint8_t out[2] = {0xC0, 0}, in[2] = {0};
        return bridge.transfer(out, in, 2) && plate.unread() == 0;
    }
    /// Meshtastic's EU_868 LongFast, the way its SX126x interface begins the radio.
    int16_t beginLongFast() { return radio.begin(869.525, 250.0, 11, 5, 0x2B, 10, 16, 0.0, false); }
};

std::vector<uint8_t> meshtasticFrame(size_t payload)
{
    // destination, sender, packet id, flags, channel hash, next hop, relay, then ciphertext
    std::vector<uint8_t> f{0xff, 0xff, 0xff, 0xff, 0x1c, 0x8f, 0xca, 0x27, 0x39, 0xa6, 0xd9, 0xa2, 0x63, 0xfe, 0x00, 0x1c};
    for (size_t i = 0; i < payload; i++)
        f.push_back((uint8_t)(0x99 + i * 3));
    return f;
}
} // namespace

TEST(begin_finds_the_chip_and_programs_longfast)
{
    Bench b(101);
    CHECK_EQ(b.beginLongFast(), RADIOLIB_ERR_NONE);
    CHECK(b.hal.ready());
    CHECK_EQ(b.plate.radio.reg(0x0740), 0x24);
    CHECK_EQ(b.plate.radio.reg(0x0741), 0xB4);
    CHECK(b.plate.radio.frequencyHz() > 869524000 && b.plate.radio.frequencyHz() < 869526000);
    CHECK_EQ(b.plate.radio.busyViolations(), 0);
    CHECK_EQ(b.plate.oversize(), 0);
    CHECK(b.level());
    CHECK_EQ(b.radio.setDio2AsRfSwitch(true), RADIOLIB_ERR_NONE);
    CHECK_EQ(b.radio.setCRC(2), RADIOLIB_ERR_NONE);
}

TEST(begin_reports_a_missing_back_cover)
{
    Bench b(103);
    b.plate.failWrites(100000);
    CHECK(b.beginLongFast() != RADIOLIB_ERR_NONE);
    CHECK(!b.hal.ready());
}

TEST(a_received_frame_is_read_back_whole)
{
    Bench b(107);
    CHECK_EQ(b.beginLongFast(), RADIOLIB_ERR_NONE);
    irqCount = 0;
    b.radio.setDio1Action(onIrq);
    CHECK_EQ(b.radio.startReceive(), RADIOLIB_ERR_NONE);
    CHECK_EQ(b.plate.radio.mode(), sim::ModeRx);
    CHECK(!b.bridge.pollOnce());

    const std::vector<uint8_t> frame = meshtasticFrame(9);
    CHECK(b.plate.radio.injectRx(frame, -52, 6));
    CHECK(b.bridge.pollOnce());
    CHECK_EQ(irqCount.load(), 1);

    const size_t len = b.radio.getPacketLength();
    CHECK_EQ(len, frame.size());
    std::vector<uint8_t> got(len);
    CHECK_EQ(b.radio.readData(got.data(), len), RADIOLIB_ERR_NONE);
    CHECK(got == frame);
    CHECK_EQ((int)b.radio.getRSSI(), -52);
    CHECK_EQ((int)b.radio.getSNR(), 6);
    CHECK_EQ(b.plate.radio.busyViolations(), 0);
    CHECK(b.level());
}

TEST(the_longest_frame_is_received_in_pieces)
{
    for (size_t frame : {16, 64, 96, 127}) {
        Bench b(109, frame);
        CHECK_EQ(b.beginLongFast(), RADIOLIB_ERR_NONE);
        b.radio.setDio1Action(onIrq);
        CHECK_EQ(b.radio.startReceive(), RADIOLIB_ERR_NONE);
        const std::vector<uint8_t> big = meshtasticFrame(255 - 16);
        CHECK(b.plate.radio.injectRx(big));
        CHECK(b.bridge.pollOnce());
        const size_t len = b.radio.getPacketLength();
        CHECK_EQ(len, 255);
        std::vector<uint8_t> got(len);
        CHECK_EQ(b.radio.readData(got.data(), len), RADIOLIB_ERR_NONE);
        CHECK(got == big);
        CHECK(b.plate.radio.longestFrame() <= frame);
        CHECK_EQ(b.plate.oversize(), 0);
    }
}

TEST(receive_after_receive_without_losing_an_edge)
{
    Bench b(113);
    CHECK_EQ(b.beginLongFast(), RADIOLIB_ERR_NONE);
    irqCount = 0;
    b.radio.setDio1Action(onIrq);
    for (int i = 1; i <= 50; i++) {
        CHECK_EQ(b.radio.startReceive(), RADIOLIB_ERR_NONE);
        const std::vector<uint8_t> frame = meshtasticFrame((size_t)(i * 4));
        CHECK(b.plate.radio.injectRx(frame));
        CHECK(b.bridge.pollOnce());
        std::vector<uint8_t> got(b.radio.getPacketLength());
        CHECK_EQ(b.radio.readData(got.data(), got.size()), RADIOLIB_ERR_NONE);
        CHECK(got == frame);
    }
    CHECK_EQ(irqCount.load(), 50);
    CHECK_EQ(b.plate.radio.busyViolations(), 0);
}

TEST(channel_scan_waits_on_the_line_that_is_not_there)
{
    Bench b(127);
    CHECK_EQ(b.beginLongFast(), RADIOLIB_ERR_NONE);
    b.plate.radio.setChannelBusy(false);
    CHECK_EQ(b.radio.scanChannel(), RADIOLIB_CHANNEL_FREE);
    b.plate.radio.setChannelBusy(true);
    CHECK_EQ(b.radio.scanChannel(), RADIOLIB_LORA_DETECTED);
    CHECK_EQ(b.plate.radio.busyViolations(), 0);
}

TEST(a_frame_is_handed_to_the_radio_for_transmission)
{
    // Simulated air only. On the phone nothing transmits until the antenna is confirmed.
    Bench b(131);
    CHECK_EQ(b.beginLongFast(), RADIOLIB_ERR_NONE);
    irqCount = 0;
    b.radio.setDio1Action(onIrq);
    for (size_t payload : {1, 9, 120, 239}) {
        std::vector<uint8_t> frame = meshtasticFrame(payload);
        CHECK_EQ(b.radio.startTransmit(frame.data(), frame.size()), RADIOLIB_ERR_NONE);
        CHECK_EQ(b.plate.radio.mode(), sim::ModeTx);
        const int before = irqCount.load();
        for (int i = 0; i < 2000 && irqCount.load() == before; i++) {
            b.clock.advance(1000);
            b.bridge.pollOnce();
        }
        CHECK_EQ(irqCount.load(), before + 1);
        CHECK_EQ(b.radio.finishTransmit(), RADIOLIB_ERR_NONE);
        CHECK(!b.plate.radio.transmitted().empty());
        CHECK(b.plate.radio.transmitted().back() == frame);
    }
    CHECK_EQ(b.plate.radio.busyViolations(), 0);
}

TEST(the_whole_round_on_a_bridge_with_a_quiet_firmware)
{
    // An ATtiny reflashed without its debug output is some eighty times faster on the SPI
    // side. Nothing may depend on the bridge being slow.
    Bench b(139, 120, 40);
    CHECK_EQ(b.beginLongFast(), RADIOLIB_ERR_NONE);
    irqCount = 0;
    b.radio.setDio1Action(onIrq);
    for (int i = 1; i <= 20; i++) {
        CHECK_EQ(b.radio.startReceive(), RADIOLIB_ERR_NONE);
        const std::vector<uint8_t> frame = meshtasticFrame((size_t)(i * 11));
        CHECK(b.plate.radio.injectRx(frame));
        CHECK(b.bridge.pollOnce());
        std::vector<uint8_t> got(b.radio.getPacketLength());
        CHECK_EQ(b.radio.readData(got.data(), got.size()), RADIOLIB_ERR_NONE);
        CHECK(got == frame);
        CHECK_EQ(b.radio.scanChannel(), RADIOLIB_CHANNEL_FREE);
    }
    CHECK_EQ(b.radio.sleep(), RADIOLIB_ERR_NONE);
    CHECK_EQ(b.radio.standby(), RADIOLIB_ERR_NONE);
    CHECK_EQ(b.plate.radio.busyViolations(), 0);
    CHECK(b.level());
}

TEST(sleep_and_standby_as_the_driver_does_them)
{
    Bench b(137);
    CHECK_EQ(b.beginLongFast(), RADIOLIB_ERR_NONE);
    CHECK_EQ(b.radio.sleep(), RADIOLIB_ERR_NONE);
    CHECK_EQ(b.plate.radio.mode(), sim::ModeSleep);
    CHECK_EQ(b.radio.standby(), RADIOLIB_ERR_NONE);
    CHECK(b.plate.radio.mode() == sim::ModeStandbyRc || b.plate.radio.mode() == sim::ModeStandbyXosc);
    CHECK_EQ(b.radio.startReceive(), RADIOLIB_ERR_NONE);
    CHECK_EQ(b.plate.radio.busyViolations(), 0);
    CHECK(b.level());
}

// What the radio does while a frame is loaded, which through this bridge takes about a second.
// The simulated radio keeps count of its crystal: when it started, and how often it was stopped.

namespace
{
const uint64_t quarterMinute = 15000000;

int16_t listenThenSend(Bench &b, bench::From from, bool scan, const std::vector<uint8_t> &frame, bench::Proof &proof)
{
    if (b.radio.startReceive() != RADIOLIB_ERR_NONE)
        return RADIOLIB_ERR_UNKNOWN;
    b.clock.advance(quarterMinute);
    return bench::send(b.radio, b.bridge, b.clock, from, scan, frame.data(), frame.size(), proof);
}
} // namespace

TEST(a_frame_loaded_in_receive_never_stops_the_crystal)
{
    Bench b(149);
    CHECK_EQ(b.beginLongFast(), RADIOLIB_ERR_NONE);
    const std::vector<uint8_t> frame = meshtasticFrame(160);
    bench::Proof proof;
    CHECK_EQ(b.radio.startReceive(), RADIOLIB_ERR_NONE);
    const unsigned stops = b.plate.radio.crystalStops();
    b.clock.advance(quarterMinute);
    CHECK_EQ(bench::send(b.radio, b.bridge, b.clock, bench::From::Receive, false, frame.data(), frame.size(), proof),
             RADIOLIB_ERR_NONE);
    CHECK_EQ(proof.before, sim::ModeRx);
    CHECK_EQ(proof.loaded, sim::ModeRx);
    CHECK_EQ(b.plate.radio.transmissions().back().from, sim::ModeRx);
    CHECK(b.plate.radio.transmissions().back().crystalUs > quarterMinute);
    CHECK_EQ(b.plate.radio.crystalStops(), stops);
    CHECK(b.plate.radio.transmitted().back() == frame);
    CHECK_EQ(b.plate.radio.busyViolations(), 0);
}

TEST(the_daemons_way_starts_the_crystal_with_the_frame)
{
    // Receive, standby, channel scan, the load in standby RC: what Meshtastic's daemon does.
    Bench b(151);
    CHECK_EQ(b.beginLongFast(), RADIOLIB_ERR_NONE);
    const std::vector<uint8_t> frame = meshtasticFrame(160);
    bench::Proof proof;
    CHECK_EQ(listenThenSend(b, bench::From::StandbyRc, true, frame, proof), RADIOLIB_ERR_NONE);
    CHECK_EQ(proof.scan, RADIOLIB_CHANNEL_FREE);
    CHECK_EQ(proof.before, sim::ModeStandbyRc);
    CHECK_EQ(proof.loaded, sim::ModeStandbyRc);
    CHECK_EQ(b.plate.radio.transmissions().back().from, sim::ModeStandbyRc);
    CHECK_EQ(b.plate.radio.transmissions().back().crystalUs, 0);
    CHECK(proof.loadMs > 500); // 176 bytes at 3.5 ms each, with the crystal stopped
    CHECK(b.plate.radio.transmitted().back() == frame);
    CHECK_EQ(b.plate.radio.busyViolations(), 0);
}

TEST(standby_with_the_crystal_keeps_it_running_through_the_load)
{
    Bench b(157);
    b.radio.standbyXOSC = true;
    CHECK_EQ(b.beginLongFast(), RADIOLIB_ERR_NONE);
    const std::vector<uint8_t> frame = meshtasticFrame(160);
    bench::Proof proof;
    CHECK_EQ(b.radio.startReceive(), RADIOLIB_ERR_NONE);
    const unsigned stops = b.plate.radio.crystalStops();
    b.clock.advance(quarterMinute);
    CHECK_EQ(bench::send(b.radio, b.bridge, b.clock, bench::From::StandbyXosc, false, frame.data(), frame.size(), proof),
             RADIOLIB_ERR_NONE);
    CHECK_EQ(proof.before, sim::ModeStandbyXosc);
    CHECK_EQ(proof.loaded, sim::ModeStandbyXosc);
    CHECK_EQ(b.plate.radio.transmissions().back().from, sim::ModeStandbyXosc);
    CHECK(b.plate.radio.transmissions().back().crystalUs > quarterMinute);
    CHECK_EQ(b.plate.radio.crystalStops(), stops);

    // The frame over, the driver's own standby and the next receive keep it running too.
    irqCount = 0;
    b.radio.setDio1Action(onIrq);
    for (int i = 0; i < 4000 && irqCount.load() == 0; i++) {
        b.clock.advance(1000);
        b.bridge.pollOnce();
    }
    CHECK_EQ(irqCount.load(), 1);
    CHECK_EQ(b.plate.radio.mode(), sim::ModeStandbyXosc);
    CHECK_EQ(b.radio.finishTransmit(), RADIOLIB_ERR_NONE);
    CHECK_EQ(b.radio.startReceive(), RADIOLIB_ERR_NONE);
    CHECK_EQ(b.plate.radio.crystalStops(), stops);
    CHECK_EQ(b.plate.radio.busyViolations(), 0);
}

TEST(a_channel_scan_stops_the_crystal_whatever_the_standby)
{
    // After a scan the radio is in standby RC. Asking for the crystal again at once leaves it
    // the time of the load to settle, not the quarter of a minute it had been running.
    Bench b(163);
    b.radio.standbyXOSC = true;
    CHECK_EQ(b.beginLongFast(), RADIOLIB_ERR_NONE);
    const std::vector<uint8_t> frame = meshtasticFrame(160);
    bench::Proof proof;
    CHECK_EQ(b.radio.startReceive(), RADIOLIB_ERR_NONE);
    const unsigned stops = b.plate.radio.crystalStops();
    b.clock.advance(quarterMinute);
    CHECK_EQ(bench::send(b.radio, b.bridge, b.clock, bench::From::StandbyXosc, true, frame.data(), frame.size(), proof),
             RADIOLIB_ERR_NONE);
    CHECK_EQ(proof.loaded, sim::ModeStandbyXosc);
    CHECK_EQ(b.plate.radio.crystalStops(), stops + 1);
    CHECK(b.plate.radio.transmissions().back().crystalUs > 0);
    CHECK(b.plate.radio.transmissions().back().crystalUs < 3000000);
}

TEST(a_packet_that_came_in_before_the_frame_is_not_the_end_of_the_frame)
{
    // What the tool used to get wrong: its interrupt latch could be set by a packet received while
    // the frame was being loaded, and the wait for the end of the frame returned at once.
    Bench b(173);
    CHECK_EQ(b.beginLongFast(), RADIOLIB_ERR_NONE);
    CHECK_EQ(b.radio.startReceive(), RADIOLIB_ERR_NONE);
    CHECK(b.plate.radio.injectRx(meshtasticFrame(20)));
    std::atomic<bool> edge{true}; // the latch, set by that packet and never cleared
    const std::vector<uint8_t> frame = meshtasticFrame(160);
    bench::Proof proof;
    CHECK_EQ(bench::send(b.radio, b.bridge, b.clock, bench::From::Receive, false, frame.data(), frame.size(), proof),
             RADIOLIB_ERR_NONE);
    CHECK_EQ(b.plate.radio.mode(), sim::ModeTx);
    const uint64_t launched = b.clock.nowUs();
    const uint16_t flags = bench::awaitTransmitDone(b.radio, b.clock, launched + 10000000, &edge, nullptr);
    CHECK(flags & RADIOLIB_SX126X_IRQ_TX_DONE);
    // The simulated frame takes 20 ms and 1.5 ms a byte: the wait cannot have ended before that.
    CHECK(b.clock.nowUs() - launched >= 20000 + 1500ULL * frame.size());
    CHECK_EQ(b.plate.radio.mode(), sim::ModeStandbyRc);
    CHECK(b.plate.radio.transmitted().back() == frame);
    CHECK_EQ(b.radio.finishTransmit(), RADIOLIB_ERR_NONE);
    CHECK(b.level());
}

TEST(without_a_transmit_done_the_wait_ends_empty_handed)
{
    // Nothing on the air: the flags come back without TX_DONE when the time is up, and the
    // caller must not count the frame as sent. A radio that stopped answering ends the same way.
    Bench b(179);
    CHECK_EQ(b.beginLongFast(), RADIOLIB_ERR_NONE);
    std::atomic<bool> edge{true};
    const uint64_t from = b.clock.nowUs();
    const uint16_t flags = bench::awaitTransmitDone(b.radio, b.clock, from + 500000, &edge, nullptr);
    CHECK(!(flags & RADIOLIB_SX126X_IRQ_TX_DONE));
    CHECK(b.clock.nowUs() >= from + 500000);
    std::atomic<bool> stop{true};
    const uint64_t again = b.clock.nowUs();
    CHECK(!(bench::awaitTransmitDone(b.radio, b.clock, again + 500000000, &edge, &stop) & RADIOLIB_SX126X_IRQ_TX_DONE));
    CHECK(b.clock.nowUs() < again + 1000000); // a stop request ends it at once
}

TEST(a_frame_that_is_over_leaves_nothing_for_the_next)
{
    Bench b(181);
    CHECK_EQ(b.beginLongFast(), RADIOLIB_ERR_NONE);
    std::atomic<bool> edge{false};
    for (int i = 0; i < 3; i++) {
        const std::vector<uint8_t> frame = meshtasticFrame((size_t)(30 + 50 * i));
        bench::Proof proof;
        CHECK_EQ(bench::send(b.radio, b.bridge, b.clock, bench::From::StandbyRc, false, frame.data(), frame.size(), proof),
                 RADIOLIB_ERR_NONE);
        CHECK_EQ(proof.loaded, sim::ModeStandbyRc);
        const uint64_t launched = b.clock.nowUs();
        CHECK(bench::awaitTransmitDone(b.radio, b.clock, launched + 10000000, &edge, nullptr) & RADIOLIB_SX126X_IRQ_TX_DONE);
        CHECK(b.clock.nowUs() - launched >= 20000 + 1500ULL * frame.size());
        CHECK_EQ(b.radio.finishTransmit(), RADIOLIB_ERR_NONE);
        // The flags are clean: the next frame's wait cannot be satisfied by this one.
        CHECK_EQ(b.radio.getIrqFlags() & RADIOLIB_SX126X_IRQ_TX_DONE, 0);
        CHECK(b.plate.radio.transmitted().back() == frame);
    }
    CHECK_EQ(b.plate.radio.transmitted().size(), 3);
    CHECK_EQ(b.plate.radio.busyViolations(), 0);
}

TEST(the_mode_read_back_is_the_mode_the_radio_is_in)
{
    Bench b(167);
    CHECK_EQ(b.beginLongFast(), RADIOLIB_ERR_NONE);
    CHECK_EQ(bench::chipMode(b.bridge), sim::ModeStandbyRc);
    CHECK_EQ(b.radio.standby(RADIOLIB_SX126X_STANDBY_XOSC), RADIOLIB_ERR_NONE);
    CHECK_EQ(bench::chipMode(b.bridge), sim::ModeStandbyXosc);
    CHECK_EQ(b.radio.startReceive(), RADIOLIB_ERR_NONE);
    CHECK_EQ(bench::chipMode(b.bridge), sim::ModeRx);
    // Asking does not change it, and leaves the ring level.
    CHECK_EQ(bench::chipMode(b.bridge), sim::ModeRx);
    CHECK_EQ(b.plate.radio.mode(), sim::ModeRx);
    CHECK(b.level());
}

int main(int argc, char **argv)
{
    return check::runAll(argc, argv);
}
