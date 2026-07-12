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

#include "scriptrunner.hpp"
#include "scriptengine.hpp"
#include "thread.hpp"

extern "C" {
#include "checkpoint_api.h"
}

bool ScriptRunner::start(std::string scriptPath, std::string displayName, std::string titleIdHex)
{
    if (mState.load() != State::Idle) {
        return false;
    }

    mPath       = std::move(scriptPath);
    mName       = std::move(displayName);
    mTitleIdHex = std::move(titleIdHex);
    mBridge.reset();

    mState.store(State::Running);
    if (!Threads::create(std::optional<size_t>(THREAD_STACK), ScriptRunner::runThread)) {
        mState.store(State::Idle);
        return false;
    }
    return true;
}

void ScriptRunner::runThread(void)
{
    get().run();
}

void ScriptRunner::run(void)
{
    ScriptEngine::Outcome out = ScriptEngine::run(mPath, {mTitleIdHex});
    // The run is over whatever the exit path was (return, exit(), parse-error
    // longjmp): reclaim any archive handles the script left open.
    ckpt_sav_close_all();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mOutcome = Outcome{mName, out.exitValue, std::move(out.output)};
    }
    mState.store(State::Done);
}

std::optional<ScriptRunner::Outcome> ScriptRunner::takeResult(void)
{
    if (mState.load() != State::Done) {
        return std::nullopt;
    }

    Outcome outcome;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        outcome = mOutcome;
    }
    mState.store(State::Idle);
    return outcome;
}
