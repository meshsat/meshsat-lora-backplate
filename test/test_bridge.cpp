// SPDX-License-Identifier: GPL-3.0-or-later
// The bridge transport against the simulated back cover. No radio library involved.
#include "PineDioBridge.h"
#include "check.h"
#include "sim/SimBackplate.h"

#include <atomic>
#include <chrono>
#include <thread>

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

std::vector<uint8_t> ramp(size_t n, uint8_t start = 1)
{
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++)
        v[i] = (uint8_t)(start + i * 7);
    return v;
}

bool writeBuffer(Bridge &b, uint8_t offset, const std::vector<uint8_t> &data)
{
    std::vector<uint8_t> out{0x0E, offset};
    out.insert(out.end(), data.begin(), data.end());
    return b.transfer(out.data(), nullptr, out.size());
}

std::vector<uint8_t> readBuffer(Bridge &b, uint8_t offset, size_t n, bool *ok = nullptr)
{
    std::vector<uint8_t> out(3 + n, 0x00), in(3 + n, 0xEE);
    out[0] = 0x1E;
    out[1] = offset;
    const bool good = b.transfer(out.data(), in.data(), out.size());
    if (ok)
        *ok = good;
    return std::vector<uint8_t>(in.begin() + 3, in.end());
}

uint8_t readRegister(Bridge &b, uint16_t address)
{
    uint8_t out[5] = {0x1D, (uint8_t)(address >> 8), (uint8_t)address, 0, 0}, in[5] = {0};
    b.transfer(out, in, sizeof(out));
    return in[4];
}

/// True when, after one more reply has been fetched, nothing is left unread in the ring.
bool level(Bridge &b, Backplate &plate)
{
    uint8_t out[2] = {0xC0, 0}, in[2] = {0};
    return b.transfer(out, in, 2) && plate.unread() == 0;
}

void startReceiving(Bridge &b, uint16_t mask = sim::IrqRxDone)
{
    const uint8_t dio[9] = {0x08, (uint8_t)(mask >> 8), (uint8_t)mask, (uint8_t)(mask >> 8), (uint8_t)mask, 0, 0, 0, 0};
    const uint8_t rx[4] = {0x82, 0xFF, 0xFF, 0xFF};
    b.transfer(dio, nullptr, sizeof(dio));
    b.transfer(rx, nullptr, sizeof(rx));
}
} // namespace

TEST(sync_lines_up_from_any_ring_position)
{
    for (unsigned seed = 1; seed <= 60; seed++) {
        SimClock clock;
        Backplate plate(clock, seed);
        Bridge bridge(plate, clock);
        CHECK(bridge.begin());
        CHECK(!bridge.inError());
        CHECK_EQ(plate.unread(), 0);
        uint8_t out[2] = {0xC0, 0}, in[2] = {0};
        CHECK(bridge.transfer(out, in, 2));
        CHECK_EQ(in[1], 0x22); // standby, no error: the reply is this frame's, not an old one
    }
}

TEST(sync_is_not_fooled_by_the_pattern_of_an_earlier_driver)
{
    SimClock clock;
    Backplate plate(clock, 7);
    // JF002's driver always writes this pattern; a copy of it is waiting in the ring.
    plate.plantStale({0x10, 0x20, 0x30, 0x40, 0x50, 0xAA, 0x55, 0x00, 0xFF}, 3);
    Bridge bridge(plate, clock);
    CHECK(bridge.begin());
    CHECK_EQ(plate.unread(), 0);
}

TEST(sync_fails_cleanly_when_nothing_answers)
{
    SimClock clock;
    Backplate plate(clock, 3);
    plate.failWrites(1000);
    Bridge bridge(plate, clock);
    CHECK(!bridge.begin());
    CHECK(bridge.inError());
    uint8_t out[2] = {0xC0, 0}, in[2] = {9, 9};
    CHECK(!bridge.transfer(out, in, 2));
    CHECK_EQ(in[1], 0);
}

