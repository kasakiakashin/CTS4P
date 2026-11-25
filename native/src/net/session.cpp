#include "net/session.h"

#include <rtc/rtc.hpp>

#include "bridge/bridge.h"
#include "build/build_observer.h"
#include "core/config.h"
#include "core/log.h"
#include "diag/diagnostics.h"
#include "net/room_code.h"
#include "sync/save_transfer.h"

namespace cts4p::net {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr auto kTick = std::chrono::milliseconds(250);
constexpr auto kPingEvery = std::chrono::milliseconds(1000);
constexpr auto kRosterEvery = std::chrono::milliseconds(2000);
constexpr auto kHandshakeTimeout = std::chrono::seconds(25);
constexpr size_t kChatHistory = 200;
constexpr size_t kMaxChatChars = 300;
constexpr int kMaxHostRetries = 3;

bool terminal(LinkState s) { return s == LinkState::Closed || s == LinkState::Failed || s == LinkState::Incompatible; }

}  // namespace

void init_rtc_logging() {
    static std::once_flag once;
    std::call_once(once, [] {
        rtc::InitLogger(rtc::LogLevel::Warning, [](rtc::LogLevel level, std::string message) {
            if (level <= rtc::LogLevel::Error) log::error("rtc: {}", message);
            else log::warn("rtc: {}", message);
        });
    });
}

rtc::Configuration make_rtc_config() {
    rtc::Configuration cfg;
    for (const auto& url : config().ice_servers) {
        try {
            cfg.iceServers.emplace_back(url);
        } catch (const std::exception& e) {
            log::warn("ignoring invalid ICE server '{}': {}", url, e.what());
        }
    }
    return cfg;
}

Session& Session::instance() {
    static Session s;
    return s;
}

void Session::init(std::string local_name, std::string game_version) {
    std::scoped_lock lock(mutex_);
    if (initialized_) return;
    initialized_ = true;
    local_name_ = std::move(local_name);
    game_version_ = std::move(game_version);
    init_rtc_logging();
    build::set_sender([](const std::vector<uint8_t>& frame, const std::string& except) {
        return Session::instance().send_build_op(frame, except);
    });
    ticker_ = std::jthread([this](std::stop_token st) { tick_loop(st); });
}

void Session::set_local_name(std::string name) {
    std::scoped_lock lock(mutex_);
    if (!name.empty()) local_name_ = std::move(name);
}

void Session::reset_locked(SessionMode mode, std::string room) {
    mode_ = mode;
    room_ = std::move(room);
    error_.clear();
    my_signal_id_.clear();
    host_signal_id_.clear();
    roster_.clear();
    ++generation_;
}

void Session::host() {
    leave("nova sala");
    {
        std::scoped_lock lock(mutex_);
        reset_locked(SessionMode::Hosting, generate_room_code());
        host_retries_ = 0;
        status_ = "Criando sala...";
    }
    std::string room;
    {
        std::scoped_lock lock(mutex_);
        room = room_;
    }
    connect_signaling(room, "host");
    post_session_event();
}

void Session::join(const std::string& room_code) {
    auto code = normalize_room_code(room_code);
    if (!code) {
        std::scoped_lock lock(mutex_);
        error_ = "Código inválido. Use o formato CTS-XXXX.";
        return;
    }
    leave("trocando de sala");
    {
        std::scoped_lock lock(mutex_);
        reset_locked(SessionMode::Joining, *code);
        status_ = "Procurando sala " + *code + "...";
    }
    connect_signaling(*code, "guest");
    post_session_event();
}

void Session::connect_signaling(const std::string& room, const std::string& role) {
    auto client = std::make_shared<SignalingClient>();
    uint64_t gen = 0;
    std::string name;
    {
        std::scoped_lock lock(mutex_);
        gen = generation_;
        name = local_name_;
        signaling_ = client;
    }
    SignalingClient::Callbacks cb;
    cb.on_message = [this, gen](const json& msg) { on_signal_message(gen, msg); };
    cb.on_closed = [this, gen](const std::string& reason) { on_signal_closed(gen, reason); };
    if (!client->connect(config().signaling_url, room, role, name, std::move(cb))) {
        std::scoped_lock lock(mutex_);
        error_ = "URL de sinalização inválida em config.json: " + config().signaling_url;
        status_ = "Erro";
        mode_ = SessionMode::Idle;
    }
}

