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

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

void usage()
{
    std::puts("mw-d3d12-lab <command> [options]\n"
              "  caps    what every GPU answers (driver, queues, fences, D3D12 Video Encode)\n"
              "Run a command with --help for its options.\n");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    ::SetConsoleOutputCP(CP_UTF8);
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
    usage();
    return command == L"--help" ? 0 : 2;
}
