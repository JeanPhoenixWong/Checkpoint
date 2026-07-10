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

#ifndef TRANSFERPROTOCOL_HPP
#define TRANSFERPROTOCOL_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Wire-protocol core shared by the 3DS and Switch transfer implementations and
// the chlink PC CLI. Everything here is pure byte/string logic — no filesystem,
// no sockets, no platform types — so a fix to the framing, CRC, path-safety or
// auth rules lands once and applies to both consoles. The per-target
// transfer.cpp files own the file+socket IO and call into these.
namespace TransferProto {
    // Zip central-directory record accumulated while a store-only archive is
    // streamed out; consumed when the central directory is written.
    struct ZipEntry {
        std::string name;
        uint32_t crc;
        uint32_t size;
        uint32_t offset;
        bool isDirectory;
    };

    // CRC32 with the zip/zlib polynomial (0xEDB88320). Hardware intrinsic on
    // ARMv8 (Switch), slicing-by-8 software table elsewhere (3DS ARM11); both
    // produce identical checksums. The software table self-initializes on first
    // use, so callers never have to prime it.
    uint32_t updateCrc(uint32_t crc, const uint8_t* data, size_t len);

    // Append a little-endian integer to a byte buffer (zip fields are LE).
    void appendLe16(std::string& out, uint16_t v);
    void appendLe32(std::string& out, uint32_t v);

    // Exact byte size of the store-only zip that sendZipStream emits, used as the
    // streamed HTTP Content-Length. Store-only entries make this deterministic.
    // Templated over the per-target file-entry type (needs `.relPath` and
    // `.size`) so the platform's absolute-path member can be whatever it needs.
    template <class FileEntryT>
    uint32_t zipStreamSize(const std::vector<FileEntryT>& files, const std::vector<std::string>& dirs)
    {
        uint32_t total = 22; // end of central directory
        for (const auto& dir : dirs) {
            total += 30 + dir.size(); // local header
            total += 46 + dir.size(); // central directory
        }
        for (const auto& entry : files) {
            total += 30 + entry.relPath.size() + entry.size + 16; // local header + data + data descriptor
            total += 46 + entry.relPath.size();                   // central directory
        }
        return total;
    }

    // Rejects a zip entry name that is absolute, contains a backslash or colon,
    // or traverses out of the destination via a ".." component. Applied to every
    // received name before it touches the filesystem.
    bool isSafeZipRelativePath(const std::string& relPath);

    // Extracts an HTTP header value by key from a raw header block ("" if absent).
    std::string headerValue(const std::string& headers, const std::string& key);

    // Timing-safe compare so a wrong PIN/token can't be narrowed by measuring how
    // far the comparison ran before failing.
    bool constantTimeEquals(const std::string& a, const std::string& b);

    // Parses "ip:port"; nullopt on a missing colon, empty ip, or a port outside
    // [1, 65535].
    struct HostPort {
        std::string ip;
        uint16_t port;
    };
    std::optional<HostPort> parseTarget(const std::string& ipPort);

    // True iff `pin` is exactly 4 ASCII digits (the receiver token format).
    bool validPin(const std::string& pin);
}

#endif
