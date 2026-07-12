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
    // titles: referenced by catalog index; ids cross the boundary as
    // 16-hex-uppercase strings (picoc has no reliable 64-bit integers)
    { ckpt_titles_count,       "int titles_count(void);" },
    { ckpt_title_find,         "int title_find(char* idHex);" },
    { ckpt_title_id,           "char* title_id(int idx);" },
    { ckpt_title_name,         "char* title_name(int idx);" },
    { ckpt_title_product_code, "char* title_product_code(int idx);" },
    { ckpt_title_is_cart,      "int title_is_cart(int idx);" },
    { ckpt_title_has_save,     "int title_has_save(int idx);" },
    { ckpt_title_has_extdata,  "int title_has_extdata(int idx);" },
    { ckpt_title_backup_path,  "char* title_backup_path(int idx, int kind);" },
    // sd card (plus full picoc stdio: fopen("/3ds/...", ...) works)
    { ckpt_read_directory,     "struct directory* read_directory(char* dir);" },
    { ckpt_delete_directory,   "void delete_directory(struct directory* dir);" },
    { ckpt_sd_mkdirs,          "int sd_mkdirs(char* path);" },
    { ckpt_sd_exists,          "int sd_exists(char* path);" },
    // gui (all block the script until the user answers)
    { ckpt_gui_message,        "void gui_message(char* text);" },
    { ckpt_gui_confirm,        "int gui_confirm(char* text);" },
    { ckpt_gui_pick_one,       "int gui_pick_one(char* prompt, char** items, int count);" },
    { ckpt_gui_pick_many,      "int gui_pick_many(char* prompt, char** items, int count, int* selected);" },
    { ckpt_gui_keyboard,       "void gui_keyboard(char* out, char* hint, int maxChars);" },
    { ckpt_gui_status,         "void gui_status(char* text);" },
    // misc
    { ckpt_script_log,         "void script_log(char* msg);" },
    { ckpt_selected_title,     "char* selected_title(void);" },
    { NULL, NULL }
};
// clang-format on

void PlatformLibraryInit(Picoc* pc)
{
    IncludeRegister(pc, "checkpoint.h", &CheckpointSetupFunc, &CheckpointFunctions[0],
        "struct directory { int count; char** files; };"
        "struct JSON { void* dummy; };");
}
