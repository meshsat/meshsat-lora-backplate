// SPDX-License-Identifier: GPL-3.0-or-later
#include "PineDioBridge.h"

#include <chrono>
#include <cstring>
#include <random>

namespace meshsat
{
namespace pinedio
{

namespace
{
const uint8_t BRIDGE_TRANSMIT = 0x01;

// SX126x opcodes this transport has to understand. Everything else passes through unread.
const uint8_t OP_CLEAR_IRQ = 0x02;
const uint8_t OP_SET_DIO_IRQ = 0x08;
const uint8_t OP_WRITE_REGISTER = 0x0D;
const uint8_t OP_WRITE_BUFFER = 0x0E;
const uint8_t OP_GET_IRQ = 0x12;
const uint8_t OP_READ_REGISTER = 0x1D;
const uint8_t OP_READ_BUFFER = 0x1E;
const uint8_t OP_SET_STANDBY = 0x80;
const uint8_t OP_SET_RX = 0x82;
const uint8_t OP_SET_TX = 0x83;
const uint8_t OP_SET_SLEEP = 0x84;
const uint8_t OP_CALIBRATE = 0x89;
const uint8_t OP_SET_BUFFER_BASE = 0x8F;
const uint8_t OP_CALIBRATE_IMAGE = 0x98;
const uint8_t OP_GET_STATUS = 0xC0;
const uint8_t OP_SET_FS = 0xC1;
const uint8_t OP_SET_CAD = 0xC5;

const size_t BRIDGE_MAX_FRAME = 127;
const uint8_t STATUS_BITS = 0x7E; // chip mode and command status; the top bit is not ours to read
const size_t SYNC_PATTERN = 12;
const size_t BRIDGE_MIN_FRAME = 16; // the sync reads its pattern back in one frame of 3 + 12 bytes
} // namespace

uint64_t SystemClock::nowUs()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

void SystemClock::sleepUs(uint64_t us)
{
    if (us)
        std::this_thread::sleep_for(std::chrono::microseconds(us));
}

Bridge::Bridge(I2cPort &p, Clock &c, const Config &config) : port(p), clock(c), cfg(config)
{
    if (cfg.maxSpiFrame > BRIDGE_MAX_FRAME)
        cfg.maxSpiFrame = BRIDGE_MAX_FRAME;
    if (cfg.maxSpiFrame < BRIDGE_MIN_FRAME)
        cfg.maxSpiFrame = BRIDGE_MIN_FRAME;
}

Bridge::~Bridge()
{
    end();
}

Stats Bridge::stats() const
{
    std::lock_guard<std::recursive_mutex> guard(lock);
    return counters;
}

bool Bridge::fail()
{
    counters.errors++;
    error = true;
    return false;
}

bool Bridge::rawWriteOnly(const uint8_t *out, size_t len)
{
    uint8_t buf[1 + BRIDGE_MAX_FRAME];
    if (len == 0 || len > BRIDGE_MAX_FRAME)
        return false;
    buf[0] = BRIDGE_TRANSMIT;
    std::memcpy(buf + 1, out, len);
    for (int attempt = 0;; attempt++) {
        counters.i2cWrites++;
        if (port.write(buf, len + 1))
            break;
        if (attempt >= cfg.writeRetries)
            return false;
        counters.retries++;
        clock.sleepUs(cfg.retryDelayUs);
    }
    counters.frames++;
    counters.spiBytes += len;
    return true;
}

bool Bridge::rawFrame(const uint8_t *out, uint8_t *in, size_t len)
{
    if (!rawWriteOnly(out, len))
        return false;
    // The reply is read in full even when nobody wants it. The first read is what makes the
    // bridge clock the frame out, and it returns when the frame is over; and the read index
    // only moves when we read, so it has to be kept level with the write index.
    for (size_t i = 0; i < len; i++) {
        uint8_t b = 0;
        counters.i2cReads++;
        if (!port.readByte(b))
            return false; // the read index is now unknown: only begin() can recover
        if (in)
            in[i] = b;
    }
    return true;
}

void Bridge::afterFrame(const uint8_t *out, size_t len)
{
    uint32_t settle = cfg.settleUs;
    switch (out[0]) {
    case OP_SET_SLEEP:
        sleeping = true;
        settle = cfg.settleModeUs; // the radio wants 500 us of quiet before it can be woken
        break;
    case OP_SET_DIO_IRQ:
        if (len >= 5)
            dio1 = (uint16_t)((out[3] << 8) | out[4]);
        break;
    case OP_CLEAR_IRQ:
        // The line would drop here, so the next event is a new edge even if no poll saw it low.
        edgeDelivered = false;
        break;
    case OP_SET_STANDBY:
    case OP_SET_RX:
    case OP_SET_TX:
    case OP_SET_FS:
    case OP_SET_CAD:
        settle = cfg.settleModeUs;
        break;
    case OP_CALIBRATE:
    case OP_CALIBRATE_IMAGE:
        settle = cfg.settleCalUs;
        break;
    default:
        break;
    }
    clock.sleepUs(settle);
}

bool Bridge::frame(const uint8_t *out, uint8_t *in, size_t len)
{
    if (!rawFrame(out, in, len))
        return fail();
    afterFrame(out, len);
    return true;
}

bool Bridge::splitFrame(const uint8_t *out, uint8_t *in, size_t len, size_t header, bool wideAddress)
{
    // header = opcode + address (+ the dummy byte of a read). The payload follows.
    const size_t room = cfg.maxSpiFrame - header;
    uint32_t address = wideAddress ? (uint32_t)((out[1] << 8) | out[2]) : out[1];
    size_t done = 0;
    const size_t payload = len - header;
    uint8_t txBuf[BRIDGE_MAX_FRAME];
    uint8_t rxBuf[BRIDGE_MAX_FRAME];

    counters.split++;
    while (done < payload) {
        const size_t n = (payload - done < room) ? payload - done : room;
        std::memcpy(txBuf, out, header);
        if (wideAddress) {
            const uint16_t a = (uint16_t)(address + done);
            txBuf[1] = (uint8_t)(a >> 8);
            txBuf[2] = (uint8_t)a;
        } else {
            txBuf[1] = (uint8_t)(address + done); // the radio's buffer wraps at 256 as well
        }
        std::memcpy(txBuf + header, out + header + done, n);
        if (!frame(txBuf, rxBuf, header + n))
            return false;
        if (in) {
            if (done == 0)
                std::memcpy(in, rxBuf, header);
            std::memcpy(in + header + done, rxBuf + header, n);
        }
        done += n;
    }
    return true;
}

bool Bridge::wakeLocked()
{
    if (!sleeping)
        return true;
    // Any chip-select edge wakes the radio; what the frame says is lost on it.
    const uint8_t nop[2] = {OP_GET_STATUS, 0x00};
    counters.wakes++;
    if (!rawFrame(nop, nullptr, sizeof(nop)))
        return fail();
    sleeping = false;
    clock.sleepUs(cfg.wakeUs);
    return true;
}

bool Bridge::transfer(const uint8_t *out, uint8_t *in, size_t len)
{
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (in)
        std::memset(in, 0, len);
    if (!out || len == 0)
        return false;
    if (error)
        return false;
    if (!wakeLocked())
        return false;
    baselineValid = false; // whatever the driver just asked for, the status will move
    if (len <= cfg.maxSpiFrame)
        return frame(out, in, len);

    switch (out[0]) {
    case OP_WRITE_BUFFER:
        return splitFrame(out, in, len, 2, false);
    case OP_READ_BUFFER:
        return splitFrame(out, in, len, 3, false);
    case OP_WRITE_REGISTER:
        return splitFrame(out, in, len, 3, true);
    case OP_READ_REGISTER:
        return splitFrame(out, in, len, 4, true);
    default:
        // A long command that cannot be cut in two. The SX126x has none; refuse rather than guess.
        counters.errors++;
        return false;
    }
}

bool Bridge::coldRestartLocked()
{
    // No reset line: a cold-start sleep forgets the configuration like a reset does.
    const uint8_t standby[2] = {OP_SET_STANDBY, 0x00};
    const uint8_t sleep[2] = {OP_SET_SLEEP, 0x00};
    if (error)
        return false;
    if (!wakeLocked())
        return false;
    if (!frame(standby, nullptr, sizeof(standby)))
        return false;
    if (!frame(sleep, nullptr, sizeof(sleep)))
        return false;
    dio1 = 0;
    edgeDelivered = false;
    baselineValid = false;
    return wakeLocked();
}

void Bridge::digitalWrite(uint32_t pin, uint32_t level)
{
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (pin == PinCs) {
        if (!level && !error)
            wakeLocked(); // a driver pulling chip select low to wake the radio
        return;
    }
    if (pin == PinReset) {
        if (!level) {
            resetLow = true;
        } else if (resetLow) {
            resetLow = false;
            coldRestartLocked();
        }
    }
}

bool Bridge::readIrqLocked(uint16_t &flags, uint8_t &status)
{
    const uint8_t cmd[4] = {OP_GET_IRQ, 0x00, 0x00, 0x00};
    uint8_t reply[4] = {0};
    if (!rawFrame(cmd, reply, sizeof(cmd)))
        return fail();
    flags = (uint16_t)((reply[2] << 8) | reply[3]);
    status = reply[1] & STATUS_BITS;
    return true;
}

uint32_t Bridge::digitalRead(uint32_t pin)
{
    if (pin == PinBusy)
        return 0; // the settle delays have already been served
    if (pin != PinIrq)
        return 0;
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (error)
        return 1; // let a driver waiting on the line fall out of its loop and see the failure
    const uint16_t mask = dio1;
    if (mask == 0 || sleeping)
        return 0;
    uint16_t flags = 0;
    uint8_t status = 0;
    counters.polls++;
    // A look at the line is not the poller's look: the baseline stays the poller's own, or an
    // edge seen here first would never be delivered.
    if (!readIrqLocked(flags, status))
        return 1;
    return (flags & mask) ? 1 : 0;
}

void Bridge::attachInterrupt(uint32_t pin, IrqCallback cb)
{
    if (pin == PinIrq)
        callback = cb;
}

void Bridge::detachInterrupt(uint32_t pin)
{
    if (pin == PinIrq)
        callback = nullptr;
}

bool Bridge::pollOnce()
{
    IrqCallback cb = nullptr;
    {
        std::lock_guard<std::recursive_mutex> guard(lock);
        const uint16_t mask = dio1;
        if (error || sleeping || mask == 0)
            return false; // never wake a sleeping radio just to look at it
        if (cfg.statusPoll && baselineValid && clock.nowUs() - lastFullPollUs < cfg.fullPollUs) {
            const uint8_t ask = OP_GET_STATUS;
            uint8_t status = 0;
            counters.quickPolls++;
            if (!rawFrame(&ask, &status, 1))
                return fail();
            if ((status & STATUS_BITS) == baseline)
                return false; // nothing moved since the flags were last fetched
        }
        uint16_t flags = 0;
        uint8_t status = 0;
        counters.polls++;
        if (!readIrqLocked(flags, status))
            return false;
        baseline = status;
        baselineValid = true;
        lastFullPollUs = clock.nowUs();
        if (!(flags & mask)) {
            edgeDelivered = false;
            return false;
        }
        if (edgeDelivered)
            return false;
        edgeDelivered = true;
        counters.irqEdges++;
        cb = callback;
    }
    if (cb)
        cb(); // outside the lock: the handler may talk to the radio
    return true;
}

void Bridge::pollLoop()
{
    std::unique_lock<std::mutex> guard(pollLock);
    while (!pollStop) {
        pollWake.wait_for(guard, std::chrono::microseconds(cfg.pollIntervalUs));
        if (pollStop)
            break;
        guard.unlock();
        if (callback.load())
            pollOnce();
        guard.lock();
    }
}

void Bridge::startPolling()
{
    std::lock_guard<std::mutex> guard(pollLock);
    if (!pollStop)
        return;
    pollStop = false;
    poller = std::thread(&Bridge::pollLoop, this);
}

void Bridge::stopPolling()
{
    {
        std::lock_guard<std::mutex> guard(pollLock);
        if (pollStop)
            return;
        pollStop = true;
    }
    pollWake.notify_all();
    if (poller.joinable())
        poller.join();
}

void Bridge::end()
{
    stopPolling();
}

bool Bridge::begin()
{
    std::lock_guard<std::recursive_mutex> guard(lock);
    error = false;
    sleeping = false;
    dio1 = 0;
    edgeDelivered = false;
    resetLow = false;
    baselineValid = false;

    // The ring buffer's two indices are wherever the last user left them. Write a pattern
    // nobody has written before into the radio's data buffer, read it back through the
    // bridge, and consume reply bytes until that pattern has gone by: from there on the
    // indices are level. A fixed pattern would match its own stale copy from an earlier run.
    uint8_t pattern[SYNC_PATTERN];
    std::random_device entropy;
    std::mt19937 gen(entropy() ^ (uint32_t)clock.nowUs());
    for (size_t i = 0; i < SYNC_PATTERN; i++) {
        // No 0x00 or 0xFF (an idle bus), and no byte twice: with every byte different, a
        // mismatch can only restart the match at the pattern's first byte.
        for (;;) {
            bool fresh = true;
            pattern[i] = (uint8_t)gen();
            if (pattern[i] == 0x00 || pattern[i] == 0xFF)
                continue;
            for (size_t k = 0; k < i; k++)
                fresh = fresh && pattern[k] != pattern[i];
            if (fresh)
                break;
        }
    }

    const uint8_t wake[2] = {OP_GET_STATUS, 0x00};
    const uint8_t standby[2] = {OP_SET_STANDBY, 0x00};
    const uint8_t base[3] = {OP_SET_BUFFER_BASE, 0x00, 0x00};
    uint8_t writeBuf[2 + SYNC_PATTERN] = {OP_WRITE_BUFFER, 0x00};
    uint8_t readBuf[3 + SYNC_PATTERN] = {OP_READ_BUFFER, 0x00, 0x00};
    std::memcpy(writeBuf + 2, pattern, SYNC_PATTERN);

    // The ring is not level yet, so what these reads return is somebody else's reply. They
    // are made all the same: a frame reaches the radio when the read after it begins, and
    // reading as many bytes as were written keeps the unknown distance what it was.
    if (!rawFrame(wake, nullptr, sizeof(wake)))
        return fail();
    clock.sleepUs(cfg.wakeUs);
    if (!rawFrame(standby, nullptr, sizeof(standby)))
        return fail();
    clock.sleepUs(cfg.settleModeUs);
    if (!rawFrame(base, nullptr, sizeof(base)))
        return fail();
    clock.sleepUs(cfg.settleUs);
    if (!rawFrame(writeBuf, nullptr, sizeof(writeBuf)))
        return fail();
    clock.sleepUs(cfg.settleUs);
    if (!rawWriteOnly(readBuf, sizeof(readBuf)))
        return fail();

    size_t matched = 0;
    unsigned reads = 0;
    while (matched < SYNC_PATTERN) {
        uint8_t b = 0;
        if (reads++ >= cfg.syncMaxReads)
            return fail();
        counters.i2cReads++;
        if (!port.readByte(b))
            return fail();
        if (b == pattern[matched])
            matched++;
        else
            matched = (b == pattern[0]) ? 1 : 0;
    }
    counters.syncReads = reads;

    // Level now. The radio must answer a status request with something a radio would say.
    uint8_t reply[2] = {0};
    if (!rawFrame(wake, reply, sizeof(wake)))
        return fail();
    if (reply[1] == 0x00 || reply[1] == 0xFF)
        return fail();
    return true;
}

} // namespace pinedio
} // namespace meshsat
