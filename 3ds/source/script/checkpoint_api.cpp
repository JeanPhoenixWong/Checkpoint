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

// Native implementations of the <checkpoint.h> script API, 3DS flavour. All of
// these run on the script worker thread: TitleCatalog reads are safe (recursive
// mutex since arch-review S4), gui_* park the thread on the ScriptUiBridge, and
// nothing here may trigger a catalog refresh. A binding must close every RAII
// scope before calling back into picoc — ProgramFail longjmps to the run's exit
// point, so it is only ever called before C++ objects holding resources exist.

#include "backuptarget.hpp"
#include "loader.hpp"
#include "logging.hpp"
#include "paths.hpp"
#include "scriptrunner.hpp"
#include "title.hpp"
#include "util.hpp"
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

extern "C" {
#include "checkpoint_api.h"
#include "interpreter.h"
}

namespace {
    // Strings returned to a script are plain malloc'd copies:
    // the interpreter treats them as ordinary char* and they live until the
    // process (or the script) frees them — acceptable for script-sized data.
    void* strToRet(const std::string& str)
    {
        char* ret = (char*)malloc(str.size() + 1);
        if (ret) {
            memcpy(ret, str.c_str(), str.size() + 1);
        }
        return (void*)ret;
    }

    int titleCount(void)
    {
        return TitleCatalog::get().getTitleCount(BackupKind::Save);
    }

    // Copies the idx-th title of the Save list — the catalog index space every
    // titles_* binding shares. Fails the script on a bad index (longjmp; called
    // before any local C++ object exists in the binding).
    Title titleAt(struct ParseState* Parser, int idx)
    {
        if (idx < 0 || idx >= titleCount()) {
            ProgramFail(Parser, "title index %d out of range", idx);
        }
        Title title;
        TitleCatalog::get().getTitle(title, idx, BackupKind::Save);
        return title;
    }

    std::string idToHex(u64 id)
    {
        return StringUtils::format("%016llX", (unsigned long long)id);
    }

    // mkdir wants an "sdmc:"-prefixed POSIX path (matching the boot bootstrap);
    // stat/fopen accept the bare one.
    std::string sdmcPrefixed(const std::string& path)
    {
        return path.rfind("sdmc:", 0) == 0 ? path : "sdmc:" + path;
    }

    ScriptUiBridge& bridge(void)
    {
        return ScriptRunner::get().bridge();
    }
}

/* ---- titles ---------------------------------------------------------- */

void ckpt_titles_count(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    ReturnValue->Val->Integer = titleCount();
}

void ckpt_title_find(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    const u64 id    = strtoull((char*)Param[0]->Val->Pointer, nullptr, 16);
    const int count = titleCount();
    int found       = -1;
    for (int i = 0; i < count && found < 0; i++) {
        Title title;
        TitleCatalog::get().getTitle(title, i, BackupKind::Save);
        if (title.id() == id) {
            found = i;
        }
    }
    ReturnValue->Val->Integer = found;
}

void ckpt_title_id(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    Title title               = titleAt(Parser, Param[0]->Val->Integer);
    ReturnValue->Val->Pointer = strToRet(idToHex(title.id()));
}

void ckpt_title_name(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    Title title               = titleAt(Parser, Param[0]->Val->Integer);
    ReturnValue->Val->Pointer = strToRet(title.longDescription());
}

void ckpt_title_product_code(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    Title title               = titleAt(Parser, Param[0]->Val->Integer);
    ReturnValue->Val->Pointer = strToRet(title.productCode);
}

void ckpt_title_is_cart(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    Title title               = titleAt(Parser, Param[0]->Val->Integer);
    ReturnValue->Val->Integer = title.mediaType() == MEDIATYPE_GAME_CARD ? 1 : 0;
}

void ckpt_title_has_save(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    Title title               = titleAt(Parser, Param[0]->Val->Integer);
    ReturnValue->Val->Integer = title.accessibleSave() ? 1 : 0;
}

void ckpt_title_has_extdata(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    Title title               = titleAt(Parser, Param[0]->Val->Integer);
    ReturnValue->Val->Integer = title.accessibleExtdata() ? 1 : 0;
}

void ckpt_title_backup_path(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    const int kind = Param[1]->Val->Integer;
    if (kind != 0 && kind != 1) {
        ProgramFail(Parser, "backup kind %d must be 0 (save) or 1 (extdata)", kind);
    }
    Title title               = titleAt(Parser, Param[0]->Val->Integer);
    BackupTarget target       = title.backup(kind == 0 ? BackupKind::Save : BackupKind::Extdata);
    ReturnValue->Val->Pointer = strToRet(StringUtils::UTF16toUTF8(target.rootPath()) + "/");
}

/* ---- sd card --------------------------------------------------------- */

