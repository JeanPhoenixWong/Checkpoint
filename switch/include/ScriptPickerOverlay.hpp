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

#ifndef SCRIPTPICKEROVERLAY_HPP
#define SCRIPTPICKEROVERLAY_HPP

#include "Overlay.hpp"
#include "scriptcatalog.hpp"
#include <functional>
#include <string>
#include <vector>

// Modal script picker raised by the Scripts action: universal scripts first,
// then the selected title's specific ones (tagged with `titleTag`, its display
// name). A invokes onPick with the chosen entry after dismissing, B cancels.
// Running the pick (confirm prompt + ScriptRunner) is the caller's business.
class ScriptPickerOverlay : public Overlay {
public:
    ScriptPickerOverlay(Screen& screen, std::vector<ScriptCatalog::Entry> entries, const std::string& titleTag,
        std::function<void(const ScriptCatalog::Entry&)> onPick);
    void draw(void) const override;
    void update(const InputState& input) override;

private:
    std::vector<ScriptCatalog::Entry> mEntries;
    std::string mTitleTag;
    std::function<void(const ScriptCatalog::Entry&)> mOnPick;
    int mCursor = 0;
    int mScroll = 0;
};

#endif
