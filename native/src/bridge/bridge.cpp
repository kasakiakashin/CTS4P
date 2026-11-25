#include "bridge/bridge.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "build/build_observer.h"
#include "core/app.h"
#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/version.h"
#include "ipc/ipc_api.h"
#include "net/session.h"
#include "sync/save_transfer.h"
#include "sync/tray.h"

namespace cts4p::bridge {
namespace {

using json = nlohmann::json;

std::once_flag g_once;
HANDLE g_wake = nullptr;
std::mutex g_post_mutex;  // the native->Python ring has many native producers; serialize them
std::mutex g_status_mutex;
Status g_status;
std::atomic<uint64_t> g_from_py{0}, g_to_py{0}, g_dropped{0};

void post_frame(const std::vector<std::byte>& frame) {
    bool ok = false;
    {
        std::scoped_lock lock(g_post_mutex);
        ok = ipc::post_to_python(frame.data(), static_cast<uint32_t>(frame.size()));
    }
    (ok ? g_to_py : g_dropped).fetch_add(1, std::memory_order_relaxed);
}

void handle_request(const json& req) {
    const std::string op = req.value("op", "");
    if (op == "hello") {
        {
            std::scoped_lock lock(g_status_mutex);
            g_status.python_attached = true;
            g_status.python_version = req.value("python", "?");
            g_status.python_mod_version = req.value("mod", "?");
        }
        log::info("python bridge attached (python {}, scripts {})", req.value("python", "?"), req.value("mod", "?"));
        if (req.value("mod", "") != kModVersion)
            net::Session::instance().notice(std::string("Versão dos scripts (") + req.value("mod", "?") + ") difere da DLL (" + kModVersion + ")");
        const auto snap = net::Session::instance().snapshot();
        json peers = json::array();
        for (const auto& p : snap.peers)
            if (p.state == net::LinkState::Ready) peers.push_back({{"id", p.id}, {"name", p.name}, {"role", p.role}});
        post_event(json{{"ev", "hello"},
                        {"native", kModVersion},
                        {"api", kApiVersion},
                        {"mode", snap.mode == net::SessionMode::Hosting ? "host" : snap.mode == net::SessionMode::Joining ? "guest" : "idle"},
                        {"room", snap.room},
                        {"local_name", snap.local_name},
                        {"saves_dir", paths::saves_dir() ? paths::to_utf8(paths::saves_dir()->wstring()) : ""},
                        {"auto_sync_save", config().auto_sync_save},
                        {"peers", peers}});
    } else if (op == "ping") {
        post_event(json{{"ev", "pong"}, {"n", req.value("n", 0)}});
    } else if (op == "log") {
        const std::string level = req.value("level", "info");
        log::write(log::parse_level(level, log::Level::Info), "py: " + req.value("msg", ""));
    } else if (op == "notice") {
        net::Session::instance().notice(req.value("text", ""));
    } else if (op == "send_save") {
        const std::string path = req.value("path", "");
        const uint32_t slot = req.value("slot_id", 0u);
        const std::string label = req.value("label", "CTS4P");
        const std::string to = req.value("to", "");
        const bool background = req.value("background", false);
        auto targets = net::Session::instance().ready_links();
        if (!to.empty()) std::erase_if(targets, [&](const auto& l) { return l->id() != to; });
        std::string error;
        const bool ok = sync::SaveTransfer::instance().send_file(std::filesystem::path(paths::from_utf8(path)), slot, label, targets, error, background);
        if (!ok) {
            log::warn("send_save failed: {}", error);
            if (!background) net::Session::instance().notice("Não foi possível enviar o save: " + error);
        }
        post_event(json{{"ev", "save_sending"}, {"ok", ok}, {"error", error}, {"targets", targets.size()}});
    } else if (op == "send_tray") {
        std::string error;
        uint64_t id = req.value("id", 0ull);
        if (id == 0)
            if (auto dir = paths::tray_dir())
                if (auto list = sync::tray::list_households(*dir); !list.empty()) id = list.front().id;
        const bool ok = id != 0 && sync::SaveTransfer::instance().send_household(id, net::Session::instance().ready_links(), error);
        if (id == 0) error = "nenhuma família na Biblioteca";
        post_event(json{{"ev", "tray_sending"}, {"ok", ok}, {"error", error}});
    } else if (op == "presence") {
        build::Presence p;
        p.name = req.value("name", "?");
        p.mode = req.value("mode", "vivo");
        p.has_position = req.contains("x");
        p.x = req.value("x", 0.0);
        p.y = req.value("y", 0.0);
        p.z = req.value("z", 0.0);
        p.level = req.value("level", 0);
        if (p.mode == "construção" && p.name != net::Session::instance().snapshot().local_name)
            net::Session::instance().notice(p.name + " entrou no Modo Construção");
        build::set_presence(std::move(p));
    } else if (op == "build_gate") {
        build::set_gate(req.value("open", false));
    } else if (op == "lot_hash") {
        build::set_lot_hash({req.value("local", ""), req.value("summary", ""), req.value("last_heal", ""), -1});
    } else if (op == "status") {
        post_event(json{{"ev", "status"}, {"status", json::parse(app::status_json(), nullptr, false)}});
    } else {
        log::warn("bridge: unknown request op '{}'", op);
    }
}

void handle_net_send(std::span<const std::byte> f) {
    // flags | msg_type | target_len | target | payload
    if (f.size() < 3) return;
    const bool reliable = (static_cast<uint8_t>(f[0]) & 1) != 0;
    const uint8_t type = static_cast<uint8_t>(f[1]);
    const size_t tlen = static_cast<uint8_t>(f[2]);
    if (f.size() < 3 + tlen) return;
    const std::string target(reinterpret_cast<const char*>(f.data() + 3), tlen);
    const std::string payload(reinterpret_cast<const char*>(f.data() + 3 + tlen), f.size() - 3 - tlen);
    net::Session::instance().send_game(target, type, payload, reliable);
}

void dispatch(std::span<const std::byte> frame) {
    if (frame.empty()) return;
    const auto kind = static_cast<uint8_t>(frame[0]);
    const auto body = frame.subspan(1);
    if (kind == NetSend) {
        handle_net_send(body);
    } else if (kind == Request) {
        const json req = json::parse(reinterpret_cast<const char*>(body.data()), reinterpret_cast<const char*>(body.data()) + body.size(), nullptr, false);
        if (req.is_object()) handle_request(req);
        else log::warn("bridge: malformed request from python");
    } else {
        log::warn("bridge: unknown frame kind 0x{:02X} from python", kind);
    }
}

void worker() {
    std::vector<std::byte> buf(64 * 1024);
    for (;;) {
        WaitForSingleObject(g_wake, 50);
        for (;;) {
            const int64_t n = ipc::take_from_python(buf.data(), static_cast<uint32_t>(buf.size()));
            if (n == 0) break;
            if (n < 0) {  // frame bigger than our buffer: grow and retry
                buf.resize(static_cast<size_t>(-n));
                continue;
            }
            g_from_py.fetch_add(1, std::memory_order_relaxed);
            try {
                dispatch(std::span(buf.data(), static_cast<size_t>(n)));
            } catch (const std::exception& e) {
                log::error("bridge: exception handling python frame: {}", e.what());
            }
        }
    }
}

}  // namespace

void start() {
    std::call_once(g_once, [] {
        g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::thread(worker).detach();
        log::info("bridge: ready");
    });
}

void notify_python_frame() {
    if (g_wake) SetEvent(g_wake);
}

void post_event(const json& event) {
    const std::string text = event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    std::vector<std::byte> frame(1 + text.size());
    frame[0] = static_cast<std::byte>(Event);
    std::memcpy(frame.data() + 1, text.data(), text.size());
    post_frame(frame);
}

void post_net_recv(const std::string& from, uint8_t msg_type, std::span<const std::byte> payload) {
    const size_t flen = std::min<size_t>(from.size(), 255);
    std::vector<std::byte> frame(3 + flen + payload.size());
    frame[0] = static_cast<std::byte>(NetRecv);
    frame[1] = static_cast<std::byte>(msg_type);
    frame[2] = static_cast<std::byte>(flen);
    std::memcpy(frame.data() + 3, from.data(), flen);
    if (!payload.empty()) std::memcpy(frame.data() + 3 + flen, payload.data(), payload.size());
    post_frame(frame);
}

Status status() {
    std::scoped_lock lock(g_status_mutex);
    Status s = g_status;
    s.frames_from_python = g_from_py.load();
    s.frames_to_python = g_to_py.load();
    s.dropped_to_python = g_dropped.load();
    return s;
}

}  // namespace cts4p::bridge