void Session::on_signal_message(uint64_t gen, const json& msg) {
    const std::string t = msg.value("t", "");
    std::shared_ptr<PeerLink> link;
    bool create_offer = false;
    bool retry_host = false;
    std::string remote;
    {
        std::scoped_lock lock(mutex_);
        if (gen != generation_) return;
        if (t == "welcome") {
            my_signal_id_ = msg.value("id", "");
            if (msg.contains("host") && msg["host"].is_string()) host_signal_id_ = msg["host"].get<std::string>();
            status_ = mode_ == SessionMode::Hosting ? "Sala " + room_ + " aberta. Passe o código para o outro jogador."
                                                    : "Sala encontrada. Conectando ao host...";
            log::info("signaling: {} room {}", mode_ == SessionMode::Hosting ? "hosting" : "joined", room_);
            return;
        }
        if (t == "error") {
            const std::string code = msg.value("code", "");
            if (code == "room_taken" && mode_ == SessionMode::Hosting && host_retries_ < kMaxHostRetries) {
                ++host_retries_;
                room_ = generate_room_code();
                ++generation_;
                retry_host = true;
            } else {
                error_ = code == "no_host"     ? "Sala não encontrada (código errado ou host offline)."
                         : code == "room_full" ? "Sala cheia."
                                               : "Servidor de sinalização: " + msg.value("message", code);
                status_ = "Erro";
                mode_ = SessionMode::Idle;
                ++generation_;
            }
        } else if (t == "peer_joined" && mode_ == SessionMode::Hosting) {
            remote = msg.value("id", "");
            if (remote.empty() || links_.contains(remote)) return;
            if (static_cast<int>(links_.size()) >= config().max_peers) {
                log::warn("room full, ignoring peer {}", remote);
                return;
            }
            create_offer = true;
        } else if (t == "signal") {
            remote = msg.value("from", "");
            if (auto it = links_.find(remote); it != links_.end()) link = it->second;
            else if (mode_ == SessionMode::Joining && remote == host_signal_id_) create_offer = false;  // answerer below
            else return;
        } else if (t == "peer_left") {
            remote = msg.value("id", "");
            if (auto it = links_.find(remote); it != links_.end()) link = it->second;
        } else if (t == "room_closed") {
            error_ = "O host fechou a sala.";
            status_ = "Desconectado";
        }
    }

    if (retry_host) {
        std::string room;
        {
            std::scoped_lock lock(mutex_);
            room = room_;
        }
        log::info("room code taken, retrying with {}", room);
        connect_signaling(room, "host");
        return;
    }
    if (t == "peer_joined" && create_offer) {
        make_link(remote, /*offerer=*/true);
        notice(msg.value("name", "Alguém") + " está entrando...");
    } else if (t == "signal") {
        if (!link) link = make_link(remote, /*offerer=*/false);
        if (link && msg.contains("data")) link->handle_signal(msg["data"]);
    } else if (t == "peer_left" && link) {
        link->close("saiu");
    } else if (t == "room_closed") {
        leave("sala fechada");
    }
}

void Session::on_signal_closed(uint64_t gen, const std::string& reason) {
    std::scoped_lock lock(mutex_);
    if (gen != generation_ || mode_ == SessionMode::Idle) return;
    // Once P2P links are up the signaling server is no longer needed.
    bool any_ready = false;
    for (const auto& [id, link] : links_) any_ready |= link->state() == LinkState::Ready;
    if (!any_ready) {
        error_ = "Sem conexão com o servidor de sinalização (" + reason + "). Verifique signaling_url em config.json.";
        status_ = "Erro";
    }
}

std::shared_ptr<PeerLink> Session::make_link(const std::string& remote_id, bool offerer) {
    std::shared_ptr<SignalingClient> sig;
    LocalIdentity me;
    {
        std::scoped_lock lock(mutex_);
        sig = signaling_;
        me.name = local_name_;
        me.role = mode_ == SessionMode::Hosting ? "host" : "guest";
        me.game_version = game_version_;
    }
    LinkCallbacks cb;
    std::weak_ptr<SignalingClient> weak_sig = sig;
    cb.send_signal = [weak_sig, remote_id](const json& data) {
        if (auto s = weak_sig.lock()) s->send(json{{"t", "signal"}, {"to", remote_id}, {"data", data}});
    };
    cb.on_state = [this](const std::string& id, LinkState s) { on_link_state(id, s); };
    cb.on_message = [this](const std::string& id, const Message& m) { on_link_message(id, m); };

    std::shared_ptr<PeerLink> link;
    try {
        link = PeerLink::create(remote_id, offerer, make_rtc_config(), std::move(me), std::move(cb));
    } catch (const std::exception& e) {
        log::error("could not create peer connection: {}", e.what());
        return nullptr;
    }
    std::scoped_lock lock(mutex_);
    links_[remote_id] = link;
    return link;
}

