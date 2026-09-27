// SPDX-License-Identifier: GPL-3.0-or-later
// RadioLib's own SX1262 driver, unmodified, over the bridge and the simulated back cover.
// What passes here is what the Meshtastic daemon will ask of the transport.
#include "PineDioBridgeHal.h"
#include "check.h"
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

    explicit Bench(unsigned seed, size_t frame = 64)
        : plate(clock, seed), bridge(plate, clock, config(frame)), hal(bridge, clock, false),
          module(&hal, Bridge::PinCs, Bridge::PinIrq, Bridge::PinReset, Bridge::PinBusy), radio(&module)
    {
    }
    static Config config(size_t frame)
    {
        Config c;
        c.maxSpiFrame = frame;
        return c;
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
    CHECK_EQ(b.plate.unread(), 0);
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
    CHECK_EQ(b.plate.unread(), 0);
}

TEST(the_longest_frame_is_received_in_pieces)
{
    for (size_t frame : {16, 64, 127}) {
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
    CHECK_EQ(b.plate.unread(), 0);
}

int main(int argc, char **argv)
{
    return check::runAll(argc, argv);
}