void ckpt_read_directory(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    // Layout must match the registered "struct directory { int count; char** files; }".
    struct dirData {
        int count;
        char** files;
    };

    const std::string dir = (char*)Param[0]->Val->Pointer;
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (struct dirent* ent = readdir(d)) {
            const std::string name = ent->d_name;
            if (name != "." && name != "..") {
                names.push_back(dir + "/" + name);
            }
        }
        closedir(d);
    }

    dirData* ret = (dirData*)malloc(sizeof(dirData));
    if (ret) {
        ret->count = (int)names.size();
        ret->files = names.empty() ? nullptr : (char**)malloc(sizeof(char*) * names.size());
        if (ret->files) {
            for (size_t i = 0; i < names.size(); i++) {
                ret->files[i] = (char*)strToRet(names[i]);
            }
        }
        else {
            ret->count = 0;
        }
    }
    ReturnValue->Val->Pointer = ret;
}

void ckpt_delete_directory(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    struct dirData {
        int count;
        char** files;
    };

    dirData* dir = (dirData*)Param[0]->Val->Pointer;
    if (dir) {
        for (int i = 0; i < dir->count; i++) {
            free(dir->files[i]);
        }
        free(dir->files);
        free(dir);
    }
}

void ckpt_sd_mkdirs(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    const std::string path = sdmcPrefixed((char*)Param[0]->Val->Pointer);

    // mkdir -p: create every component; existing ones fail harmlessly, and the
    // final stat is the actual success check.
    for (size_t pos = path.find('/', strlen("sdmc:/")); pos != std::string::npos; pos = path.find('/', pos + 1)) {
        mkdir(path.substr(0, pos).c_str(), 777);
    }
    mkdir(path.c_str(), 777);

    struct stat st;
    ReturnValue->Val->Integer = (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
}

void ckpt_sd_exists(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    struct stat st;
    ReturnValue->Val->Integer = stat((char*)Param[0]->Val->Pointer, &st) == 0 ? 1 : 0;
}

/* ---- gui -------------------------------------------------------------- */

void ckpt_gui_message(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    UiRequest req;
    req.kind   = UiRequest::Kind::Message;
    req.prompt = (char*)Param[0]->Val->Pointer;
    bridge().request(std::move(req));
}

void ckpt_gui_confirm(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    UiRequest req;
    req.kind                  = UiRequest::Kind::Confirm;
    req.prompt                = (char*)Param[0]->Val->Pointer;
    ReturnValue->Val->Integer = bridge().request(std::move(req)).confirmed ? 1 : 0;
}

void ckpt_gui_pick_one(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    char** items    = (char**)Param[1]->Val->Pointer;
    const int count = Param[2]->Val->Integer;

    UiRequest req;
    req.kind   = UiRequest::Kind::PickOne;
    req.prompt = (char*)Param[0]->Val->Pointer;
    for (int i = 0; i < count; i++) {
        req.items.push_back(items[i]);
    }
    ReturnValue->Val->Integer = bridge().request(std::move(req)).index;
}

void ckpt_gui_pick_many(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    char** items    = (char**)Param[1]->Val->Pointer;
    const int count = Param[2]->Val->Integer;
    int* selected   = (int*)Param[3]->Val->Pointer;

    UiRequest req;
    req.kind   = UiRequest::Kind::PickMany;
    req.prompt = (char*)Param[0]->Val->Pointer;
    for (int i = 0; i < count; i++) {
        req.items.push_back(items[i]);
        req.preselected.push_back(selected[i] != 0);
    }

    UiResponse resp = bridge().request(std::move(req));
    if (resp.confirmed) {
        for (int i = 0; i < count && i < (int)resp.selected.size(); i++) {
            selected[i] = resp.selected[i] ? 1 : 0;
        }
    }
    ReturnValue->Val->Integer = resp.confirmed ? 1 : 0;
}

void ckpt_gui_keyboard(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    char* out          = (char*)Param[0]->Val->Pointer;
    const int maxChars = Param[2]->Val->Integer;
    if (maxChars <= 0) {
        ProgramFail(Parser, "gui_keyboard maxChars must be positive");
    }

    UiRequest req;
    req.kind     = UiRequest::Kind::Keyboard;
    req.prompt   = (char*)Param[1]->Val->Pointer;
    req.maxChars = maxChars;

    // maxChars is the out buffer's size, terminator included (PKSM semantics).
    UiResponse resp = bridge().request(std::move(req));
    snprintf(out, (size_t)maxChars, "%s", resp.text.c_str());
}

void ckpt_gui_status(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    bridge().setStatus((char*)Param[0]->Val->Pointer);
}

/* ---- misc -------------------------------------------------------------- */

void ckpt_script_log(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    Logging::info("[script] {}", (char*)Param[0]->Val->Pointer);
}

void ckpt_selected_title(struct ParseState* Parser, struct Value* ReturnValue, struct Value** Param, int NumArgs)
{
    ReturnValue->Val->Pointer = strToRet(ScriptRunner::get().selectedTitle());
}
