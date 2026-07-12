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

#include "scriptcatalog.hpp"
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>

namespace {
    void scanDir(const std::string& dir, bool universal, std::vector<ScriptCatalog::Entry>& out)
    {
        DIR* d = opendir(dir.c_str());
        if (!d) {
            return;
        }

        const size_t first = out.size();
        while (struct dirent* ent = readdir(d)) {
            const std::string name = ent->d_name;
            if (name.size() <= 2 || name.compare(name.size() - 2, 2, ".c") != 0) {
                continue;
            }
            // Only files directly in the directory count; subfolders are assets.
            const std::string path = dir + "/" + name;
            struct stat st;
            if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }
            out.push_back({name.substr(0, name.size() - 2), path, universal});
        }
        closedir(d);

        std::sort(out.begin() + first, out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    }
}

std::vector<ScriptCatalog::Entry> ScriptCatalog::scan(const std::string& universalDir, const std::string& specificDir)
{
    std::vector<Entry> entries;
    scanDir(universalDir, true, entries);
    if (!specificDir.empty()) {
        scanDir(specificDir, false, entries);
    }
    return entries;
}
