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

#include "ReceiveOverlay.hpp"
#include "backupsize.hpp"
#include "colors.hpp"
#include "gfx.hpp"
#include "gfxutils.hpp"
#include "i18n.hpp"
#include "shapes.hpp"
#include "titlecatalog.hpp"
#include "transfer.hpp"
#include "transferstatus.hpp"
#include "uikit.hpp"

namespace {
    constexpr int CARD_W = 560, CARD_H = 300;
    constexpr int CARD_X = (1280 - CARD_W) / 2, CARD_Y = (720 - CARD_H) / 2;

    // Number of frames B must be held to trigger a cancel (~0.75s at 60fps), so a
    // stray tap can't kill a long transfer.
    constexpr int CANCEL_HOLD_FRAMES = 45;

    void drawCentered(int size, int y, Color color, const std::string& text)
    {
        u32 tw, th;
        Gfx::GetTextDimensions(size, text.c_str(), &tw, &th);
        Gfx::DrawText(size, CARD_X + (CARD_W - (int)tw) / 2, y, color, text.c_str());
    }
}

void ReceiveOverlay::draw(void) const
{
    Gfx::DrawRect(0, 0, 1280, 720, COLOR_SCRIM);
    Shapes::cardRound(CARD_X, CARD_Y, CARD_W, CARD_H, 0, COLOR_SURFACE, COLOR_STROKE2, 1);

    TransferSnapshot ts      = TransferStatus::snapshot();
    const bool networkActive = ts.active && ts.kind == TransferKind::Network;
    const std::string notice = Transfer::receiverNotice();

    drawCentered(24, CARD_Y + 20, COLOR_TEXT, i18n::t("transfer.receive"));

    // Completion: a transfer finished and nothing is in flight.
    if (Transfer::receiverHasCompleted() && !networkActive) {
        drawCentered(20, CARD_Y + 74, COLOR_TEXT, i18n::t("transfer.file_received"));
        std::string name = Transfer::receiverCompletedName();
        if (name.empty()) {
            name = i18n::t("transfer.unnamed");
        }
        drawCentered(16, CARD_Y + 112, COLOR_TEXT2, trimToFit(name, CARD_W - 40, 16));
        if (!notice.empty()) {
            drawCentered(14, CARD_Y + 150, COLOR_TEXT2, trimToFit(notice, CARD_W - 40, 14));
        }
        drawCentered(15, CARD_Y + CARD_H - 40, COLOR_TEXT2, i18n::t("transfer.refresh_hint"));
        return;
    }

    // Connection details.
    int y = CARD_Y + 74;
    if (Transfer::receiverRunning()) {
        drawCentered(18, y, COLOR_TEXT, "IP: " + Transfer::receiverIp());
        y += 34;
        drawCentered(18, y, COLOR_TEXT, "Port: " + std::to_string(Transfer::receiverPort()));
        y += 34;
        drawCentered(22, y, COLOR_ACCENT, "PIN: " + Transfer::receiverToken());
        y += 40;
    }
    else {
        drawCentered(18, y, COLOR_TEXT2, i18n::t("transfer.receiver_stopped"));
        y += 40;
    }

    // In-flight download progress.
    if (networkActive) {
        std::string mode = ts.mode.empty() ? i18n::t("transfer.downloading") : ts.mode;
        drawCentered(16, y, COLOR_TEXT2, mode);
        y += 28;
        const int barX = CARD_X + 30, barW = CARD_W - 60, barH = 16;
        float frac = ts.bytesTotal > 0 ? (float)ts.bytesDone / (float)ts.bytesTotal : 0.0f;
        if (frac > 1.0f) {
            frac = 1.0f;
        }
        Shapes::fillRound(barX, y, barW, barH, 0, COLOR_FILL2);
        if (frac > 0.0f) {
            Shapes::fillRound(barX, y, (int)(barW * frac), barH, 0, COLOR_ACCENT);
        }
        drawCentered(14, y + barH + 6, COLOR_TEXT2, TransferStatus::bytesToMB(ts.bytesDone, ts.bytesTotal));
        y += barH + 30;
    }

    if (!notice.empty()) {
        drawCentered(14, y, COLOR_TEXT2, trimToFit(notice, CARD_W - 40, 14));
    }

    std::string hint;
    if (networkActive) {
        hint = TransferStatus::cancelRequested() ? i18n::t("transfer.cancelling") : i18n::t("transfer.cancel_hint");
    }
    else {
        hint = i18n::t("transfer.close_hint");
    }
    drawCentered(15, CARD_Y + CARD_H - 40, COLOR_TEXT2, hint);
}

void ReceiveOverlay::closeReceiver(void)
{
    Transfer::stopReceiver();
    // Refresh the backup list of the title that just received (no-op for id 0 /
    // an unknown title, which won't appear until the next full reload).
    u64 id = Transfer::consumeCompletedTitleId();
    Transfer::consumePendingRefresh();
    if (id != 0) {
        TitleCatalog::get().refreshDirectories(id);
        BackupSizeCache::get().invalidate(id);
    }
    Transfer::clearReceiverCompletion();
    Transfer::clearReceiverNotice();
    screen.removeOverlay();
}

void ReceiveOverlay::update(const InputState& input)
{
    // While an upload is in flight, B means "cancel the transfer", never "close
    // the overlay": the receiver must stay registered until the server thread has
    // abandoned the request, so closing waits for the transfer to end.
    TransferSnapshot ts = TransferStatus::snapshot();
    if (ts.active && ts.kind == TransferKind::Network) {
        if (input.kHeld & HidNpadButton_B) {
            if (++mCancelHoldFrames >= CANCEL_HOLD_FRAMES && !TransferStatus::cancelRequested()) {
                TransferStatus::requestCancel();
            }
        }
        else {
            mCancelHoldFrames = 0;
        }
        return; // no close path while active
    }
    mCancelHoldFrames = 0;

    if (Transfer::receiverHasCompleted() && (input.kDown & HidNpadButton_A)) {
        closeReceiver();
        return;
    }
    if (input.kDown & HidNpadButton_B) {
        closeReceiver();
    }
}
