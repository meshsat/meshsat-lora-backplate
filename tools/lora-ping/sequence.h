// SPDX-License-Identifier: GPL-3.0-or-later
// How a frame gets from the host to the air, as a choice that is written down.
//
// Loading a frame through the back cover's bridge takes about a second, and what the radio
// does during that second decides whether its crystal is running when the transmission
// begins. Meshtastic's daemon leaves receive for standby on the RC oscillator, scans the
// channel and loads the frame there: the crystal is stopped and starts with the frame. The
// three ways below differ in that and in nothing else. The mode the radio reports is read
// back before and after the load, so that a run proves which way it took.
#pragma once

#include "PineDioBridge.h"
#include "payload.h"

#include <RadioLib.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace meshsat
{
namespace pinedio
{
namespace bench
{

enum class From {
    Legacy,      ///< as the tool always did: standby RC for a run's first frame, receive for the others
    Receive,     ///< the frame is loaded while the radio receives; the crystal never stops
    StandbyRc,   ///< standby on the RC oscillator, the daemon's way; the crystal starts with the frame
    StandbyXosc, ///< standby with the crystal kept running
};

inline const char *name(From from)
{
    switch (from) {
    case From::Receive:
        return "rx";
    case From::StandbyRc:
        return "standby";
    case From::StandbyXosc:
        return "standby-xosc";
    default:
        return "legacy";
    }
}

inline bool parse(const char *text, From &from)
{
    for (From f : {From::Legacy, From::Receive, From::StandbyRc, From::StandbyXosc})
        if (std::strcmp(text, name(f)) == 0) {
            from = f;
            return true;
        }
    return false;
}

/// The radio's modes as its status byte names them.
inline const char *modeName(int mode)
{
    switch (mode) {
    case 2:
        return "standby RC";
    case 3:
        return "standby XOSC";
    case 4:
        return "synthesizer";
    case 5:
        return "receive";
    case 6:
        return "transmit";
    default:
        return mode < 0 ? "not read" : "unknown";
    }
}

/// Asks the radio for its status and returns the chip mode in it, or -1.
inline int chipMode(Bridge &bridge)
{
    const uint8_t out[2] = {0xC0, 0x00};
    uint8_t in[2] = {0, 0};
    if (!bridge.transfer(out, in, sizeof(out)))
        return -1;
    return (in[1] >> 4) & 7;
}

struct Proof {
    int before = -1;  ///< chip mode when the load began
    int loaded = -1;  ///< chip mode when the load was over: what the transmit command met
    int16_t scan = 0; ///< what the channel scan said, when one was asked for
    double loadMs = 0;
};

/// Puts the radio into the state asked for, loads the frame and starts the transmission.
/// For StandbyXosc the driver must have been begun with `standbyXOSC` set, so that every
/// standby it takes by itself, and the mode it falls back to, keep the crystal running.
inline int16_t send(SX1262 &radio, Bridge &bridge, Clock &clock, From from, bool scan, const uint8_t *data, size_t len,
                    Proof &proof)
{
    int16_t state = RADIOLIB_ERR_NONE;
    if (from == From::StandbyRc || from == From::StandbyXosc) {
        const uint8_t mode = from == From::StandbyXosc ? RADIOLIB_SX126X_STANDBY_XOSC : RADIOLIB_SX126X_STANDBY_RC;
        state = radio.standby(mode);
        if (state != RADIOLIB_ERR_NONE)
            return state;
        if (scan) {
            // A scan leaves the radio in standby RC whatever it was told before.
            proof.scan = radio.scanChannel();
            state = radio.standby(mode);
            if (state != RADIOLIB_ERR_NONE)
                return state;
        }
    }
    proof.before = chipMode(bridge);
    RadioModeConfig_t cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.transmit.data = data;
    cfg.transmit.len = len;
    const uint64_t t0 = clock.nowUs();
    state = radio.stageMode(RADIOLIB_RADIO_MODE_TX, &cfg);
    proof.loadMs = (clock.nowUs() - t0) / 1000.0;
    if (state != RADIOLIB_ERR_NONE)
        return state;
    proof.loaded = chipMode(bridge);
    return radio.launchMode();
}

} // namespace bench
} // namespace pinedio
} // namespace meshsat
