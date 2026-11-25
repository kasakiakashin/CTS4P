#include "build/build_observer.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>

#include <nlohmann/json.hpp>

#include "bridge/bridge.h"
#include "core/log.h"
#include "core/paths.h"
#include "hook/hooks.h"
#include "hook/memory.h"

namespace cts4p::build {
namespace {

using json = nlohmann::json;
using DispatchFn = uint64_t(__fastcall*)(void* self, uint32_t a2, uint32_t msg_id, uint32_t a4, void* a5, void* stream);

// Message ids handled by the dispatcher (extracted from Simulation_x64.dll 1.129.39.1030).
struct KnownMessage {
    uint32_t id;
    const char* name;
    bool build;
};
constexpr std::array<KnownMessage, 41> kMessages{{
    {0x3e6edcc2, "CasToZoneIFace::ZoneReservationRequest", false},
    {0x1e61a063, "CasToZoneIFace::ZoneFamilyCreateResponse", false},
    {0x28d85e31, "CasToZoneIFace::RemoveFamily", false},
    {0x66d5d75b, "ExecuteCommandWithArgsOnServer", false},
    {0x79dde5d9, "ExecuteCommandStringOnServer", false},
    {0x50d37f46, "RequestClientDisconnect", false},
    {0xdb90b061, "(unnamed 0xdb90b061)", false},
    {0x721b19e4, "ReplyClockSyncUpdate", false},
    {0xed01fc2c, "RequestServerFps", false},
    {0x3a0f613c, "RequestBuildBuyOp", true},
    {0xe1aeefab, "RequestBuildBuyVenueUpdate", true},
    {0x75b5a1fb, "RequestBuildBuyVenueOwnerUpdate", true},
    {0x2051737f, "RequestSetSubVenue", true},
    {0xaec84577, "RequestLotTraitsUpdate", true},
    {0xa0211975, "RequestBusinessRulesUpdate", false},
    {0x2b6ec145, "RequestBuildBuyLotUpdate", true},
    {0xf57e9539, "(unnamed 0xf57e9539)", false},
    {0xb0b3b5e3, "RequestHouseholdFundsUpdate", true},
    {0xbe531d83, "(unnamed 0xbe531d83)", false},
    {0x73315ee3, "RequestSetLotOwner", false},
    {0xbd537b06, "RequestHouseholdInventoryViewed", false},
    {0x53f2e990, "RequestLoadPlexObjects", false},
    {0x97739144, "RequestRemoteTravelReservation", false},
    {0x8e887f70, "SendLotDisplayLevel", true},
    {0x47874597, "OnClientReady", false},
    {0xea2b2be5, "ClientSendSystemEvent", false},
    {0xd5f39c51, "SendBuildBuyEvent", true},
    {0x55e05d68, "SetBuildBuyGigLots", true},
    {0x499b9185, "ZonesUpdatedResponse", false},
    {0xfe215d56, "ChunkedUploadRequestNewUpload", false},
    {0x39b00055, "ChunkedUploadRequestNewUploadWithPersistence", false},
    {0x179fd370, "ChunkedUploadValidatePersistence", false},
    {0xbfd1aedf, "ChunkedUploadSendChunk", false},
    {0xb4c70a02, "ChunkedUploadCancel", false},
    {0xce125af0, "RequestMemoriesListSimId", false},
    {0x67a4aae7, "RequestMemoriesListSimId(2)", false},
    {0x3011bdc6, "RequestMemoriesListHouseholdId", false},
    {0xb986d23d, "FavoriteMemory", false},
    {0xefe21a45, "UnfavoriteMemory", false},
    {0xd5f27c91, "DeleteMemories", false},
    {0xa7e54298, "RequestMemoriesID", false},
}};

constexpr const char* kPrologue =
    "48 8B C4 55 41 56 41 57 48 8D A8 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 C7 85 ?? ?? ?? ?? FE FF FF FF "
    "48 89 58 08 48 89 70 10 48 89 78 20 45 8B F9 41 8B F0 44 8B F2 48 8B F9 41 81 F8";
constexpr const char* kAnchorString = "CasToZoneIFace::ZoneReservationRequest";
constexpr size_t kMaxRecordsToFile = 5000;

DispatchFn g_original = nullptr;
std::chrono::steady_clock::time_point g_t0;
std::mutex g_mutex;
Status g_status;
std::array<std::atomic<uint64_t>, kMessages.size() + 1> g_counts{};  // last slot = unknown ids
std::deque<Record> g_recent;
std::ofstream g_capture;

std::vector<uint8_t> le32(uint32_t v) {
    return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
}

size_t index_of(uint32_t id) {
    for (size_t i = 0; i < kMessages.size(); ++i)
        if (kMessages[i].id == id) return i;
    return kMessages.size();
}

// Dumps `n` bytes at p and, for every aligned qword that points to readable memory, 0x80 bytes there.
json dump_object(const void* p, size_t n) {
    json out = json::object();
    std::vector<uint8_t> buf(n);
    const size_t got = mem::safe_read(p, buf.data(), n);
    if (got == 0) return out;
    out["bytes"] = mem::hex(buf.data(), got);
    json pointees = json::object();
    for (size_t off = 0; off + 8 <= got; off += 8) {
        uint64_t q = 0;
        std::memcpy(&q, buf.data() + off, 8);
        if (q < 0x10000 || (q & 0x7) != 0) continue;
        uint8_t inner[0x80];
        const size_t g = mem::safe_read(reinterpret_cast<const void*>(q), inner, sizeof(inner));
        if (g) pointees[std::to_string(off)] = mem::hex(inner, g);
    }
    if (!pointees.empty()) out["pointees"] = pointees;
    return out;
}

// ---------------------------------------------------------------- replay
// Stream object layout decoded from in-game captures (1.129.39.1030):
//   +0x20 u32 total frame size   +0x28 u32 total frame size (capacity)   +0x30 u8* frame
// Frame: u32 msg_id | u32 channel | u32 payload_len | payload[payload_len]
constexpr size_t kStreamObjSize = 0x60;
constexpr size_t kStreamLenOff = 0x20;
constexpr size_t kStreamCapOff = 0x28;
constexpr size_t kStreamBufOff = 0x30;
constexpr size_t kFrameHeader = 12;
constexpr size_t kMaxFrame = 1u << 20;
constexpr uint32_t kRequestBuildBuyOp = 0x3a0f613c;

struct DispatchContext {
    bool valid = false;
    void* self = nullptr;
    uint32_t a4 = 0;
    void* a5 = nullptr;
    DWORD thread = 0;
    std::array<uint8_t, kStreamObjSize> stream_template{};
};
DispatchContext g_ctx;               // written/read only on the dispatch thread
std::atomic<DWORD> g_dispatch_thread{0};
std::atomic<bool> g_ctx_ready{false};
bool g_build_template = false;      // dispatch thread only
bool g_have_build_a5 = false;       // dispatch thread only
// Opened by the Python side only while both games show the same lot (guest welcomed /
// host with a synced guest). Closed: nothing is sent and received frames are dropped.
std::atomic<bool> g_gate{false};
std::atomic<uint64_t> g_dropped_gate{0};
std::atomic<bool> g_replicate{true};
std::atomic<bool> g_in_replay{false};
std::mutex g_queue_mutex;
std::deque<std::vector<uint8_t>> g_replay_queue;
std::vector<uint8_t> g_last_local_frame;  // guarded by g_queue_mutex
std::atomic<uint64_t> g_sent{0}, g_replayed{0}, g_replay_failed{0};
SendFn g_send = nullptr;

std::optional<std::vector<uint8_t>> extract_frame(const void* stream, uint32_t msg_id, std::array<uint8_t, kStreamObjSize>* obj_out) {
    std::array<uint8_t, kStreamObjSize> obj{};
    if (mem::safe_read(stream, obj.data(), obj.size()) != obj.size()) return std::nullopt;
    uint32_t len = 0;
    uint64_t buf = 0;
    std::memcpy(&len, obj.data() + kStreamLenOff, 4);
    std::memcpy(&buf, obj.data() + kStreamBufOff, 8);
    if (len < kFrameHeader || len > kMaxFrame || buf < 0x10000) return std::nullopt;
    std::vector<uint8_t> frame(len);
    if (mem::safe_read(reinterpret_cast<const void*>(buf), frame.data(), len) != len) return std::nullopt;
    uint32_t id = 0, payload = 0;
    std::memcpy(&id, frame.data(), 4);
    std::memcpy(&payload, frame.data() + 8, 4);
    if (id != msg_id || kFrameHeader + payload != len) return std::nullopt;  // layout changed: refuse
    if (obj_out) *obj_out = obj;
    return frame;
}

// SEH wrapper: no C++ objects with destructors allowed in this function.
bool call_guarded(void* self, uint32_t channel, uint32_t msg_id, uint32_t a4, void* a5, void* stream, uint64_t* result) {
    __try {
        *result = g_original(self, channel, msg_id, a4, a5, stream);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

size_t run_replays() {
    if (!g_ctx.valid || GetCurrentThreadId() != g_ctx.thread) return 0;
    std::deque<std::vector<uint8_t>> batch;
    {
        std::scoped_lock lock(g_queue_mutex);
        batch.swap(g_replay_queue);
    }
    size_t done = 0;
    for (auto& frame : batch) {
        uint32_t msg_id = 0, channel = 0;
        std::memcpy(&msg_id, frame.data(), 4);
        std::memcpy(&channel, frame.data() + 4, 4);
        alignas(16) std::array<uint8_t, kStreamObjSize> fake = g_ctx.stream_template;
        const uint64_t buf = reinterpret_cast<uint64_t>(frame.data());
        const uint32_t len = static_cast<uint32_t>(frame.size());
        std::memcpy(fake.data() + kStreamLenOff, &len, 4);
        std::memcpy(fake.data() + kStreamCapOff, &len, 4);
        std::memcpy(fake.data() + kStreamBufOff, &buf, 8);
        uint64_t result = 0;
        g_in_replay = true;
        const bool ok = call_guarded(g_ctx.self, channel, msg_id, g_ctx.a4, g_ctx.a5, fake.data(), &result);
        g_in_replay = false;
        if (ok && result == 0) {
            g_replayed.fetch_add(1);
            ++done;
        } else {
            g_replay_failed.fetch_add(1);
            log::warn("build: replay of {} bytes {} (result {})", frame.size(), ok ? "returned an error" : "FAULTED", result);
            bridge::post_event(json{{"ev", "bb_replay_failed"}, {"faulted", !ok}, {"result", result}});
        }
    }
    if (done) bridge::post_event(json{{"ev", "bb_replayed"}, {"count", done}});
    return done;
}

// Any dispatched message's stream works as a template (all observed messages use the same
// stream class), so a player who never built anything can still apply remote operations.
bool capture_template(const void* stream, uint32_t msg_id) {
    std::array<uint8_t, kStreamObjSize> obj{};
    if (mem::safe_read(stream, obj.data(), obj.size()) != obj.size()) return false;
    uint32_t len = 0;
    uint64_t buf = 0;
    std::memcpy(&len, obj.data() + kStreamLenOff, 4);
    std::memcpy(&buf, obj.data() + kStreamBufOff, 8);
    uint32_t first = 0;
    if (len < 8 || len > kMaxFrame || mem::safe_read(reinterpret_cast<const void*>(buf), &first, 4) != 4 || first != msg_id) return false;
    g_ctx.stream_template = obj;
    g_ctx.valid = true;
    g_ctx_ready = true;
    return true;
}

uint64_t __fastcall detour(void* self, uint32_t a2, uint32_t msg_id, uint32_t a4, void* a5, void* stream) {
    const size_t idx = index_of(msg_id);
    g_counts[idx].fetch_add(1, std::memory_order_relaxed);
    const bool build = idx < kMessages.size() && kMessages[idx].build;

    // Remember this process's own dispatch context; replays reuse it on this same thread.
    if (!g_in_replay) {
        g_ctx.self = self;
        g_ctx.a4 = a4;
        g_ctx.thread = GetCurrentThreadId();
        g_dispatch_thread = g_ctx.thread;
        if (build || !g_have_build_a5) g_ctx.a5 = a5;  // build messages carry the value build ops expect
        if (msg_id == kRequestBuildBuyOp) g_have_build_a5 = true;
        if (!g_build_template) capture_template(stream, msg_id);
    }
    run_replays();

    if (msg_id == kRequestBuildBuyOp && !g_in_replay) {
        std::array<uint8_t, kStreamObjSize> obj{};
        if (auto frame = extract_frame(stream, msg_id, &obj)) {
            g_ctx.stream_template = obj;
            g_ctx.valid = true;
            g_ctx_ready = true;
            g_build_template = true;
            {
                std::scoped_lock lock(g_queue_mutex);
                g_last_local_frame = *frame;
            }
            if (g_gate && g_replicate && g_send && g_send(*frame, std::string())) g_sent.fetch_add(1);
        }
    }

    json capture;
    if (build) {
        // Dump BEFORE the original runs: it consumes the stream.
        capture = json{{"t", std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count()},
                       {"id", msg_id},
                       {"name", kMessages[idx].name},
                       {"a2", a2},
                       {"a4", a4},
                       {"a5", reinterpret_cast<uint64_t>(a5)},
                       {"stream", reinterpret_cast<uint64_t>(stream)},
                       {"stream_obj", dump_object(stream, 0x60)},
                       {"a5_obj", dump_object(a5, 0x40)}};
        if (auto frame = extract_frame(stream, msg_id, nullptr)) capture["frame"] = mem::hex(frame->data(), frame->size());
    }

    const uint64_t result = g_original(self, a2, msg_id, a4, a5, stream);

    if (build) {
        Record r;
        r.t = capture["t"].get<double>();
        r.id = msg_id;
        r.name = kMessages[idx].name;
        r.a2 = a2;
        r.a4 = a4;
        r.a5 = reinterpret_cast<uint64_t>(a5);
        r.stream = reinterpret_cast<uint64_t>(stream);
        r.result = result;
        capture["result"] = result;
        {
            std::scoped_lock lock(g_mutex);
            g_status.build_messages++;
            g_recent.push_back(r);
            while (g_recent.size() > 50) g_recent.pop_front();
            if (g_status.capture_to_file && g_capture.is_open() && g_status.records_written < kMaxRecordsToFile) {
                g_capture << capture.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
                g_capture.flush();
                g_status.records_written++;
            }
        }
        bridge::post_event(json{{"ev", "bb_msg"}, {"id", msg_id}, {"name", kMessages[idx].name}});
    }
    return result;
}

std::optional<uintptr_t> locate(const mem::Module& sim, std::string& how) {
    const auto pattern = mem::Pattern::parse(kPrologue);
    if (pattern) {
        const auto hits = mem::scan(sim.text_begin, sim.text_end, *pattern, 4);
        if (hits.size() == 1) {
            how = "aob";
            return hits.front();
        }
        log::warn("build: prologue signature matched {} times (expected 1); trying string xref", hits.size());
    }
    // Fallback: the function that references the ZoneReservationRequest name with a RIP-relative LEA.
    const std::string anchor = std::string(kAnchorString) + '\0';
    const auto* image = reinterpret_cast<const char*>(sim.base);
    uintptr_t str = 0;
    for (size_t i = 0; i + anchor.size() <= sim.size; ++i) {
        if (image[i] == anchor[0] && std::memcmp(image + i, anchor.data(), anchor.size()) == 0) {
            str = sim.base + i;
            break;
        }
    }
    if (!str) return std::nullopt;
    const auto* code = reinterpret_cast<const uint8_t*>(sim.text_begin);
    for (size_t i = 0; sim.text_begin + i + 7 <= sim.text_end; ++i) {
        if ((code[i] == 0x48 || code[i] == 0x4C) && code[i + 1] == 0x8D && (code[i + 2] & 0xC7) == 0x05) {
            int32_t disp = 0;
            std::memcpy(&disp, code + i + 3, 4);
            if (sim.text_begin + i + 7 + static_cast<intptr_t>(disp) == str) {
                if (auto f = mem::function_at(sim.text_begin + i)) {
                    how = "string-xref";
                    return f->begin;
                }
            }
        }
    }
    return std::nullopt;
}

bool validate(uintptr_t fn) {
    const auto range = mem::function_at(fn);
    if (!range || range->begin != fn || range->end - range->begin < 0x2000) return false;
    for (uint32_t id : {0x3a0f613cu, 0x3e6edcc2u, 0x2b6ec145u})
        if (!mem::contains(range->begin, range->end, le32(id))) return false;
    return true;
}

}  // namespace

bool install() {
    std::optional<mem::Module> sim;
    for (int i = 0; i < 600 && !(sim = mem::find_module(L"Simulation_x64.dll")); ++i) Sleep(100);
    if (!sim) {
        std::scoped_lock lock(g_mutex);
        g_status.how_found = "Simulation_x64.dll não carregada";
        log::warn("build: Simulation_x64.dll not loaded; build observer disabled");
        return false;
    }
    std::string how;
    const auto fn = locate(*sim, how);
    if (!fn || !validate(*fn)) {
        std::scoped_lock lock(g_mutex);
        g_status.how_found = "assinatura não encontrada (versão do jogo mudou?)";
        log::error("build: dispatcher not found/validated in this game version; build sync disabled");
        return false;
    }
    if (auto vt = mem::find_vtable(*sim, ".?AUPkgRequestBuildBuyOpMsg@Sims4@EA@@"))
        log::info("build: RTTI PkgRequestBuildBuyOpMsg vtable at rva {:#x}", *vt - sim->base);

    {
        std::scoped_lock lock(g_mutex);
        const auto file = paths::mod_dir() / L"build_capture.jsonl";
        g_capture.open(file, std::ios::out | std::ios::trunc);
        g_t0 = std::chrono::steady_clock::now();
    }
    if (!hooks::create(reinterpret_cast<void*>(*fn), &detour, &g_original, "Simulation::ClientMessageDispatch") || !hooks::enable_all()) {
        std::scoped_lock lock(g_mutex);
        g_status.how_found = "falha ao instalar o hook";
        return false;
    }
    std::scoped_lock lock(g_mutex);
    g_status.installed = true;
    g_status.how_found = how;
    g_status.rva = *fn - sim->base;
    log::info("build: client->sim dispatcher hooked at rva {:#x} (found by {}), observing build messages", g_status.rva, how);
    return true;
}

std::string probe(const wchar_t* module_name) {
    json out{{"module", paths::to_utf8(module_name)}};
    const auto sim = mem::find_module(module_name);
    if (!sim) {
        out["error"] = "module not loaded";
        return out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    std::string how;
    const auto fn = locate(*sim, how);
    out["found"] = fn.has_value();
    if (fn) {
        out["how"] = how;
        out["rva"] = *fn - sim->base;
        out["validated"] = validate(*fn);
        if (auto range = mem::function_at(*fn)) out["size"] = range->end - range->begin;
    }
    if (auto vt = mem::find_vtable(*sim, ".?AUPkgRequestBuildBuyOpMsg@Sims4@EA@@")) out["vtable_PkgRequestBuildBuyOpMsg"] = *vt - sim->base;
    if (auto vt = mem::find_vtable(*sim, ".?AVLotOperations@BuildBuy@Sims4@EA@@")) out["vtable_LotOperations"] = *vt - sim->base;
    return out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

namespace {
std::mutex g_hash_mutex;
LotHashView g_hash;
std::chrono::steady_clock::time_point g_hash_at{};
}  // namespace

void set_lot_hash(LotHashView v) {
    std::scoped_lock lock(g_hash_mutex);
    g_hash = std::move(v);
    g_hash_at = std::chrono::steady_clock::now();
}

LotHashView lot_hash() {
    std::scoped_lock lock(g_hash_mutex);
    LotHashView v = g_hash;
    if (g_hash_at.time_since_epoch().count())
        v.age_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_hash_at).count();
    return v;
}

namespace {
struct PresenceEntry {
    Presence p;
    std::chrono::steady_clock::time_point at;
};
std::mutex g_presence_mutex;
std::vector<PresenceEntry> g_presence;
}  // namespace

void set_presence(Presence p) {
    std::scoped_lock lock(g_presence_mutex);
    for (auto& e : g_presence) {
        if (e.p.name == p.name) {
            if (!p.has_position && e.p.has_position) {  // mode change only: keep last position
                p.has_position = true;
                p.x = e.p.x;
                p.y = e.p.y;
                p.z = e.p.z;
                p.level = e.p.level;
            }
            e = {std::move(p), std::chrono::steady_clock::now()};
            return;
        }
    }
    g_presence.push_back({std::move(p), std::chrono::steady_clock::now()});
}

std::vector<Presence> presences() {
    std::scoped_lock lock(g_presence_mutex);
    std::vector<Presence> out;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& e : g_presence) {
        Presence p = e.p;
        p.age_seconds = std::chrono::duration<double>(now - e.at).count();
        out.push_back(std::move(p));
    }
    return out;
}

void set_sender(SendFn fn) { g_send = fn; }

void on_remote_op(const std::string& from_peer, const uint8_t* data, size_t size, bool relay_to_others) {
    // Only RequestBuildBuyOp frames with a consistent header are accepted from the network.
    if (size < kFrameHeader || size > kMaxFrame) return;
    uint32_t id = 0, payload = 0;
    std::memcpy(&id, data, 4);
    std::memcpy(&payload, data + 8, 4);
    if (id != kRequestBuildBuyOp || kFrameHeader + payload != size) {
        log::warn("build: rejected malformed build op from {} ({} bytes)", from_peer, size);
        return;
    }
    if (!g_gate) {  // not in the same lot state as the sender: applying it could corrupt our lot
        g_dropped_gate.fetch_add(1);
        return;
    }
    std::vector<uint8_t> frame(data, data + size);
    if (relay_to_others && g_send) g_send(frame, from_peer);  // host: forward to the other guests
    std::scoped_lock lock(g_queue_mutex);
    g_replay_queue.push_back(std::move(frame));
}

void set_gate(bool open) {
    if (g_gate.exchange(open) != open) log::info("build: replication gate {}", open ? "OPEN" : "closed");
    if (!open) {
        std::scoped_lock lock(g_queue_mutex);
        g_replay_queue.clear();
    }
}

size_t pump() { return run_replays(); }

bool replay_last_local() {
    std::scoped_lock lock(g_queue_mutex);
    if (g_last_local_frame.empty()) return false;
    g_replay_queue.push_back(g_last_local_frame);
    return true;
}

void set_replication(bool enabled) { g_replicate = enabled; }

Status status() {
    std::scoped_lock lock(g_mutex);
    Status s = g_status;
    s.replication = g_replicate.load();
    s.context_ready = g_ctx_ready.load();
    s.gate_open = g_gate.load();
    s.dropped_gate = g_dropped_gate.load();
    s.ops_sent = g_sent.load();
    s.ops_replayed = g_replayed.load();
    s.ops_failed = g_replay_failed.load();
    {
        std::scoped_lock qlock(g_queue_mutex);
        s.queued = g_replay_queue.size();
    }
    s.total_messages = 0;
    for (const auto& c : g_counts) s.total_messages += c.load(std::memory_order_relaxed);
    return s;
}

std::vector<MessageStat> stats() {
    std::vector<MessageStat> out;
    for (size_t i = 0; i < kMessages.size(); ++i) {
        const uint64_t n = g_counts[i].load(std::memory_order_relaxed);
        if (n) out.push_back({kMessages[i].id, kMessages[i].name, n, kMessages[i].build});
    }
    if (const uint64_t n = g_counts[kMessages.size()].load(); n) out.push_back({0, "(id desconhecido)", n, false});
    return out;
}

std::vector<Record> recent(size_t max) {
    std::scoped_lock lock(g_mutex);
    const size_t n = std::min(max, g_recent.size());
    return {g_recent.end() - static_cast<std::ptrdiff_t>(n), g_recent.end()};
}

void set_capture(bool enabled) {
    std::scoped_lock lock(g_mutex);
    g_status.capture_to_file = enabled;
}

}  // namespace cts4p::build
