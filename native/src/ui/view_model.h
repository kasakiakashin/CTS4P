#pragma once
// Everything the overlay shows, gathered once per frame from the runtime.
// With CTS4P_UI_DEMO=1 (screenshot tests only) it returns example data instead,
// so every screen can be rendered and reviewed without a real multiplayer session.

#include <string>
#include <vector>

#include "bridge/bridge.h"
#include "build/build_observer.h"
#include "net/selftest.h"
#include "net/session.h"
#include "sync/save_transfer.h"
#include "sync/tray.h"

namespace cts4p::ui {

struct ViewModel {
    bool demo = false;
    net::SessionSnapshot session;
    std::vector<sync::TransferStatus> transfers;
    build::Status build;
    build::LotHashView lot;
    std::vector<build::Presence> presence;
    std::vector<build::MessageStat> messages;
    bridge::Status python;
    net::TestReport selftest;
    std::string signaling_url;
    std::string game_version;
    bool auto_sync_save = true;
    std::vector<sync::tray::Household> library;  // households in the Library, newest first
    int64_t now_unix = 0;
};

ViewModel gather();
int demo_mode();  // 0 = off, 1 = in a room, 2 = not in a room

}  // namespace cts4p::ui
