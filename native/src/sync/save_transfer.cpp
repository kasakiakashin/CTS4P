#include "sync/save_transfer.h"

#include <cstring>
#include <format>
#include <fstream>
#include <functional>

#include <nlohmann/json.hpp>

#include "bridge/bridge.h"
#include "core/log.h"
#include "core/paths.h"
#include "net/session.h"
#include "sync/compress.h"
#include "sync/tray.h"

namespace cts4p::sync {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr size_t kMaxBuffered = 4u << 20;  // keep at most 4 MiB queued in SCTP per peer
constexpr int kMaxHouseholdsPerPeer = 20;  // "Criar juntos": plenty for a session, stops a peer from filling the disk
constexpr uint32_t kSyncSlot = 0x00054D50;  // the only slot a host may write on a guest (srcScripts sync/state.py)

uint32_t read_u32(const std::byte* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

std::string mb(uint64_t bytes) { return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0)); }

bool link_alive(const std::shared_ptr<net::PeerLink>& link) { return link && link->state() == net::LinkState::Ready; }

}  // namespace

void SaveTransfer::spawn_locked(std::function<void(std::stop_token)> fn) {
    std::erase_if(workers_, [](const Worker& w) { return w.done->load(); });  // finished: join is immediate
    auto done = std::make_shared<std::atomic<bool>>(false);
    workers_.push_back({std::jthread([fn = std::move(fn), done](std::stop_token stop) {
                            try {
                                fn(stop);
                            } catch (const std::exception& e) {
                                log::error("save: worker failed: {}", e.what());
                            }
                            done->store(true);
                        }),
                        done});
}

void SaveTransfer::on_peer_gone(const std::string& peer_id) {
    std::scoped_lock lock(mutex_);
    for (auto it = incoming_.begin(); it != incoming_.end();) {
        if (it->first.first != peer_id) {
            ++it;
            continue;
        }
        if (it->second.status_index < statuses_.size()) {
            auto& s = statuses_[it->second.status_index];
            s.finished = true;
            s.state = "interrompido (jogador saiu)";
        }
        it = incoming_.erase(it);
    }
    for (auto it = outgoing_index_.begin(); it != outgoing_index_.end();) {
        if (it->first.first != peer_id) {
            ++it;
            continue;
        }
        if (it->second < statuses_.size() && !statuses_[it->second].finished) {
            statuses_[it->second].finished = true;
            statuses_[it->second].state = "interrompido (jogador saiu)";
        }
        it = outgoing_index_.erase(it);
    }
}

SaveTransfer& SaveTransfer::instance() {
    static SaveTransfer s;
    return s;
}

size_t SaveTransfer::add_status(TransferStatus s) {
    std::scoped_lock lock(mutex_);
    statuses_.push_back(std::move(s));
    return statuses_.size() - 1;
}

void SaveTransfer::update_status(size_t index, const std::function<void(TransferStatus&)>& fn) {
    std::scoped_lock lock(mutex_);
    if (index < statuses_.size()) fn(statuses_[index]);
}

std::vector<TransferStatus> SaveTransfer::statuses() const {
    std::scoped_lock lock(mutex_);
    return statuses_;
}

bool SaveTransfer::send_file(const std::filesystem::path& file, uint32_t slot_id, std::string label,
                             std::vector<std::shared_ptr<net::PeerLink>> targets, std::string& error, bool background) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        error = "não consegui abrir " + paths::to_utf8(file.wstring());
        return false;
    }
    const auto size = static_cast<uint64_t>(in.tellg());
    if (size == 0 || size > kMaxRawBytes) {
        error = std::format("tamanho de save inválido ({} bytes)", size);
        return false;
    }
    std::vector<std::byte> raw(static_cast<size_t>(size));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(size));
    if (!in) {
        error = "falha ao ler o save";
        return false;
    }
    return start_send(std::move(raw), slot_id, std::move(label), background, false, std::move(targets), error);
}

