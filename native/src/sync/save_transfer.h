#pragma once
// Host -> guest transfer of the current .save file over the reliable DataChannel.
//
//   SaveBegin  {xfer, slot_id, label, raw_size, comp_size, hash, chunk_size, chunks}
//   SaveChunk  u32 xfer | u32 index | bytes      (64 KiB, flow-controlled by bufferedAmount)
//   SaveEnd    {xfer}
//   SaveResult {xfer, ok, error}                 (guest -> host)
//
// The file is zstd-compressed in memory; the guest verifies sizes and an FNV-1a
// hash of the decompressed bytes, backs up any existing file in the same slot,
// and writes atomically (temp file + rename) into its saves folder.

#include <atomic>
#include <chrono>
#include <list>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/peer_link.h"

namespace cts4p::sync {

struct TransferStatus {
    uint32_t xfer = 0;
    bool outgoing = false;
    std::string peer;   // remote player name
    std::string label;  // save name shown in the game's load menu
    std::string state;  // pt-BR, human readable
    bool finished = false;
    bool ok = false;
    uint64_t done_bytes = 0;
    uint64_t total_bytes = 0;  // compressed bytes on the wire
    uint64_t raw_bytes = 0;
    std::string path;          // guest: where the save was written
    bool background = false;   // automatic copy update: the guest keeps playing, no reload needed
    bool tray = false;         // a household from the Library ("Criar juntos"), not a save
    double seconds = 0;
};

class SaveTransfer {
public:
    static SaveTransfer& instance();

    static constexpr size_t kChunkSize = 64 * 1024;
    static constexpr uint64_t kMaxRawBytes = 2ull << 30;   // 2 GiB
    static constexpr uint64_t kMaxCompBytes = 1ull << 30;  // 1 GiB

    // Host side. Returns false (and fills `error`) if the file can't be read.
    bool send_file(const std::filesystem::path& file, uint32_t slot_id, std::string label,
                   std::vector<std::shared_ptr<net::PeerLink>> targets, std::string& error, bool background = false);

    // Any player: copies a household from the Library (<user>\Tray) into the others' Library.
    bool send_household(uint64_t tray_id, std::vector<std::shared_ptr<net::PeerLink>> targets, std::string& error);

    // Both sides: Save* messages arriving from a peer.
    void on_message(const std::shared_ptr<net::PeerLink>& from, const net::Message& msg);

    // A link closed: drop its half-received transfers and stop waiting for its confirmations.
    void on_peer_gone(const std::string& peer_id);

    std::vector<TransferStatus> statuses() const;
    void shutdown();

private:
    SaveTransfer() = default;
    struct Incoming {
        uint32_t slot_id = 0;
        std::string label;
        bool background = false;
        bool tray = false;
        uint64_t raw_size = 0;
        unsigned long long hash = 0;
        uint32_t chunks = 0;
        uint32_t chunk_size = 0;
        std::vector<std::byte> data;
        std::vector<bool> got;
        uint32_t received = 0;
        size_t status_index = 0;
        bool finishing = false;
    };
    void send_worker(std::stop_token stop, std::vector<std::byte> comp, uint64_t raw_size, unsigned long long hash, uint32_t slot_id,
                     std::string label, bool background, bool tray, std::vector<std::shared_ptr<net::PeerLink>> targets,
                     std::vector<size_t> status_indexes);
    bool start_send(std::vector<std::byte> raw, uint32_t slot_id, std::string label, bool background, bool tray,
                    std::vector<std::shared_ptr<net::PeerLink>> targets, std::string& error);
    void finish_incoming(std::string peer_id, uint32_t xfer, std::shared_ptr<net::PeerLink> from);
    void handle_message(const std::shared_ptr<net::PeerLink>& from, const net::Message& msg);
    void spawn_locked(std::function<void(std::stop_token)> fn);  // mutex_ held
    size_t add_status(TransferStatus s);
    void update_status(size_t index, const std::function<void(TransferStatus&)>& fn);

    mutable std::mutex mutex_;
    std::vector<TransferStatus> statuses_;
    std::map<std::pair<std::string, uint32_t>, Incoming> incoming_;
    std::map<std::pair<std::string, uint32_t>, size_t> outgoing_index_;  // (peer id, xfer) -> status
    struct Worker {
        std::jthread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::list<Worker> workers_;
    std::map<std::string, int> households_from_;  // peer id -> households received this session
    uint32_t next_xfer_ = 1;
};

}  // namespace cts4p::sync
