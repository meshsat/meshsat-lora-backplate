// SPDX-License-Identifier: GPL-3.0-or-later
// A back cover on the bench that is not there: the ATtiny84 bridge as its firmware behaves
// (one command, a 128-byte reply ring read one byte at a time) and enough of an SX1262 behind
// it to run a driver against. Time is simulated, so a test can tell whether the radio was
// spoken to while it would have held BUSY high.
#pragma once

#include "PineDioBridge.h"

#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace meshsat
{
namespace pinedio
{
namespace sim
{

class SimClock : public Clock
{
  public:
    uint64_t nowUs() override { return now; }
    void sleepUs(uint64_t us) override { now += us ? us : 1; }
    void advance(uint64_t us) { now += us; }

  private:
    uint64_t now = 1000000;
};

enum Irq : uint16_t {
    IrqTxDone = 0x0001,
    IrqRxDone = 0x0002,
    IrqPreamble = 0x0004,
    IrqSyncWord = 0x0008,
    IrqHeaderValid = 0x0010,
    IrqHeaderErr = 0x0020,
    IrqCrcErr = 0x0040,
    IrqCadDone = 0x0080,
    IrqCadDetected = 0x0100,
    IrqTimeout = 0x0200,
};

enum Mode : uint8_t { ModeSleep = 0, ModeStandbyRc = 2, ModeStandbyXosc = 3, ModeFs = 4, ModeRx = 5, ModeTx = 6, ModeCad = 7 };

class Sx1262
{
  public:
    explicit Sx1262(SimClock &c);

    /// One chip-select frame taking `durationUs` on the wire. Returns what the radio clocked
    /// back, byte for byte. BUSY is judged at the falling edge of chip select and any new busy
    /// period starts at its rising edge.
    std::vector<uint8_t> spiFrame(const std::vector<uint8_t> &mosi, uint64_t durationUs = 0);

    /// A packet arriving on air. False when the radio is not receiving.
    bool injectRx(const std::vector<uint8_t> &payload, int rssiDbm = -60, int snrDb = 8);
    void setChannelBusy(bool busy) { channelBusy = busy; }
    /// Set interrupt flags without touching mode or command status, as a bad header does.
    void raiseQuietly(uint16_t flags) { raise(flags); }

    Mode mode() const { return chipMode; }
    /// One entry for every transmit command: the mode it met, and for how long the crystal had
    /// been running by then, in microseconds. Zero when the crystal had to start for the frame.
    struct Transmission {
        Mode from;
        uint64_t crystalUs;
    };
    const std::vector<Transmission> &transmissions() const { return entries; }
    /// How often the crystal was stopped. Standby on the RC oscillator and sleep stop it.
    unsigned crystalStops() const { return stops; }
    uint16_t irqStatus() const { return irq; }
    uint8_t reg(uint16_t address) const;
    uint32_t frequencyHz() const { return (uint32_t)((uint64_t)rfFreq * 32000000ULL >> 25); }
    const std::vector<std::vector<uint8_t>> &transmitted() const { return sent; }
    unsigned busyViolations() const { return violations; }
    unsigned ignoredWhileAsleep() const { return ignored; }
    unsigned frames() const { return frameCount; }
    unsigned longestFrame() const { return longest; }
    const std::vector<uint8_t> &opcodes() const { return seen; }

  private:
    void tick();
    void enter(Mode next);
    Mode fallbackMode() const;
    void coldStart();
    void warmStart();
    void raise(uint16_t flags) { irq |= (uint16_t)(flags & irqMask); }
    uint8_t status() const;
    void busyFor(uint64_t us);

    SimClock &clock;
    Mode chipMode = ModeStandbyRc;
    uint8_t cmdStatus = 1;
    std::array<uint8_t, 256> buffer{};
    std::map<uint16_t, uint8_t> regs;
    uint16_t irq = 0, irqMask = 0, dio1Mask = 0;
    uint8_t txBase = 0, rxBase = 0, rxLen = 0, rxStart = 0;
    uint8_t pktRssi = 0, pktSnr = 0, payloadLen = 0;
    uint8_t packetType = 0;
    uint32_t rfFreq = 0;
    uint16_t deviceErrors = 0;
    bool rxContinuous = false;
    bool channelBusy = false;
    bool sleptWarm = false;
    uint64_t busyUntil = 0, txDoneAt = 0, cadDoneAt = 0;
    unsigned violations = 0, ignored = 0, frameCount = 0, longest = 0;
    uint8_t fallback = 0x20;
    bool crystalOn = false;
    uint64_t crystalSince = 0;
    unsigned stops = 0;
    std::vector<Transmission> entries;
    std::vector<std::vector<uint8_t>> sent;
    std::vector<uint8_t> seen;
};

/// The ATtiny84 with tiny-i2c-spi, and the radio behind it.
class Backplate : public I2cPort
{
  public:
    /// usPerI2cByte: 25 is a 400 kHz bus. usPerSpiByte: 3450 is the bridge as it ships, 40 one
    /// with a quiet firmware. The faster both are, the less BUSY is hidden by accident.
    explicit Backplate(SimClock &c, unsigned seed = 1, uint32_t usPerI2cByte = 25, uint32_t usPerSpiByte = 3450);

    bool write(const uint8_t *data, size_t len) override;
    bool readByte(uint8_t &out) override;

    Sx1262 radio;

    void failWrites(unsigned n) { writeFaults = n; }
    void failReads(unsigned n) { readFaults = n; }
    /// Leave text in the ring as an earlier user would have, at a place the next sync walks over.
    void plantStale(const std::vector<uint8_t> &bytes, unsigned ahead);
    unsigned writes() const { return writeCount; }
    unsigned reads() const { return readCount; }
    unsigned longestWrite() const { return longestWr; }
    unsigned oversize() const { return tooLong; }
    unsigned unread() const { return (uint8_t)(writeIndex - readIndex + 128) % 128; }
    /// A frame written and not yet clocked out: the bridge does that when it is next spoken to.
    bool framePending() const { return !pending.empty(); }

  private:
    void flush();

    SimClock &clock;
    uint32_t usPerByte;
    uint32_t usPerSpi;
    std::vector<uint8_t> pending;
    std::array<uint8_t, 128> ring{};
    uint8_t writeIndex = 0, readIndex = 0;
    unsigned writeFaults = 0, readFaults = 0;
    unsigned writeCount = 0, readCount = 0, longestWr = 0, tooLong = 0;
};

} // namespace sim
} // namespace pinedio
} // namespace meshsat
