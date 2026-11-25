#pragma once
// Build/Buy message observer (Phase 4, step 1).
//
// Static analysis of Simulation_x64.dll (1.129.39.1030) found the function that
// dispatches every message the game client (TS4_x64.exe) sends to the
// simulation, switching on a 32-bit message id - including RequestBuildBuyOp
// (0x3a0f613c), which carries walls, floors, roofs, stairs, terrain and
// undo/redo. Its signature (from the prologue):
//
//   u64 Dispatch(void* self, u32 a2, u32 msg_id, u32 a4, void* a5, Stream* stream)
//
// It is located at runtime by an AOB signature of its prologue (unique in the
// module), validated by message-id constants that must appear in its body, and
// falls back to a string-xref search if the signature ever stops matching.
// Nothing is hooked unless validation passes.
//
// In this step the hook only OBSERVES: it counts every client->sim message,
// records build-related ones (ids, arguments and a safe memory dump of the
// stream object) to Mods/cts4p/build_capture.jsonl, and notifies the Python
// side so the Lot State Hash can run after build activity. The dumps are the
// data needed to decode the stream and implement replay on the other player.

#include <cstdint>
#include <string>
#include <vector>

namespace cts4p::build {

struct MessageStat {
    uint32_t id = 0;
    const char* name = "";
    uint64_t count = 0;
    bool build_related = false;
};

struct Record {
    double t = 0;  // seconds since hook install
    uint32_t id = 0;
    std::string name;
    uint32_t a2 = 0;
    uint32_t a4 = 0;
    uint64_t a5 = 0;
    uint64_t stream = 0;
    uint64_t result = 0;
};

// ---------------------------------------------------------------- replication (experimental)
// Each local RequestBuildBuyOp frame (u32 id | u32 channel | u32 len | payload, copied from
// the client's stream at +0x30/+0x20) is sent to the other players; received frames are
// queued and re-dispatched on this game's own dispatch thread with a stream object built
// from a template of a real stream (same process, same layout).
using SendFn = bool (*)(const std::vector<uint8_t>& frame, const std::string& except_peer);
void set_sender(SendFn fn);                                   // set by the session at startup
void on_remote_op(const std::string& from_peer, const uint8_t* data, size_t size, bool relay_to_others);
size_t pump();                                                // runs queued replays if on the dispatch thread
bool replay_last_local();                                     // single-PC test: re-dispatch our last op
void set_replication(bool enabled);
// Python opens the gate only while both games show the same lot (see sync/lot_hash.py).
void set_gate(bool open);

struct Status {
    bool installed = false;
    bool capture_to_file = true;
    bool replication = true;
    bool context_ready = false;  // has a dispatch context (template stream + thread) from this game
    bool gate_open = false;
    uint64_t dropped_gate = 0;
    uint64_t ops_sent = 0;
    uint64_t ops_replayed = 0;
    uint64_t ops_failed = 0;
    size_t queued = 0;
    std::string how_found;   // "aob" | "string-xref" | error text
    uintptr_t rva = 0;
    uint64_t total_messages = 0;
    uint64_t build_messages = 0;
    size_t records_written = 0;
};

// Waits for Simulation_x64.dll, locates and validates the dispatcher, hooks it.
bool install();

// Locates and validates without hooking (test harness: works on a module mapped
// with LoadLibraryEx(DONT_RESOLVE_DLL_REFERENCES)). Returns a JSON report.
std::string probe(const wchar_t* module_name);

// Lot State Hash results reported by the Python side, shown in the overlay.
struct LotHashView {
    std::string local;
    std::string summary;   // pt-BR, e.g. "idêntico ao host" / "diferente: corrigindo"
    std::string last_heal;
    double age_seconds = -1;
};
void set_lot_hash(LotHashView v);
LotHashView lot_hash();

// Where each player is acting (from the Python side), for the overlay.
struct Presence {
    std::string name;
    std::string mode;  // "vivo" | "construção"
    bool has_position = false;
    double x = 0, y = 0, z = 0;
    int level = 0;
    double age_seconds = 0;
};
void set_presence(Presence p);
std::vector<Presence> presences();

Status status();
std::vector<MessageStat> stats();
std::vector<Record> recent(size_t max = 20);
void set_capture(bool enabled);

}  // namespace cts4p::build
