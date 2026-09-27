// SPDX-License-Identifier: GPL-3.0-or-later
// lora-ping: does the back cover transmit, and does anybody hear it?
//
// Sends a frame in Meshtastic's air format from a node number made up for the occasion, on
// a channel hash nobody uses. With a hop left, a Meshtastic node that hears it cannot read
// it and relays it all the same, once, and the tool listens for that relay. For a verdict on
// a frame that matters, read the receiver's own log with tools/bench/verdict.py.
//
// Every frame is written down before it is sent, in a file that also keeps the airtime of
// the hour; the tool does not send what it cannot write down or what the hour has no room
// for. THIS TOOL TRANSMITS: antenna on, region right.
#include "LinuxI2cPort.h"
#include "PineDioBridgeHal.h"
#include "ledger.h"
#include "sequence.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
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
double epoch()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count() / 1e6;
}
/// True when a process of that name runs. Two users of one bridge would each read the other's
/// replies, and the daemon's frames would go uncounted: the tool does not run beside it.
bool processRunning(const char *name)
{
    DIR *proc = opendir("/proc");
    if (!proc)
        return false;
    bool found = false;
    while (dirent *entry = readdir(proc)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
            continue;
        std::ifstream comm(std::string("/proc/") + entry->d_name + "/comm");
        std::string line;
        if (std::getline(comm, line) && line == name) {
            found = true;
            break;
        }
    }
    closedir(proc);
    return found;
}

std::string plain(const std::string &text)
{
    std::string out;
    for (char c : text)
        out += (c == '"' || c == '\\' || (unsigned char)c < 0x20) ? ' ' : c;
    return out;
}
} // namespace