bool SaveTransfer::send_household(uint64_t tray_id, std::vector<std::shared_ptr<net::PeerLink>> targets, std::string& error) {
    const auto dir = paths::tray_dir();
    if (!dir) {
        error = "pasta da Biblioteca (Tray) não encontrada";
        return false;
    }
    for (const auto& h : tray::list_households(*dir)) {
        if (h.id != tray_id) continue;
        auto blob = tray::pack(h.files, error);
        if (!blob) return false;
        log::info("tray: sending household '{}' ({} files, {} bytes)", h.name, h.files.size(), blob->size());
        return start_send(std::move(*blob), 0, h.name.empty() ? "Família" : h.name, false, true, std::move(targets), error);
    }
    error = "essa família não está mais na Biblioteca";
    return false;
}

bool SaveTransfer::start_send(std::vector<std::byte> raw, uint32_t slot_id, std::string label, bool background, bool tray,
                              std::vector<std::shared_ptr<net::PeerLink>> targets, std::string& error) {
    std::erase_if(targets, [](const auto& l) { return !link_alive(l); });
    if (targets.empty()) {
        error = tray ? "nenhum jogador conectado para receber o Sim" : "nenhum jogador conectado para receber o save";
        return false;
    }
    const uint64_t size = raw.size();
    const unsigned long long hash = fnv1a64(raw);
    std::vector<std::byte> comp = zstd_compress(raw, 3);
    if (comp.empty()) {
        error = "falha na compressão zstd";
        return false;
    }
    log::info("{}: {} -> {} compressed ({:.0f}%), sending to {} player(s)", tray ? "tray" : "save", mb(size), mb(comp.size()),
              100.0 * static_cast<double>(comp.size()) / static_cast<double>(size), targets.size());

    std::vector<size_t> indexes;
    for (const auto& t : targets) {
        TransferStatus st;
        st.outgoing = true;
        st.peer = t->info().name;
        st.label = label;
        st.background = background;
        st.tray = tray;
        st.state = "enviando";
        st.total_bytes = comp.size();
        st.raw_bytes = size;
        indexes.push_back(add_status(std::move(st)));
    }
    std::scoped_lock lock(mutex_);
    auto shared_comp = std::make_shared<std::vector<std::byte>>(std::move(comp));
    spawn_locked([this, shared_comp, size, hash, slot_id, label, background, tray, targets, indexes](std::stop_token stop) {
        send_worker(stop, std::move(*shared_comp), size, hash, slot_id, label, background, tray, targets, indexes);
    });
    return true;
}