TEST(register_round_trip)
{
    SimClock clock;
    Backplate plate(clock, 11);
    Bridge bridge(plate, clock);
    CHECK(bridge.begin());
    CHECK_EQ(readRegister(bridge, 0x0740), 0x14);
    const uint8_t sync[5] = {0x0D, 0x07, 0x40, 0x24, 0xB4};
    CHECK(bridge.transfer(sync, nullptr, sizeof(sync)));
    CHECK_EQ(readRegister(bridge, 0x0740), 0x24);
    CHECK_EQ(readRegister(bridge, 0x0741), 0xB4);
    CHECK_EQ(plate.radio.reg(0x0741), 0xB4);
}

TEST(every_buffer_length_survives_the_trip)
{
    SimClock clock;
    Backplate plate(clock, 5);
    Bridge bridge(plate, clock);
    CHECK(bridge.begin());
    for (size_t n = 1; n <= 255; n++) {
        const std::vector<uint8_t> data = ramp(n, (uint8_t)n);
        CHECK(writeBuffer(bridge, 0, data));
        bool ok = false;
        const std::vector<uint8_t> back = readBuffer(bridge, 0, n, &ok);
        CHECK(ok);
        CHECK(back == data);
        if (back != data)
            break;
    }
    CHECK_EQ(plate.oversize(), 0);
    CHECK(plate.longestWrite() <= 97);        // 96 SPI bytes and the bridge's command byte
    CHECK(plate.radio.longestFrame() <= 96);
    CHECK_EQ(plate.radio.busyViolations(), 0);
    CHECK(level(bridge, plate));
    CHECK(bridge.stats().split > 0);
}

TEST(a_full_frame_is_split_at_the_smallest_and_largest_settings)
{
    for (size_t frame : {16, 32, 127}) {
        SimClock clock;
        Backplate plate(clock, 9);
        Config cfg;
        cfg.maxSpiFrame = frame;
        Bridge bridge(plate, clock, cfg);
        CHECK(bridge.begin());
        const std::vector<uint8_t> data = ramp(255, 3);
        CHECK(writeBuffer(bridge, 0, data));
        CHECK(readBuffer(bridge, 0, 255) == data);
        CHECK(plate.radio.longestFrame() <= frame);
        CHECK_EQ(plate.oversize(), 0);
    }
}

TEST(a_frame_setting_out_of_range_is_brought_back_in)
{
    for (size_t asked : {0, 3, 500}) {
        SimClock clock;
        Backplate plate(clock, 71);
        Config cfg;
        cfg.maxSpiFrame = asked;
        Bridge bridge(plate, clock, cfg);
        CHECK(bridge.begin());
        const std::vector<uint8_t> data = ramp(255, 1);
        CHECK(writeBuffer(bridge, 0, data));
        CHECK(readBuffer(bridge, 0, 255) == data);
        CHECK(plate.radio.longestFrame() >= 16 && plate.radio.longestFrame() <= 127);
        CHECK_EQ(plate.oversize(), 0);
    }
}

TEST(buffer_offsets_wrap_like_the_radio_does)
{
    SimClock clock;
    Backplate plate(clock, 13);
    Bridge bridge(plate, clock);
    CHECK(bridge.begin());
    const std::vector<uint8_t> data = ramp(200, 9);
    CHECK(writeBuffer(bridge, 128, data)); // runs past 255 and comes back round to 71
    CHECK(readBuffer(bridge, 128, 200) == data);
    CHECK(readBuffer(bridge, 0, 72) == std::vector<uint8_t>(data.begin() + 128, data.end()));
}

TEST(long_register_block_is_split_with_the_address_moved_along)
{
    SimClock clock;
    Backplate plate(clock, 17);
    Bridge bridge(plate, clock);
    CHECK(bridge.begin());
    std::vector<uint8_t> out{0x0D, 0x06, 0xF0};
    const std::vector<uint8_t> data = ramp(100, 5);
    out.insert(out.end(), data.begin(), data.end());
    CHECK(bridge.transfer(out.data(), nullptr, out.size()));
    std::vector<uint8_t> rd(4 + 100, 0), in(4 + 100, 0);
    rd[0] = 0x1D;
    rd[1] = 0x06;
    rd[2] = 0xF0;
    CHECK(bridge.transfer(rd.data(), in.data(), rd.size()));
    CHECK(std::vector<uint8_t>(in.begin() + 4, in.end()) == data);
    CHECK_EQ(plate.radio.reg(0x06F0 + 99), data[99]);
}

