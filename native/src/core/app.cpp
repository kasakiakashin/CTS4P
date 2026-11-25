#include "core/app.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "bridge/bridge.h"
#include "build/build_observer.h"
#include "core/config.h"
#include "core/debug_pipe.h"
#include "core/log.h"
#include "sync/save_transfer.h"
#include "core/paths.h"
#include "core/version.h"
#include "diag/diagnostics.h"
#include "hook/hooks.h"
#include "net/session.h"
#include "render/dx11_hook.h"
#include "ui/overlay.h"

namespace cts4p::app {
namespace {

std::once_flag g_once;
std::atomic<bool> g_started{false};

struct FindCtx {
    DWORD pid;
    HWND found;
};

BOOL CALLBACK enum_windows(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    RECT r{};
    GetClientRect(hwnd, &r);
    if (r.right - r.left < 320 || r.bottom - r.top < 200) return TRUE;
    ctx->found = hwnd;
    return FALSE;
}

HWND wait_for_main_window(std::chrono::minutes timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        FindCtx ctx{GetCurrentProcessId(), nullptr};
        EnumWindows(enum_windows, reinterpret_cast<LPARAM>(&ctx));
        if (ctx.found) return ctx.found;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return nullptr;
}

std::string default_nickname() {
    wchar_t buf[128]{};
    DWORD n = static_cast<DWORD>(std::size(buf));
    if (GetUserNameW(buf, &n) && n > 1) return paths::to_utf8(std::wstring(buf, n - 1));
    return "Jogador";
}

void bootstrap() {
    const auto mod_dir = paths::mod_dir();
    log::init(mod_dir / L"cts4p_native.log", log::Level::Info);
    log::info("cts4p native {} (protocol v{}, api v{}) starting in pid {}", kModVersion, kProtocolVersion, kApiVersion,
              GetCurrentProcessId());
    log::info("exe: {}", paths::to_utf8(paths::game_bin_dir().wstring()));
    log::info("mod dir: {}", paths::to_utf8(mod_dir.wstring()));

    config() = Config::load_or_create(config_path());
    log::init(mod_dir / L"cts4p_native.log", log::parse_level(config().log_level, log::Level::Info));
    log::info("signaling: {} | ICE servers: {}", config().signaling_url, config().ice_servers.size());

    diag::refresh_cache();
    const auto env = diag::cached();
    log::info("game {} | {} packs | {} mod files", env->game_version, env->packs.size(), env->mods.size());

    const std::string name = config().nickname.empty() ? default_nickname() : config().nickname;
    net::Session::instance().init(name, env->game_version);
    bridge::start();
    if (config().debug_control) debug_pipe::start();

    if (!hooks::init()) return;
    HWND hwnd = wait_for_main_window(std::chrono::minutes(10));
    if (!hwnd) {
        log::error("game window not found after 10 minutes; overlay not installed");
        return;
    }
    log::info("game window found ({}); installing DX11 hooks", static_cast<void*>(hwnd));
    dx11::Callbacks cb;
    cb.on_present = &overlay::on_present;
    cb.on_before_resize = &overlay::on_before_resize;
    cb.on_after_resize = &overlay::on_after_resize;
    if (!dx11::install(cb)) log::error("DX11 hooks failed; overlay unavailable (networking still runs)");
    build::install();  // client->sim message observer (Phase 4); disables itself if the game changed
}

const char* mode_name(net::SessionMode m) {
    switch (m) {
        case net::SessionMode::Idle: return "idle";
        case net::SessionMode::Hosting: return "hosting";
        case net::SessionMode::Joining: return "joining";
    }
    return "?";
}

}  // namespace

void start() {
    std::call_once(g_once, [] {
        g_started = true;
        std::thread([] {
            try {
                bootstrap();
            } catch (const std::exception& e) {
                log::error("bootstrap crashed: {}", e.what());
            }
        }).detach();
    });
}

void stop() {
    if (!g_started) return;
    net::Session::instance().shutdown();
    hooks::shutdown();
}

bool started() { return g_started; }

std::string status_json() {
    using nlohmann::json;
    auto& dx = dx11::stats();
    auto& ov = overlay::stats();
    const auto snap = net::Session::instance().snapshot();
    json peers = json::array();
    for (const auto& p : snap.peers)
        peers.push_back({{"id", p.id}, {"name", p.name}, {"state", net::state_name(p.state)}, {"ready", p.state == net::LinkState::Ready},
                         {"rtt_ms", p.rtt_ms}, {"game_version", p.game_version}, {"error", p.error}});
    json compat = json::array();
    for (const auto& c : snap.compat)
        compat.push_back({{"id", c.peer_id}, {"compatible", c.report.fully_compatible()}, {"summary", c.report.summary()}});
    json transfers = json::array();
    for (const auto& t : sync::SaveTransfer::instance().statuses())
        transfers.push_back({{"outgoing", t.outgoing}, {"peer", t.peer}, {"label", t.label}, {"state", t.state}, {"finished", t.finished},
                             {"ok", t.ok}, {"done", t.done_bytes}, {"total", t.total_bytes}, {"raw", t.raw_bytes}, {"path", t.path}});
    const auto br = bridge::status();
    const auto bs = build::status();
    json bstats = json::array();
    for (const auto& m : build::stats()) bstats.push_back({{"name", m.name}, {"count", m.count}, {"build", m.build_related}});
    json chat = json::array();
    const size_t first = snap.chat.size() > 20 ? snap.chat.size() - 20 : 0;
    for (size_t i = first; i < snap.chat.size(); ++i)
        chat.push_back({{"from", snap.chat[i].from}, {"text", snap.chat[i].text}, {"system", snap.chat[i].system}});
    const json j{{"mod_version", kModVersion},
                 {"protocol", kProtocolVersion},
                 {"api", kApiVersion},
                 {"started", g_started.load()},
                 {"dx11",
                  {{"installed", dx.installed.load()},
                   {"present_calls", dx.present_calls.load()},
                   {"present1_calls", dx.present1_calls.load()},
                   {"resize_calls", dx.resize_calls.load()}}},
                 {"overlay",
                  {{"initialized", ov.initialized.load()},
                   {"failed", ov.failed.load()},
                   {"visible", ov.visible.load()},
                   {"frames_drawn", ov.frames_drawn.load()},
                   {"width", ov.width.load()},
                   {"height", ov.height.load()}}},
                 {"session",
                  {{"mode", mode_name(snap.mode)}, {"local_name", snap.local_name}, {"room", snap.room}, {"status", snap.status}, {"error", snap.error}, {"signaling_open", snap.signaling_open}, {"peers", peers}, {"chat", chat}, {"compat", compat}}},
                 {"transfers", transfers},
                 {"build",
                  {{"installed", bs.installed},
                   {"how", bs.how_found},
                   {"rva", bs.rva},
                   {"total_messages", bs.total_messages},
                   {"build_messages", bs.build_messages},
                   {"captured", bs.records_written},
                   {"ops_sent", bs.ops_sent},
                   {"ops_replayed", bs.ops_replayed},
                   {"ops_failed", bs.ops_failed},
                   {"queued", bs.queued},
                   {"gate_open", bs.gate_open},
                   {"dropped_gate", bs.dropped_gate},
                   {"messages", bstats},
                   {"lot_hash", build::lot_hash().local}}},
                 {"bridge",
                  {{"python_attached", br.python_attached},
                   {"python_version", br.python_version},
                   {"python_mod_version", br.python_mod_version},
                   {"from_python", br.frames_from_python},
                   {"to_python", br.frames_to_python},
                   {"dropped", br.dropped_to_python}}},
                 {"config", {{"signaling_url", config().signaling_url}, {"path", paths::to_utf8(config_path().wstring())}}}};
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace cts4p::app
