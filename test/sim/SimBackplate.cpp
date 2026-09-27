// SPDX-License-Identifier: GPL-3.0-or-later
#include "SimBackplate.h"

#include <cstring>
#include <random>

namespace meshsat
{
namespace pinedio
{
namespace sim
{

Sx1262::Sx1262(SimClock &c) : clock(c)
{
    coldStart();
}

void Sx1262::coldStart()
{
    static const char version[16] = "SX1261 V2D 2D02";
    regs.clear();
    for (unsigned i = 0; i < 16; i++)
        regs[(uint16_t)(0x0320 + i)] = (uint8_t)version[i];
    regs[0x0740] = 0x14; // LoRa sync word, the private-network default
    regs[0x0741] = 0x24;
    regs[0x08AC] = 0x94; // receiver gain
    regs[0x08E7] = 0x38; // over-current protection
    regs[0x0911] = 0x05;
    regs[0x0912] = 0x05;
    regs[0x0736] = 0x0D;
    fallback = 0x20;
    enter(ModeStandbyRc);
    cmdStatus = 1;
    irq = irqMask = dio1Mask = 0;
    txBase = rxBase = rxLen = rxStart = 0;
    packetType = 0;
    rfFreq = 0;
    deviceErrors = 0;
    rxContinuous = false;
    txDoneAt = cadDoneAt = 0;
}

void Sx1262::warmStart()
{
    // What the radio keeps in its retention memory: the settings made by commands, and the
    // LoRa registers while LoRa is the packet type. The receiver gain is not among them.
    const uint8_t sync0 = reg(0x0740), sync1 = reg(0x0741);
    const uint8_t type = packetType;
    const uint32_t freq = rfFreq;
    const uint16_t mask = irqMask, line = dio1Mask;
    const uint8_t tx = txBase, rx = rxBase, len = payloadLen;
    const uint8_t after = fallback;
    coldStart();
    fallback = after;
    packetType = type;
    rfFreq = freq;
    irqMask = mask;
    dio1Mask = line;
    txBase = tx;
    rxBase = rx;
    payloadLen = len;
    if (type == 1) {
        regs[0x0740] = sync0;
        regs[0x0741] = sync1;
    }
}

uint8_t Sx1262::reg(uint16_t address) const
{
    auto it = regs.find(address);
    return it == regs.end() ? 0 : it->second;
}

void Sx1262::enter(Mode next)
{
    // The crystal runs wherever the synthesizer may be needed, and in standby when asked to.
    const bool runs = next == ModeStandbyXosc || next == ModeFs || next == ModeRx || next == ModeTx || next == ModeCad;
    if (runs && !crystalOn)
        crystalSince = clock.nowUs();
    if (!runs && crystalOn)
        stops++;
    crystalOn = runs;
    chipMode = next;
}

Mode Sx1262::fallbackMode() const
{
    // SetRxTxFallbackMode, from the datasheet and not yet seen on the bench: where the radio
    // goes after a packet sent or received. After a channel scan it is always standby RC.
    return fallback == 0x40 ? ModeFs : (fallback == 0x30 ? ModeStandbyXosc : ModeStandbyRc);
}

uint8_t Sx1262::status() const
{
    return (uint8_t)((chipMode << 4) | (cmdStatus << 1));
}

void Sx1262::busyFor(uint64_t us)
{
    busyUntil = clock.nowUs() + us;
}

void Sx1262::tick()
{
    const uint64_t now = clock.nowUs();
    if (txDoneAt && now >= txDoneAt) {
        txDoneAt = 0;
        enter(fallbackMode());
        cmdStatus = 6;
        raise(IrqTxDone);
    }
    if (cadDoneAt && now >= cadDoneAt) {
        cadDoneAt = 0;
        enter(ModeStandbyRc);
        raise((uint16_t)(IrqCadDone | (channelBusy ? IrqCadDetected : 0)));
    }
}

bool Sx1262::injectRx(const std::vector<uint8_t> &payload, int rssiDbm, int snrDb)
{
    tick();
    if (chipMode != ModeRx || payload.empty() || payload.size() > 255)
        return false;
    for (size_t i = 0; i < payload.size(); i++)
        buffer[(uint8_t)(rxBase + i)] = payload[i];
    rxStart = rxBase;
    rxLen = (uint8_t)payload.size();
    pktRssi = (uint8_t)(-rssiDbm * 2);
    pktSnr = (uint8_t)(snrDb * 4);
    cmdStatus = 2;
    raise((uint16_t)(IrqPreamble | IrqHeaderValid | IrqRxDone));
    if (!rxContinuous)
        enter(fallbackMode());
    return true;
}

std::vector<uint8_t> Sx1262::spiFrame(const std::vector<uint8_t> &mosi, uint64_t durationUs)
{
    std::vector<uint8_t> miso(mosi.size(), 0x00);
    if (mosi.empty())
        return miso;
    frameCount++;
    if (mosi.size() > longest)
        longest = (unsigned)mosi.size();

    if (chipMode == ModeSleep) {
        // Chip select woke it. The command that did so is lost.
        ignored++;
        if (sleptWarm)
            warmStart();
        else
            coldStart();
        clock.advance(durationUs);
        busyFor(3500);
        return miso;
    }
    if (clock.nowUs() < busyUntil) {
        violations++; // BUSY was high: a real radio may drop or mangle this
        clock.advance(durationUs);
        return miso;
    }
    tick();

    const uint8_t op = mosi[0];
    const size_t n = mosi.size();
    auto arg = [&](size_t i) -> uint8_t { return i < n ? mosi[i] : 0; };
    seen.push_back(op);
    for (auto &b : miso)
        b = status();
    miso[0] |= 0x80; // the byte that goes out under the opcode has its top bit set
    uint64_t busy = 60;

    switch (op) {
    case 0x80: // SetStandby
        enter(arg(1) ? ModeStandbyXosc : ModeStandbyRc);
        txDoneAt = cadDoneAt = 0;
        busy = 150;
        break;
    case 0x84: // SetSleep
        enter(ModeSleep);
        sleptWarm = (arg(1) & 0x04) != 0;
        busy = 500;
        break;
    case 0xC1: // SetFs
        enter(ModeFs);
        busy = 150;
        break;
    case 0x82: { // SetRx
        const uint32_t timeout = (uint32_t)((arg(1) << 16) | (arg(2) << 8) | arg(3));
        rxContinuous = timeout == 0xFFFFFF;
        enter(ModeRx);
        busy = 150;
        break;
    }
    case 0x83: { // SetTx
        std::vector<uint8_t> frame;
        for (unsigned i = 0; i < payloadLen; i++)
            frame.push_back(buffer[(uint8_t)(txBase + i)]);
        sent.push_back(frame);
        entries.push_back({chipMode, crystalOn ? clock.nowUs() - crystalSince : 0});
        enter(ModeTx);
        txDoneAt = clock.nowUs() + 20000 + 1500ULL * payloadLen;
        busy = 150;
        break;
    }
    case 0xC5: // SetCad
        enter(ModeCad);
        cadDoneAt = clock.nowUs() + 30000;
        busy = 150;
        break;
    case 0x89: // Calibrate
    case 0x98: // CalibrateImage
        busy = 3500;
        break;
    case 0x8F: // SetBufferBaseAddress
        txBase = arg(1);
        rxBase = arg(2);
        break;
    case 0x0E: // WriteBuffer
        for (size_t i = 2; i < n; i++)
            buffer[(uint8_t)(arg(1) + (i - 2))] = mosi[i];
        break;
    case 0x1E: // ReadBuffer
        for (size_t i = 3; i < n; i++)
            miso[i] = buffer[(uint8_t)(arg(1) + (i - 3))];
        break;
    case 0x0D: { // WriteRegister
        const uint16_t a = (uint16_t)((arg(1) << 8) | arg(2));
        for (size_t i = 3; i < n; i++)
            regs[(uint16_t)(a + (i - 3))] = mosi[i];
        break;
    }
    case 0x1D: { // ReadRegister
        const uint16_t a = (uint16_t)((arg(1) << 8) | arg(2));
        for (size_t i = 4; i < n; i++)
            miso[i] = reg((uint16_t)(a + (i - 4)));
        break;
    }
    case 0x08: // SetDioIrqParams
        irqMask = (uint16_t)((arg(1) << 8) | arg(2));
        dio1Mask = (uint16_t)((arg(3) << 8) | arg(4));
        break;
    case 0x12: // GetIrqStatus
        if (n > 2)
            miso[2] = (uint8_t)(irq >> 8);
        if (n > 3)
            miso[3] = (uint8_t)irq;
        break;
    case 0x02: // ClearIrqStatus
        irq &= (uint16_t) ~((arg(1) << 8) | arg(2));
        break;
    case 0x8A: // SetPacketType
        packetType = arg(1);
        break;
    case 0x93: // SetRxTxFallbackMode
        fallback = arg(1);
        break;
    case 0x11: // GetPacketType
        if (n > 2)
            miso[2] = packetType;
        break;
    case 0x86: // SetRfFrequency
        rfFreq = ((uint32_t)arg(1) << 24) | ((uint32_t)arg(2) << 16) | ((uint32_t)arg(3) << 8) | arg(4);
        break;
    case 0x8C: // SetPacketParams (LoRa: preamble 2, header 1, payload length 1, crc 1, iq 1)
        payloadLen = arg(4);
        break;
    case 0x13: // GetRxBufferStatus
        if (n > 2)
            miso[2] = rxLen;
        if (n > 3)
            miso[3] = rxStart;
        break;
    case 0x14: // GetPacketStatus
        if (n > 2)
            miso[2] = pktRssi;
        if (n > 3)
            miso[3] = pktSnr;
        if (n > 4)
            miso[4] = pktRssi;
        break;
    case 0x15: // GetRssiInst
        if (n > 2)
            miso[2] = 200; // -100 dBm
        break;
    case 0x17: // GetDeviceErrors
        if (n > 2)
            miso[2] = (uint8_t)(deviceErrors >> 8);
        if (n > 3)
            miso[3] = (uint8_t)deviceErrors;
        break;
    case 0x07: // ClearDeviceErrors
        deviceErrors = 0;
        break;
    case 0xC0: // GetStatus
    case 0x8B: // SetModulationParams
    case 0x8E: // SetTxParams
    case 0x95: // SetPaConfig
    case 0x96: // SetRegulatorMode
    case 0x9D: // SetDIO2AsRfSwitchCtrl
    case 0x97: // SetDIO3AsTcxoCtrl
    case 0x88: // SetCadParams
    case 0x9F: // StopTimerOnPreamble
    case 0xA0: // SetLoRaSymbNumTimeout
    case 0x10: // GetStats
    case 0x00: // ResetStats
        break;
    default:
        cmdStatus = 4; // not a command this radio knows
        break;
    }
    clock.advance(durationUs);
    busyFor(busy);
    return miso;
}

Backplate::Backplate(SimClock &c, unsigned seed, uint32_t perByte, uint32_t perSpi)
    : radio(c), clock(c), usPerByte(perByte), usPerSpi(perSpi)
{
    std::mt19937 gen(seed);
    for (auto &b : ring)
        b = (uint8_t)gen();
    writeIndex = (uint8_t)(gen() % 128);
    readIndex = (uint8_t)(gen() % 128);
}

void Backplate::plantStale(const std::vector<uint8_t> &bytes, unsigned ahead)
{
    for (size_t i = 0; i < bytes.size(); i++)
        ring[(readIndex + ahead + i) % 128] = bytes[i];
}

void Backplate::flush()
{
    // tiny-i2c-spi learns that a write is over when the next transaction starts, and clocks
    // the frame out then, holding that transaction up until it is done.
    if (pending.empty())
        return;
    const std::vector<uint8_t> mosi = pending;
    pending.clear();
    const std::vector<uint8_t> miso = radio.spiFrame(mosi, (uint64_t)usPerSpi * mosi.size());
    for (uint8_t b : miso) {
        ring[writeIndex] = b;
        writeIndex = (uint8_t)((writeIndex + 1) % 128);
    }
}

bool Backplate::write(const uint8_t *data, size_t len)
{
    writeCount++;
    flush();
    clock.advance(100 + (uint64_t)usPerByte * (len + 1));
    if (writeFaults) {
        writeFaults--;
        return false; // address not acknowledged: nothing reached the radio
    }
    if (len > longestWr)
        longestWr = (unsigned)len;
    if (len > 128) {
        tooLong++; // the ATtiny's receive buffer would have overrun
        return false;
    }
    if (len < 2 || data[0] != 0x01)
        return true; // configure, or nothing: no SPI traffic
    pending.assign(data + 1, data + len);
    return true;
}

bool Backplate::readByte(uint8_t &out)
{
    readCount++;
    flush();
    clock.advance(100 + 2ULL * usPerByte);
    if (readFaults) {
        readFaults--;
        return false;
    }
    out = ring[readIndex];
    readIndex = (uint8_t)((readIndex + 1) % 128);
    return true;
}

} // namespace sim
} // namespace pinedio
} // namespace meshsat