TEST(a_long_command_that_cannot_be_split_is_refused)
{
    SimClock clock;
    Backplate plate(clock, 19);
    Bridge bridge(plate, clock);
    CHECK(bridge.begin());
    const unsigned before = plate.radio.frames();
    std::vector<uint8_t> out(100, 0x00);
    out[0] = 0x8B; // SetModulationParams is never this long
    CHECK(!bridge.transfer(out.data(), nullptr, out.size()));
    CHECK_EQ(plate.radio.frames(), before);
    CHECK(!bridge.inError()); // refusing is not a bridge failure
}

TEST(the_ring_stays_level_over_thousands_of_frames)
{
    SimClock clock;
    Backplate plate(clock, 23);
    Bridge bridge(plate, clock);
    CHECK(bridge.begin());
    for (int i = 0; i < 3000; i++) {
        const uint8_t v = (uint8_t)(i * 13 + 1);
        const uint8_t wr[4] = {0x0D, 0x07, 0x40, v};
        CHECK(bridge.transfer(wr, nullptr, sizeof(wr)));
        if (readRegister(bridge, 0x0740) != v) {
            CHECK(false);
            break;
        }
    }
    CHECK(level(bridge, plate));
    CHECK_EQ(plate.radio.busyViolations(), 0);
}

TEST(the_reply_to_a_command_that_only_writes_is_never_read)
{
    SimClock clock;
    Backplate plate(clock, 73);
    Bridge bridge(plate, clock);
    CHECK(bridge.begin());
    const unsigned readsBefore = plate.reads();
    const uint8_t freq[5] = {0x86, 0x36, 0x58, 0x66, 0x66};
    uint8_t in[5] = {0};
    for (int i = 0; i < 40; i++)
        CHECK(bridge.transfer(freq, in, sizeof(freq)));
    CHECK_EQ(plate.reads(), readsBefore);
    CHECK_EQ(in[1], 0x22); // the status the radio gave when it was last asked
    CHECK(writeBuffer(bridge, 0, ramp(255)));
    CHECK_EQ(plate.reads(), readsBefore);
    CHECK(bridge.stats().readsSaved >= 40 * 5 + 257);

    // and the next reply that matters is still the right one
    CHECK(readBuffer(bridge, 0, 255) == ramp(255));
    CHECK_EQ(readRegister(bridge, 0x0740), 0x14);
    CHECK(level(bridge, plate));
    CHECK_EQ(plate.radio.busyViolations(), 0);
}

TEST(a_stale_complaint_is_not_pinned_on_the_next_write)
{
    SimClock clock;
    Backplate plate(clock, 97);
    Bridge bridge(plate, clock);
    CHECK(bridge.begin());
    uint8_t bogus[3] = {0x55, 0, 0}, in[3] = {0}; // not a command: the radio answers "invalid"
    CHECK(bridge.transfer(bogus, in, sizeof(bogus)));
    uint8_t status[2] = {0xC0, 0}, reply[2] = {0};
    CHECK(bridge.transfer(status, reply, 2));
    const uint8_t freq[5] = {0x86, 0x36, 0x58, 0x66, 0x66};
    uint8_t out[5] = {0};
    CHECK(bridge.transfer(freq, out, sizeof(freq)));
    CHECK_EQ((out[1] >> 1) & 7, 1);  // no complaint
    CHECK_EQ((out[1] >> 4) & 7, 2);  // standby, as last seen
}

TEST(the_ring_is_levelled_from_every_leftover)
{
    for (unsigned leftover = 1; leftover < 128; leftover++) {
        SimClock clock;
        Backplate plate(clock, 79 + leftover);
        Bridge bridge(plate, clock);
        CHECK(bridge.begin());
        // leave exactly `leftover` reply bytes unread, in write-only frames of mixed length
        unsigned left = leftover;
        while (left) {
            const unsigned n = left > 60 ? 60 : left;
            std::vector<uint8_t> out(n, 0x00);
            out[0] = n >= 2 ? 0x0E : 0x80; // WriteBuffer, or a bare SetStandby opcode
            CHECK(bridge.transfer(out.data(), nullptr, n));
            left -= n;
        }
        CHECK_EQ(plate.unread(), leftover);
        const unsigned readsBefore = plate.reads();
        const uint8_t sync[5] = {0x0D, 0x07, 0x40, 0x24, (uint8_t)leftover};
        CHECK(bridge.transfer(sync, nullptr, sizeof(sync)));
        CHECK_EQ(readRegister(bridge, 0x0741), leftover);
        // reading is only chosen when it is the cheaper way
        const unsigned spent = plate.reads() - readsBefore;
        CHECK(spent <= 5 + 3);
        CHECK_EQ(plate.unread(), 0);
        CHECK_EQ(plate.radio.busyViolations(), 0);
        CHECK_EQ(plate.oversize(), 0);
    }
}