void Session::on_link_state(const std::string& id, LinkState s) {
    std::string name;
    {
        std::scoped_lock lock(mutex_);
        auto it = links_.find(id);
        if (it == links_.end()) return;
        name = it->second->info().name;
        if (s == LinkState::Ready) {
            status_ = mode_ == SessionMode::Hosting ? "Sala " + room_ + " - jogador conectado!" : "Conectado ao host!";
        } else if (terminal(s) && mode_ == SessionMode::Joining && id == host_signal_id_) {
            status_ = "Conexão com o host encerrada";
            if (s != LinkState::Closed) error_ = it->second->info().error;
        }
    }
    if (name.empty()) name = "Jogador";
    std::shared_ptr<PeerLink> link;
    std::string role;
    {
        std::scoped_lock lock(mutex_);
        if (auto it = links_.find(id); it != links_.end()) link = it->second;
        if (terminal(s)) compat_.erase(id);
    }
    if (terminal(s)) sync::SaveTransfer::instance().on_peer_gone(id);
    if (link) role = link->info().role;
    if (s == LinkState::Ready) {
        add_chat({"", name + " entrou na sala", true, std::chrono::system_clock::now()});
        notice(name + " conectado");
        if (link) send_manifest(link);
        bridge::post_event(json{{"ev", "peer_ready"}, {"id", id}, {"name", name}, {"role", role}});
    } else if (terminal(s)) {
        add_chat({"", name + " saiu (" + std::string(state_name(s)) + ")", true, std::chrono::system_clock::now()});
        bridge::post_event(json{{"ev", "peer_left"}, {"id", id}, {"name", name}, {"reason", state_name(s)}});
    }
}

void Session::send_manifest(const std::shared_ptr<PeerLink>& link) {
    const auto env = diag::cached();
    const auto bytes = sync::encode(sync::from_environment(*env));
    const std::string payload(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (payload.size() > kMaxMessageSize - kHeaderSize) {
        log::warn("manifest too large to send ({} bytes, {} mods)", payload.size(), env->mods.size());
        return;
    }
    link->send(MsgType::Manifest, payload);
    log::info("manifest sent to {} ({} packs, {} mods, {} bytes)", link->id(), env->packs.size(), env->mods.size(), payload.size());
}

void Session::on_manifest(const std::shared_ptr<PeerLink>& link, const Message& m) {
    auto remote = sync::decode(m.payload.data(), m.payload.size());
    if (!remote) {
        log::warn("invalid manifest from {}", link->id());
        return;
    }
    const std::string name = link->info().name;
    sync::CompatReport report = sync::compare(sync::from_environment(*diag::cached()), *remote, name);
    log::info("compat with {}: {}", name, report.summary());
    if (!report.fully_compatible()) notice("Atenção (" + name + "): " + report.summary());
    auto first = [](const std::vector<std::string>& v) {
        return std::vector<std::string>(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(v.size(), 50)));
    };
    bridge::post_event(json{{"ev", "compat"},
                            {"id", link->id()},
                            {"name", name},
                            {"compatible", report.fully_compatible()},
                            {"game_version_matches", report.game_version_matches()},
                            {"summary", report.summary()},
                            {"packs_missing_here", report.packs_missing_here},
                            {"packs_missing_there", report.packs_missing_there},
                            {"mods_missing_here", first(report.mods_missing_here)},
                            {"mods_missing_there", first(report.mods_missing_there)},
                            {"mods_different", first(report.mods_different)}});
    std::scoped_lock lock(mutex_);
    compat_[link->id()] = std::move(report);
}

