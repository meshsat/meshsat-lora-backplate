// SPDX-License-Identifier: GPL-3.0-or-later
// The bridge on a Linux i2c-dev device, for example /dev/i2c-5 on a PinePhone Pro.
#pragma once

#include "PineDioBridge.h"

#include <string>

namespace meshsat
{
namespace pinedio
{

class LinuxI2cPort : public I2cPort
{
  public:
    LinuxI2cPort() = default;
    ~LinuxI2cPort() override;
    LinuxI2cPort(const LinuxI2cPort &) = delete;
    LinuxI2cPort &operator=(const LinuxI2cPort &) = delete;

    /// Opens the bus and selects the bridge. False with lastError() set when it cannot.
    bool open(const std::string &device, int address = 0x28);
    void close();
    bool isOpen() const { return fd >= 0; }
    const std::string &lastError() const { return message; }

    bool write(const uint8_t *data, size_t len) override;
    bool readByte(uint8_t &out) override;

  private:
    int fd = -1;
    std::string message;
};

} // namespace pinedio
} // namespace meshsat
