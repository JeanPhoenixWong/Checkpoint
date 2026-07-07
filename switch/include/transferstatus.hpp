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

#ifndef TRANSFERSTATUS_HPP
#define TRANSFERSTATUS_HPP

#include <string>
#include <switch.h>

// Whether the active transfer is an on-device file copy (the blocking
// backup/restore loop) or a network send/receive. The UI renders a different
// modal for each.
enum class TransferKind { Local, Network };

// A flat, immutable copy of the transfer progress, handed to the UI so it never
// touches the live state directly. Local copies fill in the file counters;
// network send/receive fills in the byte counters (the wireless-transfer feature
// ported from the 3DS build reports through the same state).
struct TransferSnapshot {
    bool active       = false;
    TransferKind kind = TransferKind::Local;
    // Whether this run may be aborted mid-flight (backup yes; restore no — a
    // restore aborted mid-write could leave a truncated save). The UI reads this
    // instead of comparing the mode string.
    bool cancellable = false;
    std::string mode;

    // Local copy: file-by-file copy, run on the TransferJob worker thread.
    std::string currentFile;
    u64 currentFileSize   = 0;
    u64 currentFileOffset = 0;
    size_t copyCount      = 0; // files done within the current save
    size_t copyTotal      = 0; // files in the current save
    size_t saveCount      = 0; // saves done within the batch
    size_t saveTotal      = 0; // saves in the batch (1 for a single backup/restore)

    // Network: HTTP send/receive byte counters.
    u64 bytesDone  = 0;
    u64 bytesTotal = 0;
};

// The single owner of "a save transfer is in progress" state, replacing the loose
// globals that used to live in main.hpp. All access is mutex-guarded: the figures
// are written by the TransferJob worker thread while the UI thread reads them, so
// it always renders from a consistent snapshot rather than half-written counters.
namespace TransferStatus {
    // Local copy lifecycle. The batch (one or more saves) is framed by the
    // TransferJob: beginLocalBatch raises the modal and owns the active flag;
    // setSaveCount advances the per-save bar before each save. Within a save,
    // UiProgressSink drives beginLocalRun (per-save file run) and the file
    // figures. end() lowers the modal once the whole batch is done.
    void beginLocalBatch(size_t totalSaves);
    void setSaveCount(size_t count);
    void beginLocalRun(const std::string& mode, size_t totalFiles, bool cancellable);
    void startFile(const std::string& name, u64 size);
    void setFileOffset(u64 offset);
    void finishFile();

    // Network lifecycle, driven by the HTTP server thread and the sender.
    // beginNetwork starts a transfer of `totalBytes` labelled `mode`.
    void beginNetwork(const std::string& mode, u64 totalBytes);
    void setMode(const std::string& mode);
    void setBytes(u64 done, u64 total);
    void setBytesDone(u64 done);
    void addBytesDone(u64 delta);

    // Ends the run (success or failure); clears the active flag.
    void end();

    // Cooperative cancellation for the network send/receive path. requestCancel()
    // is set by the UI thread ("hold B to cancel"); the sender/server loops poll
    // cancelRequested() at chunk granularity and unwind through their normal
    // failure paths. Cleared whenever a run begins (begin*()) and when a run ends
    // (end()), so a stale request can never bleed into the next transfer.
    void requestCancel();
    bool cancelRequested();

    // Thread-safe flat copy for the UI to render from.
    TransferSnapshot snapshot();

    // Formats a "done / total" byte pair in MB for the transfer progress UI.
    std::string bytesToMB(u64 done, u64 total);
}

#endif // TRANSFERSTATUS_HPP
