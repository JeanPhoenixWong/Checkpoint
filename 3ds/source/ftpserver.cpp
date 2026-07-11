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

#include "ftpserver.hpp"
#include "configuration.hpp"
#include "logging.hpp"
#include "thread.hpp"
#include <3ds.h>
#include <arpa/inet.h>
#include <atomic>
#include <netinet/in.h>
#include <string>
#include <unistd.h>

extern "C" {
#include "ftp.h"

// Strong override of the weak diagnostic hook in ftp.c: routes the core's FTP_LOG
// lines into Checkpoint's logger so they show up in the in-memory / file logs and
// the /logs endpoints. C linkage to match the C core's declaration.
void ftp_log(const char* msg)
{
    Logging::info("[ftp] {}", msg);
}
}

namespace {
    // ftp.c's LISTEN_PORT. Kept in sync manually (the core doesn't export it).
    constexpr int FTP_PORT = 50000;

    // Set once init() bound the listen socket; the loop thread only touches the
    // ftpd core while this is set.
    std::atomic<bool> available{false};
    // Cleared by requestStop() to tell ftpLoop() to return so the thread can be
    // joined. Same lifecycle shape as server.cpp's serverRunning flag.
    std::atomic_flag running = ATOMIC_FLAG_INIT;

    std::string address;

    void ftpLoop(void)
    {
        while (running.test_and_set()) {
            // The toggle only gates whether we advance FTP work; the listen
            // socket stays bound regardless (Switch parity). When off, idle
            // instead of busy-spinning a core for the app's lifetime.
            if (!Configuration::getInstance().isFTPEnabled()) {
                svcSleepThread(100'000'000ULL); // 100 ms
                continue;
            }

            // A prior LOOP_EXIT tore the core down (available=false). Nothing polls
            // a dead core: idle until re-armed instead of hammering ftp_loop() on it
            // every iteration for the app's lifetime.
            if (!available.load()) {
                svcSleepThread(100'000'000ULL); // 100 ms
                continue;
            }

            loop_status_t status = ftp_loop();
            if (status == LOOP_EXIT) {
                // Unrecoverable socket error: stop serving but keep the thread
                // alive and idling so teardown still joins cleanly.
                Logging::error("ftp_loop reported LOOP_EXIT; FTP server stopped.");
                available.store(false);
                svcSleepThread(100'000'000ULL);
                continue;
            }
            else if (status == LOOP_RESTART) {
                // Wi-Fi dropped (ENETDOWN). Re-init the core like ftpd upstream
                // does, after a short pause to avoid a tight failure loop.
                Logging::warning("ftp_loop requested restart; reinitializing.");
                ftp_exit();
                svcSleepThread(2'000'000'000ULL); // 2 s
                available.store(ftp_init() == 0);
                continue;
            }

            // LOOP_CONTINUE: sleep a short fixed interval every iteration.
            // ftp_loop() never blocks (timeout-0 polls), so a bare loop would
            // pin a core. 10 ms caps throughput well above 3DS wifi (~1-2 MB/s)
            // while keeping idle CPU negligible. Do NOT raise this to ~100 ms:
            // it would throttle active transfers to ~640 KB/s.
            svcSleepThread(10'000'000ULL); // 10 ms
        }
    }
}

void FTPServer::init(void)
{
    if (ftp_init() != 0) {
        Logging::warning("ftp_init failed; FTP server unavailable.");
        return;
    }

    // Mirror server.cpp: derive the console's address for the UI. The listen
    // socket already bound to gethostid() inside ftp_init.
    struct in_addr addr;
    addr.s_addr = gethostid();
    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, ipStr, INET_ADDRSTRLEN);
    address = "ftp://" + std::string(ipStr) + ":" + std::to_string(FTP_PORT);

    available.store(true);
    running.test_and_set();
    // 32 KB stack: ftp.c has multi-KB stack locals, so DEFAULT_STACK (16 KB) is
    // too tight to trust here.
    Threads::create(32 * 1024, ftpLoop);
    Logging::info("FTP server listening on {}", address);
}

void FTPServer::requestStop(void)
{
    running.clear();
}

void FTPServer::exit(void)
{
    running.clear();
    if (available.exchange(false)) {
        // Only tear down the core if init() actually brought it up. Safe to call
        // here because the loop thread has already been joined by Threads::exit.
        ftp_exit();
    }
    Logging::trace("FTP server stopped");
}

std::string FTPServer::getAddress(void)
{
    return address;
}
