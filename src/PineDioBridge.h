// SPDX-License-Identifier: GPL-3.0-or-later
// MeshSat: the Pine64 LoRa back cover's I2C-to-SPI bridge, as a transport for an SX1262 driver.
//
// The back cover carries an SX1262 behind an ATtiny84 running tiny-i2c-spi. The bridge offers
// one useful command (0x01 + bytes: clock them out over SPI in one chip-select frame) and keeps
// what the radio clocked back in a 128-byte ring buffer that is read one byte per I2C read.
// It has no reset line, no BUSY and no DIO1. This class carries SPI frames over that bridge and
// stands in for the three missing lines:
//   BUSY  -> settle delays sized per command, and a wake pulse when the radio sleeps
//   DIO1  -> GetIrqStatus polled through the bridge, edges delivered to a callback
//   RESET -> a cold-start sleep followed by a wake
// It knows nothing about RadioLib or Meshtastic; PineDioBridgeHal.h adapts it to RadioLib.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

namespace meshsat
{
namespace pinedio
{

/// The I2C side of one bridge: address already selected.
class I2cPort
{
  public:
    virtual ~I2cPort() = default;
    /// One I2C write transaction. True when every byte was acknowledged.
    virtual bool write(const uint8_t *data, size_t len) = 0;
    /// One I2C read transaction of exactly one byte.
    virtual bool readByte(uint8_t &out) = 0;
};

class Clock
{
  public:
    virtual ~Clock() = default;
    virtual uint64_t nowUs() = 0;
    virtual void sleepUs(uint64_t us) = 0;
};

class SystemClock : public Clock
{
  public:
    uint64_t nowUs() override;
    void sleepUs(uint64_t us) override;
};

struct Config {
    /// SPI bytes carried by one bridge transaction. The ATtiny takes 128 bytes per I2C write
    /// (command byte included) and remembers 128 bytes of reply, so 127 is the ceiling.
    /// The floor is 16: the sync reads its pattern back in a single frame.
    size_t maxSpiFrame = 64;
    /// Period of the DIO1 stand-in.
    uint32_t pollIntervalUs = 10000;
    /// Settle time after any command, after a mode change, after a calibration, after a wake.
    uint32_t settleUs = 300;
    uint32_t settleModeUs = 1000;
    uint32_t settleCalUs = 5000;
    uint32_t wakeUs = 5000;
    /// An unacknowledged I2C write clocked nothing out, so it is safe to repeat.
    int writeRetries = 3;
    uint32_t retryDelayUs = 10000;
    /// Single-byte reads spent looking for the sync pattern before giving up.
    unsigned syncMaxReads = 400;
};

struct Stats {
    uint64_t frames = 0;     ///< SPI frames carried
    uint64_t spiBytes = 0;   ///< bytes clocked out
    uint64_t i2cWrites = 0;  ///< I2C write transactions
    uint64_t i2cReads = 0;   ///< I2C read transactions
    uint64_t retries = 0;    ///< repeated I2C writes
    uint64_t errors = 0;     ///< transfers that failed
    uint64_t split = 0;      ///< buffer or register commands carried in several frames
    uint64_t polls = 0;      ///< DIO1 polls
    uint64_t irqEdges = 0;   ///< rising edges delivered
    uint64_t wakes = 0;      ///< wake pulses sent
    unsigned syncReads = 0;  ///< reads the last sync needed
};

class Bridge
{
  public:
    using IrqCallback = void (*)(void);

    /// Pin numbers a radio driver is given for the lines that do not exist.
    enum Pin : uint32_t { PinCs = 1, PinIrq = 2, PinBusy = 3, PinReset = 4 };

    Bridge(I2cPort &port, Clock &clock, const Config &config = Config());
    ~Bridge();
    Bridge(const Bridge &) = delete;
    Bridge &operator=(const Bridge &) = delete;

    /// Wake the radio, line up the ring buffer with a fresh pattern and check the radio answers.
    /// Leaves the radio in standby with its data buffer overwritten. Clears a previous error.
    bool begin();
    /// Stop polling. The radio keeps its state.
    void end();

    /// One SPI frame: `len` bytes out, the bytes clocked back into `in` (may be null).
    /// WriteBuffer, ReadBuffer, WriteRegister and ReadRegister longer than one bridge
    /// transaction are carried as several frames with the offset moved along.
    bool transfer(const uint8_t *out, uint8_t *in, size_t len);

    void digitalWrite(uint32_t pin, uint32_t level);
    uint32_t digitalRead(uint32_t pin);
    void attachInterrupt(uint32_t pin, IrqCallback callback);
    void detachInterrupt(uint32_t pin);

    /// One DIO1 poll. True when a rising edge was delivered. The polling thread calls this;
    /// tests call it directly.
    bool pollOnce();
    void startPolling();
    void stopPolling();

    bool inError() const { return error.load(); }
    bool asleep() const { return sleeping.load(); }
    uint16_t dio1Mask() const { return dio1.load(); }
    Stats stats() const;

  private:
    bool rawFrame(const uint8_t *out, uint8_t *in, size_t len);
    bool rawWriteOnly(const uint8_t *out, size_t len);
    bool frame(const uint8_t *out, uint8_t *in, size_t len);
    bool splitFrame(const uint8_t *out, uint8_t *in, size_t len, size_t header, bool wideAddress);
    void afterFrame(const uint8_t *out, size_t len);
    bool wakeLocked();
    bool coldRestartLocked();
    bool readIrqLocked(uint16_t &flags);
    bool fail();
    void pollLoop();

    I2cPort &port;
    Clock &clock;
    Config cfg;

    mutable std::recursive_mutex lock; ///< one bridge transaction at a time
    Stats counters;

    std::atomic<bool> error{false};
    std::atomic<bool> sleeping{false};
    std::atomic<uint16_t> dio1{0};
    std::atomic<bool> edgeDelivered{false};
    std::atomic<IrqCallback> callback{nullptr};
    bool resetLow = false;

    std::thread poller;
    std::mutex pollLock;
    std::condition_variable pollWake;
    bool pollStop = true;
};

} // namespace pinedio
} // namespace meshsat
