// Network tests through the DLL's exported API.
//   test_net loopback                         WebRTC loopback on 127.0.0.1 (single process)
//   test_net signaling <ws-url>               signaling server probe
//   test_net host <ws-url> <code-file>        real room host: writes the room code, waits for a guest
//   test_net join <ws-url> <code-file>        real room guest: reads the code and joins
// host + join run as two separate processes, exactly like two PCs (minus the internet).

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <cstring>
#include <filesystem>
#include <vector>

#include <nlohmann/json.hpp>

#include "test_common.h"

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

HMODULE g_dll = nullptr;

template <class Fn>
Fn fn(const char* name) {
    auto f = test::get<Fn>(g_dll, name);
    if (!f) {
        std::printf("missing export %s\n", name);
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 2);
    }
    return f;
}

json status() {
    std::vector<char> buf(64 * 1024);
    fn<uint32_t (*)(char*, uint32_t)>("cts4p_status_json")(buf.data(), static_cast<uint32_t>(buf.size()));
    return json::parse(buf.data(), nullptr, false);
}

template <class Pred>
bool wait_until(Pred pred, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred(status())) return true;
        std::this_thread::sleep_for(50ms);
    }
    return pred(status());
}

bool any_ready(const json& s) {
    for (const auto& p : s["session"]["peers"])
        if (p.value("ready", false)) return true;
    return false;
}

bool has_chat(const json& s, const std::string& text) {
    for (const auto& c : s["session"]["chat"])
        if (c.value("text", "") == text && !c.value("system", false)) return true;
    return false;
}

double first_rtt(const json& s) {
    for (const auto& p : s["session"]["peers"])
        if (p.value("ready", false)) return p.value("rtt_ms", -1.0);
    return -1.0;
}

// Starts the runtime with an isolated mod dir whose config points at `url`
// and uses host ICE candidates only (no STUN), so the test never leaves this PC.
void start_runtime(const std::string& role, const std::string& url) {
    const std::wstring dir = test::exe_dir() + L"\\test_mod_dir_" + std::wstring(role.begin(), role.end());
    CreateDirectoryW(dir.c_str(), nullptr);
    std::ofstream(dir + L"\\config.json")
        << json{{"signaling_url", url}, {"ice_servers", json::array()}, {"nickname", role == "host" ? "Host Teste" : "Convidado Teste"}}.dump(2);
    SetEnvironmentVariableW(L"CTS4P_MOD_DIR", dir.c_str());
    fn<int32_t (*)()>("cts4p_debug_start")();
    if (!wait_until([](const json& s) { return !s["session"].value("local_name", "").empty(); }, 10s)) {
        std::printf("runtime did not initialize\n");
        TerminateProcess(GetCurrentProcess(), 2);
    }
}

[[noreturn]] void exit_with(test::Checker& c, const char* suite) {
    std::printf("final status: %s\n", status().dump().c_str());
    const int rc = c.finish(suite);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(rc));
    std::abort();
}

void run_host(const std::string& url, const std::string& code_file) {
    test::Checker c;
    start_runtime("host", url);
    fn<int32_t (*)()>("cts4p_session_host")();
    const bool open = wait_until([](const json& s) { return s["session"].value("signaling_open", false) && !s["session"].value("room", "").empty(); }, 10s);
    c.check(open, "room created on signaling server");
    if (!open) exit_with(c, "test_net host");
    const std::string room = status()["session"].value("room", "");
    std::printf("  room code: %s\n", room.c_str());
    std::ofstream(code_file) << room;
    std::fflush(stdout);

    c.check(wait_until(any_ready, 45s), "guest connected and handshake done");
    c.check(wait_until([](const json& s) { return has_chat(s, "oi do convidado ção"); }, 15s), "received guest chat");
    fn<int32_t (*)(const char*)>("cts4p_session_chat")("olá do host");
    c.check(wait_until([](const json& s) { return first_rtt(s) > 0; }, 10s), "RTT measured");
    std::printf("  rtt to guest: %.2f ms\n", first_rtt(status()));
    std::this_thread::sleep_for(3s);  // give the guest time to see our chat before we close
    fn<int32_t (*)()>("cts4p_session_leave")();
    exit_with(c, "test_net host");
}

