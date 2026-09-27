// SPDX-License-Identifier: GPL-3.0-or-later
// The payload of a test frame follows from its packet id, so that whoever holds the bytes a
// receiver got can tell whether they are the bytes that were sent. tools/bench/verdict.py
// has the same function.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace meshsat
{
namespace pinedio
{
namespace bench
{

/// xorshift32 seeded with the packet id, the top byte of every step.
inline std::vector<uint8_t> payloadFor(uint32_t packetId, size_t length)
{
    std::vector<uint8_t> bytes;
    uint32_t x = packetId ? packetId : 0x6d657368u;
    for (size_t i = 0; i < length; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        bytes.push_back((uint8_t)(x >> 24));
    }
    return bytes;
}

} // namespace bench
} // namespace pinedio
} // namespace meshsat
