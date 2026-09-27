// SPDX-License-Identifier: GPL-3.0-or-later
// RadioLib's hardware abstraction over the back cover bridge. Give the radio module the pin
// numbers Bridge::PinCs, PinIrq, PinBusy and PinReset: none of them is a real line.
#pragma once

#include "PineDioBridge.h"

#include <RadioLib.h>

namespace meshsat
{
namespace pinedio
{

class PineDioBridgeHal : public RadioLibHal
{
  public:
    /// `polling` false leaves the DIO1 stand-in to the caller, who then calls Bridge::pollOnce().
    PineDioBridgeHal(Bridge &b, Clock &c, bool polling = true) : RadioLibHal(0, 1, 0, 1, 1, 2), bridge(b), clock(c), poll(polling)
    {
    }

    /// RadioLib calls init() when a module begins: sync the bridge and start the DIO1 stand-in.
    void init() override
    {
        if (!started) {
            started = bridge.begin();
            if (started && poll)
                bridge.startPolling();
        }
    }
    void term() override
    {
        bridge.end();
        started = false;
    }
    bool ready() const { return started && !bridge.inError(); }

    void pinMode(uint32_t, uint32_t) override {}
    void digitalWrite(uint32_t pin, uint32_t value) override
    {
        if (pin != RADIOLIB_NC)
            bridge.digitalWrite(pin, value);
    }
    uint32_t digitalRead(uint32_t pin) override { return pin == RADIOLIB_NC ? 0 : bridge.digitalRead(pin); }
    void attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t) override
    {
        if (interruptNum != RADIOLIB_NC)
            bridge.attachInterrupt(interruptNum, interruptCb);
    }
    void detachInterrupt(uint32_t interruptNum) override
    {
        if (interruptNum != RADIOLIB_NC)
            bridge.detachInterrupt(interruptNum);
    }

    void delay(RadioLibTime_t ms) override { clock.sleepUs((uint64_t)ms * 1000); }
    void delayMicroseconds(RadioLibTime_t us) override { clock.sleepUs(us); }
    void yield() override { clock.sleepUs(0); }
    RadioLibTime_t millis() override { return (RadioLibTime_t)(clock.nowUs() / 1000); }
    RadioLibTime_t micros() override { return (RadioLibTime_t)clock.nowUs(); }
    long pulseIn(uint32_t, uint32_t, RadioLibTime_t) override { return 0; }

    void spiBegin() override {}
    void spiBeginTransaction() override {}
    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override { bridge.transfer(out, in, len); }
    void spiEndTransaction() override {}
    void spiEnd() override {}

  private:
    Bridge &bridge;
    Clock &clock;
    bool poll;
    bool started = false;
};

} // namespace pinedio
} // namespace meshsat
