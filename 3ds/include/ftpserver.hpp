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

#ifndef FTPSERVER_HPP
#define FTPSERVER_HPP

#include <string>

// mtheall's ftpd core (3rd-party/ftp) driven on a dedicated background thread.
// The 3DS main loop is far slower than the Switch's, so we cannot poll ftp_loop
// per-frame like the Switch build does without stuttering the UI and starving
// transfers. The listen socket is bound once at boot (port 50000); the toggle
// only gates whether the loop advances FTP work, mirroring the Switch behavior.
namespace FTPServer {
    // Binds the listen socket and spawns the loop thread. Call once, after
    // socInit succeeds. No-op if ftp_init() fails (leaves the server unavailable).
    void init(void);

    // Raises the stop flag so the loop thread returns from ftpLoop(). Must be
    // called before Threads::exit() joins (see main.cpp / util.cpp teardown).
    void requestStop(void);

    // Tears down the ftpd core (closes sockets/sessions). Call AFTER the loop
    // thread has been joined by Threads::exit() — never while it may be inside
    // ftp_loop().
    void exit(void);

    // "ftp://<ip>:50000" once init succeeded, otherwise empty.
    std::string getAddress(void);
}

#endif
