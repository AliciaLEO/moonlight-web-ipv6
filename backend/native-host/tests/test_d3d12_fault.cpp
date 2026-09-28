/*
 * MoonlightWeb — native capture & encoding engine.
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

// MW_D3D12_FAULT as the D3D12 chain reads it: which fault, and where.

#include "core/D3d12Fault.h"
#include "native_test_framework.h"

#include <string>

using namespace mw::native;

void run_d3d12_fault_tests()
{
    SECTION("D3d12Fault — each kind reads back from its name, at 1 when no place is said");
    {
        for (D3d12Fault::Kind kind :
             {D3d12Fault::Kind::Open, D3d12Fault::Kind::Convert, D3d12Fault::Kind::Timeout,
              D3d12Fault::Kind::Removed, D3d12Fault::Kind::Encode}) {
            D3d12Fault f;
            CHECK(parseD3d12Fault(toString(kind), f));
            CHECK(f.kind == kind);
            CHECK_EQ(f.at, uint64_t(1));
            CHECK(static_cast<bool>(f));
        }
        CHECK(!static_cast<bool>(D3d12Fault{}));
    }

    SECTION("D3d12Fault — a place, and the name in any case");
    {
        D3d12Fault f;
        CHECK(parseD3d12Fault("removed@300", f));
        CHECK(f.kind == D3d12Fault::Kind::Removed);
        CHECK_EQ(f.at, uint64_t(300));
        CHECK(parseD3d12Fault("Timeout@120", f));
        CHECK(f.kind == D3d12Fault::Kind::Timeout);
        CHECK_EQ(f.at, uint64_t(120));
        CHECK(parseD3d12Fault("OPEN@2", f));
        CHECK(f.kind == D3d12Fault::Kind::Open);
        CHECK_EQ(f.at, uint64_t(2));
        CHECK_EQ(describe(f), std::string("MW_D3D12_FAULT=open@2"));
    }

    SECTION("D3d12Fault — anything else is refused, and leaves the fault as it was");
    {
        D3d12Fault f;
        CHECK(parseD3d12Fault("encode@7", f));
        for (const char* wrong :
             {"", "none", "hang", "removed@", "removed@0", "removed@-1", "removed@3x", "removed@ 3",
              "removed@1234567890", "@5", "encode@@5", "encode 5"}) {
            CHECK(!parseD3d12Fault(wrong, f));
            CHECK(f.kind == D3d12Fault::Kind::Encode);
            CHECK_EQ(f.at, uint64_t(7));
        }
    }

    SECTION("D3d12Fault — the log says what is armed");
    {
        D3d12Fault f;
        CHECK(parseD3d12Fault("timeout@40", f));
        CHECK(effect(f).find("conversion 40") != std::string::npos);
        CHECK(parseD3d12Fault("open", f));
        CHECK(effect(f).find("chain 1") != std::string::npos);
        CHECK_EQ(effect(D3d12Fault{}), std::string("nothing"));
    }
}
