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

#include "transfer.hpp"
#include "common.hpp"
#include "directory.hpp"
#include "io.hpp"
#include "json.hpp"
#include "logging.hpp"
#include "server.hpp"
#include "titlecatalog.hpp"
#include "transferstatus.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
    constexpr int TRANSFER_PORT        = 8000;
    const char* CHECKPOINT_ROOT        = "sdmc:/switch/Checkpoint/";
    const char* SAVES_ROOT             = "sdmc:/switch/Checkpoint/saves/";
    const char* TEMP_UPLOAD            = "sdmc:/switch/Checkpoint/transfer_upload.tmp";
    const std::string TEMP_SEND_PREFIX = "transfer_send_";
    const std::string TEMP_SEND_SUFFIX = ".zip";

    std::string g_token;
    std::string g_receiverIp;
    int g_receiverPort     = TRANSFER_PORT;
    bool g_receiverRunning = false;
    std::atomic<bool> g_pendingRefresh{false};
    std::atomic<bool> g_receiverCompleted{false};
    std::atomic<u64> g_completedTitleId{0};
    // Guards the receiver state shared UI<->server thread: g_token, g_receiverIp,
    // g_receiverPort, g_receiverRunning, and the notice/name strings.
    std::mutex g_receiverMutex;
    std::string g_receiverNotice;
    std::string g_receiverCompletedName;

    void setReceiverNotice(const std::string& notice)
    {
        std::lock_guard<std::mutex> lock(g_receiverMutex);
        g_receiverNotice = notice;
    }

    void setReceiverCompletedName(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(g_receiverMutex);
        g_receiverCompletedName = name;
    }

    struct FileEntry {
        std::string absPath;
        std::string relPath;
        u32 size;
        u32 crc;
    };

    struct ZipEntry {
        std::string name;
        u32 crc;
        u32 size;
        u32 offset;
        bool isDirectory;
    };

    u32 crcTable[256];
    bool crcInit = false;

    void initCrc(void)
    {
        if (crcInit) {
            return;
        }
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int j = 0; j < 8; ++j) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            crcTable[i] = c;
        }
        crcInit = true;
    }

    u32 updateCrc(u32 crc, const u8* data, size_t len)
    {
        u32 c = crc;
        for (size_t i = 0; i < len; ++i) {
            c = crcTable[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
        }
        return c;
    }

    u64 fileSize(const std::string& path)
    {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            return 0;
        }
        return (u64)st.st_size;
    }

    void collectFiles(const std::string& root, const std::string& sub, std::vector<FileEntry>& out, std::vector<std::string>* outDirs = nullptr)
    {
        std::string current = root + sub;
        if (current.empty() || current.back() != '/') {
            current += "/";
        }
        Directory items(current);
        if (!items.good()) {
            return;
        }
        for (size_t i = 0, sz = items.size(); i < sz; i++) {
            std::string name = items.entry(i);
            if (name == "." || name == "..") {
                continue;
            }
            if (items.folder(i)) {
                std::string nextSub = sub + name + "/";
                if (outDirs != nullptr) {
                    outDirs->push_back(nextSub);
                }
                collectFiles(root, nextSub, out, outDirs);
            }
            else {
                FileEntry entry;
                entry.absPath = current + name;
                entry.relPath = sub + name;
                entry.size    = (u32)fileSize(entry.absPath);
                entry.crc     = 0;
                out.push_back(entry);
            }
        }
    }

    u32 totalFileBytes(const std::vector<FileEntry>& files)
    {
        u32 total = 0;
        for (const auto& entry : files) {
            total += entry.size;
        }
        return total;
    }

    void writeLe16(FILE* out, u16 v)
    {
        u8 b[2] = {(u8)(v & 0xFF), (u8)((v >> 8) & 0xFF)};
        fwrite(b, 1, 2, out);
    }

    void writeLe32(FILE* out, u32 v)
    {
        u8 b[4] = {(u8)(v & 0xFF), (u8)((v >> 8) & 0xFF), (u8)((v >> 16) & 0xFF), (u8)((v >> 24) & 0xFF)};
        fwrite(b, 1, 4, out);
    }

    // Writes a store-only (method 0), no-data-descriptor, no-zip64 ZIP mirroring
    // the 3DS writer, so the receiver's extractZip and the PC CLI accept it. The
    // CRC of each file is backfilled into its local header after the single
    // streaming pass, so each file is read from disk only once.
    bool writeZip(const std::string& root, const std::string& zipPath, u32& outZipSize, std::string& outError)
    {
        std::vector<FileEntry> files;
        std::vector<std::string> dirs;
        collectFiles(root, "", files, &dirs);
        if (files.empty() && dirs.empty()) {
            outError = "No files or folders found to package.";
            return false;
        }

        u32 total = 22; // end of central directory
        for (const auto& dir : dirs) {
            total += 30 + dir.size();
            total += 46 + dir.size();
        }
        for (const auto& entry : files) {
            total += 30 + entry.relPath.size() + entry.size;
            total += 46 + entry.relPath.size();
        }
        outZipSize = total;

        FILE* output = fopen(zipPath.c_str(), "wb");
        if (output == nullptr) {
            outError = StringUtils::format("Cannot create package file (errno %d).", errno);
            return false;
        }

        std::vector<ZipEntry> central;
        central.reserve(dirs.size() + files.size());

        static const size_t kBuf = 0x8000;
        std::unique_ptr<u8[]> buf(new u8[kBuf]);
        initCrc();

        for (const auto& dir : dirs) {
            ZipEntry centralEntry;
            centralEntry.name        = dir;
            centralEntry.crc         = 0;
            centralEntry.size        = 0;
            centralEntry.offset      = (u32)ftell(output);
            centralEntry.isDirectory = true;

            writeLe32(output, 0x04034b50);
            writeLe16(output, 20);
            writeLe16(output, 0);
            writeLe16(output, 0);
            writeLe16(output, 0);
            writeLe16(output, 0);
            writeLe32(output, 0);
            writeLe32(output, 0);
            writeLe32(output, 0);
            writeLe16(output, (u16)dir.size());
            writeLe16(output, 0);
            fwrite(dir.data(), 1, dir.size(), output);

            central.push_back(centralEntry);
        }

        for (const auto& entry : files) {
            ZipEntry centralEntry;
            centralEntry.name        = entry.relPath;
            centralEntry.crc         = 0;
            centralEntry.size        = entry.size;
            centralEntry.offset      = (u32)ftell(output);
            centralEntry.isDirectory = false;

            long crcFieldOffset = (long)centralEntry.offset + 14;

            writeLe32(output, 0x04034b50);
            writeLe16(output, 20);
            writeLe16(output, 0);
            writeLe16(output, 0);
            writeLe16(output, 0);
            writeLe16(output, 0);
            writeLe32(output, 0); // CRC placeholder, backfilled after streaming
            writeLe32(output, entry.size);
            writeLe32(output, entry.size);
            writeLe16(output, (u16)entry.relPath.size());
            writeLe16(output, 0);
            fwrite(entry.relPath.data(), 1, entry.relPath.size(), output);

            FILE* input = fopen(entry.absPath.c_str(), "rb");
            if (input == nullptr) {
                fclose(output);
                outError = StringUtils::format("Cannot read source file for packaging (errno %d).", errno);
                return false;
            }
            u32 crc = 0xFFFFFFFFu;
            while (true) {
                if (TransferStatus::cancelRequested()) {
                    fclose(input);
                    fclose(output);
                    outError = "Transfer cancelled.";
                    return false;
                }
                size_t rd = fread(buf.get(), 1, kBuf, input);
                if (rd == 0) {
                    break;
                }
                crc = updateCrc(crc, buf.get(), rd);
                fwrite(buf.get(), 1, rd, output);
                TransferStatus::addBytesDone(rd);
            }
            fclose(input);
            crc ^= 0xFFFFFFFFu;

            long endOffset = ftell(output);
            fseek(output, crcFieldOffset, SEEK_SET);
            writeLe32(output, crc);
            fseek(output, endOffset, SEEK_SET);

            centralEntry.crc = crc;
            central.push_back(centralEntry);
        }

        u32 centralOffset = (u32)ftell(output);
        for (const auto& entry : central) {
            writeLe32(output, 0x02014b50);
            writeLe16(output, 20);
            writeLe16(output, 20);
            writeLe16(output, 0);
            writeLe16(output, 0);
            writeLe16(output, 0);
            writeLe16(output, 0);
            writeLe32(output, entry.crc);
            writeLe32(output, entry.size);
            writeLe32(output, entry.size);
            writeLe16(output, (u16)entry.name.size());
            writeLe16(output, 0);
            writeLe16(output, 0);
            writeLe16(output, 0);
            writeLe16(output, 0);
            writeLe32(output, entry.isDirectory ? 0x10 : 0); // FILE_ATTRIBUTE_DIRECTORY
            writeLe32(output, entry.offset);
            fwrite(entry.name.data(), 1, entry.name.size(), output);
        }

        u32 centralSize = (u32)ftell(output) - centralOffset;
        writeLe32(output, 0x06054b50);
        writeLe16(output, 0);
        writeLe16(output, 0);
        writeLe16(output, (u16)central.size());
        writeLe16(output, (u16)central.size());
        writeLe32(output, centralSize);
        writeLe32(output, centralOffset);
        writeLe16(output, 0);

        if (fclose(output) != 0) {
            outError = StringUtils::format("Failed to finalize package file (errno %d).", errno);
            return false;
        }
        return true;
    }

    void ensureDirectoryPath(const std::string& base, const std::string& relPath)
    {
        std::string current = base;
        size_t start        = 0;
        while (true) {
            size_t pos = relPath.find('/', start);
            if (pos == std::string::npos) {
                break;
            }
            std::string part = relPath.substr(start, pos - start);
            if (!part.empty()) {
                current += part;
                if (!io::directoryExists(current)) {
                    io::createDirectory(current);
                }
                current += "/";
            }
            start = pos + 1;
        }
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

    // Extracts a store-only ZIP that lives inside `zipFilePath` at [startOffset,
    // startOffset+limit). Reading straight from the multipart body temp file at
    // the file part's byte range avoids ever holding the payload in RAM or
    // copying it to a second staging file.
    bool extractZip(const std::string& zipFilePath, u64 startOffset, u64 limit, const std::string& destRoot, std::string& outError)
    {
        FILE* input = fopen(zipFilePath.c_str(), "rb");
        if (input == nullptr) {
            outError = "Failed to open received package.";
            return false;
        }
        if (fseek(input, (long)startOffset, SEEK_SET) != 0) {
            fclose(input);
            outError = "Failed to open received package.";
            return false;
        }

        u64 consumed  = 0;
        auto readInto = [&](void* dst, size_t n) -> size_t {
            if (consumed + n > limit) {
                n = (size_t)(limit - consumed);
            }
            if (n == 0) {
                return 0;
            }
            size_t rd = fread(dst, 1, n, input);
            consumed += rd;
            return rd;
        };

        bool ok = true;
        while (consumed + 4 <= limit) {
            u8 sigBuf[4];
            if (readInto(sigBuf, 4) != 4) {
                break;
            }
            u32 sig = (u32)(sigBuf[0] | (sigBuf[1] << 8) | (sigBuf[2] << 16) | (sigBuf[3] << 24));
            if (sig != 0x04034b50) {
                break;
            }
            u8 hdr[26];
            if (readInto(hdr, 26) != 26) {
                outError = "Corrupted ZIP header.";
                ok       = false;
                break;
            }
            u16 flags       = (u16)(hdr[2] | (hdr[3] << 8));
            u16 compression = (u16)(hdr[4] | (hdr[5] << 8));
            u32 crc         = (u32)(hdr[10] | (hdr[11] << 8) | (hdr[12] << 16) | (hdr[13] << 24));
            u32 compSize    = (u32)(hdr[14] | (hdr[15] << 8) | (hdr[16] << 16) | (hdr[17] << 24));
            u32 uncompSize  = (u32)(hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | (hdr[21] << 24));
            (void)uncompSize;
            u16 nameLen  = (u16)(hdr[22] | (hdr[23] << 8));
            u16 extraLen = (u16)(hdr[24] | (hdr[25] << 8));

            std::string name;
            name.resize(nameLen);
            if (nameLen > 0 && readInto(name.data(), nameLen) != nameLen) {
                outError = "Corrupted ZIP header.";
                ok       = false;
                break;
            }
            if (extraLen > 0) {
                std::unique_ptr<u8[]> extra(new u8[extraLen]);
                if (readInto(extra.get(), extraLen) != extraLen) {
                    outError = "Corrupted ZIP header.";
                    ok       = false;
                    break;
                }
            }

            if (compression != 0 || (flags & 0x08)) {
                outError = "Unsupported ZIP compression.";
                ok       = false;
                break;
            }
            if (!isSafeZipRelativePath(name)) {
                outError = "Invalid ZIP entry path.";
                ok       = false;
                break;
            }

            if (!name.empty() && name.back() == '/') {
                std::string dirPath = destRoot + name;
                if (!io::directoryExists(dirPath)) {
                    io::createDirectory(dirPath);
                }
                continue;
            }

            ensureDirectoryPath(destRoot, name);
            std::string outPath = destRoot + name;
            FILE* output        = fopen(outPath.c_str(), "wb");
            if (output == nullptr) {
                outError = "Failed to write extracted file.";
                ok       = false;
                break;
            }

            static const size_t kBuf = 0x8000;
            std::unique_ptr<u8[]> buf(new u8[kBuf]);
            initCrc();
            u32 computedCrc = 0xFFFFFFFFu;
            u32 remaining   = compSize;
            bool fileOk     = true;
            while (remaining > 0) {
                if (TransferStatus::cancelRequested()) {
                    outError = "Transfer cancelled.";
                    fileOk   = false;
                    break;
                }
                u32 chunk = remaining > kBuf ? kBuf : remaining;
                size_t rd = readInto(buf.get(), chunk);
                if (rd == 0) {
                    outError = "Corrupted ZIP payload.";
                    fileOk   = false;
                    break;
                }
                computedCrc = updateCrc(computedCrc, buf.get(), rd);
                fwrite(buf.get(), 1, rd, output);
                remaining -= (u32)rd;
                TransferStatus::addBytesDone(rd);
            }
            fclose(output);
            if (!fileOk) {
                ok = false;
                break;
            }

            computedCrc ^= 0xFFFFFFFFu;
            if (computedCrc != crc) {
                outError = "Checksum mismatch in received file.";
                ok       = false;
                break;
            }
        }

        fclose(input);
        return ok;
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

    // Locates the multipart parts inside the streamed body temp file. The meta
    // part is small and read into RAM; the file part is returned as a byte range
    // [outFileOffset, outFileOffset+outFileLen) into the body file, so the (large)
    // payload is never buffered. Assumes the protocol layout: meta part first,
    // then a single file part that is the last part before the closing delimiter.
    bool parseMultipartFile(const std::string& bodyPath, u64 bodyLen, const std::string& boundary, std::string& outMeta, u64& outFileOffset,
        u64& outFileLen, std::string& outError)
    {
        outMeta.clear();
        outFileOffset = 0;
        outFileLen    = 0;

        FILE* f = fopen(bodyPath.c_str(), "rb");
        if (f == nullptr) {
            outError = "Failed to open upload body.";
            return false;
        }

        // The meta part and both part headers sit at the very start of the body;
        // read a bounded head window to locate them without scanning the payload.
        const size_t headWindow = 64 * 1024;
        size_t headLen          = (size_t)std::min<u64>(headWindow, bodyLen);
        std::string head;
        head.resize(headLen);
        if (headLen > 0 && fread(head.data(), 1, headLen, f) != headLen) {
            fclose(f);
            outError = "Failed to read upload body.";
            return false;
        }
        fclose(f);

        std::string boundaryMarker     = "--" + boundary;
        std::string nextBoundaryMarker = "\r\n" + boundaryMarker;

        // Meta part.
        size_t metaPos = head.find(boundaryMarker);
        if (metaPos == std::string::npos) {
            outError = "Missing boundary.";
            return false;
        }
        // Find the file part header.
        size_t filePos = head.find("name=\"file\"");
        if (filePos == std::string::npos) {
            outError = "Incomplete form data.";
            return false;
        }
        // Meta content: between the meta part's header terminator and the boundary
        // that precedes the file part.
        size_t metaHeaderEnd = head.find("\r\n\r\n", metaPos);
        if (metaHeaderEnd == std::string::npos) {
            outError = "Incomplete form data.";
            return false;
        }
        size_t metaDataStart = metaHeaderEnd + 4;
        size_t metaDataEnd   = head.find(nextBoundaryMarker, metaDataStart);
        if (metaDataEnd == std::string::npos || metaDataEnd > filePos) {
            outError = "Incomplete form data.";
            return false;
        }
        outMeta = head.substr(metaDataStart, metaDataEnd - metaDataStart);

        // File data begins right after the file part's header terminator.
        size_t fileHeaderEnd = head.find("\r\n\r\n", filePos);
        if (fileHeaderEnd == std::string::npos) {
            outError = "Incomplete form data.";
            return false;
        }
        u64 fileDataStart = (u64)fileHeaderEnd + 4;

        // The body ends with "\r\n--boundary--\r\n"; the file data is everything
        // between fileDataStart and that trailing delimiter.
        std::string trailer = "\r\n" + boundaryMarker + "--\r\n";
        if (bodyLen < fileDataStart + trailer.size()) {
            outError = "Incomplete form data.";
            return false;
        }
        outFileOffset = fileDataStart;
        outFileLen    = bodyLen - fileDataStart - trailer.size();
        return true;
    }

    // Constant-time comparison so a wrong PIN can't be narrowed down by timing.
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

    Server::HttpResponse handleInfo(const std::string&, const std::string&)
    {
        // The PIN is intentionally NOT exposed here: it must only ever be shown on
        // the receiver's screen and typed on the sender, so a device on the same
        // network can't read it and authenticate itself.
        nlohmann::json info;
        info["device"]         = "Switch";
        info["version"]        = StringUtils::format("%d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO);
        info["maxUploadBytes"] = 0; // 0 = unlimited: the body is streamed to SD, not buffered in RAM
        info["freeSpaceBytes"] = 0;
        return {200, "application/json", info.dump()};
    }

    // Streaming upload handler: the server has already written the raw multipart
    // body to `req.bodyPath`. We parse the meta out of the head, resolve the
    // destination title, then extract the ZIP (or copy the raw file) straight
    // from the body temp file's file-part byte range.
    Server::HttpResponse handleUpload(const Server::UploadRequest& req)
    {
        auto cleanup = []() { TransferStatus::end(); };

        std::string token = headerValue(req.headers, "X-CP-Token");
        std::string expectedToken;
        {
            std::lock_guard<std::mutex> lock(g_receiverMutex);
            expectedToken = g_token;
        }
        if (!constantTimeEquals(token, expectedToken)) {
            cleanup();
            return {403, "application/json", "{\"ok\":false,\"error\":\"Invalid token\"}"};
        }

        std::string contentType = headerValue(req.headers, "Content-Type");
        size_t bpos             = contentType.find("boundary=");
        if (bpos == std::string::npos) {
            cleanup();
            return {400, "application/json", "{\"ok\":false,\"error\":\"Missing boundary\"}"};
        }
        std::string boundary = contentType.substr(bpos + 9);
        if (!boundary.empty() && boundary.front() == '"' && boundary.back() == '"') {
            boundary = boundary.substr(1, boundary.size() - 2);
        }

        std::string metaJson;
        u64 fileOffset = 0;
        u64 fileLen    = 0;
        std::string error;
        if (!parseMultipartFile(req.bodyPath, req.bodyLength, boundary, metaJson, fileOffset, fileLen, error)) {
            cleanup();
            return {400, "application/json", "{\"ok\":false,\"error\":\"Bad upload\"}"};
        }

        auto meta = nlohmann::json::parse(metaJson, nullptr, false);
        if (meta.is_discarded()) {
            cleanup();
            return {400, "application/json", "{\"ok\":false,\"error\":\"Invalid meta\"}"};
        }

        std::string titleId    = meta.value("titleId", "");
        std::string titleName  = meta.value("titleName", "Unknown");
        std::string backupName = meta.value("backupName", "");
        bool isZip             = meta.value("isZip", false);
        setReceiverNotice("");
        if (backupName.empty()) {
            backupName = "Received_" + DateTime::dateTimeStr();
        }

        std::string destRoot;
        bool foundTitle   = false;
        bool mappedByName = false;
        u64 tid           = 0;
        u64 resolvedId    = 0;
        if (!titleId.empty()) {
            tid = strtoull(titleId.c_str(), nullptr, 16);
        }
        if (tid != 0) {
            Title t;
            if (TitleCatalog::get().getTitleById(t, tid)) {
                destRoot   = t.path();
                resolvedId = t.id();
                foundTitle = true;
            }
        }
        if (!foundTitle && !titleName.empty()) {
            Title t;
            if (TitleCatalog::get().getTitleByName(t, titleName)) {
                destRoot     = t.path();
                resolvedId   = t.id();
                foundTitle   = true;
                mappedByName = true;
                setReceiverNotice("Warning: title ID mismatch. Backup mapped by title name.");
                Logging::warning("Title ID {} not found, mapped by title name '{}'.", titleId, titleName);
            }
        }

        if (!foundTitle) {
            std::string safeName = titleName.empty() ? "Unknown" : titleName;
            std::string folder   = safeName;
            if (!titleId.empty()) {
                folder = titleId + " " + safeName;
            }
            destRoot = std::string(SAVES_ROOT) + StringUtils::removeForbiddenCharacters(folder);
            if (!io::directoryExists(destRoot)) {
                io::createDirectory(destRoot);
            }
            setReceiverNotice("Warning: unknown title. Stored in:\n" + destRoot);
            Logging::warning("Received backup for unknown title {} (stored under {}).", titleId, destRoot);
        }

        std::string backupRoot = destRoot + "/" + StringUtils::removeForbiddenCharacters(backupName) + "/";
        if (io::directoryExists(backupRoot)) {
            io::deleteFolderRecursively(backupRoot);
        }
        io::createDirectory(backupRoot);

        if (isZip) {
            TransferStatus::beginNetwork("Extracting package", fileLen);
            std::string extractError;
            bool extracted = extractZip(req.bodyPath, fileOffset, fileLen, backupRoot, extractError);
            if (!extracted) {
                // A failed (or cancelled) extract leaves a half-populated backup
                // folder behind; remove it so the receiver never keeps a backup
                // it can't trust.
                io::deleteFolderRecursively(backupRoot);
                cleanup();
                std::string message = extractError.empty() ? "Failed to extract package." : extractError;
                nlohmann::json err;
                err["ok"]    = false;
                err["error"] = message;
                return {500, "application/json", err.dump()};
            }
            TransferStatus::setBytesDone(fileLen);
        }
        else {
            std::string fileName = meta.value("fileName", "");
            if (fileName.empty()) {
                fileName = "received.bin";
            }
            std::string safeFileName = StringUtils::removeForbiddenCharacters(fileName);
            if (safeFileName.empty()) {
                safeFileName = "received.bin";
            }
            ensureDirectoryPath(backupRoot, safeFileName);
            std::string outPath = backupRoot + safeFileName;

            FILE* src = fopen(req.bodyPath.c_str(), "rb");
            FILE* dst = fopen(outPath.c_str(), "wb");
            if (src == nullptr || dst == nullptr) {
                if (src) {
                    fclose(src);
                }
                if (dst) {
                    fclose(dst);
                }
                io::deleteFolderRecursively(backupRoot);
                cleanup();
                return {500, "application/json", "{\"ok\":false,\"error\":\"Failed to store file\"}"};
            }
            fseek(src, (long)fileOffset, SEEK_SET);
            static const size_t kBuf = 0x8000;
            std::unique_ptr<u8[]> buf(new u8[kBuf]);
            u64 remaining = fileLen;
            bool copyOk   = true;
            while (remaining > 0) {
                if (TransferStatus::cancelRequested()) {
                    copyOk = false;
                    break;
                }
                size_t chunk = remaining > kBuf ? kBuf : (size_t)remaining;
                size_t rd    = fread(buf.get(), 1, chunk, src);
                if (rd == 0) {
                    copyOk = false;
                    break;
                }
                fwrite(buf.get(), 1, rd, dst);
                remaining -= rd;
            }
            fclose(src);
            fclose(dst);
            if (!copyOk) {
                io::deleteFolderRecursively(backupRoot);
                cleanup();
                return {500, "application/json", "{\"ok\":false,\"error\":\"Failed to store file\"}"};
            }
        }

        cleanup();

        if (!mappedByName && foundTitle) {
            setReceiverNotice("");
        }
        setReceiverCompletedName(backupName);
        g_completedTitleId.store(resolvedId);
        g_receiverCompleted.store(true);
        g_pendingRefresh.store(true);

        nlohmann::json resp;
        resp["ok"]        = true;
        resp["savedPath"] = backupRoot;
        return {200, "application/json", resp.dump()};
    }

    bool sendAll(int sock, const void* data, size_t len)
    {
        const u8* ptr = static_cast<const u8*>(data);
        size_t sent   = 0;
        while (sent < len) {
            int rc = send(sock, ptr + sent, len - sent, 0);
            if (rc <= 0) {
                return false;
            }
            sent += rc;
        }
        return true;
    }
}

void Transfer::sweepTempFiles(void)
{
    if (io::fileExists(TEMP_UPLOAD)) {
        std::remove(TEMP_UPLOAD);
        Logging::info("Removed leftover {} from a previous run.", TEMP_UPLOAD);
    }

    Directory dir(CHECKPOINT_ROOT);
    if (!dir.good()) {
        return;
    }
    for (size_t i = 0, sz = dir.size(); i < sz; i++) {
        if (dir.folder(i)) {
            continue;
        }
        std::string name = dir.entry(i);
        if (name.size() > TEMP_SEND_PREFIX.size() + TEMP_SEND_SUFFIX.size() && name.compare(0, TEMP_SEND_PREFIX.size(), TEMP_SEND_PREFIX) == 0 &&
            name.compare(name.size() - TEMP_SEND_SUFFIX.size(), TEMP_SEND_SUFFIX.size(), TEMP_SEND_SUFFIX) == 0) {
            std::string path = std::string(CHECKPOINT_ROOT) + name;
            std::remove(path.c_str());
            Logging::info("Removed leftover {} from a previous run.", name);
        }
    }
}

bool Transfer::startReceiver(std::string& outError)
{
    {
        std::lock_guard<std::mutex> lock(g_receiverMutex);
        if (g_receiverRunning) {
            return true;
        }
    }
    if (Server::getAddress().empty()) {
        outError = "HTTP server not available.";
        return false;
    }

    srand((unsigned int)time(nullptr));
    int pin           = 1000 + (rand() % 9000);
    std::string token = StringUtils::format("%04d", pin);
    std::string ip    = Server::getAddress();
    setReceiverNotice("");
    setReceiverCompletedName("");
    g_receiverCompleted.store(false);
    g_completedTitleId.store(0);
    size_t pos = ip.find("://");
    if (pos != std::string::npos) {
        ip = ip.substr(pos + 3);
    }
    pos = ip.find(":");
    if (pos != std::string::npos) {
        ip = ip.substr(0, pos);
    }

    // Publish token/ip/port before the handler is reachable, so an upload that
    // arrives the instant it registers validates against the real token.
    {
        std::lock_guard<std::mutex> lock(g_receiverMutex);
        g_token        = token;
        g_receiverIp   = ip;
        g_receiverPort = TRANSFER_PORT;
    }

    Server::registerHandler("/transfer/info", handleInfo);
    Server::registerUploadHandler("/transfer/upload", TEMP_UPLOAD, handleUpload);

    {
        std::lock_guard<std::mutex> lock(g_receiverMutex);
        g_receiverRunning = true;
    }

    return true;
}

void Transfer::stopReceiver(void)
{
    {
        std::lock_guard<std::mutex> lock(g_receiverMutex);
        if (!g_receiverRunning) {
            return;
        }
    }
    Server::unregisterHandler("/transfer/info");
    Server::unregisterHandler("/transfer/upload");
    {
        std::lock_guard<std::mutex> lock(g_receiverMutex);
        g_receiverRunning = false;
    }
}

bool Transfer::receiverRunning(void)
{
    std::lock_guard<std::mutex> lock(g_receiverMutex);
    return g_receiverRunning;
}

bool Transfer::consumePendingRefresh(void)
{
    return g_pendingRefresh.exchange(false);
}

u64 Transfer::consumeCompletedTitleId(void)
{
    return g_completedTitleId.exchange(0);
}

std::string Transfer::receiverToken(void)
{
    std::lock_guard<std::mutex> lock(g_receiverMutex);
    return g_token;
}

std::string Transfer::receiverIp(void)
{
    std::lock_guard<std::mutex> lock(g_receiverMutex);
    return g_receiverIp;
}

int Transfer::receiverPort(void)
{
    std::lock_guard<std::mutex> lock(g_receiverMutex);
    return g_receiverPort;
}

std::string Transfer::receiverNotice(void)
{
    std::lock_guard<std::mutex> lock(g_receiverMutex);
    return g_receiverNotice;
}

bool Transfer::receiverHasCompleted(void)
{
    return g_receiverCompleted.load();
}

std::string Transfer::receiverCompletedName(void)
{
    std::lock_guard<std::mutex> lock(g_receiverMutex);
    return g_receiverCompletedName;
}

void Transfer::clearReceiverNotice(void)
{
    setReceiverNotice("");
}

void Transfer::clearReceiverCompletion(void)
{
    setReceiverCompletedName("");
    g_receiverCompleted.store(false);
}

Transfer::SendOutcome Transfer::sendBackup(Title& title, const std::string& backupPath, const std::string& backupName, const std::string& dataType,
    const std::string& ip, u16 port, const std::string& token)
{
    // Every exit path must clear the transfer modal and remove the temp zip;
    // scope guards make that hold for each early return below.
    struct StatusGuard {
        ~StatusGuard() { TransferStatus::end(); }
    } statusGuard;

    struct ZipGuard {
        std::string path;
        bool armed = false;
        ~ZipGuard()
        {
            if (armed) {
                std::remove(path.c_str());
            }
        }
    } zipGuard;

    std::vector<FileEntry> files;
    std::vector<std::string> dirs;
    collectFiles(backupPath, "", files, &dirs);
    if (files.empty() && dirs.empty()) {
        return SendOutcome{false, SendStage::EmptyBackup, ""};
    }

    bool isZip = files.size() != 1 || !dirs.empty();
    std::string payloadPath;
    std::string payloadName;
    u32 payloadSize = 0;

    if (isZip) {
        TransferStatus::beginNetwork("Preparing backup package", totalFileBytes(files));

        u32 zipSize = 0;
        std::string zipPath =
            StringUtils::format("%s%s%s%s", CHECKPOINT_ROOT, TEMP_SEND_PREFIX.c_str(), DateTime::dateTimeStr().c_str(), TEMP_SEND_SUFFIX.c_str());
        if (io::fileExists(zipPath)) {
            std::remove(zipPath.c_str());
        }
        zipGuard.path  = zipPath;
        zipGuard.armed = true;
        std::string zipError;
        if (!writeZip(backupPath, zipPath, zipSize, zipError)) {
            return SendOutcome{false, TransferStatus::cancelRequested() ? SendStage::Cancelled : SendStage::Zip, ""};
        }
        payloadPath = zipPath;
        payloadName = "backup.zip";
        payloadSize = zipSize;
    }
    else {
        const FileEntry& entry = files.front();
        payloadPath            = entry.absPath;
        payloadName            = entry.relPath;
        payloadSize            = entry.size;
    }

    TransferStatus::beginNetwork("Sending backup", payloadSize);

    nlohmann::json meta;
    meta["titleId"]        = StringUtils::format("%016llX", title.id());
    meta["titleName"]      = title.displayName();
    meta["dataType"]       = dataType;
    meta["backupName"]     = backupName;
    meta["isZip"]          = isZip;
    meta["fileBytesTotal"] = payloadSize;
    meta["fileName"]       = payloadName;
    meta["timestamp"]      = DateTime::logDateTime();

    std::string metaStr  = meta.dump();
    std::string boundary = StringUtils::format("----checkpoint-boundary-%llu", (unsigned long long)time(nullptr));

    std::string partMeta = "--" + boundary +
                           "\r\n"
                           "Content-Disposition: form-data; name=\"meta\"\r\n"
                           "Content-Type: application/json\r\n\r\n" +
                           metaStr + "\r\n";

    std::string fileName = payloadName;
    size_t slashPos      = fileName.find_last_of('/');
    if (slashPos != std::string::npos) {
        fileName = fileName.substr(slashPos + 1);
    }
    if (fileName.empty()) {
        fileName = "backup.bin";
    }
    std::string partFileHeader = "--" + boundary +
                                 "\r\n"
                                 "Content-Disposition: form-data; name=\"file\"; filename=\"" +
                                 fileName +
                                 "\"\r\n"
                                 "Content-Type: application/octet-stream\r\n\r\n";

    std::string partEnd = "\r\n--" + boundary + "--\r\n";

    u32 contentLength = partMeta.size() + partFileHeader.size() + payloadSize + partEnd.size();

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        return SendOutcome{false, SendStage::Socket, ""};
    }

    struct SockGuard {
        int fd;
        ~SockGuard() { close(fd); }
    } sockGuard{sock};

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        return SendOutcome{false, SendStage::Resolve, ""};
    }
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        return SendOutcome{false, SendStage::Connect, ""};
    }

    std::string header = StringUtils::format("POST /transfer/upload HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n", ip.c_str(), port);
    header += StringUtils::format("X-CP-Token: %s\r\n", token.c_str());
    header += StringUtils::format("Content-Type: multipart/form-data; boundary=%s\r\n", boundary.c_str());
    header += StringUtils::format("Content-Length: %u\r\n\r\n", contentLength);

    bool ok = sendAll(sock, header.data(), header.size()) && sendAll(sock, partMeta.data(), partMeta.size()) &&
              sendAll(sock, partFileHeader.data(), partFileHeader.size());

    if (ok) {
        FILE* input = fopen(payloadPath.c_str(), "rb");
        if (input == nullptr) {
            ok = false;
        }
        else {
            static const size_t kBuf = 0x8000;
            std::unique_ptr<u8[]> buf(new u8[kBuf]);
            while (true) {
                if (TransferStatus::cancelRequested()) {
                    fclose(input);
                    // The scope guards close the socket (dropping the connection,
                    // which the receiver treats as an aborted request) and delete
                    // the temp zip.
                    return SendOutcome{false, SendStage::Cancelled, ""};
                }
                size_t rd = fread(buf.get(), 1, kBuf, input);
                if (rd == 0) {
                    break;
                }
                if (!sendAll(sock, buf.get(), rd)) {
                    ok = false;
                    break;
                }
                TransferStatus::addBytesDone(rd);
            }
            fclose(input);
        }
    }

    if (ok) {
        ok = sendAll(sock, partEnd.data(), partEnd.size());
    }

    std::string response;
    {
        char responseBuf[512];
        while (true) {
            int rc = recv(sock, responseBuf, sizeof(responseBuf), 0);
            if (rc <= 0) {
                break;
            }
            response.append(responseBuf, (size_t)rc);
        }
    }

    if (!ok) {
        return SendOutcome{false, TransferStatus::cancelRequested() ? SendStage::Cancelled : SendStage::Send, ""};
    }

    bool httpOk = response.rfind("HTTP/1.1 200", 0) == 0 || response.rfind("HTTP/1.0 200", 0) == 0;
    if (!httpOk) {
        std::string detail;
        if (!response.empty()) {
            size_t bodyPos = response.find("\r\n\r\n");
            if (bodyPos != std::string::npos && bodyPos + 4 < response.size()) {
                std::string body = response.substr(bodyPos + 4);
                auto j           = nlohmann::json::parse(body, nullptr, false);
                if (!j.is_discarded() && j.contains("error") && j["error"].is_string()) {
                    detail = j["error"].get<std::string>();
                }
            }
            if (detail.empty()) {
                size_t lineEnd = response.find("\r\n");
                detail         = lineEnd == std::string::npos ? response : response.substr(0, lineEnd);
            }
        }
        return SendOutcome{false, SendStage::Response, detail};
    }

    return SendOutcome{true, SendStage::Response, ""};
}

std::optional<Transfer::TransferTarget> Transfer::parseTarget(const std::string& ipPort)
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
    return TransferTarget{std::move(ip), (u16)port};
}

bool Transfer::validPin(const std::string& pin)
{
    return pin.size() == 4 && std::all_of(pin.begin(), pin.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}
