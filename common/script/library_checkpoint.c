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

#include "checkpoint_api.h"
#include "interpreter.h"

// picoc calls this once per interpreter to register everything that is not part
// of its own stdlib. Checkpoint's native API is one include, "checkpoint.h": the
// function table below plus the struct definitions scripts need to name.

void CheckpointSetupFunc(Picoc* pc)
{
    (void)pc;
}

// clang-format off
struct LibraryFunction CheckpointFunctions[] =
{
    // Phase 0: no native bindings yet.
    { NULL, NULL }
};
// clang-format on

void PlatformLibraryInit(Picoc* pc)
{
    IncludeRegister(pc, "checkpoint.h", &CheckpointSetupFunc, &CheckpointFunctions[0],
        "struct directory { int count; char** files; };"
        "struct JSON { void* dummy; };");
}
