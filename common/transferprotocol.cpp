/*
 *   This file is part of Checkpoint
 *   Copyright (C) 2017-2026 Bernardo Giordano, FlagBrew
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *   Additional Terms 7.b and 7.c of GPLv3 apply to this file:
 *       * Requiring preservation of specified reasonable legal notices or
 *         author attributions in that material or in the Appropriate Legal
 *         Notices displayed by works containing it.
 *       * Prohibiting misrepresentation of the origin of that material,
 *         or requiring that modified versions of such material be marked in
 *         reasonable ways as different from the original version.
 */

#include "transferprotocol.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#if defined(__aarch64__)
#include <arm_acle.h>
#endif

namespace TransferProto {

#if defined(__aarch64__)
    // Hardware CRC32 on ARMv8. devkitA64 builds with -march=armv8-a+crc, so the
    // checksum is effectively free instead of a bytewise table loop.
    uint32_t updateCrc(uint32_t crc, const uint8_t* data, size_t len)
    {
        uint32_t c = crc;
        while (len >= 8) {
            uint64_t v;
            std::memcpy(&v, data, 8);
            c = __crc32d(c, v);
            data += 8;
            len -= 8;
        }
        while (len > 0) {
            c = __crc32b(c, *data++);
            len--;
        }
        return c;
    }
#else
    namespace {
        uint32_t crcTable[8][256];
        bool crcInit = false;

        void initCrc(void)
        {
            if (crcInit) {
                return;
            }
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int j = 0; j < 8; ++j) {
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                }
                crcTable[0][i] = c;
            }
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = crcTable[0][i];
                for (int t = 1; t < 8; ++t) {
                    c              = crcTable[0][c & 0xFFu] ^ (c >> 8);
                    crcTable[t][i] = c;
                }
            }
            crcInit = true;
        }
    }

    // Slicing-by-8 CRC32: 8 bytes per iteration instead of 1. On the 268MHz
    // ARM11 the bytewise loop is slow enough to rival the SD card, so this
    // keeps the checksum off the transfer's critical path.
    uint32_t updateCrc(uint32_t crc, const uint8_t* data, size_t len)
    {
        initCrc();
        uint32_t c = crc;
        while (len >= 8) {
            uint32_t lo = (uint32_t)(data[0] | (data[1] << 8) | (data[2] << 16) | ((uint32_t)data[3] << 24)) ^ c;
            uint32_t hi = (uint32_t)(data[4] | (data[5] << 8) | (data[6] << 16) | ((uint32_t)data[7] << 24));
            c           = crcTable[7][lo & 0xFFu] ^ crcTable[6][(lo >> 8) & 0xFFu] ^ crcTable[5][(lo >> 16) & 0xFFu] ^ crcTable[4][lo >> 24] ^
                crcTable[3][hi & 0xFFu] ^ crcTable[2][(hi >> 8) & 0xFFu] ^ crcTable[1][(hi >> 16) & 0xFFu] ^ crcTable[0][hi >> 24];
            data += 8;
            len -= 8;
        }
        for (size_t i = 0; i < len; ++i) {
            c = crcTable[0][(c ^ data[i]) & 0xFFu] ^ (c >> 8);
        }
        return c;
    }
#endif

    void appendLe16(std::string& out, uint16_t v)
    {
        out.push_back((char)(v & 0xFF));
        out.push_back((char)((v >> 8) & 0xFF));
    }

    void appendLe32(std::string& out, uint32_t v)
    {
        out.push_back((char)(v & 0xFF));
        out.push_back((char)((v >> 8) & 0xFF));
        out.push_back((char)((v >> 16) & 0xFF));
        out.push_back((char)((v >> 24) & 0xFF));
    }

    bool isSafeZipRelativePath(const std::string& relPath)
    {
        if (relPath.empty()) {
            return false;
        }
        if (relPath.front() == '/' || relPath.front() == '\\') {
            return false;
        }
        if (relPath.find('\\') != std::string::npos) {
            return false;
        }
        if (relPath.find(':') != std::string::npos) {
            return false;
        }

        size_t start = 0;
        while (start <= relPath.size()) {
            size_t pos       = relPath.find('/', start);
            size_t len       = (pos == std::string::npos) ? relPath.size() - start : pos - start;
            std::string part = relPath.substr(start, len);
            if (part == "..") {
                return false;
            }
            if (pos == std::string::npos) {
                break;
            }
            start = pos + 1;
        }

        return true;
    }

    std::string headerValue(const std::string& headers, const std::string& key)
    {
        std::string needle = key + ":";
        size_t pos         = headers.find(needle);
        if (pos == std::string::npos) {
            return "";
        }
        pos += needle.size();
        while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t')) {
            pos++;
        }
        size_t end = headers.find("\r\n", pos);
        if (end == std::string::npos) {
            end = headers.size();
        }
        return headers.substr(pos, end - pos);
    }

    bool constantTimeEquals(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size()) {
            return false;
        }
        unsigned char diff = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            diff |= (unsigned char)(a[i] ^ b[i]);
        }
        return diff == 0;
    }

    std::optional<HostPort> parseTarget(const std::string& ipPort)
    {
        size_t colon = ipPort.find(':');
        if (colon == std::string::npos) {
            return std::nullopt;
        }
        std::string ip = ipPort.substr(0, colon);
        int port       = atoi(ipPort.substr(colon + 1).c_str());
        if (ip.empty() || port <= 0 || port > 65535) {
            return std::nullopt;
        }
        return HostPort{std::move(ip), (uint16_t)port};
    }

    bool validPin(const std::string& pin)
    {
        return pin.size() == 4 && std::all_of(pin.begin(), pin.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    }
}