TEST(padding_is_made_of_commands_the_radio_knows)
{
    SimClock clock;
    Backplate plate(clock, 83);
    Config cfg;
    cfg.maxSpiFrame = 32; // padding has to come in several frames
    Bridge bridge(plate, clock, cfg);
    CHECK(bridge.begin());
    CHECK(writeBuffer(bridge, 0, ramp(10)));
    CHECK(readBuffer(bridge, 0, 10) == ramp(10));
    CHECK(bridge.stats().padBytes > 0);
    for (uint8_t op : plate.radio.opcodes())
        CHECK(op == 0xC0 || op == 0x80 || op == 0x8F || op == 0x0E || op == 0x1E);
    CHECK(plate.radio.longestFrame() <= 32);
    CHECK_EQ(readRegister(bridge, 0x0740), 0x14); // padding wrote nothing anywhere
    CHECK(readBuffer(bridge, 0, 10) == ramp(10));
}

TEST(every_reply_can_still_be_read_when_asked)
{
    SimClock clock;
    Backplate plate(clock, 89);
    Config cfg;
    cfg.skipWriteReplies = false;
    Bridge bridge(plate, clock, cfg);
    CHECK(bridge.begin());
    const unsigned readsBefore = plate.reads();
    const uint8_t freq[5] = {0x86, 0x36, 0x58, 0x66, 0x66};
    uint8_t in[5] = {0};
    CHECK(bridge.transfer(freq, in, sizeof(freq)));
    CHECK_EQ(plate.reads(), readsBefore + 5);
    CHECK_EQ(plate.unread(), 0);
    CHECK_EQ(bridge.stats().padBytes, 0);
}

TEST(dio1_edge_is_delivered_once_per_event)
{
    SimClock clock;
    Backplate plate(clock, 29);
    Bridge bridge(plate, clock);
    CHECK(bridge.begin());
    irqCount = 0;
    bridge.attachInterrupt(Bridge::PinIrq, onIrq);
    CHECK(!bridge.pollOnce()); // no mask yet: the line cannot rise
    startReceiving(bridge);
    CHECK_EQ(bridge.dio1Mask(), sim::IrqRxDone);
    CHECK(!bridge.pollOnce());
    CHECK_EQ(bridge.digitalRead(Bridge::PinIrq), 0);

    CHECK(plate.radio.injectRx(ramp(25)));
    CHECK_EQ(bridge.digitalRead(Bridge::PinIrq), 1);
    CHECK(bridge.pollOnce());
    CHECK_EQ(irqCount.load(), 1);
    CHECK(!bridge.pollOnce()); // still high, already delivered
    CHECK(!bridge.pollOnce());
    CHECK_EQ(irqCount.load(), 1);

    const uint8_t clear[3] = {0x02, 0xFF, 0xFF};
    CHECK(bridge.transfer(clear, nullptr, sizeof(clear)));
    CHECK_EQ(bridge.digitalRead(Bridge::PinIrq), 0);
    CHECK(!bridge.pollOnce());
    CHECK(plate.radio.injectRx(ramp(40)));
    CHECK(bridge.pollOnce());
    CHECK_EQ(irqCount.load(), 2);
}

TEST(an_event_right_after_a_clear_is_a_new_edge_even_without_a_poll_in_between)
{
    SimClock clock;
    Backplate plate(clock, 31);
    Bridge bridge(plate, clock);
    CHECK(bridge.begin());
    irqCount = 0;
    bridge.attachInterrupt(Bridge::PinIrq, onIrq);
    startReceiving(bridge);
    CHECK(plate.radio.injectRx(ramp(10)));
    CHECK(bridge.pollOnce());
    const uint8_t clear[3] = {0x02, 0xFF, 0xFF};
    CHECK(bridge.transfer(clear, nullptr, sizeof(clear)));
    CHECK(plate.radio.injectRx(ramp(12))); // lands before the poller looks again
    CHECK(bridge.pollOnce());
    CHECK_EQ(irqCount.load(), 2);
}

