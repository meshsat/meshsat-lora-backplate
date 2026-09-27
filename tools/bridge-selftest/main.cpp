// SPDX-License-Identifier: GPL-3.0-or-later
// bridge-selftest: the back cover on the bench, without a radio library and without a
// transmission. It syncs the bridge, reads the radio's identity, carries buffers of every
// awkward length both ways, finds the largest bridge transaction this unit takes, exercises
// sleep, wake and the reset stand-in, and times a poll. The radio is left in standby.
#include "LinuxI2cPort.h"
#include "PineDioBridge.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace meshsat::pinedio;

namespace
{
int failed = 0;

void report(bool ok, const char *what, const std::string &detail = "")
{
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  ", detail.c_str());
    if (!ok)
        failed++;
}

std::string hex(const uint8_t *data, size_t n)
{
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; i++) {
        std::snprintf(b, sizeof(b), "%02x ", data[i]);
        s += b;
    }
    return s;
}

bool roundTrip(Bridge &bridge, size_t n, uint8_t salt)
{
    std::vector<uint8_t> wr(2 + n), rd(3 + n, 0), in(3 + n, 0);
    wr[0] = 0x0E;
    wr[1] = 0;
    for (size_t i = 0; i < n; i++)
        wr[2 + i] = (uint8_t)(salt + i * 11);
    rd[0] = 0x1E;
    if (!bridge.transfer(wr.data(), nullptr, wr.size()))
        return false;
    if (!bridge.transfer(rd.data(), in.data(), rd.size()))
        return false;
    return std::memcmp(in.data() + 3, wr.data() + 2, n) == 0;
}

bool readRegisters(Bridge &bridge, uint16_t address, uint8_t *dest, size_t n)
{
    std::vector<uint8_t> out(4 + n, 0), in(4 + n, 0);
    out[0] = 0x1D;
    out[1] = (uint8_t)(address >> 8);
    out[2] = (uint8_t)address;
    if (!bridge.transfer(out.data(), in.data(), out.size()))
        return false;
    std::memcpy(dest, in.data() + 4, n);
    return true;
}
} // namespace

