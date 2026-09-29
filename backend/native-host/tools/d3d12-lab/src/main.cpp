/*
 * MoonlightWeb — native capture & encoding engine: D3D12 lab.
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

// mw-d3d12-lab — measure before building (plan pipeline-video-d3d12-v2).
//
// One subcommand per question the D3D12 pipeline has to answer before any of
// it enters the engine. See tools/d3d12-lab/CMakeLists.txt for the why.

#include "Caps.h"
#include "Encode.h"
#include "Interop.h"
#include "Queues.h"
#include "Scale.h"
#include "Vendors.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

void usage()
{
    std::puts(
        "mw-d3d12-lab <command> [options]\n"
        "  caps    what every GPU answers (driver, queues, fences, D3D12 Video Encode)\n"
        "  queues  where the conversion waits: D3D11, D3D12 DIRECT and COMPUTE, by priority,\n"
        "          and the driver's Video Process (Intel's SFC)\n"
        "  interop what the DDA handshake costs, and what reading without it does\n"
        "  encode  D3D12 Video Encode HEVC with the product's rate control\n"
        "  vendors NVENC and AMF fed D3D12 pictures\n"
        "  scale   the Lanczos-2 resample, timed, the product's way and three cheaper ones\n"
        "Run a command with --help for its options.\n");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    ::SetConsoleOutputCP(CP_UTF8);
    // A driver that faults mid-probe takes the process down with what stdout
    // still holds (the N95's caps, 27/09): written as it comes, the last line
    // says where it stopped. The CRT has no line buffering, hence none at all.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::wstring command = argv[1];
    const bool help = argc >= 3 && std::wstring(argv[2]) == L"--help";
    if (command == L"caps") {
        if (help) {
            lab::capsUsage();
            return 0;
        }
        return lab::runCaps(argc - 2, argv + 2);
    }
    if (command == L"queues") {
        if (help) {
            lab::queuesUsage();
            return 0;
        }
        return lab::runQueues(argc - 2, argv + 2);
    }
    if (command == L"interop") {
        if (help) {
            lab::interopUsage();
            return 0;
        }
        return lab::runInterop(argc - 2, argv + 2);
    }
    if (command == L"encode") {
        if (help) {
            lab::encodeUsage();
            return 0;
        }
        return lab::runEncode(argc - 2, argv + 2);
    }
    if (command == L"vendors") {
        if (help) {
            lab::vendorsUsage();
            return 0;
        }
        return lab::runVendors(argc - 2, argv + 2);
    }
    if (command == L"scale") {
        if (help) {
            lab::scaleUsage();
            return 0;
        }
        return lab::runScale(argc - 2, argv + 2);
    }
    usage();
    return command == L"--help" ? 0 : 2;
}
