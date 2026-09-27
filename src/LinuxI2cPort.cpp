// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(__linux__)
#include "LinuxI2cPort.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace meshsat
{
namespace pinedio
{

LinuxI2cPort::~LinuxI2cPort()
{
    close();
}

bool LinuxI2cPort::open(const std::string &device, int address)
{
    close();
    fd = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        message = device + ": " + std::strerror(errno);
        return false;
    }
    if (ioctl(fd, I2C_SLAVE, address) < 0) {
        message = device + ": cannot select the bridge: " + std::strerror(errno);
        close();
        return false;
    }
    message.clear();
    return true;
}

void LinuxI2cPort::close()
{
    if (fd >= 0)
        ::close(fd);
    fd = -1;
}

bool LinuxI2cPort::write(const uint8_t *data, size_t len)
{
    if (fd < 0)
        return false;
    const ssize_t n = ::write(fd, data, len);
    if (n == (ssize_t)len)
        return true;
    message = std::string("i2c write: ") + (n < 0 ? std::strerror(errno) : "short write");
    return false;
}

bool LinuxI2cPort::readByte(uint8_t &out)
{
    if (fd < 0)
        return false;
    const ssize_t n = ::read(fd, &out, 1);
    if (n == 1)
        return true;
    message = std::string("i2c read: ") + (n < 0 ? std::strerror(errno) : "no data");
    return false;
}

} // namespace pinedio
} // namespace meshsat
#endif
