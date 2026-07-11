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

#include "backupsize.hpp"
#include "directory.hpp"
#include "io.hpp"
#include "logging.hpp"
#include <sys/stat.h>

void BackupSizeCache::ensureWorker(void)
{
    // Caller holds mMutex. Spawn the single worker lazily on the first request.
    if (!mWorker.joinable() && !mStop.load()) {
        mWorker = std::thread([this]() { this->workerLoop(); });
    }
}

void BackupSizeCache::request(u64 id, const std::string& rootPath)
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (mStop.load() || mCache.count(id) != 0 || mPending.count(id) != 0) {
        return;
    }
    mPending.insert(id);
    mQueue.emplace_back(id, rootPath);
    ensureWorker();
    mCond.notify_one();
}

std::optional<u64> BackupSizeCache::total(u64 id)
{
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mCache.find(id);
    if (it == mCache.end()) {
        return std::nullopt;
    }
    return it->second.total;
}

std::optional<u64> BackupSizeCache::backupSize(u64 id, const std::string& fullPath)
{
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mCache.find(id);
    if (it == mCache.end()) {
        return std::nullopt;
    }
    auto bit = it->second.perBackup.find(fullPath);
    if (bit == it->second.perBackup.end()) {
        return std::nullopt;
    }
    return bit->second;
}

void BackupSizeCache::invalidate(u64 id)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mCache.erase(id);
    // If a compute for this id is in flight, tell it to discard its (now stale)
    // result so the next request recomputes against the current folders.
    if (mPending.count(id) != 0) {
        mDirty.insert(id);
    }
    mGeneration.fetch_add(1);
}

void BackupSizeCache::pause(void)
{
    mPauseCount.fetch_add(1);
}

void BackupSizeCache::resume(void)
{
    mPauseCount.fetch_sub(1);
    mCond.notify_all();
}

void BackupSizeCache::gate(void)
{
    if (mPauseCount.load() <= 0 || mStop.load()) {
        return;
    }
    std::unique_lock<std::mutex> lock(mMutex);
    mCond.wait(lock, [this]() { return mPauseCount.load() <= 0 || mStop.load(); });
}

void BackupSizeCache::shutdown(void)
{
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mStop.load()) {
            return;
        }
        mStop.store(true);
    }
    mCond.notify_all();
    if (mWorker.joinable()) {
        mWorker.join();
    }
}

void BackupSizeCache::workerLoop(void)
{
    // std::thread inherits the creator's priority (0x2C, same as the main thread)
    // and default core. Horizon does not time-slice same-priority threads on the
    // same core, so a long directory walk starves the UI until it blocks on fs
    // IPC. Drop below the main thread so the UI always preempts the walk.
    // Applications may only use priorities 0x2C-0x3B; anything else fails with
    // kernel InvalidPriority (0xE001), so take the lowest allowed one.
    const Result prioRc = svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
    if (R_FAILED(prioRc)) {
        Logging::warning("[sizecache] svcSetThreadPriority failed: 0x{:08X}", prioRc);
    }

    for (;;) {
        std::pair<u64, std::string> task;
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mCond.wait(lock, [this]() { return mStop.load() || !mQueue.empty(); });
            if (mStop.load()) {
                return;
            }
            // LIFO: take the most recently queued task. While browsing the title list
            // this computes the size for the title the user just landed on before the
            // now-unfocused ones queued earlier.
            task = std::move(mQueue.back());
            mQueue.pop_back();
        }
        compute(task.first, task.second);
    }
}

void BackupSizeCache::compute(u64 id, const std::string& rootPath)
{
    // Enumerate the immediate backup folders, summing each subtree (and keeping
    // the per-backup totals). walkSize does the recursive walk, honoring the
    // pause gate; the mStop check between folders lets a shutdown abort a long
    // scan promptly.
    Entry entry;
    std::string base = rootPath;
    if (!base.empty() && base.back() != '/') {
        base += "/";
    }

    Directory items(base);
    if (items.good()) {
        for (size_t i = 0, sz = items.size(); i < sz && !mStop.load(); i++) {
            gate();
            const std::string full = base + items.entry(i);
            if (items.folder(i)) {
                const u64 s = walkSize(full + "/");
                entry.total += s;
                entry.perBackup[full] = s;
            }
            else {
                struct stat st;
                if (stat(full.c_str(), &st) == 0) {
                    entry.total += (u64)st.st_size;
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(mMutex);
    mPending.erase(id);
    // Discard a partial result produced during shutdown, or one invalidated
    // while the walk was running.
    if (mStop.load() || mDirty.erase(id) != 0) {
        return;
    }
    mCache[id] = std::move(entry);
    mGeneration.fetch_add(1);
}

u64 BackupSizeCache::walkSize(const std::string& path)
{
    u64 total = 0;
    Directory items(path);
    if (!items.good()) {
        return 0;
    }
    std::string base = path;
    if (!base.empty() && base.back() != '/') {
        base += "/";
    }
    for (size_t i = 0, sz = items.size(); i < sz && !mStop.load(); i++) {
        gate();
        const std::string child = base + items.entry(i);
        if (items.folder(i)) {
            total += walkSize(child + "/");
        }
        else {
            struct stat st;
            if (stat(child.c_str(), &st) == 0) {
                total += (u64)st.st_size;
            }
        }
    }
    return total;
}
