// SPDX-License-Identifier: GPL-3.0-or-later
// lora-ping: does the back cover transmit, and does anybody hear it?
//
// Sends a short frame in Meshtastic's air format from a node number made up for the
// occasion, on a channel hash nobody uses, with one hop left. A Meshtastic node that hears
// it cannot read it and relays it all the same, once. The tool then listens for that relay:
// the same sender and packet id, another relay byte, no hop left. Hearing it proves the
// transmission without a second instrument. THIS TOOL TRANSMITS: antenna on, region right.
#include "LinuxI2cPort.h"
#include "PineDioBridgeHal.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <vector>

using namespace meshsat::pinedio;

namespace
{
std::atomic<bool> lineRose{false};
std::atomic<bool> stopping{false};

void onDio1()
{
    lineRose = true;
}
void onSignal(int)
{
    stopping = true;
}

void put32(std::vector<uint8_t> &v, uint32_t x)
{
    for (int i = 0; i < 4; i++)
        v.push_back((uint8_t)(x >> (8 * i)));
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
    int address = 0x28, power = 10, count = 3, waitSeconds = 12, hops = 1, payload = 16;
    double freq = 869.525, bw = 250.0;
    int sf = 11, cr = 5;
    Config cfg;

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--addr" && more)
            address = (int)std::strtol(argv[++i], nullptr, 0);
        else if (a == "--power" && more)
            power = std::atoi(argv[++i]);
        else if (a == "--count" && more)
            count = std::atoi(argv[++i]);
        else if (a == "--wait" && more)
            waitSeconds = std::atoi(argv[++i]);
        else if (a == "--hops" && more)
            hops = std::atoi(argv[++i]);
        else if (a == "--payload" && more)
            payload = std::atoi(argv[++i]);
        else if (a == "--freq" && more)
            freq = std::atof(argv[++i]);
        else if (a == "-h" || a == "--help") {
            std::printf("usage: lora-ping [/dev/i2c-N] [--power dBm] [--count N] [--wait seconds] [--hops N]\n"
                        "                 [--payload bytes] [--freq MHz]\n"
                        "Transmits on Meshtastic's EU_868 LongFast by default, 10 dBm, and listens for a relay.\n");
            return 0;
        } else
            device = a;
    }
    if (power < -9 || power > 22 || hops < 0 || hops > 7 || payload < 1 || payload > 239 || count < 1) {
        std::fprintf(stderr, "power -9..22 dBm, hops 0..7, payload 1..239 bytes\n");
        return 2;
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

    int16_t state = radio.begin(freq, bw, sf, cr, 0x2B, power, 16, 0.0, false);
    if (state != RADIOLIB_ERR_NONE) {
        std::fprintf(stderr, "radio.begin failed: %d (%s)\n", state, port.lastError().c_str());
        return 1;
    }
    radio.setDio2AsRfSwitch(true);
    radio.setCRC(2);
    radio.setCurrentLimit(140.0);
    radio.setRxBoostedGainMode(true);
    radio.setDio1Action(onDio1);

    std::mt19937 gen(std::random_device{}());
    const uint32_t sender = 0x4d530000u | (gen() & 0xffff); // "MS" and sixteen bits of chance
    stamp();
    std::printf("%.3f MHz, SF%d, BW %.0f kHz, %d dBm, as !%08x, %d hop%s\n", freq, sf, bw, power, sender, hops,
                hops == 1 ? "" : "s");

    int sent = 0, relayed = 0;
    for (int n = 0; n < count && !stopping && !bridge.inError(); n++) {
        std::vector<uint8_t> frame;
        const uint32_t id = (uint32_t)gen();
        put32(frame, 0xffffffffu);
        put32(frame, sender);
        put32(frame, id);
        frame.push_back((uint8_t)((hops & 7) | ((hops & 7) << 5)));
        frame.push_back(0x5a); // a channel nobody has
        frame.push_back(0x00);
        frame.push_back((uint8_t)sender);
        for (int i = 0; i < payload; i++)
            frame.push_back((uint8_t)gen());

        const double airMs = radio.getTimeOnAir(frame.size()) / 1000.0;
        lineRose = false;
        const uint64_t t0 = clock.nowUs();
        state = radio.startTransmit(frame.data(), frame.size());
        const double loadedMs = (clock.nowUs() - t0) / 1000.0;
        if (state != RADIOLIB_ERR_NONE) {
            std::printf("startTransmit failed: %d\n", state);
            break;
        }
        while (!lineRose && !stopping && clock.nowUs() - t0 < (uint64_t)((airMs + 3000) * 1000))
            clock.sleepUs(2000);
        const double doneMs = (clock.nowUs() - t0) / 1000.0;
        const uint16_t flags = (uint16_t)radio.getIrqFlags();
        radio.finishTransmit();
        stamp();
        if (!lineRose || !(flags & RADIOLIB_SX126X_IRQ_TX_DONE)) {
            std::printf("frame %d: NOT SENT, no transmit-done from the radio (flags 0x%04x)\n", n + 1, flags);
            continue;
        }
        sent++;
        std::printf("frame %d sent: %zu bytes, id 0x%08x, time on air %.0f ms, handed over in %.0f ms, done after %.0f ms\n",
                    n + 1, frame.size(), id, airMs, loadedMs, doneMs);

        lineRose = false;
        state = radio.startReceive();
        if (state != RADIOLIB_ERR_NONE) {
            std::printf("startReceive failed: %d\n", state);
            break;
        }
        const uint64_t listenFrom = clock.nowUs();
        bool heard = false;
        while (!stopping && !bridge.inError() && clock.nowUs() - listenFrom < (uint64_t)waitSeconds * 1000000ULL) {
            if (!lineRose.exchange(false)) {
                clock.sleepUs(5000);
                continue;
            }
            const size_t len = radio.getPacketLength();
            std::vector<uint8_t> data(len ? len : 1);
            state = radio.readData(data.data(), len);
            const double afterMs = (clock.nowUs() - listenFrom) / 1000.0;
            stamp();
            if (state == RADIOLIB_ERR_NONE && len >= 16) {
                const bool ours = le32(&data[4]) == sender && le32(&data[8]) == id;
                std::printf("%s %zu bytes from !%08x id 0x%08x hop limit %u relay 0x%02x  RSSI %.1f dBm  SNR %.2f dB  %.0f ms "
                            "after ours\n",
                            ours ? "RELAY OF OURS:" : "other traffic:", len, le32(&data[4]), le32(&data[8]), data[12] & 7,
                            data[15], radio.getRSSI(), radio.getSNR(), afterMs);
                if (ours && !heard) {
                    heard = true;
                    relayed++;
                }
            } else {
                std::printf("a frame that did not read cleanly: %d, %zu bytes\n", state, len);
            }
            radio.startReceive();
            if (heard)
                break;
        }
        if (!heard) {
            stamp();
            std::printf("frame %d: no relay heard in %d s\n", n + 1, waitSeconds);
        }
    }

    radio.clearDio1Action();
    radio.standby();
    hal.term();
    const Stats s = bridge.stats();
    stamp();
    std::printf("%d sent, %d relayed back; bridge: %llu frames, %llu retries, %llu errors\n", sent, relayed,
                (unsigned long long)s.frames, (unsigned long long)s.retries, (unsigned long long)s.errors);
    if (bridge.inError())
        return 1;
    return sent == 0 ? 1 : (relayed ? 0 : 3);
}
