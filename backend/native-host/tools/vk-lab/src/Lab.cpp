/*
 * MoonlightWeb — native capture & encoding engine: Vulkan lab.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "Lab.h"

#include <pwd.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>

namespace lab {

void say(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
}

std::string hex(unsigned long long value)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llx", value);
    return buf;
}

std::string decode(unsigned long long flags, std::initializer_list<FlagName> names, bool rest)
{
    std::string out;
    unsigned long long left = flags;
    for (const FlagName& n : names) {
        if (!(flags & n.bit)) continue;
        if (!out.empty()) out += '|';
        out += n.name;
        left &= ~n.bit;
    }
    if (rest && left) {
        if (!out.empty()) out += '|';
        out += hex(left);
    }
    return out.empty() ? "none" : out;
}

std::string hostName()
{
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) != 0) return "?";
    return buf;
}

std::string kernelRelease()
{
    struct utsname u = {};
    if (::uname(&u) != 0) return "?";
    return u.release;
}

std::string osName()
{
    std::ifstream in("/etc/os-release");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("PRETTY_NAME=", 0) != 0) continue;
        std::string value = line.substr(12);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        return value;
    }
    return "";
}

std::string userText()
{
    const uid_t uid = ::getuid();
    const struct passwd* pw = ::getpwuid(uid);
    return std::string(pw ? pw->pw_name : "?") + " (uid " + std::to_string(uid) + ")";
}

namespace {

std::string timeText(const char* format)
{
    const std::time_t now = std::time(nullptr);
    struct tm local = {};
    ::localtime_r(&now, &local);
    char buf[64];
    std::strftime(buf, sizeof(buf), format, &local);
    return buf;
}

/// The hexadecimal mask after @p field ("CapEff:") in /proc/self/status.
unsigned long long capMask(const char* field)
{
    std::ifstream in("/proc/self/status");
    std::string line;
    const size_t length = std::strlen(field);
    while (std::getline(in, line)) {
        if (line.compare(0, length, field) != 0) continue;
        return std::strtoull(line.c_str() + length, nullptr, 16);
    }
    return 0;
}

} // namespace

std::string nowText()
{
    return timeText("%Y-%m-%d %H:%M:%S");
}

std::string nowStamp()
{
    return timeText("%Y%m%d-%H%M%S");
}

int64_t nowUs()
{
    struct timespec ts = {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

void sleepUntilUs(int64_t deadlineUs)
{
    struct timespec ts = {};
    ts.tv_sec = static_cast<time_t>(deadlineUs / 1000000);
    ts.tv_nsec = static_cast<long>((deadlineUs % 1000000) * 1000);
    while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
}

Stats stats(std::vector<double> values)
{
    Stats s;
    if (values.empty()) return s;
    std::sort(values.begin(), values.end());
    double sum = 0;
    for (double x : values)
        sum += x;
    const size_t n = values.size();
    s.mean = sum / static_cast<double>(n);
    s.p50 = values[n / 2];
    s.p99 = values[std::min(n - 1, static_cast<size_t>(static_cast<double>(n) * 0.99))];
    s.max = values.back();
    return s;
}

bool matchesDevice(const std::string& spec, uint32_t index, const char* name)
{
    if (spec.empty()) return true;
    if (spec.find_first_not_of("0123456789") == std::string::npos) return std::stoul(spec) == index;
    std::string lowerName = name, lowerSpec = spec;
    for (char& c : lowerName)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : lowerSpec)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lowerName.find(lowerSpec) != std::string::npos;
}

bool capEffective(int cap)
{
    return (capMask("CapEff:") >> cap) & 1;
}

bool capPermitted(int cap)
{
    return (capMask("CapPrm:") >> cap) & 1;
}

std::string env(const char* name)
{
    const char* value = std::getenv(name);
    return value ? value : "";
}

} // namespace lab