bool Session::send_game(const std::string& target, uint8_t msg_type, const std::string& payload, bool reliable) {
    if (!is_gameplay(static_cast<MsgType>(msg_type))) return false;
    std::vector<std::shared_ptr<PeerLink>> targets;
    {
        std::scoped_lock lock(mutex_);
        for (const auto& [id, link] : links_)
            if ((target.empty() || id == target) && link->state() == LinkState::Ready) targets.push_back(link);
    }
    bool ok = !targets.empty();
    for (auto& link : targets) ok = link->send(static_cast<MsgType>(msg_type), payload, reliable) && ok;
    return ok;
}

bool Session::send_build_op(const std::vector<uint8_t>& frame, const std::string& except_peer) {
    const std::string payload(reinterpret_cast<const char*>(frame.data()), frame.size());
    bool any = false;
    for (auto& link : ready_links()) {
        if (link->id() == except_peer) continue;
        any = link->send(MsgType::BuildOp, payload) || any;
    }
    return any;
}

std::vector<std::shared_ptr<PeerLink>> Session::ready_links() const {
    std::scoped_lock lock(mutex_);
    std::vector<std::shared_ptr<PeerLink>> out;
    for (const auto& [id, link] : links_)
        if (link->state() == LinkState::Ready) out.push_back(link);
    return out;
}

SessionMode Session::mode() const {
    std::scoped_lock lock(mutex_);
    return mode_;
}

void Session::post_session_event() {
    std::string mode, room, name;
    {
        std::scoped_lock lock(mutex_);
        mode = mode_ == SessionMode::Hosting ? "host" : mode_ == SessionMode::Joining ? "guest" : "idle";
        room = room_;
        name = local_name_;
    }
    bridge::post_event(json{{"ev", "session"}, {"mode", mode}, {"room", room}, {"local_name", name}});
}

