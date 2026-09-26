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

#pragma once

// What every subcommand of the lab needs: the machine it runs on, the GPUs it
// sees, and the few Windows facts a measurement means nothing without (driver
// version, hardware scheduling, the privilege a GLOBAL_REALTIME queue asks for).

#include <windows.h>

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace lab {

using Microsoft::WRL::ComPtr;

/// "0x887A0005" — an HRESULT as Windows prints it.
std::string hr(HRESULT value);
/// "0x174d" — a flag word.
std::string hex(unsigned long long value);
std::string utf8(const wchar_t* text);
std::wstring wide(const std::string& text);

/// "10.0.26200.6584": major.minor.build.UBR.
std::string osBuild();
/// The version resource of a file, "a.b.c.d", or "" when it has none.
std::string fileVersion(const std::wstring& path);
/// The version of the D3D12 runtime this process loads (System32\D3D12Core.dll).
std::string d3d12CoreVersion();
std::string computerName();
/// "2026-09-26 14:03:12" and "20260926-140312".
std::string nowText();
std::string nowStamp();

/// "SYSTEM", "elevated" or "limited" — what a GLOBAL_REALTIME queue and the
/// REALTIME scheduling class care about.
std::string tokenKind();
/// SeIncreaseBasePriorityPrivilege switched on for this process. Held is not
/// enabled: an elevated token carries it off.
bool enableBasePriorityPrivilege();

/// Microseconds on the performance counter.
int64_t nowUs();

struct Adapter
{
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc = {};
    UINT index = 0;
    std::string name;
    /// The same GPU listed again under another index — the virtual display
    /// drivers (IddCx) do that. Kept so the report can say so.
    std::vector<UINT> duplicates;
};

/// The machine's GPUs, one entry per LUID, in DXGI order. WARP only when asked.
std::vector<Adapter> adapters(bool includeSoftware);

/// The adapter @p spec names: its DXGI index ("2"), or a piece of its name,
/// case-insensitive ("RTX", "arc"); empty = the first hardware one. Null when
/// nothing matches. Indexes move from one boot to the next; names do not.
const Adapter* pickAdapter(const std::vector<Adapter>& all, const std::string& spec);

/// The user-mode driver version DXGI reports, "a.b.c.d", or "" when refused.
std::string umdVersion(IDXGIAdapter* adapter);
/// Hardware-accelerated GPU scheduling for this GPU: "on", "off",
/// "not supported" or "unknown (<status>)".
std::string hagsState(const LUID& luid);
/// "0x0000000000012419" and the "high,low" decimal pair Chrome's
/// --use-adapter-luid wants.
std::string luidHex(const LUID& luid);
std::string luidChrome(const LUID& luid);

} // namespace lab