TEST(flags_outside_the_dio1_mask_do_not_raise_the_line)
{
    SimClock clock;
    Backplate plate(clock, 37);
    Bridge bridge(plate, clock);
    CHECK(bridge.begin());
    irqCount = 0;
    bridge.attachInterrupt(Bridge::PinIrq, onIrq);
    // The radio records preamble and header, the line carries RxDone only.
    const uint16_t all = sim::IrqRxDone | sim::IrqPreamble | sim::IrqHeaderValid;
    const uint8_t dio[9] = {0x08, (uint8_t)(all >> 8), (uint8_t)all, 0x00, 0x04, 0, 0, 0, 0}; // DIO1 = preamble only
    const uint8_t rx[4] = {0x82, 0xFF, 0xFF, 0xFF};
    bridge.transfer(dio, nullptr, sizeof(dio));
    bridge.transfer(rx, nullptr, sizeof(rx));
    CHECK_EQ(bridge.dio1Mask(), 0x0004);
    CHECK(plate.radio.injectRx(ramp(9)));
    CHECK(bridge.pollOnce()); // preamble is in the mask
    const uint8_t clearPreamble[3] = {0x02, 0x00, 0x04};
    bridge.transfer(clearPreamble, nullptr, sizeof(clearPreamble));
    CHECK(!bridge.pollOnce()); // RxDone is still set in the radio, and not in the mask
    CHECK_EQ(irqCount.load(), 1);
}

TEST(sleep_is_followed_by_a_wake_pulse_and_no_command_is_lost)
{
    SimClock clock;
    Backplate plate(clock, 41);
    Bridge bridge(plate, clock);
    CHECK(bridge.begin());
    const uint8_t sync[5] = {0x0D, 0x07, 0x40, 0x24, 0xB4};
    CHECK(bridge.transfer(sync, nullptr, sizeof(sync)));
    const uint8_t sleep[2] = {0x84, 0x04}; // warm start
    CHECK(bridge.transfer(sleep, nullptr, sizeof(sleep)));
    CHECK(bridge.asleep());
    CHECK_EQ(plate.radio.mode(), sim::ModeSleep);
    CHECK(!bridge.pollOnce()); // looking would wake it
    CHECK_EQ(plate.radio.mode(), sim::ModeSleep);

    const uint8_t freq[5] = {0x86, 0x36, 0x58, 0x66, 0x66}; // 869.525 MHz
    CHECK(bridge.transfer(freq, nullptr, sizeof(freq)));
    CHECK(!bridge.asleep());
    CHECK_EQ(plate.radio.ignoredWhileAsleep(), 1); // the pulse, not the command
    CHECK(plate.radio.frequencyHz() > 869524000 && plate.radio.frequencyHz() < 869526000);
    CHECK_EQ(plate.radio.busyViolations(), 0);
    CHECK_EQ(bridge.stats().wakes, 1);
}

TEST(chip_select_low_wakes_a_sleeping_radio)
{
    SimClock clock;
    Backplate plate(clock, 43);
    Bridge bridge(plate, clock);
    CHECK(bridge.begin());
    const uint8_t sleep[2] = {0x84, 0x04};
    CHECK(bridge.transfer(sleep, nullptr, sizeof(sleep)));
    bridge.digitalWrite(Bridge::PinCs, 0);
    bridge.digitalWrite(Bridge::PinCs, 1);
    CHECK(!bridge.asleep());
    CHECK(plate.radio.mode() != sim::ModeSleep);
    CHECK_EQ(plate.radio.busyViolations(), 0);
}