void run_join(const std::string& url, const std::string& code_file) {
    test::Checker c;
    start_runtime("guest", url);
    std::string code;
    for (int i = 0; i < 200 && code.empty(); ++i) {
        std::ifstream(code_file) >> code;
        if (code.empty()) std::this_thread::sleep_for(100ms);
    }
    c.check(!code.empty(), "read room code from host");
    if (code.empty()) exit_with(c, "test_net join");
    std::printf("  joining %s\n", code.c_str());
    fn<int32_t (*)(const char*)>("cts4p_session_join")(code.c_str());

    c.check(wait_until(any_ready, 45s), "connected to host and handshake done");
    fn<int32_t (*)(const char*)>("cts4p_session_chat")("oi do convidado ção");
    c.check(wait_until([](const json& s) { return has_chat(s, "olá do host"); }, 15s), "received host chat");
    c.check(wait_until([](const json& s) { return first_rtt(s) > 0; }, 10s), "RTT measured");
    std::printf("  rtt to host: %.2f ms\n", first_rtt(status()));
    const bool closed = wait_until([](const json& s) { return !any_ready(s); }, 15s);
    c.check(closed, "saw host leave the room");
    exit_with(c, "test_net join");
}

// ---------------------------------------------------------------- Phase 3: act as the Python scripts
// through the real IPC exports, between two processes with different Mods folders.

struct Frame {
    uint8_t kind = 0;
    std::vector<uint8_t> body;
};

void ipc_send_raw(const std::vector<uint8_t>& frame) {
    auto send = fn<int32_t (*)(const void*, uint32_t)>("cts4p_ipc_send");
    for (int i = 0; i < 200 && !send(frame.data(), static_cast<uint32_t>(frame.size())); ++i) std::this_thread::sleep_for(5ms);
}

void ipc_request(const json& req) {
    const std::string s = req.dump();
    std::vector<uint8_t> f{0x02};
    f.insert(f.end(), s.begin(), s.end());
    ipc_send_raw(f);
}

void ipc_net_send(uint8_t type, const std::string& target, const std::string& payload) {
    std::vector<uint8_t> f{0x01, 1, type, static_cast<uint8_t>(target.size())};
    f.insert(f.end(), target.begin(), target.end());
    f.insert(f.end(), payload.begin(), payload.end());
    ipc_send_raw(f);
}

std::vector<Frame> g_inbox;

void ipc_drain() {
    auto recv = fn<int64_t (*)(void*, uint32_t)>("cts4p_ipc_recv");
    std::vector<uint8_t> buf(1 << 20);
    for (;;) {
        const int64_t n = recv(buf.data(), static_cast<uint32_t>(buf.size()));
        if (n <= 0) break;
        g_inbox.push_back({buf[0], std::vector<uint8_t>(buf.begin() + 1, buf.begin() + n)});
    }
}

std::optional<json> wait_event(const std::string& ev, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        ipc_drain();
        for (auto it = g_inbox.begin(); it != g_inbox.end(); ++it) {
            if (it->kind != 0x82) continue;
            json j = json::parse(it->body.begin(), it->body.end(), nullptr, false);
            if (j.is_object() && j.value("ev", "") == ev) {
                g_inbox.erase(it);
                return j;
            }
        }
        std::this_thread::sleep_for(20ms);
    }
    return std::nullopt;
}

struct NetMsg {
    uint8_t type;
    std::string from;
    std::string payload;
};

std::optional<NetMsg> wait_net(uint8_t type, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        ipc_drain();
        for (auto it = g_inbox.begin(); it != g_inbox.end(); ++it) {
            if (it->kind != 0x81 || it->body.size() < 2 || it->body[0] != type) continue;
            const size_t flen = it->body[1];
            NetMsg m{type, std::string(it->body.begin() + 2, it->body.begin() + 2 + static_cast<std::ptrdiff_t>(flen)),
                     std::string(it->body.begin() + 2 + static_cast<std::ptrdiff_t>(flen), it->body.end())};
            g_inbox.erase(it);
            return m;
        }
        std::this_thread::sleep_for(10ms);
    }
    return std::nullopt;
}

std::string to_utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

void write_file(const std::wstring& path, const std::string& content) {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
}