void SaveTransfer::send_worker(std::stop_token stop, std::vector<std::byte> comp, uint64_t raw_size, unsigned long long hash,
                               uint32_t slot_id, std::string label, bool background, bool tray, std::vector<std::shared_ptr<net::PeerLink>> targets,
                               std::vector<size_t> indexes) {
    const uint32_t chunks = static_cast<uint32_t>((comp.size() + kChunkSize - 1) / kChunkSize);
    for (size_t t = 0; t < targets.size(); ++t) {
        const auto& link = targets[t];
        const size_t si = indexes[t];
        uint32_t xfer = 0;
        {
            std::scoped_lock lock(mutex_);
            xfer = next_xfer_++;
            statuses_[si].xfer = xfer;
            outgoing_index_[{link->id(), xfer}] = si;
        }
        const auto t0 = Clock::now();
        const json begin{{"xfer", xfer},           {"slot_id", slot_id}, {"label", label}, {"raw_size", raw_size},
                         {"comp_size", comp.size()}, {"hash", hash},       {"chunk_size", kChunkSize}, {"chunks", chunks},
                         {"background", background}, {"kind", tray ? "tray" : "save"}};
        bool ok = link->send(net::MsgType::SaveBegin, begin.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        std::string payload;
        for (uint32_t i = 0; ok && i < chunks && !stop.stop_requested(); ++i) {
            while (link_alive(link) && link->buffered_amount() > kMaxBuffered && !stop.stop_requested()) std::this_thread::sleep_for(2ms);
            const size_t off = static_cast<size_t>(i) * kChunkSize;
            const size_t n = std::min(kChunkSize, comp.size() - off);
            payload.resize(8 + n);
            std::memcpy(payload.data(), &xfer, 4);
            std::memcpy(payload.data() + 4, &i, 4);
            std::memcpy(payload.data() + 8, comp.data() + off, n);
            ok = link_alive(link) && link->send(net::MsgType::SaveChunk, payload);
            update_status(si, [&](TransferStatus& s) {
                s.done_bytes = off + n;
                s.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
            });
        }
        if (ok) ok = link->send(net::MsgType::SaveEnd, json{{"xfer", xfer}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        update_status(si, [&](TransferStatus& s) {
            s.state = ok ? "aguardando confirmação" : "falhou (conexão perdida)";
            if (!ok) s.finished = true;
        });
        log::info("save: {} to {} ({} chunks)", ok ? "sent" : "FAILED sending", link->info().name, chunks);
    }
}

void SaveTransfer::on_message(const std::shared_ptr<net::PeerLink>& from, const net::Message& msg) {
    try {  // peer-supplied JSON: a wrong type must never take the game down
        handle_message(from, msg);
    } catch (const std::exception& e) {
        log::warn("save: bad message from {}: {}", from->id(), e.what());
    }
}

void SaveTransfer::handle_message(const std::shared_ptr<net::PeerLink>& from, const net::Message& msg) {
    const std::string peer = from->id();
    if (msg.type == net::MsgType::SaveBegin) {
        const json j = json::parse(msg.payload_string(), nullptr, false);
        if (!j.is_object()) return;
        Incoming in;
        const uint32_t xfer = j.value("xfer", 0u);
        in.slot_id = j.value("slot_id", 0u);
        in.label = j.value("label", "CTS4P");
        in.background = j.value("background", false);
        in.tray = j.value("kind", "save") == "tray";
        in.raw_size = j.value("raw_size", 0ull);
        const uint64_t comp_size = j.value("comp_size", 0ull);
        in.hash = j.value("hash", 0ull);
        in.chunks = j.value("chunks", 0u);
        in.chunk_size = j.value("chunk_size", 0u);
        const bool sane = xfer != 0 && in.raw_size > 0 && in.raw_size <= kMaxRawBytes && comp_size > 0 && comp_size <= kMaxCompBytes &&
                          in.chunk_size > 0 && in.chunk_size <= 256 * 1024 &&
                          in.chunks == (comp_size + in.chunk_size - 1) / in.chunk_size &&
                          comp_size <= in.raw_size + in.raw_size / 64 + 64 * 1024 &&  // zstd never expands more than this
                          (in.tray ? in.raw_size <= tray::kMaxBytes + 64 * 1024 : in.slot_id != 0 && in.slot_id != 0xFFFFFFFFu);
        // Saves only flow host -> guest, and only into the cts4p slot: a peer can never overwrite
        // the player's own saves. Households (Library) may come from anyone in the room.
        // Decided from OUR state (we joined a room, so our only link is the host), never from the peer's claims.
        const bool allowed = in.tray || (net::Session::instance().mode() == net::SessionMode::Joining && in.slot_id == kSyncSlot);
        if (!allowed) {
            log::warn("save: refusing save from {} (slot {:08x})", peer, in.slot_id);
            from->send(net::MsgType::SaveResult, json{{"xfer", xfer}, {"ok", false}, {"error", "save recusado: só o host envia, e só para o slot do CTS4P"}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
            return;
        }
        if (!sane) {
            log::warn("save: rejecting invalid SaveBegin from {}", peer);
            from->send(net::MsgType::SaveResult, json{{"xfer", xfer}, {"ok", false}, {"error", "cabeçalho inválido"}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
            return;
        }
        {
            std::scoped_lock lock(mutex_);
            const bool busy = std::any_of(incoming_.begin(), incoming_.end(), [&](const auto& e) { return e.first.first == peer; });
            const bool quota = in.tray && households_from_[peer] >= kMaxHouseholdsPerPeer;
            if (busy || quota) {
                log::warn("save: refusing transfer from {} ({})", peer, busy ? "another one in progress" : "household quota");
                from->send(net::MsgType::SaveResult,
                           json{{"xfer", xfer}, {"ok", false}, {"error", busy ? "outra transferência em andamento" : "limite de famílias atingido"}}
                               .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
                return;
            }
            if (in.tray) households_from_[peer]++;
        }
        in.data.resize(static_cast<size_t>(comp_size));
        in.got.assign(in.chunks, false);
        TransferStatus st;
        st.xfer = xfer;
        st.peer = from->info().name;
        st.label = in.label;
        st.background = in.background;
        st.tray = in.tray;
        st.state = "recebendo";
        st.total_bytes = comp_size;
        st.raw_bytes = in.raw_size;
        in.status_index = add_status(std::move(st));
        std::scoped_lock lock(mutex_);
        incoming_[{peer, xfer}] = std::move(in);
        log::info("save: receiving '{}' ({} raw) from {}", j.value("label", ""), mb(j.value("raw_size", 0ull)), peer);
    } else if (msg.type == net::MsgType::SaveChunk) {
        if (msg.payload.size() < 8) return;
        const uint32_t xfer = read_u32(msg.payload.data());
        const uint32_t index = read_u32(msg.payload.data() + 4);
        std::scoped_lock lock(mutex_);
        auto it = incoming_.find({peer, xfer});
        if (it == incoming_.end()) return;
        Incoming& in = it->second;
        const size_t off = static_cast<size_t>(index) * in.chunk_size;
        const size_t n = msg.payload.size() - 8;
        if (index >= in.chunks || in.got[index] || off + n > in.data.size() || (index + 1 < in.chunks && n != in.chunk_size)) return;
        std::memcpy(in.data.data() + off, msg.payload.data() + 8, n);
        in.got[index] = true;
        in.received++;
        if (in.status_index < statuses_.size()) statuses_[in.status_index].done_bytes += n;
    } else if (msg.type == net::MsgType::SaveEnd) {
        const json j = json::parse(msg.payload_string(), nullptr, false);
        if (!j.is_object()) return;
        const uint32_t xfer = j.value("xfer", 0u);
        std::scoped_lock lock(mutex_);
        auto it = incoming_.find({peer, xfer});
        if (it == incoming_.end() || it->second.finishing) return;  // unknown or duplicate SaveEnd
        it->second.finishing = true;
        spawn_locked([this, peer, xfer, from](std::stop_token) { finish_incoming(peer, xfer, from); });
    } else if (msg.type == net::MsgType::SaveResult) {
        const json j = json::parse(msg.payload_string(), nullptr, false);
        if (!j.is_object()) return;
        const uint32_t xfer = j.value("xfer", 0u);
        const bool ok = j.value("ok", false);
        const std::string error = j.value("error", "");
        size_t si = SIZE_MAX;
        {
            std::scoped_lock lock(mutex_);
            if (auto it = outgoing_index_.find({peer, xfer}); it != outgoing_index_.end()) si = it->second;
        }
        if (si == SIZE_MAX) return;
        bool background = false;
        update_status(si, [&](TransferStatus& s) {
            s.finished = true;
            s.ok = ok;
            s.state = ok ? "entregue" : "recusado: " + error;
            background = s.background;
        });
        const std::string who = from->info().name;
        bool tray = false;
        update_status(si, [&](TransferStatus& s) { tray = s.tray; });
        if (tray)
            net::Session::instance().notice(ok ? "Família entregue para " + who + " (está na Biblioteca dele(a))"
                                               : "Família recusada por " + who + ": " + error);
        else if (!ok || !background)  // automatic copy updates stay quiet unless they fail
            net::Session::instance().notice(ok ? "Save entregue para " + who : "Save recusado por " + who + ": " + error);
        bridge::post_event(json{{"ev", "save_delivered"}, {"peer", peer}, {"name", who}, {"ok", ok}, {"error", error}});
    }
}

void SaveTransfer::finish_incoming(std::string peer, uint32_t xfer, std::shared_ptr<net::PeerLink> from) {
    Incoming in;
    {
        std::scoped_lock lock(mutex_);
        auto it = incoming_.find({peer, xfer});
        if (it == incoming_.end()) return;
        in = std::move(it->second);
        incoming_.erase(it);
    }
    std::string error;
    std::filesystem::path written;
    if (in.received != in.chunks) {
        error = std::format("faltaram {} de {} blocos", in.chunks - in.received, in.chunks);
    } else if (auto raw = zstd_decompress(in.data, static_cast<size_t>(in.raw_size)); !raw || raw->size() != in.raw_size) {
        error = "falha ao descomprimir";
    } else if (fnv1a64(*raw) != in.hash) {
        error = "hash não confere (arquivo corrompido)";
    } else if (in.tray) {
        if (auto dir = paths::tray_dir(); !dir) {
            error = "pasta da Biblioteca (Tray) não encontrada";
        } else if (auto un = tray::unpack_into(*raw, *dir, error)) {
            written = *dir;
            if (!un->name.empty()) in.label = un->name;
        }
    } else if (auto saves = paths::saves_dir(); !saves) {
        error = "pasta de saves não encontrada";
    } else {
        std::error_code ec;
        std::filesystem::create_directories(*saves, ec);
        const auto target = *saves / std::format(L"Slot_{:08x}.save", in.slot_id);
        const auto temp = std::filesystem::path(target).concat(L".cts4p-tmp");
        if (std::filesystem::exists(target, ec)) {
            std::filesystem::copy_file(target, std::filesystem::path(target).concat(L".cts4p-bak"),
                                       std::filesystem::copy_options::overwrite_existing, ec);
        }
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(raw->data()), static_cast<std::streamsize>(raw->size()));
            if (!out) error = "falha ao gravar o save";
        }
        if (error.empty()) {
            std::filesystem::rename(temp, target, ec);
            if (ec) error = "falha ao renomear: " + ec.message();
            else written = target;
        }
    }

    const bool ok = error.empty();
    update_status(in.status_index, [&](TransferStatus& s) {
        s.finished = true;
        s.ok = ok;
        s.state = ok ? "salvo" : "erro: " + error;
        s.path = paths::to_utf8(written.wstring());
    });
    from->send(net::MsgType::SaveResult, json{{"xfer", xfer}, {"ok", ok}, {"error", error}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    if (ok && in.tray) {
        const std::string who = from->info().name;
        log::info("tray: household '{}' from {} written to {}", in.label, who, paths::to_utf8(written.wstring()));
        net::Session::instance().notice(who + " mandou a família \"" + in.label + "\": já está na sua Biblioteca");
        bridge::post_event(json{{"ev", "tray_received"}, {"name", in.label}, {"from", peer}, {"dir", paths::to_utf8(written.wstring())}});
    } else if (ok) {
        log::info("save: '{}' written to {}", in.label, paths::to_utf8(written.wstring()));
        net::Session::instance().notice(in.background ? "Cópia do save atualizada (pode continuar jogando)"
                                                      : "Save do host recebido: em Carregar Jogo, abra '" + in.label + "'");
        bridge::post_event(json{{"ev", "save_received"}, {"path", paths::to_utf8(written.wstring())}, {"slot_id", in.slot_id},
                                {"label", in.label}, {"bytes", in.raw_size}, {"from", peer}, {"background", in.background}});
    } else {
        log::error("save: transfer {} from {} failed: {}", xfer, peer, error);
        net::Session::instance().notice((in.tray ? "Falha ao receber a família: " : "Falha ao receber o save: ") + error);
    }
}

void SaveTransfer::shutdown() {
    std::list<Worker> workers;
    {
        std::scoped_lock lock(mutex_);
        workers.swap(workers_);
    }
    for (auto& w : workers) w.thread.request_stop();
}

}  // namespace cts4p::sync
