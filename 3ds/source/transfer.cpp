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
#include "fsstream.hpp"
#include "io.hpp"
#include "json.hpp"
#include "loader.hpp"
#include "logging.hpp"
#include "server.hpp"
#include "transferstatus.hpp"
#include "util.hpp"
#include <3ds.h>
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {
    static const int TRANSFER_PORT = 8000;
    // The server streams the raw upload body here; handleUpload extracts/copies
    // straight from the file-part byte range, so no separate staged zip is needed.
    static const char* TEMP_UPLOAD = "/3ds/Checkpoint/transfer_upload.tmp";
    // Legacy staging file from before the streaming server; still swept on boot.
    static const char* TEMP_ZIP_RECV_LEGACY = "/3ds/Checkpoint/transfer_recv.zip";

    std::string g_token;
    std::string g_receiverIp;
    int g_receiverPort     = TRANSFER_PORT;
    bool g_receiverRunning = false;
    std::atomic<bool> g_pendingRefresh{false};
    std::atomic<bool> g_receiverCompleted{false};
    // Guards the receiver state shared UI<->network thread: g_token (read by the
    // server thread in handleUpload, written by the UI thread), g_receiverIp,
    // g_receiverPort, g_receiverRunning, and g_receiverNotice /
    // g_receiverCompletedName (written by handleUpload, read by the UI thread).
    // Pass an empty string to clear the notice/name. TransferReceiver (4.1) is the
    // real home for this state.
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
        std::u16string absPath;
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

    u32 crcTable[8][256];
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
            crcTable[0][i] = c;
        }
        for (u32 i = 0; i < 256; ++i) {
            u32 c = crcTable[0][i];
            for (int t = 1; t < 8; ++t) {
                c              = crcTable[0][c & 0xFFu] ^ (c >> 8);
                crcTable[t][i] = c;
            }
        }
        crcInit = true;
    }

    // Slicing-by-8 CRC32: 8 bytes per iteration instead of 1. On the 268MHz
    // ARM11 the bytewise loop is slow enough to rival the SD card, so this
    // keeps the checksum off the transfer's critical path.
    u32 updateCrc(u32 crc, const u8* data, size_t len)
    {
        u32 c = crc;
        while (len >= 8) {
            u32 lo = (u32)(data[0] | (data[1] << 8) | (data[2] << 16) | ((u32)data[3] << 24)) ^ c;
            u32 hi = (u32)(data[4] | (data[5] << 8) | (data[6] << 16) | ((u32)data[7] << 24));
            c      = crcTable[7][lo & 0xFFu] ^ crcTable[6][(lo >> 8) & 0xFFu] ^ crcTable[5][(lo >> 16) & 0xFFu] ^ crcTable[4][lo >> 24] ^
                crcTable[3][hi & 0xFFu] ^ crcTable[2][(hi >> 8) & 0xFFu] ^ crcTable[1][(hi >> 16) & 0xFFu] ^ crcTable[0][hi >> 24];
            data += 8;
            len -= 8;
        }
        for (size_t i = 0; i < len; ++i) {
            c = crcTable[0][(c ^ data[i]) & 0xFFu] ^ (c >> 8);
        }
        return c;
    }

    void collectFiles(FS_Archive arch, const std::u16string& root, const std::u16string& sub, std::vector<FileEntry>& out,
        std::vector<std::string>* outDirs = nullptr)
    {
        std::u16string current = root;
        if (!current.empty() && current.back() != u'/') {
            current += StringUtils::UTF8toUTF16("/");
        }
        current += sub;
        Directory items(arch, current);
        if (!items.good()) {
            return;
        }
        for (size_t i = 0, sz = items.size(); i < sz; i++) {
            std::u16string name = items.entry(i);
            if (name == u"." || name == u"..") {
                continue;
            }
            if (items.folder(i)) {
                std::u16string nextSub = sub + name + StringUtils::UTF8toUTF16("/");
                if (outDirs != nullptr) {
                    outDirs->push_back(StringUtils::UTF16toUTF8(nextSub));
                }
                collectFiles(arch, root, nextSub, out, outDirs);
            }
            else {
                std::u16string abs = current + name;
                std::string rel    = StringUtils::UTF16toUTF8(sub + name);
                FSStream input(arch, abs, FS_OPEN_READ);
                if (!input.good()) {
                    continue;
                }
                FileEntry entry;
                entry.absPath = abs;
                entry.relPath = rel;
                entry.size    = input.size();
                input.close();
                out.push_back(entry);
            }
        }
    }

    void appendLe16(std::string& out, u16 v)
    {
        out.push_back((char)(v & 0xFF));
        out.push_back((char)((v >> 8) & 0xFF));
    }

    void appendLe32(std::string& out, u32 v)
    {
        out.push_back((char)(v & 0xFF));
        out.push_back((char)((v >> 8) & 0xFF));
        out.push_back((char)((v >> 16) & 0xFF));
        out.push_back((char)((v >> 24) & 0xFF));
    }

    // Exact byte size of the ZIP produced by sendZipStream. Store-only entries
    // make it fully deterministic, which is what lets the zip be streamed
    // straight into the socket with a correct Content-Length and no staged
    // temp file on the SD card.
    u32 zipStreamSize(const std::vector<FileEntry>& files, const std::vector<std::string>& dirs)
    {
        u32 total = 22; // end of central directory
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

    bool ensureDirectoryPath(const std::u16string& base, const std::string& relPath)
    {
        std::u16string current = base;
        size_t start           = 0;
        while (true) {
            size_t pos = relPath.find('/', start);
            if (pos == std::string::npos) {
                break;
            }
            std::string part = relPath.substr(start, pos - start);
            if (!part.empty()) {
                current += StringUtils::UTF8toUTF16(part.c_str());
                if (!io::directoryExists(Archive::sdmc(), current)) {
                    io::createDirectory(Archive::sdmc(), current);
                }
                current += StringUtils::UTF8toUTF16("/");
            }
            start = pos + 1;
        }
        return true;
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

    // Extracts a store-only ZIP that lives inside `zipPath` at [startOffset,
    // startOffset+limit). Reading straight from the streamed multipart body at
    // the file part's byte range avoids holding the payload in RAM or copying it
    // to a second staging file.
    bool extractZip(const std::u16string& zipPath, u64 startOffset, u64 limit, const std::u16string& destRoot, std::string& outError)
    {
        FSStream input(Archive::sdmc(), zipPath, FS_OPEN_READ);
        if (!input.good()) {
            outError = "Failed to open received package.";
            return false;
        }
        input.offset((u32)startOffset);

        u64 consumed  = 0;
        auto readInto = [&](void* dst, size_t n) -> u32 {
            if (consumed + n > limit) {
                n = (size_t)(limit - consumed);
            }
            if (n == 0) {
                return 0;
            }
            u32 rd = input.read(dst, (u32)n);
            consumed += rd;
            return rd;
        };

        static const u32 kBuf = 0x40000;
        std::unique_ptr<u8[]> buf(new u8[kBuf]);
        initCrc();

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
            u16 nameLen     = (u16)(hdr[22] | (hdr[23] << 8));
            u16 extraLen    = (u16)(hdr[24] | (hdr[25] << 8));

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

            // Data-descriptor entries (flag bit 3) are accepted as long as the
            // local header still carries the real sizes, which is what the
            // console senders emit when streaming a zip without staging it.
            if (compression != 0) {
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
                std::u16string dirPath = destRoot + StringUtils::UTF8toUTF16(name.c_str());
                if (!io::directoryExists(Archive::sdmc(), dirPath)) {
                    io::createDirectory(Archive::sdmc(), dirPath);
                }
                continue;
            }

            ensureDirectoryPath(destRoot, name);
            std::u16string outPath = destRoot + StringUtils::UTF8toUTF16(name.c_str());
            FSStream output(Archive::sdmc(), outPath, FS_OPEN_WRITE, uncompSize);
            if (!output.good()) {
                outError = "Failed to write extracted file.";
                ok       = false;
                break;
            }

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
                u32 rd    = readInto(buf.get(), chunk);
                if (rd == 0) {
                    outError = "Corrupted ZIP payload.";
                    fileOk   = false;
                    break;
                }
                computedCrc = updateCrc(computedCrc, buf.get(), rd);
                output.write(buf.get(), rd);
                remaining -= rd;
                TransferStatus::addBytesDone(rd);
            }
            output.close();
            if (!fileOk) {
                ok = false;
                break;
            }

            // With flag bit 3 the local-header CRC field is zero and the real
            // CRC follows the data in a data descriptor (optionally prefixed
            // with its own signature).
            if (flags & 0x08) {
                u8 desc[16];
                if (readInto(desc, 4) != 4) {
                    outError = "Corrupted ZIP payload.";
                    ok       = false;
                    break;
                }
                u32 first = (u32)(desc[0] | (desc[1] << 8) | (desc[2] << 16) | ((u32)desc[3] << 24));
                if (first == 0x08074b50) {
                    if (readInto(desc + 4, 12) != 12) {
                        outError = "Corrupted ZIP payload.";
                        ok       = false;
                        break;
                    }
                    crc = (u32)(desc[4] | (desc[5] << 8) | (desc[6] << 16) | ((u32)desc[7] << 24));
                }
                else {
                    if (readInto(desc + 4, 8) != 8) {
                        outError = "Corrupted ZIP payload.";
                        ok       = false;
                        break;
                    }
                    crc = first;
                }
            }

            // Verify the extracted data against the CRC stored in the ZIP entry,
            // so a corrupted or truncated transfer is rejected instead of being
            // written out as-is.
            computedCrc ^= 0xFFFFFFFFu;
            if (computedCrc != crc) {
                outError = "Checksum mismatch in received file.";
                ok       = false;
                break;
            }
        }

        input.close();
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
    bool parseMultipartFile(const std::u16string& bodyPath, u64 bodyLen, const std::string& boundary, std::string& outMeta, u64& outFileOffset,
        u64& outFileLen, std::string& outError)
    {
        outMeta.clear();
        outFileOffset = 0;
        outFileLen    = 0;

        FSStream f(Archive::sdmc(), bodyPath, FS_OPEN_READ);
        if (!f.good()) {
            outError = "Failed to open upload body.";
            return false;
        }

        // The meta part and both part headers sit at the very start of the body;
        // read a bounded head window to locate them without scanning the payload.
        const u32 headWindow = 64 * 1024;
        u32 headLen          = (u32)(bodyLen < headWindow ? bodyLen : headWindow);
        std::string head;
        head.resize(headLen);
        if (headLen > 0 && f.read(head.data(), headLen) != headLen) {
            f.close();
            outError = "Failed to read upload body.";
            return false;
        }
        f.close();

        std::string boundaryMarker     = "--" + boundary;
        std::string nextBoundaryMarker = "\r\n" + boundaryMarker;

        size_t metaPos = head.find(boundaryMarker);
        if (metaPos == std::string::npos) {
            outError = "Missing boundary.";
            return false;
        }
        size_t filePos = head.find("name=\"file\"");
        if (filePos == std::string::npos) {
            outError = "Incomplete form data.";
            return false;
        }
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

    Server::HttpResponse handleInfo(const std::string&, const std::string&)
    {
        // Note: the PIN token is intentionally NOT exposed here. It must only ever
        // be shown on the receiver's screen and entered manually on the sender, so
        // that a device on the same network cannot read it and authenticate itself.
        nlohmann::json info;
        info["device"]         = "3DS";
        info["version"]        = StringUtils::format("%d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO);
        info["maxUploadBytes"] = 0;
        info["freeSpaceBytes"] = 0;
        return {200, "application/json", info.dump()};
    }

    // Constant-time comparison so a wrong PIN can't be narrowed down by timing
    // how far the comparison got before it failed.
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

    Server::HttpResponse handleUpload(const Server::UploadRequest& req)
    {
        auto cleanup      = []() { TransferStatus::end(); };
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

        std::u16string bodyPath = StringUtils::UTF8toUTF16(req.bodyPath.c_str());
        std::string metaJson;
        u64 fileOffset = 0;
        u64 fileLen    = 0;
        std::string error;
        if (!parseMultipartFile(bodyPath, req.bodyLength, boundary, metaJson, fileOffset, fileLen, error)) {
            cleanup();
            return {400, "application/json", "{\"ok\":false,\"error\":\"Bad upload\"}"};
        }

        auto meta = nlohmann::json::parse(metaJson, nullptr, false);
        if (meta.is_discarded()) {
            cleanup();
            return {400, "application/json", "{\"ok\":false,\"error\":\"Invalid meta\"}"};
        }

        std::string dataType   = meta.value("dataType", "save");
        std::string titleId    = meta.value("titleId", "");
        std::string titleName  = meta.value("titleName", "Unknown");
        std::string backupName = meta.value("backupName", "");
        bool isZip             = meta.value("isZip", false);
        setReceiverNotice("");
        if (backupName.empty()) {
            backupName = "Received_" + DateTime::dateTimeStr();
        }

        std::u16string basePath = StringUtils::UTF8toUTF16(dataType == "extdata" ? "/3ds/Checkpoint/extdata/" : "/3ds/Checkpoint/saves/");

        std::u16string destRoot;
        bool foundTitle   = false;
        bool mappedByName = false;
        u64 tid           = 0;
        if (!titleId.empty()) {
            tid = strtoull(titleId.c_str(), nullptr, 16);
        }
        if (tid != 0) {
            Title t;
            if (TitleCatalog::get().getTitleById(t, tid)) {
                destRoot   = (dataType == "extdata") ? t.extdataPath() : t.savePath();
                foundTitle = true;
            }
        }
        if (!foundTitle && !titleName.empty()) {
            Title t;
            if (TitleCatalog::get().getTitleByName(t, titleName)) {
                destRoot     = (dataType == "extdata") ? t.extdataPath() : t.savePath();
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
            destRoot = basePath + StringUtils::removeForbiddenCharacters(StringUtils::UTF8toUTF16(folder.c_str()));
            if (!io::directoryExists(Archive::sdmc(), destRoot)) {
                io::createDirectory(Archive::sdmc(), destRoot);
            }
            setReceiverNotice("Warning: unknown title. Stored in:\n" + StringUtils::UTF16toUTF8(destRoot));
            Logging::warning("Received backup for unknown title {} (stored under {}).", titleId, StringUtils::UTF16toUTF8(destRoot));
        }

        std::u16string backupRoot = destRoot + StringUtils::UTF8toUTF16("/") +
                                    StringUtils::removeForbiddenCharacters(StringUtils::UTF8toUTF16(backupName.c_str())) +
                                    StringUtils::UTF8toUTF16("/");
        if (io::directoryExists(Archive::sdmc(), backupRoot)) {
            io::deleteFolderRecursively(Archive::sdmc(), backupRoot);
        }
        io::createDirectory(Archive::sdmc(), backupRoot);

        if (isZip) {
            TransferStatus::beginNetwork("Extracting package", fileLen);

            std::string extractError;
            bool extracted = extractZip(bodyPath, fileOffset, fileLen, backupRoot, extractError);
            if (!extracted) {
                // A failed (or cancelled) extract leaves a half-populated backup
                // folder behind; remove it so the receiver never keeps a backup
                // it can't trust.
                io::deleteFolderRecursively(Archive::sdmc(), backupRoot);
                cleanup();
                std::string message = extractError.empty() ? "Failed to extract package." : extractError;
                return {500, "application/json", "{\"ok\":false,\"error\":\"" + message + "\"}"};
            }
            TransferStatus::setBytesDone(fileLen);
        }
        else {
            std::string fileName = meta.value("fileName", "");
            if (fileName.empty()) {
                fileName = "received.bin";
            }
            std::u16string safeFileName  = StringUtils::removeForbiddenCharacters(StringUtils::UTF8toUTF16(fileName.c_str()));
            std::string safeFileNameUtf8 = StringUtils::UTF16toUTF8(safeFileName);
            if (safeFileNameUtf8.empty()) {
                safeFileNameUtf8 = "received.bin";
                safeFileName     = StringUtils::UTF8toUTF16("received.bin");
            }
            ensureDirectoryPath(backupRoot, safeFileNameUtf8);
            std::u16string outputPath = backupRoot + safeFileName;

            // Copy the file-part byte range straight out of the streamed body.
            FSStream input(Archive::sdmc(), bodyPath, FS_OPEN_READ);
            FSStream output(Archive::sdmc(), outputPath, FS_OPEN_WRITE, (u32)fileLen);
            if (!input.good() || !output.good()) {
                if (input.good()) {
                    input.close();
                }
                if (output.good()) {
                    output.close();
                }
                io::deleteFolderRecursively(Archive::sdmc(), backupRoot);
                cleanup();
                return {500, "application/json", "{\"ok\":false,\"error\":\"Failed to store file\"}"};
            }
            input.offset((u32)fileOffset);
            static const u32 kBuf = 0x40000;
            std::unique_ptr<u8[]> buf(new u8[kBuf]);
            u64 remaining = fileLen;
            bool copyOk   = true;
            while (remaining > 0) {
                if (TransferStatus::cancelRequested()) {
                    copyOk = false;
                    break;
                }
                u32 chunk = remaining > kBuf ? kBuf : (u32)remaining;
                u32 rd    = input.read(buf.get(), chunk);
                if (rd == 0) {
                    copyOk = false;
                    break;
                }
                output.write(buf.get(), rd);
                remaining -= rd;
                TransferStatus::addBytesDone(rd);
            }
            input.close();
            output.close();
            if (!copyOk) {
                io::deleteFolderRecursively(Archive::sdmc(), backupRoot);
                cleanup();
                return {500, "application/json", "{\"ok\":false,\"error\":\"Failed to store file\"}"};
            }
        }

        cleanup();

        if (!mappedByName && foundTitle) {
            setReceiverNotice("");
        }
        setReceiverCompletedName(backupName);
        g_receiverCompleted.store(true);
        g_pendingRefresh.store(true);

        nlohmann::json resp;
        resp["ok"]        = true;
        resp["savedPath"] = StringUtils::UTF16toUTF8(backupRoot);
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

    // Streams the store-only ZIP straight into the socket, so each backup byte
    // is read from SD exactly once (the old path staged a temp zip: read +
    // write + re-read). File CRCs are unknown until the data has been sent, so
    // file entries set general-purpose flag bit 3 and carry the real CRC in a
    // signed 16-byte data descriptor after the data; the local header still
    // holds the real sizes (store-only makes them known upfront), which is
    // what the console receivers rely on to frame each entry. Total byte
    // count must match zipStreamSize exactly for Content-Length to hold.
    bool sendZipStream(int sock, const std::vector<FileEntry>& files, const std::vector<std::string>& dirs, bool& cancelled)
    {
        cancelled = false;
        initCrc();

        std::vector<ZipEntry> central;
        central.reserve(dirs.size() + files.size());
        u32 offset = 0;

        auto sendChunk = [&](const void* data, u32 n) -> bool {
            if (!sendAll(sock, data, n)) {
                return false;
            }
            offset += n;
            TransferStatus::addBytesDone(n);
            return true;
        };

        for (const auto& dir : dirs) {
            ZipEntry centralEntry;
            centralEntry.name        = dir;
            centralEntry.crc         = 0;
            centralEntry.size        = 0;
            centralEntry.offset      = offset;
            centralEntry.isDirectory = true;

            std::string hdr;
            hdr.reserve(30 + dir.size());
            appendLe32(hdr, 0x04034b50);
            appendLe16(hdr, 20);
            appendLe16(hdr, 0);
            appendLe16(hdr, 0);
            appendLe16(hdr, 0);
            appendLe16(hdr, 0);
            appendLe32(hdr, 0);
            appendLe32(hdr, 0);
            appendLe32(hdr, 0);
            appendLe16(hdr, (u16)dir.size());
            appendLe16(hdr, 0);
            hdr.append(dir);
            if (!sendChunk(hdr.data(), hdr.size())) {
                return false;
            }

            central.push_back(centralEntry);
        }

        static const u32 kBuf = 0x40000;
        std::unique_ptr<u8[]> buf(new u8[kBuf]);

        for (const auto& entry : files) {
            ZipEntry centralEntry;
            centralEntry.name        = entry.relPath;
            centralEntry.crc         = 0;
            centralEntry.size        = entry.size;
            centralEntry.offset      = offset;
            centralEntry.isDirectory = false;

            std::string hdr;
            hdr.reserve(30 + entry.relPath.size());
            appendLe32(hdr, 0x04034b50);
            appendLe16(hdr, 20);
            appendLe16(hdr, 0x0008); // flag bit 3: CRC in the data descriptor
            appendLe16(hdr, 0);
            appendLe16(hdr, 0);
            appendLe16(hdr, 0);
            appendLe32(hdr, 0); // CRC, carried by the data descriptor instead
            appendLe32(hdr, entry.size);
            appendLe32(hdr, entry.size);
            appendLe16(hdr, (u16)entry.relPath.size());
            appendLe16(hdr, 0);
            hdr.append(entry.relPath);
            if (!sendChunk(hdr.data(), hdr.size())) {
                return false;
            }

            FSStream input(Archive::sdmc(), entry.absPath, FS_OPEN_READ);
            if (!input.good()) {
                return false;
            }
            u32 crc       = 0xFFFFFFFFu;
            u32 remaining = entry.size;
            while (remaining > 0) {
                if (TransferStatus::cancelRequested()) {
                    input.close();
                    cancelled = true;
                    return false;
                }
                u32 chunk = remaining > kBuf ? kBuf : remaining;
                u32 rd    = input.read(buf.get(), chunk);
                if (rd == 0) {
                    // Short read: the promised sizes can no longer be met, so
                    // the transfer must fail rather than desync the stream.
                    input.close();
                    return false;
                }
                crc = updateCrc(crc, buf.get(), rd);
                if (!sendChunk(buf.get(), rd)) {
                    input.close();
                    return false;
                }
                remaining -= rd;
            }
            input.close();
            crc ^= 0xFFFFFFFFu;

            std::string desc;
            desc.reserve(16);
            appendLe32(desc, 0x08074b50);
            appendLe32(desc, crc);
            appendLe32(desc, entry.size);
            appendLe32(desc, entry.size);
            if (!sendChunk(desc.data(), desc.size())) {
                return false;
            }

            centralEntry.crc = crc;
            central.push_back(centralEntry);
        }

        u32 centralOffset = offset;
        std::string tail;
        for (const auto& entry : central) {
            appendLe32(tail, 0x02014b50);
            appendLe16(tail, 20);
            appendLe16(tail, 20);
            appendLe16(tail, entry.isDirectory ? 0 : 0x0008);
            appendLe16(tail, 0);
            appendLe16(tail, 0);
            appendLe16(tail, 0);
            appendLe32(tail, entry.crc);
            appendLe32(tail, entry.size);
            appendLe32(tail, entry.size);
            appendLe16(tail, (u16)entry.name.size());
            appendLe16(tail, 0);
            appendLe16(tail, 0);
            appendLe16(tail, 0);
            appendLe16(tail, 0);
            appendLe32(tail, entry.isDirectory ? (((u32)FS_ATTRIBUTE_DIRECTORY) << 16) : 0);
            appendLe32(tail, entry.offset);
            tail.append(entry.name);
        }

        u32 centralSize = (u32)tail.size();
        appendLe32(tail, 0x06054b50);
        appendLe16(tail, 0);
        appendLe16(tail, 0);
        appendLe16(tail, (u16)central.size());
        appendLe16(tail, (u16)central.size());
        appendLe32(tail, centralSize);
        appendLe32(tail, centralOffset);
        appendLe16(tail, 0);

        return sendChunk(tail.data(), tail.size());
    }
}

void Transfer::sweepTempFiles(void)
{
    for (const char* leftover : {TEMP_UPLOAD, TEMP_ZIP_RECV_LEGACY}) {
        std::u16string p = StringUtils::UTF8toUTF16(leftover);
        if (io::fileExists(Archive::sdmc(), p)) {
            FSUSER_DeleteFile(Archive::sdmc(), fsMakePath(PATH_UTF16, p.data()));
            Logging::info("Removed leftover {} from a previous run.", leftover);
        }
    }

    const std::u16string root = StringUtils::UTF8toUTF16("/3ds/Checkpoint/");
    Directory dir(Archive::sdmc(), root);
    if (!dir.good()) {
        return;
    }
    const std::string prefix = "transfer_send_";
    const std::string suffix = ".zip";
    for (size_t i = 0, sz = dir.size(); i < sz; i++) {
        if (dir.folder(i)) {
            continue;
        }
        std::string name = StringUtils::UTF16toUTF8(dir.entry(i));
        if (name.size() > prefix.size() + suffix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            std::u16string path = root + dir.entry(i);
            FSUSER_DeleteFile(Archive::sdmc(), fsMakePath(PATH_UTF16, path.data()));
            Logging::info("Removed leftover {} from a previous run.", name);
        }
    }
}

bool Transfer::startReceiver(std::string& outError)
{
    if (!Server::isRunning()) {
        outError = "HTTP server not available.";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_receiverMutex);
        if (g_receiverRunning) {
            return true;
        }
    }

    srand((unsigned int)osGetTime());
    int pin           = 1000 + (rand() % 9000);
    std::string token = StringUtils::format("%04d", pin);
    std::string ip    = Server::getAddress();
    setReceiverNotice("");
    setReceiverCompletedName("");
    g_receiverCompleted.store(false);
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

Transfer::SendOutcome Transfer::sendBackup(const Title& title, const std::u16string& backupPath, const std::string& backupName,
    const std::string& dataType, const std::string& ip, u16 port, const std::string& token)
{
    // Every exit path must clear the transfer modal; the scope guard makes
    // that hold for each early return below.
    struct StatusGuard {
        ~StatusGuard() { TransferStatus::end(); }
    } statusGuard;

    std::vector<FileEntry> files;
    std::vector<std::string> dirs;
    collectFiles(Archive::sdmc(), backupPath, StringUtils::UTF8toUTF16(""), files, &dirs);
    if (files.empty() && dirs.empty()) {
        return SendOutcome{false, SendStage::EmptyBackup, ""};
    }

    // Multi-file backups are zipped on the fly by sendZipStream — no staged
    // temp zip on SD; the exact zip size is known upfront because entries are
    // store-only.
    bool isZip = files.size() != 1 || !dirs.empty();
    std::u16string payloadPath;
    std::string payloadName;
    u32 payloadSize = 0;

    if (isZip) {
        payloadName = "backup.zip";
        payloadSize = zipStreamSize(files, dirs);
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
    meta["titleName"]      = title.shortDescription();
    meta["dataType"]       = dataType;
    meta["backupName"]     = backupName;
    meta["isZip"]          = isZip;
    meta["fileBytesTotal"] = payloadSize;
    meta["fileName"]       = payloadName;
    meta["timestamp"]      = DateTime::logDateTime();

    std::string metaStr  = meta.dump();
    std::string boundary = StringUtils::format("----checkpoint-boundary-%llu", (unsigned long long)osGetTime());

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

    if (ok && isZip) {
        bool cancelled = false;
        if (!sendZipStream(sock, files, dirs, cancelled)) {
            if (cancelled) {
                // The scope guard closes the socket, dropping the connection,
                // which the receiver treats as an aborted request.
                return SendOutcome{false, SendStage::Cancelled, ""};
            }
            ok = false;
        }
    }
    else if (ok) {
        FSStream input(Archive::sdmc(), payloadPath, FS_OPEN_READ);
        if (!input.good()) {
            ok = false;
        }
        else {
            static const u32 kBuf = 0x40000;
            std::unique_ptr<u8[]> buf(new u8[kBuf]);
            while (!input.eof()) {
                if (TransferStatus::cancelRequested()) {
                    input.close();
                    // The scope guard closes the socket, dropping the connection,
                    // which the receiver treats as an aborted request.
                    return SendOutcome{false, SendStage::Cancelled, ""};
                }
                u32 rd = input.read(buf.get(), kBuf);
                if (rd == 0) {
                    break;
                }
                if (!sendAll(sock, buf.get(), rd)) {
                    ok = false;
                    break;
                }
                TransferStatus::addBytesDone(rd);
            }
            input.close();
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
