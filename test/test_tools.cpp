// SPDX-License-Identifier: GPL-3.0-or-later
// The parts of the bench tools that need neither a radio nor a radio library: the file that
// keeps the airtime of the hour, and the payload that follows from a packet id.
#include "check.h"
#include "ledger.h"
#include "payload.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace meshsat::pinedio::bench;

namespace
{
std::string scratch(const char *name)
{
    const char *dir = std::getenv("TMPDIR");
    return std::string(dir && *dir ? dir : "/tmp") + "/pinedio-test-" + std::to_string((long)getpid()) + "-" + name;
}
std::string attempt(double t, double airtimeMs)
{
    char line[160];
    std::snprintf(line, sizeof(line), "{\"t\":%.3f,\"event\":\"attempt\",\"tool\":\"lora-ping\",\"airtime_ms\":%.0f}", t, airtimeMs);
    return line;
}
} // namespace

TEST(the_payload_follows_from_the_packet_id)
{
    const std::vector<uint8_t> want{0x87, 0x15, 0x48, 0x81, 0x70, 0x29, 0x89, 0xc5};
    CHECK(payloadFor(0x12345678u, 8) == want);
    CHECK(payloadFor(0x12345678u, 8) == payloadFor(0x12345678u, 8));
    CHECK(payloadFor(0x12345679u, 8) != want);
    // An id of zero would leave the generator at zero for ever.
    const std::vector<uint8_t> zero{0xa2, 0x97, 0xcd, 0xae};
    CHECK(payloadFor(0, 4) == zero);
    CHECK_EQ(payloadFor(7, 239).size(), 239);
}

TEST(a_ledger_that_is_not_there_has_spent_nothing)
{
    const Ledger ledger(scratch("missing/none.jsonl"));
    CHECK(ledger.spent(1790540000.0) == 0.0);
    CHECK(ledger.allows(1790540000.0, 3000, 300));
}

TEST(only_the_last_hour_counts)
{
    const std::string path = scratch("hour.jsonl");
    std::remove(path.c_str());
    const Ledger ledger(path);
    const double now = 1790540000.0;
    CHECK(ledger.append(attempt(now - 3700, 3000))); // over an hour ago
    CHECK(ledger.append(attempt(now - 3500, 2000)));
    CHECK(ledger.append(attempt(now - 10, 1500)));
    CHECK(ledger.spent(now) > 3.49 && ledger.spent(now) < 3.51);
    std::remove(path.c_str());
}

TEST(a_frame_without_an_outcome_counts_in_full)
{
    // A run that was cut off leaves its last frame written down and nothing after it.
    const std::string path = scratch("cut.jsonl");
    std::remove(path.c_str());
    const Ledger ledger(path);
    const double now = 1790540000.0;
    CHECK(ledger.append(attempt(now - 100, 3000)));
    CHECK(ledger.append("{\"t\":1790539903.000,\"event\":\"outcome\",\"tx_done\":true,\"done_ms\":4000}"));
    CHECK(ledger.append(attempt(now - 50, 3000)));
    CHECK(ledger.spent(now) > 5.99 && ledger.spent(now) < 6.01);
    std::remove(path.c_str());
}

TEST(the_budget_refuses_the_frame_that_would_overrun_it)
{
    const std::string path = scratch("budget.jsonl");
    std::remove(path.c_str());
    const Ledger ledger(path);
    const double now = 1790540000.0;
    for (int i = 0; i < 99; i++)
        CHECK(ledger.append(attempt(now - 3000 + i, 3000)));
    CHECK(ledger.spent(now) > 296.9 && ledger.spent(now) < 297.1);
    CHECK(ledger.allows(now, 3000, 300));
    CHECK(!ledger.allows(now, 3001, 300));
    CHECK(ledger.append(attempt(now, 3000)));
    CHECK(!ledger.allows(now, 1, 300));
    std::remove(path.c_str());
}

TEST(the_daemons_frames_draw_on_the_same_budget)
{
    const std::string path = scratch("shared.jsonl");
    std::remove(path.c_str());
    const Ledger ledger(path);
    const double now = 1790540000.0;
    CHECK(ledger.append("{\"t\":1790539900.000,\"event\":\"attempt\",\"tool\":\"meshtasticd\",\"id\":\"0x03a06b55\",\"airtime_ms\":2722}"));
    CHECK(ledger.append(attempt(now - 5, 1000)));
    CHECK(ledger.spent(now) > 3.72 && ledger.spent(now) < 3.73);
    std::remove(path.c_str());
}

TEST(lines_that_are_not_frames_are_passed_over)
{
    const std::string path = scratch("noise.jsonl");
    std::remove(path.c_str());
    const Ledger ledger(path);
    const double now = 1790540000.0;
    CHECK(ledger.append(""));
    CHECK(ledger.append("not json at all"));
    CHECK(ledger.append("{\"event\":\"attempt\",\"airtime_ms\":5000}")); // no time: cannot be placed
    CHECK(ledger.append(attempt(now - 5, 1000)));
    CHECK(ledger.spent(now) > 0.99 && ledger.spent(now) < 1.01);
    std::remove(path.c_str());
}

int main(int argc, char **argv)
{
    return check::runAll(argc, argv);
}