std::string read_file(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// Fake "Documents\Electronic Arts\The Sims 4" with a role-specific Mods set.
std::wstring make_user_dir(const std::string& role) {
    const std::wstring user = test::exe_dir() + L"\\p3_" + std::wstring(role.begin(), role.end()) + L"_user";
    CreateDirectoryW(user.c_str(), nullptr);
    CreateDirectoryW((user + L"\\Mods").c_str(), nullptr);
    CreateDirectoryW((user + L"\\Mods\\CC").c_str(), nullptr);
    CreateDirectoryW((user + L"\\saves").c_str(), nullptr);
    write_file(user + L"\\Options.ini", "[options]\nscriptmodsenabled = 1\nmodsdisabled = 0\n");
    write_file(user + L"\\Mods\\CC\\Shared_Hair.package", std::string(200000, 'h'));
    write_file(user + L"\\Mods\\CC\\Changed.package", role == "host" ? std::string(5000, 'a') : std::string(5000, 'b'));
    if (role == "host") write_file(user + L"\\Mods\\HostOnly.package", "host only");
    else write_file(user + L"\\Mods\\GuestOnly.ts4script", "guest only");
    DeleteFileW((user + L"\\saves\\Slot_00054d50.save").c_str());
    std::error_code ec;
    std::filesystem::remove_all(user + L"\\Tray", ec);
    CreateDirectoryW((user + L"\\Tray").c_str(), nullptr);
    return user;
}

bool contains(const json& arr, const std::string& v) {
    for (const auto& e : arr)
        if (e.is_string() && e.get<std::string>() == v) return true;
    return false;
}

constexpr uint32_t kTestSlot = 0x00054D50;

void run_p3(const std::string& role, const std::string& url, const std::string& code_file) {
    test::Checker c;
    const std::wstring user = make_user_dir(role);
    SetEnvironmentVariableW(L"CTS4P_USER_DIR", user.c_str());
    start_runtime(role, url);
    ipc_request({{"op", "hello"}, {"python", "test-harness"}, {"mod", "0.4.2"}});
    c.check(wait_event("hello", 5s).has_value(), "bridge answers hello");
    ipc_request({{"op", "ping"}, {"n", 7}});
    auto pong = wait_event("pong", 5s);
    c.check(pong && pong->value("n", 0) == 7, "bridge ping/pong round trip");

    if (role == "host") {
        fn<int32_t (*)()>("cts4p_session_host")();
        const bool open = wait_until([](const json& s) { return s["session"].value("signaling_open", false) && !s["session"].value("room", "").empty(); }, 10s);
        c.check(open, "room created");
        if (!open) exit_with(c, "p3 host");
        std::ofstream(code_file) << status()["session"].value("room", "");
    } else {
        std::string code;
        for (int i = 0; i < 200 && code.empty(); ++i) {
            std::ifstream(code_file) >> code;
            if (code.empty()) std::this_thread::sleep_for(100ms);
        }
        fn<int32_t (*)(const char*)>("cts4p_session_join")(code.c_str());
    }

    auto ready = wait_event("peer_ready", 45s);
    c.check(ready.has_value(), "peer_ready event delivered to 'python'");
    const std::string peer = ready ? ready->value("id", "") : "";

    auto compat = wait_event("compat", 15s);
    c.check(compat.has_value() && !compat->value("compatible", true), "compat report says installs differ");
    if (compat) {
        std::printf("    summary: %s\n", compat->value("summary", "").c_str());
        const std::string mine = role == "host" ? "hostonly.package" : "guestonly.ts4script";
        const std::string theirs = role == "host" ? "guestonly.ts4script" : "hostonly.package";
        c.check(contains((*compat)["mods_missing_here"], theirs), "detects mod missing here");
        c.check(contains((*compat)["mods_missing_there"], mine), "detects mod missing on the other PC");
        c.check(contains((*compat)["mods_different"], "cc/changed.package"), "detects same mod with different content");
        c.check(!contains((*compat)["mods_different"], "cc/shared_hair.package") && !contains((*compat)["mods_missing_here"], "cc/shared_hair.package"),
                "identical mod is not reported");
    }

    const std::wstring source = test::exe_dir() + L"\\p3_fake_host.save";
    if (role == "host") {
        // ~12 MB "save": half pseudo-random (incompressible), half repetitive.
        std::string save(12u << 20, '\0');
        uint64_t x = 0x9E3779B97F4A7C15ull;
        for (size_t i = 0; i < save.size(); ++i) {
            if (i < save.size() / 2) {
                x ^= x << 13; x ^= x >> 7; x ^= x << 17;
                save[i] = static_cast<char>(x & 0xFF);
            } else {
                save[i] = static_cast<char>("cts4p-save-"[i % 11]);
            }
        }
        write_file(source, save);
        const auto t0 = std::chrono::steady_clock::now();
        ipc_request({{"op", "send_save"}, {"path", to_utf8(source)}, {"slot_id", kTestSlot}, {"label", "cts4p - Host Teste"}});
        auto sending = wait_event("save_sending", 10s);
        c.check(sending && sending->value("ok", false), "native accepted send_save request");
        auto delivered = wait_event("save_delivered", 60s);
        c.check(delivered && delivered->value("ok", false), "guest confirmed the save (hash verified)");
        std::printf("    12 MB save delivered in %.2f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());

        auto msg = wait_net(40, 15s);
        c.check(msg && msg->payload == "olá host, sou o convidado" && msg->from == peer, "gameplay msg 40 from guest reached 'python'");
        ipc_net_send(41, peer, "resposta do host");

        // "Criar juntos": the guest sends a household from its Library.
        auto tray_in = wait_event("tray_received", 30s);
        c.check(tray_in && tray_in->value("name", "") == "Bia Teste", "household from the guest arrived (tray_received)");
        const std::string hb = read_file(user + L"\\Tray\\0x00000000!0x00c0ffee00000010.householdbinary");
        c.check(hb == std::string(7000, 'H'), "household files written into the host's Library");
        // A guest must never be able to write a save on the host.
        std::this_thread::sleep_for(4s);
        c.check(!wait_event("save_received", 1s).has_value() && read_file(user + L"\\saves\\Slot_00054d50.save").empty(),
                "save pushed by the guest was refused (host saves untouched)");
    } else {
        auto got = wait_event("save_received", 60s);
        c.check(got.has_value(), "save_received event delivered to 'python'");
        const std::wstring dest = user + L"\\saves\\Slot_00054d50.save";
        const std::string a = read_file(dest), b = read_file(source);
        c.check(!a.empty() && a == b, "received save is byte-identical to the host file");
        std::printf("    %zu bytes written to %ls\n", a.size(), dest.c_str());

        ipc_net_send(40, "", "olá host, sou o convidado");
        auto reply = wait_net(41, 15s);
        c.check(reply && reply->payload == "resposta do host", "gameplay msg 41 from host reached 'python'");

        // "Criar juntos": put a household in our Library and send the newest one.
        const std::wstring tray = user + L"\\Tray\\";
        const std::string pb = std::string("\x08\x10\x10\x01\x22\x09") + "Bia Teste";
        std::string item(8, '\0');
        const auto len = static_cast<uint32_t>(pb.size());
        std::memcpy(item.data() + 4, &len, 4);
        write_file(tray + L"0x00000001!0x00c0ffee00000010.trayitem", item + pb);
        write_file(tray + L"0x00000000!0x00c0ffee00000010.householdbinary", std::string(7000, 'H'));
        write_file(tray + L"0x00000013!0x01c0ffee00000011.sgi", std::string(3000, 'S'));
        ipc_request({{"op", "send_tray"}});
        auto ts = wait_event("tray_sending", 10s);
        c.check(ts && ts->value("ok", false), "send_tray accepted (newest household)");
        auto tdel = wait_event("save_delivered", 30s);
        c.check(tdel && tdel->value("ok", false), "host confirmed the household");

        // Try to push a save to the host: must be refused.
        ipc_request({{"op", "send_save"}, {"path", to_utf8(user + L"\\saves\\Slot_00054d50.save")}, {"slot_id", kTestSlot}, {"label", "x"}});
        auto refused = wait_event("save_delivered", 20s);
        c.check(refused && !refused->value("ok", true), "host refused a save sent by a guest");
    }
    exit_with(c, role == "host" ? "p3 host" : "p3 guest");
}

// ---------------------------------------------------------------- Phase 4: build op replication path
// The game isn't running, so frames can't be applied; we check that a real captured
// RequestBuildBuyOp frame crosses the network intact into the peer's replay queue,
// and that malformed frames are rejected before queuing.
void run_p4(const std::string& role, const std::string& url, const std::string& code_file, const std::string& frame_file) {
    test::Checker c;
    start_runtime(role == "host" ? "p4host" : "p4guest", url);
    const std::string frame = read_file(std::wstring(frame_file.begin(), frame_file.end()));
    c.check(frame.size() >= 12, "loaded captured frame");
    auto emit = fn<int32_t (*)(const void*, uint32_t)>("cts4p_debug_build_emit");
    auto queued = [] { return status()["build"].value("queued", -1); };

    if (role == "host") {
        fn<int32_t (*)()>("cts4p_session_host")();
        const bool open = wait_until([](const json& s) { return s["session"].value("signaling_open", false) && !s["session"].value("room", "").empty(); }, 10s);
        c.check(open, "room created");
        std::ofstream(code_file) << status()["session"].value("room", "");
    } else {
        std::string code;
        for (int i = 0; i < 200 && code.empty(); ++i) {
            std::ifstream(code_file) >> code;
            if (code.empty()) std::this_thread::sleep_for(100ms);
        }
        fn<int32_t (*)(const char*)>("cts4p_session_join")(code.c_str());
    }
    c.check(wait_until(any_ready, 45s), "peer ready");
    std::this_thread::sleep_for(1s);

    if (role == "guest") {
        // 1) host's gate is still closed: this frame must be dropped, not queued.
        c.check(emit(frame.data(), static_cast<uint32_t>(frame.size())) == 1, "frame sent while host gate closed");
        std::this_thread::sleep_for(3s);  // host opens its gate after checking the drop
        ipc_request({{"op", "build_gate"}, {"open", true}});
        std::string bad = frame;
        bad[8] = static_cast<char>(bad[8] + 1);  // payload length no longer matches
        c.check(emit(bad.data(), static_cast<uint32_t>(bad.size())) == 1, "malformed frame sent");
        std::string wrong_id = frame;
        wrong_id[0] = static_cast<char>(wrong_id[0] ^ 0xFF);  // not RequestBuildBuyOp
        c.check(emit(wrong_id.data(), static_cast<uint32_t>(wrong_id.size())) == 1, "foreign message id sent");
        c.check(emit(frame.data(), static_cast<uint32_t>(frame.size())) == 1, "real frame sent to host");
        c.check(wait_until([&](const json&) { return queued() == 1; }, 15s), "host's real frame arrived in my replay queue");
    } else {
        c.check(wait_until([](const json& s) { return s["build"].value("dropped_gate", 0) == 1; }, 10s), "gate closed: frame dropped, nothing queued");
        c.check(queued() == 0, "queue empty while gate closed");
        ipc_request({{"op", "build_gate"}, {"open", true}});
        c.check(wait_until([](const json& s) { return s["build"].value("gate_open", false); }, 5s), "gate opened through the Python IPC request");
        c.check(wait_until([&](const json&) { return queued() == 1; }, 15s), "only the valid frame was queued (2 malformed rejected)");
        std::this_thread::sleep_for(1s);
        c.check(queued() == 1, "queue still holds exactly 1 frame");
        c.check(emit(frame.data(), static_cast<uint32_t>(frame.size())) == 1, "real frame sent to guest");
        std::this_thread::sleep_for(4s);
    }
    exit_with(c, role == "host" ? "p4 host" : "p4 guest");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "loopback";
    if (mode == "loopback" || mode == "signaling")
        SetEnvironmentVariableW(L"CTS4P_MOD_DIR", (test::exe_dir() + L"\\test_mod_dir").c_str());

    g_dll = LoadLibraryW(test::dll_path().c_str());
    if (!g_dll) {
        std::printf("cannot load %ls (%lu)\n", test::dll_path().c_str(), GetLastError());
        return 2;
    }

    if ((mode == "p4host" || mode == "p4join") && argc > 4) run_p4(mode == "p4host" ? "host" : "guest", argv[2], argv[3], argv[4]);
    if ((mode == "p3host" || mode == "p3join") && argc > 3) run_p3(mode == "p3host" ? "host" : "guest", argv[2], argv[3]);
    if ((mode == "host" || mode == "join") && argc > 3) {
        if (mode == "host") run_host(argv[2], argv[3]);
        run_join(argv[2], argv[3]);
    }

    auto run = fn<int32_t (*)(int32_t, const char*, char*, uint32_t)>("cts4p_selftest");
    std::vector<char> out(64 * 1024);
    int32_t passed = 0;
    if (mode == "loopback") {
        passed = run(0, nullptr, out.data(), static_cast<uint32_t>(out.size()));
    } else if (mode == "signaling" && argc > 2) {
        fn<int32_t (*)()>("cts4p_debug_start")();  // enables the log file (TLS/WebSocket errors land there)
        passed = run(1, argv[2], out.data(), static_cast<uint32_t>(out.size()));
    } else {
        std::printf("usage: test_net loopback | signaling <url> | host <url> <code-file> | join <url> <code-file>\n");
        return 2;
    }
    std::printf("%s\n", out.data());
    std::printf("test_net %s: %s\n", mode.c_str(), passed ? "PASSED" : "FAILED");
    std::fflush(stdout);
    // Skip static destructors of the DLL's network threads (same as the game exiting).
    TerminateProcess(GetCurrentProcess(), passed ? 0 : 1);
    return passed ? 0 : 1;
}
