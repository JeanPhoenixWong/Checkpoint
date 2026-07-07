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

#include "transferjob.hpp"
#include "progress.hpp"
#include "transferstatus.hpp"

namespace {
    constexpr size_t WORKER_STACK = 0x10000;
    constexpr int WORKER_PRIO     = 0x2C; // same as the network thread; the copy is IO-bound and yields on fs calls
}

void TransferJob::enqueueBackup(Title title, std::string dstPath, std::string successMsg)
{
    std::lock_guard<std::mutex> lock(mMutex);
    WorkItem item;
    item.kind       = Kind::Backup;
    item.title      = std::move(title);
    item.path       = std::move(dstPath);
    item.successMsg = std::move(successMsg);
    mQueue.push_back(std::move(item));
}

void TransferJob::enqueueRestore(Title title, std::string srcPath, std::string successMsg)
{
    std::lock_guard<std::mutex> lock(mMutex);
    WorkItem item;
    item.kind       = Kind::Restore;
    item.title      = std::move(title);
    item.path       = std::move(srcPath);
    item.successMsg = std::move(successMsg);
    mQueue.push_back(std::move(item));
}

void TransferJob::enqueueSend(
    Title title, std::string backupPath, std::string backupName, std::string dataType, std::string ip, u16 port, std::string token)
{
    std::lock_guard<std::mutex> lock(mMutex);
    WorkItem item;
    item.kind       = Kind::Send;
    item.title      = std::move(title);
    item.path       = std::move(backupPath);
    item.successMsg = "Transfer completed.";
    item.backupName = std::move(backupName);
    item.dataType   = std::move(dataType);
    item.ip         = std::move(ip);
    item.port       = port;
    item.token      = std::move(token);
    mQueue.push_back(std::move(item));
}

void TransferJob::start(void)
{
    if (mState.load() == State::Running) {
        return;
    }

    size_t total;
    bool anyLocal = false;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        total = mQueue.size();
        for (const auto& item : mQueue) {
            if (item.kind != Kind::Send) {
                anyLocal = true;
                break;
            }
        }
    }
    if (total == 0) {
        return;
    }

    // Sends drive TransferStatus themselves (beginNetwork inside sendBackup); only
    // local copies use the batch counter and the local-copy modal.
    if (anyLocal) {
        TransferStatus::beginLocalBatch(total);
    }
    mCancelRequested.store(false);
    mState.store(State::Running);

    if (R_FAILED(threadCreate(&mThread, TransferJob::runThread, this, nullptr, WORKER_STACK, WORKER_PRIO, -2))) {
        // Could not spawn the worker: fall back to running the batch inline so the
        // saves still happen (the UI freezes for this run, as it did before).
        run();
        return;
    }
    if (R_FAILED(threadStart(&mThread))) {
        // The thread was created but never started: close the handle so it isn't
        // leaked, then run the batch inline.
        threadClose(&mThread);
        run();
        return;
    }
    mThreadValid = true;
}

void TransferJob::runThread(void* arg)
{
    static_cast<TransferJob*>(arg)->run();
}

void TransferJob::run(void)
{
    size_t done = 0;
    std::vector<u64> refreshIds;
    JobResult last;

    for (;;) {
        WorkItem item;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mQueue.empty()) {
                break;
            }
            item = std::move(mQueue.front());
            mQueue.pop_front();
        }

        if (item.kind == Kind::Send) {
            Transfer::SendOutcome out = Transfer::sendBackup(item.title, item.path, item.backupName, item.dataType, item.ip, item.port, item.token);
            last                      = JobResult{};
            last.isRestore            = false;
            last.ok                   = out.ok;
            last.successMsg           = item.successMsg;
            last.send                 = out;
            done++;
            continue;
        }

        TransferStatus::setSaveCount(done);

        const bool isRestore = item.kind == Kind::Restore;
        // Only a backup item's sink is given the cancel flag, so cancelled() is
        // structurally always false while restoring a save.
        UiProgressSink sink(isRestore ? nullptr : &mCancelRequested);
        io::IoOutcome out = isRestore ? io::restore(item.title, item.path, sink) : io::backup(item.title, item.path, sink);
        if (out.ok && !isRestore) {
            refreshIds.push_back(item.title.id());
        }
        last = JobResult{isRestore, out.ok, out.res, out.stage, item.successMsg, {}, out.cancelled, std::nullopt};
        done++;

        if (out.cancelled) {
            // Drop the rest of the queue: a cancel ends the whole batch, not just
            // the save that was mid-copy.
            std::lock_guard<std::mutex> lock(mMutex);
            mQueue.clear();
            break;
        }
    }

    last.refreshIds = std::move(refreshIds);
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mResult = std::move(last);
    }

    TransferStatus::end();
    mState.store(State::Done);
}

std::optional<TransferJob::JobResult> TransferJob::takeResult(void)
{
    if (mState.load() != State::Done) {
        return std::nullopt;
    }

    if (mThreadValid) {
        threadWaitForExit(&mThread);
        threadClose(&mThread);
        mThreadValid = false;
    }

    JobResult result;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        result = std::move(mResult);
    }
    mState.store(State::Idle);
    return result;
}

void TransferJob::join(void)
{
    if (mThreadValid) {
        threadWaitForExit(&mThread);
        threadClose(&mThread);
        mThreadValid = false;
    }
}
