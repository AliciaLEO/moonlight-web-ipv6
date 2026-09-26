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

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace lab {

/// A streaming JSON writer, pretty-printed: enough for a report written once,
/// in order, and read by people and by test fixtures alike. No tree, no
/// parser — the lab only ever writes.
class Json
{
public:
    Json& beginObject()
    {
        open('{');
        return *this;
    }
    Json& endObject()
    {
        close('}');
        return *this;
    }
    Json& beginArray()
    {
        open('[');
        return *this;
    }
    Json& endArray()
    {
        close(']');
        return *this;
    }

    Json& key(const std::string& name)
    {
        separate();
        quote(name);
        m_Out += ": ";
        m_AfterKey = true;
        return *this;
    }

    Json& value(const std::string& v)
    {
        separate();
        quote(v);
        return *this;
    }
    Json& value(const char* v) { return value(std::string(v ? v : "")); }
    Json& value(bool v)
    {
        separate();
        m_Out += v ? "true" : "false";
        return *this;
    }
    Json& value(int v) { return number(std::to_string(v)); }
    Json& value(unsigned v) { return number(std::to_string(v)); }
    Json& value(long v) { return number(std::to_string(v)); }
    Json& value(unsigned long v) { return number(std::to_string(v)); }
    Json& value(long long v) { return number(std::to_string(v)); }
    Json& value(unsigned long long v) { return number(std::to_string(v)); }
    Json& value(double v)
    {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.6g", v);
        return number(buf);
    }
    Json& null() { return number("null"); }

    template <typename T> Json& field(const std::string& name, const T& v)
    {
        key(name);
        return value(v);
    }

    const std::string& str() const { return m_Out; }

private:
    Json& number(const std::string& text)
    {
        separate();
        m_Out += text;
        return *this;
    }

    void open(char c)
    {
        separate();
        m_Out += c;
        m_First.push_back(true);
    }

    void close(char c)
    {
        const bool empty = m_First.back();
        m_First.pop_back();
        if (!empty) newline();
        m_Out += c;
    }

    void separate()
    {
        if (m_AfterKey) {
            m_AfterKey = false;
            return;
        }
        if (m_First.empty()) return;
        if (!m_First.back()) m_Out += ',';
        m_First.back() = false;
        newline();
    }

    void newline()
    {
        m_Out += '\n';
        m_Out.append(m_First.size() * 2, ' ');
    }

    void quote(const std::string& s)
    {
        m_Out += '"';
        for (const char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            switch (c) {
            case '"': m_Out += "\\\""; break;
            case '\\': m_Out += "\\\\"; break;
            case '\n': m_Out += "\\n"; break;
            case '\r': m_Out += "\\r"; break;
            case '\t': m_Out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x", c);
                    m_Out += esc;
                } else {
                    m_Out += ch;
                }
            }
        }
        m_Out += '"';
    }

    std::string m_Out;
    std::vector<bool> m_First;
    bool m_AfterKey = false;
};

} // namespace lab
