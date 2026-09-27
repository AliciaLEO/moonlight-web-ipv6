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

#include "Lab.h"

#include "platform/windows/StreamPriority.h"

#include <cctype>
#include <cstdlib>

#include <cstdio>
#include <ctime>

namespace lab {

std::string hr(HRESULT value)
{
    char out[16];
    std::snprintf(out, sizeof(out), "0x%08lX", static_cast<unsigned long>(value));
    return out;
}

std::string hex(unsigned long long value)
{
    char out[24];
    std::snprintf(out, sizeof(out), "0x%llx", value);
    return out;
}

std::string utf8(const wchar_t* text)
{
    if (!text || !*text) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring wide(const std::string& text)
{
    if (text.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), n);
    return out;
}

std::string osBuild()
{
    // RtlGetVersion tells the truth where GetVersionEx is shimmed by the
    // manifest; ntdll is always loaded.
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW info = {};
    info.dwOSVersionInfoSize = sizeof(info);
    const auto get = reinterpret_cast<RtlGetVersionFn>(
        ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!get || get(&info) != 0) return "unknown";
    DWORD ubr = 0;
    DWORD size = sizeof(ubr);
    ::RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                   RRF_RT_REG_DWORD, nullptr, &ubr, &size);
    char out[64];
    std::snprintf(out, sizeof(out), "%lu.%lu.%lu.%lu", info.dwMajorVersion, info.dwMinorVersion,
                  info.dwBuildNumber, ubr);
    return out;
}

std::string fileVersion(const std::wstring& path)
{
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return {};
    std::vector<BYTE> data(size);
    if (!::GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT len = 0;
    if (!::VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &len) || !fixed)
        return {};
    char out[64];
    std::snprintf(out, sizeof(out), "%u.%u.%u.%u", HIWORD(fixed->dwFileVersionMS),
                  LOWORD(fixed->dwFileVersionMS), HIWORD(fixed->dwFileVersionLS),
                  LOWORD(fixed->dwFileVersionLS));
    return out;
}

std::string d3d12CoreVersion()
{
    wchar_t dir[MAX_PATH] = {};
    const UINT n = ::GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return fileVersion(std::wstring(dir) + L"\\D3D12Core.dll");
}

std::string computerName()
{
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (!::GetComputerNameW(name, &size)) return "unknown";
    return utf8(name);
}

namespace {

std::tm localNow()
{
    const std::time_t now = std::time(nullptr);
    std::tm local = {};
    localtime_s(&local, &now);
    return local;
}

} // namespace

std::string nowText()
{
    const std::tm t = localNow();
    char out[32];
    std::strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", &t);
    return out;
}

std::string nowStamp()
{
    const std::tm t = localNow();
    char out[32];
    std::strftime(out, sizeof(out), "%Y%m%d-%H%M%S", &t);
    return out;
}

std::string tokenKind()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return "unknown";
    std::string kind = "limited";
    BYTE user[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER)] = {};
    DWORD len = 0;
    if (::GetTokenInformation(token, TokenUser, user, sizeof(user), &len) &&
        ::IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(user)->User.Sid, WinLocalSystemSid)) {
        kind = "SYSTEM";
    } else {
        TOKEN_ELEVATION elevation = {};
        if (::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &len) &&
            elevation.TokenIsElevated)
            kind = "elevated";
    }
    ::CloseHandle(token);
    return kind;
}

bool enableBasePriorityPrivilege()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool ok = ::LookupPrivilegeValueW(nullptr, L"SeIncreaseBasePriorityPrivilege",
                                            &tp.Privileges[0].Luid) &&
                    ::AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr) &&
                    ::GetLastError() != ERROR_NOT_ALL_ASSIGNED;
    ::CloseHandle(token);
    return ok;
}

GpuScheduling takeGpuClass(mw::native::StreamPriority& priority, const std::string& option)
{
    using mw::native::StreamPriority;
    if (option == "high") ::SetEnvironmentVariableA("MW_GPU_PRIORITY", "high");
    if (option == "normal") ::SetEnvironmentVariableA("MW_GPU_PRIORITY", "normal");
    priority.engage();
    GpuScheduling out;
    out.token = tokenKind();
    out.privilege = enableBasePriorityPrivilege();
    out.gpuClass = StreamPriority::toString(StreamPriority::grantedClass());
    return out;
}

bool isGpuClassOption(const std::string& option)
{
    return option == "auto" || option == "high" || option == "normal";
}