int main(int argc, char **argv)
{
    std::string device = "/dev/i2c-5";
    int address = 0x28, power = 10, count = 3, waitSeconds = 12, hops = 1, payload = 16;
    double freq = 869.525, bw = 250.0;
    int sf = 11, cr = 5;
    int preamble = 16;
    int settleSeconds = -1;
    bool quietTx = false, scan = false;
    double budget = 300.0;
    bench::From from = bench::From::Legacy;
    std::string ledgerPath = bench::Ledger::defaultPath(), label;
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
        else if (a == "--cr" && more)
            cr = std::atoi(argv[++i]);
        else if (a == "--preamble" && more)
            preamble = std::atoi(argv[++i]);
        else if (a == "--frame" && more)
            cfg.maxSpiFrame = (size_t)std::atoi(argv[++i]);
        else if (a == "--poll-ms" && more)
            cfg.pollIntervalUs = (uint32_t)std::atoi(argv[++i]) * 1000;
        else if (a == "--quiet-tx")
            quietTx = true;
        else if (a == "--from" && more) {
            if (!bench::parse(argv[++i], from)) {
                std::fprintf(stderr, "--from takes legacy, rx, standby or standby-xosc\n");
                return 2;
            }
        } else if (a == "--scan")
            scan = true;
        else if (a == "--settle" && more)
            settleSeconds = std::atoi(argv[++i]);
        else if (a == "--ledger" && more)
            ledgerPath = argv[++i];
        else if (a == "--budget" && more)
            budget = std::atof(argv[++i]);
        else if (a == "--label" && more)
            label = argv[++i];
        else if (a == "-h" || a == "--help") {
            std::printf(
                "usage: lora-ping [/dev/i2c-N] [--power dBm] [--count N] [--wait seconds] [--hops N]\n"
                "                 [--payload bytes] [--freq MHz] [--cr 5..8] [--preamble symbols] [--frame N]\n"
                "                 [--poll-ms N] [--quiet-tx] [--from legacy|rx|standby|standby-xosc] [--scan]\n"
                "                 [--settle seconds] [--ledger file] [--budget seconds] [--label text]\n"
                "Transmits on Meshtastic's EU_868 LongFast by default, 10 dBm, and listens for a relay.\n"
                "--from       what the radio does while the frame is loaded, which takes about a second:\n"
                "             rx            it goes on receiving; the crystal never stops\n"
                "             standby       standby on the RC oscillator, as the daemon does; the crystal\n"
                "                           starts with the frame\n"
                "             standby-xosc  standby with the crystal kept running\n"
                "             legacy        standby for a run's first frame, rx for the others\n"
                "--scan       scan the channel before the load, as the daemon does; not with rx\n"
                "--settle     seconds of receive before a run's first frame; --wait by default\n"
                "--quiet-tx   leave the bridge alone while the frame is on the air\n"
                "--ledger     where every frame is written down; %s\n"
                "--budget     seconds on the air the ledger allows in any hour; 300, the band allows 360\n"
                "--label      a word for this run, kept with its frames\n",
                bench::Ledger::defaultPath().c_str());
            return 0;
        } else
            device = a;
    }
    if (power < -9 || power > 22 || hops < 0 || hops > 7 || payload < 1 || payload > 239 || count < 1 || cr < 5 || cr > 8 ||
        preamble < 8 || preamble > 2000) {
        std::fprintf(stderr, "power -9..22 dBm, hops 0..7, payload 1..239 bytes, coding rate 5..8\n");
        return 2;
    }
    if (scan && (from == bench::From::Receive || from == bench::From::Legacy)) {
        std::fprintf(stderr, "--scan goes with --from standby or standby-xosc: a scan ends the receive\n");
        return 2;
    }
    if (settleSeconds < 0)
        settleSeconds = waitSeconds;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGHUP, onSignal); // a session that ends finishes the frame and leaves the radio in standby
    setvbuf(stdout, nullptr, _IOLBF, 0);

    if (processRunning("meshtasticd")) {
        std::fprintf(stderr, "meshtasticd is running: stop it first, and put its frames into the ledger with\n"
                             "tools/bench/daemon_airtime.py, so that the hour is counted for both\n");
        return 6;
    }
    const bench::Ledger ledger(ledgerPath);
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
    // Before begin(): the driver then also tells the radio to fall back to standby with the crystal.
    radio.standbyXOSC = from == bench::From::StandbyXosc;

    int16_t state = radio.begin(freq, bw, sf, cr, 0x2B, power, (uint16_t)preamble, 0.0, false);
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
    const uint32_t run = (uint32_t)gen();
    stamp();
    std::printf("%.3f MHz, SF%d, BW %.0f kHz, CR 4/%d, preamble %d, %d dBm, as !%08x, %d hop%s, from %s%s, run %08x\n", freq, sf,
                bw, cr, preamble, power, sender, hops, hops == 1 ? "" : "s", bench::name(from), scan ? " after a scan" : "", run);

    int sent = 0, relayed = 0, exitCode = 0;
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
        for (uint8_t b : bench::payloadFor(id, (size_t)payload))
            frame.push_back(b);

        const double airMs = radio.getTimeOnAir(frame.size()) / 1000.0;
        const double spent = ledger.spent(epoch());
        if (spent + airMs / 1000.0 > budget) {
            stamp();
            std::printf("frame %d: NOT SENT, %.0f s were on the air in the last hour and the budget is %.0f s\n", n + 1, spent,
                        budget);
            exitCode = 4;
            break;
        }

        // A run's first frame finds the radio in standby. Give it the receive the others get.
        if (n == 0 && from != bench::From::Legacy && settleSeconds > 0) {
            state = radio.startReceive();
            if (state != RADIOLIB_ERR_NONE) {
                std::printf("startReceive failed: %d\n", state);
                break;
            }
            const uint64_t since = clock.nowUs();
            while (!stopping && clock.nowUs() - since < (uint64_t)settleSeconds * 1000000ULL)
                clock.sleepUs(5000);
            if (stopping)
                break;
        }

        char line[768];
        std::snprintf(line, sizeof(line),
                      "{\"t\":%.3f,\"event\":\"attempt\",\"tool\":\"lora-ping\",\"run\":\"%08x\",\"frame\":%d,\"label\":\"%s\","
                      "\"id\":\"0x%08x\",\"sender\":\"0x%08x\",\"length\":%zu,\"payload\":\"xorshift32 of the id\","
                      "\"airtime_ms\":%.0f,\"preamble\":%d,\"power_dbm\":%d,\"sf\":%d,\"bw_khz\":%.0f,\"cr\":%d,"
                      "\"freq_mhz\":%.3f,\"hops\":%d,\"from\":\"%s\",\"scan\":%s,\"settle_s\":%d,\"wait_s\":%d,\"quiet_tx\":%s,"
                      "\"spent_before_s\":%.1f}",
                      epoch(), run, n + 1, plain(label).c_str(), id, sender, frame.size(), airMs, preamble, power, sf, bw, cr,
                      freq, hops, bench::name(from), scan ? "true" : "false", n == 0 ? settleSeconds : waitSeconds, waitSeconds,
                      quietTx ? "true" : "false", spent);
        if (!ledger.append(line)) {
            stamp();
            std::printf("frame %d: NOT SENT, it cannot be written down in %s\n", n + 1, ledger.path.c_str());
            exitCode = 5;
            break;
        }

        bench::Proof proof;
        if (quietTx)
            bridge.stopPolling(); // not a byte on the bridge while the frame is on the air
        const uint64_t t0 = clock.nowUs();
        if (from == bench::From::Legacy) {
            lineRose = false;
            state = radio.startTransmit(frame.data(), frame.size());
        } else {
            state = bench::send(radio, bridge, clock, from, scan, frame.data(), frame.size(), proof);
        }
        // Whatever rose during the load was a packet received, not the end of this frame.
        lineRose = false;
        const double loadedMs = (clock.nowUs() - t0) / 1000.0;
        if (state != RADIOLIB_ERR_NONE) {
            std::printf("the transmission could not be started: %d\n", state);
            std::snprintf(line, sizeof(line),
                          "{\"t\":%.3f,\"event\":\"outcome\",\"run\":\"%08x\",\"frame\":%d,\"id\":\"0x%08x\",\"tx_done\":false,"
                          "\"error\":%d,\"mode_before\":%d,\"mode_loaded\":%d}",
                          epoch(), run, n + 1, id, state, proof.before, proof.loaded);
            ledger.append(line);
            break;
        }
        uint16_t flags = 0;
        const uint64_t deadline = t0 + (uint64_t)((loadedMs + airMs + 3000) * 1000);
        if (quietTx) {
            clock.sleepUs((uint64_t)((airMs + 150) * 1000));
            flags = (uint16_t)radio.getIrqFlags();
        } else {
            flags = bench::awaitTransmitDone(radio, clock, deadline, &lineRose, &stopping);
        }
        const double doneMs = (clock.nowUs() - t0) / 1000.0;
        const bool done = (flags & RADIOLIB_SX126X_IRQ_TX_DONE) != 0;
        radio.finishTransmit();
        if (quietTx)
            bridge.startPolling();
        const Stats now = bridge.stats();
        std::snprintf(line, sizeof(line),
                      "{\"t\":%.3f,\"event\":\"outcome\",\"run\":\"%08x\",\"frame\":%d,\"id\":\"0x%08x\",\"tx_done\":%s,"
                      "\"irq_flags\":\"0x%04x\",\"mode_before\":%d,\"mode_loaded\":%d,\"scan\":%d,\"load_ms\":%.0f,"
                      "\"handed_over_ms\":%.0f,\"done_ms\":%.0f,\"bridge_retries\":%llu,\"bridge_errors\":%llu}",
                      epoch(), run, n + 1, id, done ? "true" : "false", flags, proof.before, proof.loaded, proof.scan,
                      proof.loadMs, loadedMs, doneMs, (unsigned long long)now.retries, (unsigned long long)now.errors);
        ledger.append(line);
        stamp();
        if (!done) {
            std::printf("frame %d: NOT SENT, no transmit-done from the radio (flags 0x%04x)\n", n + 1, flags);
            continue;
        }
        sent++;
        std::printf("frame %d sent: %zu bytes, id 0x%08x, time on air %.0f ms, handed over in %.0f ms, done after %.0f ms", n + 1,
                    frame.size(), id, airMs, loadedMs, doneMs);
        if (from != bench::From::Legacy)
            std::printf(", loaded in %s (%s before)", bench::modeName(proof.loaded), bench::modeName(proof.before));
        std::printf("\n");

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
            // With a frame to follow, the receive goes on to its end: it is part of the setting.
            if (heard && from == bench::From::Legacy)
                break;
        }
        if (!heard && hops > 0) {
            stamp();
            std::printf("frame %d: no relay heard in %d s\n", n + 1, waitSeconds);
        }
    }

    radio.clearDio1Action();
    radio.standby(RADIOLIB_SX126X_STANDBY_RC);
    hal.term();
    const Stats s = bridge.stats();
    stamp();
    std::printf("%d sent, %d relayed back; bridge: %llu frames, %llu retries, %llu errors; written down in %s\n", sent, relayed,
                (unsigned long long)s.frames, (unsigned long long)s.retries, (unsigned long long)s.errors, ledger.path.c_str());
    if (exitCode)
        return exitCode;
    if (bridge.inError())
        return 1;
    return sent == 0 ? 1 : (relayed || hops == 0 ? 0 : 3);
}
