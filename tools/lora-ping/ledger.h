// SPDX-License-Identifier: GPL-3.0-or-later
// One file that knows every frame this transmitter sent, and refuses the next one when the
// hour is full.
//
// A frame is written down before it is sent, with its settings, and its outcome after. A
// run that is cut off leaves the frame it was sending in the file, counted in full. The
// daemon's transmissions are added from its log by tools/bench/daemon_airtime.py, so that
// the tool and the daemon draw on one budget.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace meshsat
{
namespace pinedio
{
namespace bench
{

class Ledger
{
  public:
    explicit Ledger(const std::string &file) : path(file) {}

    static std::string defaultPath()
    {
        const char *state = std::getenv("XDG_STATE_HOME");
        const char *home = std::getenv("HOME");
        const std::string base = state && *state ? state : std::string(home ? home : ".") + "/.local/state";
        return base + "/meshsat-lora-backplate/airtime.jsonl";
    }

    /// The number after `"key":` in a line of this file, or `missing`.
    static double number(const std::string &line, const char *key, double missing = -1)
    {
        const std::string tag = std::string("\"") + key + "\":";
        const size_t at = line.find(tag);
        if (at == std::string::npos)
            return missing;
        char *end = nullptr;
        const char *from = line.c_str() + at + tag.size();
        const double value = std::strtod(from, &end);
        return end == from ? missing : value;
    }

    /// Seconds on the air in the `window` seconds before `now`. Every frame that was written
    /// down counts, sent or not: a frame of which nothing more is known may have gone out.
    double spent(double now, double window = 3600.0) const
    {
        std::ifstream in(path);
        std::string line;
        double total = 0;
        while (std::getline(in, line)) {
            if (line.find("\"event\":\"attempt\"") == std::string::npos)
                continue;
            const double t = number(line, "t"), ms = number(line, "airtime_ms");
            if (t < 0 || ms < 0)
                continue;
            if (t > now - window && t <= now + 1)
                total += ms / 1000.0;
        }
        return total;
    }

    /// True when a frame of `airtimeMs` still fits into `budget` seconds for the hour.
    bool allows(double now, double airtimeMs, double budget) const { return spent(now) + airtimeMs / 1000.0 <= budget; }

    /// Appends one line and has it written to the disk before returning.
    bool append(const std::string &line) const
    {
        const size_t slash = path.rfind('/');
        if (slash != std::string::npos && slash > 0)
            makeDirs(path.substr(0, slash));
        const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0)
            return false;
        const std::string out = line + "\n";
        const bool ok = ::write(fd, out.data(), out.size()) == (ssize_t)out.size() && ::fsync(fd) == 0;
        ::close(fd);
        return ok;
    }

    const std::string path;

  private:
    static void makeDirs(const std::string &dir)
    {
        for (size_t at = 1; at <= dir.size(); at++)
            if (at == dir.size() || dir[at] == '/')
                ::mkdir(dir.substr(0, at).c_str(), 0755);
    }
};

} // namespace bench
} // namespace pinedio
} // namespace meshsat