TEST(reset_line_is_a_cold_restart)
{
    SimClock clock;
    Backplate plate(clock, 47);
    Bridge bridge(plate, clock);
    CHECK(bridge.begin());
    const uint8_t sync[5] = {0x0D, 0x07, 0x40, 0x24, 0xB4};
    CHECK(bridge.transfer(sync, nullptr, sizeof(sync)));
    startReceiving(bridge);
    CHECK_EQ(plate.radio.mode(), sim::ModeRx);

    bridge.digitalWrite(Bridge::PinReset, 1); // high without a low before it: nothing
    CHECK_EQ(plate.radio.mode(), sim::ModeRx);
    bridge.digitalWrite(Bridge::PinReset, 0);
    bridge.digitalWrite(Bridge::PinReset, 1);
    CHECK_EQ(plate.radio.mode(), sim::ModeStandbyRc);
    CHECK_EQ(readRegister(bridge, 0x0740), 0x14); // configuration forgotten
    CHECK_EQ(bridge.dio1Mask(), 0);
    CHECK_EQ(plate.radio.busyViolations(), 0);
    CHECK(level(bridge, plate));
}

TEST(calibration_and_mode_changes_never_meet_a_busy_radio)
{
    SimClock clock;
    Backplate plate(clock, 53, 3); // a bus far faster than the phone's
    Bridge bridge(plate, clock);
    CHECK(bridge.begin());
    const uint8_t seq[][5] = {{0x89, 0x7F}, {0x98, 0xD7, 0xDB}, {0x80, 0x01}, {0xC1}, {0x82, 0xFF, 0xFF, 0xFF}, {0x80, 0x00},
                              {0xC5},       {0x80, 0x00},       {0x84, 0x00}, {0xC0, 0x00}};
    const size_t len[] = {2, 3, 2, 1, 4, 2, 1, 2, 2, 2};
    for (int round = 0; round < 20; round++)
        for (size_t i = 0; i < sizeof(len) / sizeof(len[0]); i++)
            CHECK(bridge.transfer(seq[i], nullptr, len[i]));
    CHECK_EQ(plate.radio.busyViolations(), 0);
}

TEST(an_unacknowledged_write_is_repeated)
{
    SimClock clock;
    Backplate plate(clock, 59);
    Bridge bridge(plate, clock);
    CHECK(bridge.begin());
    plate.failWrites(2);
    const uint8_t sync[5] = {0x0D, 0x07, 0x40, 0x24, 0xB4};
    CHECK(bridge.transfer(sync, nullptr, sizeof(sync)));
    CHECK_EQ(plate.radio.reg(0x0740), 0x24);
    CHECK_EQ(bridge.stats().retries, 2);
    CHECK(!bridge.inError());
    CHECK(level(bridge, plate));
}

TEST(a_failed_read_stops_everything_until_the_next_begin)
{
    SimClock clock;
    Backplate plate(clock, 61);
    Bridge bridge(plate, clock);
    CHECK(bridge.begin());
    plate.failReads(1);
    uint8_t out[2] = {0xC0, 0}, in[2];
    CHECK(!bridge.transfer(out, in, 2));
    CHECK(bridge.inError());
    CHECK(!bridge.transfer(out, in, 2));
    CHECK_EQ(bridge.digitalRead(Bridge::PinIrq), 1); // a driver waiting on the line is let go
    CHECK(!bridge.pollOnce());
    CHECK(bridge.begin()); // and the same object recovers
    CHECK(!bridge.inError());
    CHECK(bridge.transfer(out, in, 2));
    CHECK_EQ(in[1], 0x22);
    CHECK_EQ(plate.unread(), 0);
}

TEST(the_polling_thread_delivers_an_edge_by_itself)
{
    SimClock simClock;
    Backplate plate(simClock, 67);
    Config cfg;
    cfg.pollIntervalUs = 2000;
    Bridge bridge(plate, simClock, cfg);
    CHECK(bridge.begin());
    irqCount = 0;
    bridge.attachInterrupt(Bridge::PinIrq, onIrq);
    startReceiving(bridge);
    bridge.startPolling();
    bridge.startPolling(); // twice is once
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_EQ(irqCount.load(), 0);
    CHECK(bridge.stats().polls > 0);
    // The simulator is single-threaded: park the poller while the test plays the transmitter.
    bridge.stopPolling();
    CHECK(plate.radio.injectRx(ramp(30)));
    bridge.startPolling();
    for (int i = 0; i < 200 && irqCount.load() == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK_EQ(irqCount.load(), 1);
    bridge.stopPolling();
    bridge.stopPolling();
    const uint64_t polls = bridge.stats().polls;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_EQ(bridge.stats().polls, polls);
}

int main(int argc, char **argv)
{
    return check::runAll(argc, argv);
}
