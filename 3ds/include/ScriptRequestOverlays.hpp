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

#ifndef SCRIPTREQUESTOVERLAYS_HPP
#define SCRIPTREQUESTOVERLAYS_HPP

#include "ListPickerOverlay.hpp"
#include "clickable.hpp"
#include <memory>
#include <string>
#include <vector>

// The overlays MainScreen raises for a script's blocking UI requests. Each one
// answers exactly one pending ScriptUiBridge request: it responds *before*
// dismissing itself, so the script thread wakes as the overlay goes away.
// Confirm reuses YesNoOverlay and Keyboard runs swkbd inline; only the shapes
// with no existing widget live here.

// gui_message: MessageOverlay's chrome, but the dismissal answers the bridge.
class ScriptMessageOverlay : public Overlay {
public:
    ScriptMessageOverlay(Screen& screen, const std::string& text);
    void drawTop(void) const override;
    void drawBottom(void) const override;
    void update(const InputState& input) override;

private:
    static constexpr float SIZE = 0.6f;
    std::string mText;
    u32 mPosx, mPosy;
    std::unique_ptr<Clickable> mButton;
};

// gui_pick_one: A responds with the row index, B with -1.
class ScriptPickOneOverlay : public ListPickerOverlay {
public:
    ScriptPickOneOverlay(Screen& screen, const std::string& prompt, std::vector<std::string> items);
    void update(const InputState& input) override;

private:
    int rowCount(void) const override;
    void drawEmptyMessage(void) const override;
    void drawRowContent(int k, int rowY, bool selected) const override;
    std::string bottomHints(void) const override;

    std::vector<std::string> mItems;
};

// gui_pick_many: A toggles the row, START confirms the set, B cancels.
class ScriptPickManyOverlay : public ListPickerOverlay {
public:
    ScriptPickManyOverlay(Screen& screen, const std::string& prompt, std::vector<std::string> items, std::vector<bool> preselected);
    void update(const InputState& input) override;

private:
    int rowCount(void) const override;
    void drawEmptyMessage(void) const override;
    void drawRowContent(int k, int rowY, bool selected) const override;
    std::string bottomHints(void) const override;

    std::vector<std::string> mItems;
    std::vector<bool> mSelected;
};

#endif
