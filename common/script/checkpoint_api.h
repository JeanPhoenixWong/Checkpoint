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

#ifndef CHECKPOINT_API_H
#define CHECKPOINT_API_H

#include "picoc.h"

// Declarations of the native bindings scripts reach through <checkpoint.h>.
// Every binding has picoc's fixed signature; the prototypes scripts actually
// see live in the table in library_checkpoint.c, and the implementations are
// per-platform (3ds/source/script/checkpoint_api.cpp).

#ifdef __cplusplus
extern "C" {
#endif

#define CKPT_BINDING(name) void name(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)

/* titles (catalog index space = the Save list) */
CKPT_BINDING(ckpt_titles_count);
CKPT_BINDING(ckpt_title_find);
CKPT_BINDING(ckpt_title_id);
CKPT_BINDING(ckpt_title_name);
CKPT_BINDING(ckpt_title_product_code);
CKPT_BINDING(ckpt_title_is_cart);
CKPT_BINDING(ckpt_title_has_save);
CKPT_BINDING(ckpt_title_has_extdata);
CKPT_BINDING(ckpt_title_backup_path);

/* sd card */
CKPT_BINDING(ckpt_read_directory);
CKPT_BINDING(ckpt_delete_directory);
CKPT_BINDING(ckpt_sd_mkdirs);
CKPT_BINDING(ckpt_sd_exists);

/* gui (block the script thread on the UI bridge) */
CKPT_BINDING(ckpt_gui_message);
CKPT_BINDING(ckpt_gui_confirm);
CKPT_BINDING(ckpt_gui_pick_one);
CKPT_BINDING(ckpt_gui_pick_many);
CKPT_BINDING(ckpt_gui_keyboard);
CKPT_BINDING(ckpt_gui_status);

/* misc */
CKPT_BINDING(ckpt_script_log);
CKPT_BINDING(ckpt_selected_title);

#ifdef __cplusplus
}
#endif

#endif
