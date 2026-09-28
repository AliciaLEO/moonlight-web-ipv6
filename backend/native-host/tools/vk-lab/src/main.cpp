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

// mw-vk-lab — measure before building (plan pipeline-video-d3d12-v2, Phase 13).
//
// One subcommand per question the Linux Vulkan chain has to answer before any
// of it enters the engine. See tools/vk-lab/CMakeLists.txt for the why.

#include "Caps.h"
#include "Encode.h"
#include "Import.h"
#ifdef MW_VK_LAB_SHADERS
#include "Queues.h"
#endif
#ifdef MW_VK_LAB_EGL
#include "Egl.h"
#endif

#include <cstdio>
#include <string>

namespace {

void usage()
{
    std::puts("mw-vk-lab <command> [options]\n"
              "  caps    what every Vulkan device answers (Vulkan Video encode, queues and\n"
              "          their priorities, timestamps, DMA-BUF import, KMS planes, sync_file)\n"
              "  encode  Vulkan Video HEVC as the Linux chain would drive it\n"
              "  import  the KMS plane's buffer into Vulkan, checked against EGL's reading\n"
#ifdef MW_VK_LAB_SHADERS
              "  queues  where the conversion waits behind a load, priority by priority\n"
#endif
#ifdef MW_VK_LAB_EGL
              "  egl     the witness: the same conversion in GLES, with a context priority\n"
#endif
              "Run a command with --help for its options.\n");
}

} // namespace

int main(int argc, char** argv)
{
    // A driver that faults mid-probe takes the process down with what stdout
    // still holds: written as it comes, the last line says where it stopped.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string command = argv[1];
    const bool help = argc >= 3 && std::string(argv[2]) == "--help";
    if (command == "caps") {
        if (help) {
            lab::capsUsage();
            return 0;
        }
        return lab::runCaps(argc - 2, argv + 2);
    }
    if (command == "encode") {
        if (help) {
            lab::encodeUsage();
            return 0;
        }
        return lab::runEncode(argc - 2, argv + 2);
    }
    if (command == "import") {
        if (help) {
            lab::importUsage();
            return 0;
        }
        return lab::runImport(argc - 2, argv + 2);
    }
#ifdef MW_VK_LAB_SHADERS
    if (command == "queues") {
        if (help) {
            lab::queuesUsage();
            return 0;
        }
        return lab::runQueues(argc - 2, argv + 2);
    }
#endif
#ifdef MW_VK_LAB_EGL
    if (command == "egl") {
        if (help) {
            lab::eglUsage();
            return 0;
        }
        return lab::runEgl(argc - 2, argv + 2);
    }
#endif
    usage();
    return command == "--help" ? 0 : 2;
}