int64_t nowUs()
{
    static const int64_t frequency = [] {
        LARGE_INTEGER f = {};
        ::QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c = {};
    ::QueryPerformanceCounter(&c);
    return static_cast<int64_t>(static_cast<double>(c.QuadPart) * 1e6 /
                                static_cast<double>(frequency));
}

std::vector<Adapter> adapters(bool includeSoftware)
{
    std::vector<Adapter> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
         ++i, adapter.Reset()) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && !includeSoftware) continue;
        bool seen = false;
        for (Adapter& known : out) {
            if (known.desc.AdapterLuid.LowPart == desc.AdapterLuid.LowPart &&
                known.desc.AdapterLuid.HighPart == desc.AdapterLuid.HighPart) {
                known.duplicates.push_back(i);
                seen = true;
                break;
            }
        }
        if (seen) continue;
        Adapter entry;
        entry.adapter = adapter;
        entry.desc = desc;
        entry.index = i;
        entry.name = utf8(desc.Description);
        out.push_back(std::move(entry));
    }
    return out;
}

const Adapter* pickAdapter(const std::vector<Adapter>& all, const std::string& spec)
{
    const bool index = !spec.empty() && spec.find_first_not_of("0123456789") == std::string::npos;
    std::string wanted = spec;
    for (char& ch : wanted)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (const Adapter& a : all) {
        if (spec.empty()) {
            if (!(a.desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return &a;
            continue;
        }
        if (index) {
            if (static_cast<int>(a.index) == std::atoi(spec.c_str())) return &a;
            continue;
        }
        std::string name = a.name;
        for (char& ch : name)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (name.find(wanted) != std::string::npos) return &a;
    }
    return nullptr;
}

std::string umdVersion(IDXGIAdapter* adapter)
{
    LARGE_INTEGER version = {};
    if (!adapter || FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
        return {};
    char out[48];
    std::snprintf(out, sizeof(out), "%u.%u.%u.%u", HIWORD(version.HighPart),
                  LOWORD(version.HighPart), HIWORD(version.LowPart), LOWORD(version.LowPart));
    return out;
}

namespace {

// d3dkmthk.h is a WDK header; the entry points live in gdi32 and are looked up,
// with their structures restated as the header lays them out — the same way
// the engine's StreamPriority reads the scheduling state.
struct KmtOpenAdapterFromLuid
{
    LUID luid;
    UINT adapter;
};
struct KmtQueryAdapterInfo
{
    UINT adapter;
    int type;
    void* data;
    UINT size;
};
struct KmtCloseAdapter
{
    UINT adapter;
};
constexpr int kKmtQaiWddm27Caps = 70; // KMTQAITYPE_WDDM_2_7_CAPS
using OpenAdapterFromLuidFn = LONG(APIENTRY*)(KmtOpenAdapterFromLuid*);
using QueryAdapterInfoFn = LONG(APIENTRY*)(const KmtQueryAdapterInfo*);
using CloseAdapterFn = LONG(APIENTRY*)(const KmtCloseAdapter*);

} // namespace

std::string hagsState(const LUID& luid)
{
    HMODULE gdi = ::GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = ::LoadLibraryW(L"gdi32.dll");
    if (!gdi) return "unknown (no gdi32)";
    const auto open =
        reinterpret_cast<OpenAdapterFromLuidFn>(::GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
    const auto query =
        reinterpret_cast<QueryAdapterInfoFn>(::GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
    const auto close =
        reinterpret_cast<CloseAdapterFn>(::GetProcAddress(gdi, "D3DKMTCloseAdapter"));
    if (!open || !query || !close) return "unknown (no D3DKMT)";
    KmtOpenAdapterFromLuid opened = {luid, 0};
    const LONG openStatus = open(&opened);
    if (openStatus != 0) return "unknown (open " + hr(openStatus) + ")";
    UINT caps = 0;
    const KmtQueryAdapterInfo info = {opened.adapter, kKmtQaiWddm27Caps, &caps, sizeof(caps)};
    const LONG status = query(&info);
    const KmtCloseAdapter closing = {opened.adapter};
    close(&closing);
    if (status != 0) return "unknown (" + hr(status) + ")";
    if (!(caps & 0x1)) return "not supported";
    return (caps & 0x2) ? "on" : "off";
}

std::string luidHex(const LUID& luid)
{
    char out[24];
    std::snprintf(out, sizeof(out), "0x%08lx%08lx", static_cast<unsigned long>(luid.HighPart),
                  static_cast<unsigned long>(luid.LowPart));
    return out;
}

std::string luidChrome(const LUID& luid)
{
    char out[32];
    std::snprintf(out, sizeof(out), "%ld,%lu", static_cast<long>(luid.HighPart),
                  static_cast<unsigned long>(luid.LowPart));
    return out;
}

} // namespace lab
