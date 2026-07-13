/*
 *   This file is part of Checkpoint
 *   Copyright (C) 2017-2025 Bernardo Giordano, FlagBrew
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

#include "KeyboardManager.hpp"
#include "backupsize.hpp"
#include <vector>

KeyboardManager::KeyboardManager(void)
{
    systemKeyboardAvailable = false;
    if (appletGetAppletType() == AppletType_Application) {
        SwkbdConfig kbd;
        res = swkbdCreate(&kbd, 0);
        if (R_SUCCEEDED(res)) {
            systemKeyboardAvailable = true;
            swkbdClose(&kbd);
        }
    }
}

std::pair<bool, std::string> KeyboardManager::keyboard(const std::string& suggestion)
{
    if (systemKeyboardAvailable) {
        // The size-cache walk floods the FS service and delays the swkbd applet
        // launch by the walk's full remaining duration; hold it while the
        // keyboard session runs.
        BackupSizeCache::PauseGuard pauseWalk;
        SwkbdConfig kbd;
        if (R_SUCCEEDED(swkbdCreate(&kbd, 0))) {
            swkbdConfigMakePresetDefault(&kbd);
            swkbdConfigSetInitialText(&kbd, suggestion.c_str());
            swkbdConfigSetStringLenMax(&kbd, CUSTOM_PATH_LEN);
            char tmpoutstr[CUSTOM_PATH_LEN] = {0};
            Result res                      = swkbdShow(&kbd, tmpoutstr, CUSTOM_PATH_LEN);
            swkbdClose(&kbd);
            if (R_SUCCEEDED(res)) {
                return std::make_pair(true, std::string(tmpoutstr));
            }
        }
    }
    return std::make_pair(false, suggestion);
}

std::string KeyboardManager::text(const std::string& suggestion, const std::string& hint, size_t maxLen)
{
    if (!systemKeyboardAvailable || maxLen == 0) {
        return "";
    }

    // Same PauseGuard rationale as keyboard(): the size-cache walk delays the
    // swkbd applet launch by the walk's full remaining duration.
    BackupSizeCache::PauseGuard pauseWalk;
    SwkbdConfig kbd;
    std::string out;
    if (R_SUCCEEDED(swkbdCreate(&kbd, 0))) {
        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetGuideText(&kbd, hint.c_str());
        swkbdConfigSetInitialText(&kbd, suggestion.c_str());
        swkbdConfigSetStringLenMax(&kbd, maxLen);
        std::vector<char> buf(maxLen * 4 + 1, 0); // UTF-8 worst case per char
        Result rc = swkbdShow(&kbd, buf.data(), buf.size());
        swkbdClose(&kbd);
        if (R_SUCCEEDED(rc)) {
            out = buf.data();
        }
    }
    return out;
}