int main(int argc, char **argv)
{
    std::string device = "/dev/i2c-5";
    int address = 0x28;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--addr") && i + 1 < argc)
            address = (int)std::strtol(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
            std::printf("usage: bridge-selftest [/dev/i2c-N] [--addr 0x28]\nNothing is transmitted.\n");
            return 0;
        } else
            device = argv[i];
    }

    LinuxI2cPort port;
    SystemClock clock;
    if (!port.open(device, address)) {
        std::printf("FAIL  open  %s\n", port.lastError().c_str());
        return 2;
    }
    std::printf("back cover bridge on %s at 0x%02x\n", device.c_str(), address);

    Bridge bridge(port, clock);
    uint64_t t0 = clock.nowUs();
    bool ok = bridge.begin();
    char line[160];
    std::snprintf(line, sizeof(line), "%u reads, %.1f ms", bridge.stats().syncReads, (clock.nowUs() - t0) / 1000.0);
    report(ok, "ring buffer sync", ok ? line : port.lastError());
    if (!ok)
        return 1;

    uint8_t out[2] = {0xC0, 0}, in[2] = {0};
    ok = bridge.transfer(out, in, 2);
    std::snprintf(line, sizeof(line), "status 0x%02x (mode %u, command status %u)", in[1], (in[1] >> 4) & 7, (in[1] >> 1) & 7);
    report(ok && ((in[1] >> 4) & 7) == 2, "radio is in standby", line);

    uint8_t version[16] = {0};
    ok = readRegisters(bridge, 0x0320, version, sizeof(version));
    version[15] = 0;
    report(ok && !std::memcmp(version, "SX126", 5), "radio identity", std::string("\"") + (const char *)version + "\"");

    uint8_t sync[2] = {0};
    ok = readRegisters(bridge, 0x0740, sync, 2);
    report(ok, "LoRa sync word registers", hex(sync, 2));

    for (size_t n : {1, 2, 16, 61, 62, 63, 64, 100, 128, 129, 200, 255}) {
        t0 = clock.nowUs();
        ok = roundTrip(bridge, n, (uint8_t)n);
        std::snprintf(line, sizeof(line), "%3zu bytes, %.1f ms", n, (clock.nowUs() - t0) / 1000.0);
        report(ok, "buffer write and read back", line);
        if (bridge.inError())
            break;
    }

    // The largest bridge transaction this ATtiny takes, judged by a full-length frame.
    size_t best = 0;
    for (size_t frame : {16, 32, 64, 96, 112, 120, 127}) {
        Config cfg;
        cfg.maxSpiFrame = frame;
        Bridge probe(port, clock, cfg);
        t0 = clock.nowUs();
        ok = probe.begin() && roundTrip(probe, 255, (uint8_t)frame) && roundTrip(probe, 255, (uint8_t)(frame + 1));
        std::snprintf(line, sizeof(line), "frame %3zu: 2 x 255 bytes in %.1f ms", frame, (clock.nowUs() - t0) / 1000.0);
        std::printf("%s  %s\n", ok ? "ok  " : "no  ", line);
        if (ok)
            best = frame;
        else
            clock.sleepUs(50000);
    }
    std::snprintf(line, sizeof(line), "%zu SPI bytes", best);
    report(best >= 64, "largest working bridge transaction", line);

    ok = bridge.begin();
    report(ok, "sync again after the probes");

    const uint8_t setSync[5] = {0x0D, 0x07, 0x40, 0x24, 0xB4};
    ok = bridge.transfer(setSync, nullptr, sizeof(setSync)) && readRegisters(bridge, 0x0740, sync, 2);
    report(ok && sync[0] == 0x24 && sync[1] == 0xB4, "sync word written", hex(sync, 2));

    const uint8_t sleep[2] = {0x84, 0x04};
    ok = bridge.transfer(sleep, nullptr, sizeof(sleep)) && bridge.asleep();
    ok = ok && readRegisters(bridge, 0x0740, sync, 2) && !bridge.asleep();
    report(ok && sync[0] == 0x24 && sync[1] == 0xB4, "warm sleep, wake, configuration kept", hex(sync, 2));

    bridge.digitalWrite(Bridge::PinReset, 0);
    bridge.digitalWrite(Bridge::PinReset, 1);
    ok = readRegisters(bridge, 0x0740, sync, 2);
    report(ok && sync[0] == 0x14 && sync[1] == 0x24, "reset stand-in, configuration forgotten", hex(sync, 2));

    ok = bridge.transfer(out, in, 2);
    report(ok && ((in[1] >> 4) & 7) == 2, "radio left in standby");

    const uint8_t dio[9] = {0x08, 0x02, 0x02, 0x00, 0x02, 0, 0, 0, 0};
    ok = bridge.transfer(dio, nullptr, sizeof(dio));
    const int polls = 200;
    t0 = clock.nowUs();
    for (int i = 0; i < polls && ok; i++)
        bridge.pollOnce();
    const double perPoll = (clock.nowUs() - t0) / 1000.0 / polls;
    std::snprintf(line, sizeof(line), "%.2f ms each", perPoll);
    report(ok && !bridge.inError(), "interrupt status poll", line);
    const uint8_t noDio[9] = {0x08, 0, 0, 0, 0, 0, 0, 0, 0};
    bridge.transfer(noDio, nullptr, sizeof(noDio));

    const Stats s = bridge.stats();
    std::printf("frames %llu, SPI bytes %llu, I2C writes %llu, I2C reads %llu, retries %llu, errors %llu\n",
                (unsigned long long)s.frames, (unsigned long long)s.spiBytes, (unsigned long long)s.i2cWrites,
                (unsigned long long)s.i2cReads, (unsigned long long)s.retries, (unsigned long long)s.errors);
    std::printf("%s\n", failed ? "SELFTEST FAILED" : "SELFTEST PASSED");
    return failed ? 1 : 0;
}