void Session::on_link_message(const std::string& id, const Message& m) {
    if (m.type == MsgType::BuildOp) {
        build::on_remote_op(id, reinterpret_cast<const uint8_t*>(m.payload.data()), m.payload.size(), mode() == SessionMode::Hosting);
        return;
    }
    if (is_gameplay(m.type)) {
        bridge::post_net_recv(id, static_cast<uint8_t>(m.type), m.payload);
        return;
    }
    if (m.type == MsgType::Manifest || (m.type >= MsgType::SaveBegin && m.type <= MsgType::SaveResult)) {
        std::shared_ptr<PeerLink> link;
        {
            std::scoped_lock lock(mutex_);
            if (auto it = links_.find(id); it != links_.end()) link = it->second;
        }
        if (!link) return;
        if (m.type == MsgType::Manifest) on_manifest(link, m);
        else sync::SaveTransfer::instance().on_message(link, m);
        return;
    }
    if (m.type == MsgType::Chat) {
        const json j = json::parse(m.payload_string(), nullptr, false);
        if (!j.is_object()) return;
        std::string from = j.value("from", "?");
        std::string text = j.value("text", "");
        if (text.empty()) return;
        if (text.size() > kMaxChatChars) text.resize(kMaxChatChars);

        std::vector<std::shared_ptr<PeerLink>> relay;
        {
            std::scoped_lock lock(mutex_);
            if (mode_ == SessionMode::Hosting) {
                auto it = links_.find(id);
                if (it != links_.end()) from = it->second->info().name;  // host vouches for the sender name
                for (const auto& [other_id, link] : links_)
                    if (other_id != id) relay.push_back(link);
            }
        }
        const std::string out = json{{"from", from}, {"text", text}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        for (auto& link : relay) link->send(MsgType::Chat, out);
        add_chat({from, text, false, std::chrono::system_clock::now()});
        notice(from + ": " + text);
    } else if (m.type == MsgType::Roster) {
        const json j = json::parse(m.payload_string(), nullptr, false);
        if (!j.is_array()) return;
        std::vector<RosterEntry> roster;
        for (const auto& e : j) {
            if (e.is_object()) roster.push_back({e.value("name", "?"), e.value("role", "?"), e.value("rtt_ms", -1.0)});
        }
        std::scoped_lock lock(mutex_);
        roster_ = std::move(roster);
    }
}

void Session::send_chat(const std::string& raw) {
    std::string text = raw;
    if (text.empty()) return;
    if (text.size() > kMaxChatChars) text.resize(kMaxChatChars);
    std::vector<std::shared_ptr<PeerLink>> targets;
    std::string me;
    {
        std::scoped_lock lock(mutex_);
        me = local_name_;
        for (const auto& [id, link] : links_)
            if (link->state() == LinkState::Ready) targets.push_back(link);
    }
    const std::string out = json{{"from", me}, {"text", text}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    for (auto& link : targets) link->send(MsgType::Chat, out);
    add_chat({me, text, false, std::chrono::system_clock::now()});
}

void Session::leave(const std::string& reason) {
    std::vector<std::shared_ptr<PeerLink>> links;
    std::shared_ptr<SignalingClient> sig;
    bool was_active = false;
    {
        std::scoped_lock lock(mutex_);
        for (auto& [id, link] : links_) links.push_back(link);
        links_.clear();
        compat_.clear();
        sig.swap(signaling_);
        was_active = mode_ != SessionMode::Idle;
        if (was_active) status_ = "Desconectado";
        mode_ = SessionMode::Idle;
        roster_.clear();
        ++generation_;
    }
    for (auto& link : links) link->close(reason);
    if (sig) sig->close();
    if (was_active) post_session_event();
}

void Session::tick_loop(std::stop_token stop) {
    auto next_ping = Clock::now();
    auto next_roster = Clock::now();
    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(kTick);
        const auto now = Clock::now();

        std::vector<std::shared_ptr<PeerLink>> links;
        SessionMode mode;
        std::string me;
        {
            std::scoped_lock lock(mutex_);
            mode = mode_;
            me = local_name_;
            // Drop links that ended (kept briefly so the UI can show why).
            std::erase_if(links_, [&](const auto& kv) {
                const PeerInfo info = kv.second->info();
                return terminal(info.state) && now - info.created_at > std::chrono::seconds(3);
            });
            for (auto& [id, link] : links_) links.push_back(link);
        }

        for (auto& link : links) {
            const PeerInfo info = link->info();
            if (!terminal(info.state) && info.state != LinkState::Ready && now - info.created_at > kHandshakeTimeout) {
                link->close("tempo esgotado no handshake (NAT restritivo? configure um servidor TURN)");
            }
        }
        if (now >= next_ping) {
            next_ping = now + kPingEvery;
            for (auto& link : links) link->ping();
        }
        if (mode == SessionMode::Hosting && now >= next_roster) {
            next_roster = now + kRosterEvery;
            json roster = json::array({json{{"name", me}, {"role", "host"}, {"rtt_ms", 0.0}}});
            for (auto& link : links) {
                const PeerInfo info = link->info();
                if (info.state == LinkState::Ready)
                    roster.push_back(json{{"name", info.name}, {"role", "guest"}, {"rtt_ms", info.rtt_ms}});
            }
            const std::string payload = roster.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            for (auto& link : links)
                if (link->state() == LinkState::Ready) link->send(MsgType::Roster, payload);
        }
    }
}

void Session::add_chat(ChatLine line) {
    std::scoped_lock lock(mutex_);
    chat_.push_back(std::move(line));
    while (chat_.size() > kChatHistory) chat_.pop_front();
}

void Session::notice(std::string text) {
    std::scoped_lock lock(mutex_);
    notices_.push_back(std::move(text));
    while (notices_.size() > 8) notices_.pop_front();
}

std::vector<std::string> Session::take_notices() {
    std::scoped_lock lock(mutex_);
    std::vector<std::string> out(notices_.begin(), notices_.end());
    notices_.clear();
    return out;
}

SessionSnapshot Session::snapshot() const {
    std::vector<std::shared_ptr<PeerLink>> links;
    SessionSnapshot s;
    std::shared_ptr<SignalingClient> sig;
    {
        std::scoped_lock lock(mutex_);
        s.mode = mode_;
        s.room = room_;
        s.status = status_;
        s.error = error_;
        s.local_name = local_name_;
        s.roster = roster_;
        s.chat.assign(chat_.begin(), chat_.end());
        sig = signaling_;
        for (const auto& [id, link] : links_) links.push_back(link);
    }
    s.signaling_open = sig && sig->is_open();
    for (auto& link : links) s.peers.push_back(link->info());
    std::scoped_lock lock(mutex_);
    for (const auto& [id, report] : compat_) s.compat.push_back({id, report});
    return s;
}

void Session::shutdown() {
    leave("jogo fechado");
    ticker_.request_stop();
    if (ticker_.joinable()) ticker_.join();
}

}  // namespace cts4p::net
