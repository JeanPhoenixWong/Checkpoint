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

#ifndef SCRIPTCATALOG_HPP
#define SCRIPTCATALOG_HPP

#include <string>
#include <vector>

// Script discovery on disk. Scripts are the *.c files sitting directly in a
// scan directory; subdirectories are a script's assets and are not descended
// into. The caller supplies the directories (Paths owns the layout rules), so
// this stays platform-neutral.
namespace ScriptCatalog {
    struct Entry {
        std::string name; // filename without ".c"
        std::string path; // full path, ready for ScriptRunner::start
        bool universal = true;
    };

    // Universal scripts first, then the title's specific ones (skipped when
    // `specificDir` is empty), each section sorted alphabetically. A missing
    // directory contributes nothing.
    std::vector<Entry> scan(const std::string& universalDir, const std::string& specificDir);
}

#endif
