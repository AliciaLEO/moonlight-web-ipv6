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

#pragma once

// What every subcommand of the lab needs: the machine it runs on, and the few
// Linux facts a measurement means nothing without (kernel, the capabilities
// the process holds — CAP_SYS_NICE decides the queue priorities, CAP_SYS_ADMIN
// the KMS handles — and the environment variables that pick a Vulkan driver).

#include <cstdint>
#include <initializer_list>
#include <string>

namespace lab {

/// printf to stdout, which main() leaves unbuffered: a driver that faults
/// mid-probe takes the process down, and the last line then says where.
void say(const char* format, ...) __attribute__((format(printf, 1, 2)));

/// "0x174d" — a flag word.
std::string hex(unsigned long long value);

struct FlagName
{
    unsigned long long bit;
    const char* name;
};

/// "a|b|0x40": the named bits of @p flags, then whatever is left unnamed when
/// @p rest is asked for; "none" for no bit at all.
std::string decode(unsigned long long flags, std::initializer_list<FlagName> names,
                   bool rest = true);

std::string hostName();
/// `uname -r`.
std::string kernelRelease();
/// PRETTY_NAME of /etc/os-release, or "".
std::string osName();
/// "bruno (uid 1000)".
std::string userText();
/// "2026-09-28 07:31:12" and "20260928-073112".
std::string nowText();
std::string nowStamp();
/// Microseconds on CLOCK_MONOTONIC.
int64_t nowUs();

/// Whether capability @p cap (CAP_SYS_NICE = 23, CAP_SYS_ADMIN = 21) is in
/// the process's effective set, read from /proc/self/status.
bool capEffective(int cap);
/// The same, in the permitted set: what the process could raise.
bool capPermitted(int cap);
constexpr int kCapSysAdmin = 21;
constexpr int kCapSysNice = 23;

/// The value of @p name in the environment, or "".
std::string env(const char* name);

} // namespace lab
