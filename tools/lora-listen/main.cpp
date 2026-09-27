// SPDX-License-Identifier: GPL-3.0-or-later
// lora-listen: RadioLib's SX1262 driver over the back cover bridge, receiving only.
// By default it listens where Meshtastic's EU_868 LongFast lives and prints every frame with
// its signal report and, when it looks like one, its Meshtastic header. It never transmits.
#include "LinuxI2cPort.h"
#include "PineDioBridgeHal.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using namespace meshsat::pinedio;

namespace
{
std::atomic<bool> frameReady{false};
std::atomic<bool> stopping{false};

void onDio1()
{
    frameReady = true;
}
void onSignal(int)
{
    stopping = true;
}

uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void stamp()
{
    char buf[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&now));
    std::printf("%s  ", buf);
}
} // namespace

int main(int argc, char **argv)
{
    std::string device = "/dev/i2c-5";
    int address = 0x28, seconds = 0, count = 0, sf = 11, cr = 5, syncWord = 0x2B, preamble = 16;
    double freq = 869.525, bw = 250.0;
    Config cfg;

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--addr" && more)
            address = (int)std::strtol(argv[++i], nullptr, 0);
        else if (a == "--seconds" && more)
            seconds = std::atoi(argv[++i]);
        else if (a == "--count" && more)
            count = std::atoi(argv[++i]);
        else if (a == "--freq" && more)
            freq = std::atof(argv[++i]);
        else if (a == "--bw" && more)
            bw = std::atof(argv[++i]);
        else if (a == "--sf" && more)
            sf = std::atoi(argv[++i]);
        else if (a == "--cr" && more)
            cr = std::atoi(argv[++i]);
        else if (a == "--sync" && more)
            syncWord = (int)std::strtol(argv[++i], nullptr, 0);
        else if (a == "--preamble" && more)
            preamble = std::atoi(argv[++i]);
        else if (a == "--frame" && more)
            cfg.maxSpiFrame = (size_t)std::atoi(argv[++i]);
        else if (a == "--poll-ms" && more)
            cfg.pollIntervalUs = (uint32_t)std::atoi(argv[++i]) * 1000;
        else if (a == "-h" || a == "--help") {
            std::printf("usage: lora-listen [/dev/i2c-N] [--seconds N] [--count N] [--freq MHz] [--bw kHz]\n"
                        "                   [--sf N] [--cr N] [--sync 0x2B] [--preamble N] [--frame N] [--poll-ms N]\n"
                        "Default: Meshtastic EU_868 LongFast. Receives only.\n");
            return 0;
        } else
            device = a;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    LinuxI2cPort port;
    SystemClock clock;
    if (!port.open(device, address)) {
        std::fprintf(stderr, "%s\n", port.lastError().c_str());
        return 2;
    }
    Bridge bridge(port, clock, cfg);
    PineDioBridgeHal hal(bridge, clock);
    Module module(&hal, Bridge::PinCs, Bridge::PinIrq, Bridge::PinReset, Bridge::PinBusy);
    SX1262 radio(&module);

    // Output power is a register here and nothing more: no transmit command is ever sent.
    int16_t state = radio.begin(freq, bw, sf, cr, syncWord, 0, preamble, 0.0, false);
    if (state != RADIOLIB_ERR_NONE) {
        std::fprintf(stderr, "radio.begin failed: %d (%s)\n", state, port.lastError().c_str());
        return 1;
    }
    radio.setDio2AsRfSwitch(true);
    radio.setCRC(2);
    radio.setRxBoostedGainMode(true);
    radio.setDio1Action(onDio1);
    state = radio.startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        std::fprintf(stderr, "startReceive failed: %d\n", state);
        return 1;
    }
    stamp();
    std::printf("listening on %.3f MHz, SF%d, BW %.0f kHz, CR 4/%d, sync 0x%02X, bridge sync after %u reads\n", freq, sf, bw,
                cr, syncWord, bridge.stats().syncReads);

    const uint64_t started = clock.nowUs();
    int frames = 0;
    while (!stopping) {
        if (bridge.inError()) {
            std::fprintf(stderr, "bridge error: %s\n", port.lastError().c_str());
            break;
        }
        if (seconds && clock.nowUs() - started > (uint64_t)seconds * 1000000ULL)
            break;
        if (!frameReady.exchange(false)) {
            clock.sleepUs(5000);
            continue;
        }
        const uint64_t t0 = clock.nowUs();
        const size_t len = radio.getPacketLength();
        std::vector<uint8_t> data(len ? len : 1);
        state = radio.readData(data.data(), len);
        const double readMs = (clock.nowUs() - t0) / 1000.0;
        stamp();
        if (state == RADIOLIB_ERR_NONE || state == RADIOLIB_ERR_CRC_MISMATCH) {
            std::printf("%3zu bytes  RSSI %.1f dBm  SNR %.2f dB  read in %.1f ms%s\n", len, radio.getRSSI(), radio.getSNR(),
                        readMs, state == RADIOLIB_ERR_CRC_MISMATCH ? "  CRC MISMATCH" : "");
            if (len >= 16 && state == RADIOLIB_ERR_NONE) {
                const uint8_t flags = data[12];
                std::printf("          to !%08x  from !%08x  id 0x%08x  hop limit %u  hop start %u%s%s  channel 0x%02x  "
                            "relay 0x%02x  payload %zu\n",
                            le32(&data[0]), le32(&data[4]), le32(&data[8]), flags & 7, (flags >> 5) & 7,
                            (flags & 0x08) ? "  want-ack" : "", (flags & 0x10) ? "  via-mqtt" : "", data[13], data[15], len - 16);
            }
            std::printf("          ");
            for (size_t i = 0; i < len; i++)
                std::printf("%02x%s", data[i], (i % 32 == 31 && i + 1 < len) ? "\n          " : " ");
            std::printf("\n");
            frames++;
        } else {
            std::printf("read failed: %d\n", state);
        }
        if (count && frames >= count)
            break;
        state = radio.startReceive();
        if (state != RADIOLIB_ERR_NONE) {
            std::fprintf(stderr, "startReceive failed: %d\n", state);
            break;
        }
    }

    radio.clearDio1Action();
    radio.standby();
    hal.term();
    const Stats s = bridge.stats();
    stamp();
    std::printf("%d frames, %llu polls, %llu bridge frames, %llu retries, %llu errors\n", frames, (unsigned long long)s.polls,
                (unsigned long long)s.frames, (unsigned long long)s.retries, (unsigned long long)s.errors);
    return bridge.inError() ? 1 : 0;
}
