#include "ui/view_model.h"

#include <chrono>
#include <mutex>

#include "core/config.h"
#include "core/paths.h"
#include "diag/diagnostics.h"

namespace cts4p::ui {
namespace {

ViewModel demo() {
    using namespace std::chrono;
    ViewModel vm;
    vm.demo = true;
    vm.game_version = "1.129.39.1030";
    vm.signaling_url = "wss://cts4p-signal.exemplo.workers.dev/ws";

    auto& s = vm.session;
    s.mode = net::SessionMode::Hosting;
    s.room = "CTS-K7QX";
    s.status = "Sala CTS-K7QX - jogador conectado!";
    s.signaling_open = true;
    s.local_name = "Dani";

    net::PeerInfo bia;
    bia.id = "a1";
    bia.name = "Bia";
    bia.role = "guest";
    bia.state = net::LinkState::Ready;
    bia.game_version = "1.129.39.1030";
    bia.rtt_ms = 34.2;
    bia.rtt_min_ms = 28.0;
    bia.rtt_max_ms = 61.5;
    net::PeerInfo leo = bia;
    leo.id = "b2";
    leo.name = "Leo";
    leo.rtt_ms = 142.7;
    leo.rtt_min_ms = 120.1;
    leo.rtt_max_ms = 190.4;
    s.peers = {bia, leo};

    net::PeerCompat ok;
    ok.peer_id = "a1";
    ok.report.received = true;
    ok.report.remote_name = "Bia";
    ok.report.local_game_version = ok.report.remote_game_version = "1.129.39.1030";
    net::PeerCompat diff = ok;
    diff.peer_id = "b2";
    diff.report.remote_name = "Leo";
    diff.report.packs_missing_here = {"EP05"};
    diff.report.mods_missing_there = {"cc/cabelo_ondulado.package", "cc/vestido_verao.package"};
    s.compat = {ok, diff};

    const auto now = system_clock::now();
    s.chat = {{"", "Bia entrou na sala", true, now},
              {"Bia", "oii! vamos construir a casa nova?", false, now},
              {"Dani", "bora! começa pela cozinha que eu faço o quintal", false, now},
              {"", "Leo entrou na sala", true, now},
              {"Leo", "cheguei, alguém pausa o jogo rapidinho?", false, now}};

    sync::TransferStatus t;
    t.outgoing = true;
    t.peer = "Leo";
    t.label = "CTS4P - Dani";
    t.state = "enviando";
    t.done_bytes = 3'400'000;
    t.total_bytes = 5'200'000;
    t.raw_bytes = 11'800'000;
    vm.transfers = {t};

    vm.build.installed = true;
    vm.build.how_found = "aob";
    vm.build.rva = 0x24380;
    vm.build.replication = true;
    vm.build.context_ready = true;
    vm.build.gate_open = true;
    vm.build.ops_sent = 18;
    vm.build.ops_replayed = 23;
    vm.build.total_messages = 1204;
    vm.build.build_messages = 61;
    vm.lot = {"9f3c1a77e2b04d18", "Idêntico ao host (14 blocos)", "", 12};
    vm.presence = {{"Dani", "vivo", true, 12.4, 0.0, 8.1, 0, 3},
                   {"Bia", "construção", true, 18.0, 0.0, 6.5, 1, 1},
                   {"Leo", "vivo", true, 3.2, 0.0, 15.0, 0, 40}};
    vm.messages = {{0x3a0f613c, "RequestBuildBuyOp", 41, true}, {0xd5f39c51, "SendBuildBuyEvent", 20, true},
                   {0x79dde5d9, "ExecuteCommandStringOnServer", 803, false}};
    vm.python = {true, "3.7.0", "0.4.3", 5821, 9935, 0};

    auto& r = vm.selftest;
    const int64_t now_s = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    vm.now_unix = now_s;
    sync::tray::Household barros;
    barros.id = 0x00e21785f0900017ull;
    barros.name = "Barros";
    barros.modified_unix = now_s - 140;
    sync::tray::Household silva = barros;
    silva.id = 0x00e21785f0900099ull;
    silva.name = "Silva";
    silva.modified_unix = now_s - 3 * 86400;
    vm.library = {barros, silva};

    r.title = "Loopback local (127.0.0.1)";
    r.finished = true;
    r.passed = true;
    r.elapsed_ms = 734;
    for (const char* n : {"Conexão WebRTC", "Canais de dados", "Handshake", "Ping", "Chat", "Encerramento"})
        r.steps.push_back({n, net::StepStatus::Ok, ""});
    return vm;
}

}  // namespace

int demo_mode() {
    static const int mode = [] {
        auto v = paths::env(L"CTS4P_UI_DEMO");
        if (!v) return 0;
        return *v == L"idle" ? 2 : 1;
    }();
    return mode;
}

// Scanning the Library folder every frame would be wasteful: refresh at most every 2 s.
std::vector<sync::tray::Household> library_cached() {
    static std::mutex m;
    static std::vector<sync::tray::Household> cache;
    static std::chrono::steady_clock::time_point last{};
    std::scoped_lock lock(m);
    const auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::seconds(2)) {
        last = now;
        if (auto dir = paths::tray_dir()) cache = sync::tray::list_households(*dir);
    }
    return cache;
}

ViewModel gather() {
    if (demo_mode() == 2) {  // not in a room yet
        ViewModel vm = demo();
        vm.session = {};
        vm.session.local_name = "Dani";
        vm.session.status = "Desconectado";
        vm.transfers.clear();
        vm.presence.clear();
        vm.build.gate_open = false;
        return vm;
    }
    if (demo_mode()) return demo();
    ViewModel vm;
    vm.session = net::Session::instance().snapshot();
    vm.transfers = sync::SaveTransfer::instance().statuses();
    vm.build = build::status();
    vm.lot = build::lot_hash();
    vm.presence = build::presences();
    vm.messages = build::stats();
    vm.python = bridge::status();
    vm.selftest = net::SelfTest::instance().report();
    vm.signaling_url = config().signaling_url;
    vm.game_version = diag::cached()->game_version;
    vm.auto_sync_save = config().auto_sync_save;
    if (vm.session.mode != net::SessionMode::Idle) vm.library = library_cached();
    vm.now_unix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return vm;
}

}  // namespace cts4p::ui
