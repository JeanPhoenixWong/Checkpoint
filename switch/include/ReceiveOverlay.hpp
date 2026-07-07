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

#ifndef RECEIVEOVERLAY_HPP
#define RECEIVEOVERLAY_HPP

#include "Overlay.hpp"

// Shows the wireless receiver's IP/port/PIN, live download progress, and the
// completion notice. Receiver lifetime == overlay lifetime: MainScreen starts
// the receiver before constructing this, and this stops it on close (B, or A
// after a completed transfer). While an upload is in flight B is captured as a
// hold-to-cancel instead of a close, so the receiver stays registered until the
// server thread has abandoned the request. Mirrors the 3DS ReceiveOverlay.
class ReceiveOverlay : public Overlay {
public:
    explicit ReceiveOverlay(Screen& screen) : Overlay(screen) {}
    ~ReceiveOverlay() = default;
    void draw(void) const override;
    void update(const InputState&) override;

private:
    void closeReceiver(void);
    int mCancelHoldFrames = 0;
};

#endif
