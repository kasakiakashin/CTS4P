#include "net/peer_link.h"

#include <cstring>

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include "core/log.h"
#include "core/version.h"

namespace cts4p::net {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr const char* kReliableLabel = "reliable";
constexpr const char* kFastLabel = "fast";

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

bool is_terminal(LinkState s) {
    return s == LinkState::Closed || s == LinkState::Failed || s == LinkState::Incompatible;
}

}  // namespace

const char* state_name(LinkState s) {
    switch (s) {
        case LinkState::New: return "novo";
        case LinkState::Connecting: return "conectando";
        case LinkState::Open: return "canal aberto";
        case LinkState::Ready: return "pronto";
        case LinkState::Incompatible: return "incompatível";
        case LinkState::Failed: return "falhou";
        case LinkState::Closed: return "fechado";
    }
    return "?";
}

std::shared_ptr<PeerLink> PeerLink::create(std::string remote_id, bool offerer, const rtc::Configuration& config,
                                           LocalIdentity me, LinkCallbacks callbacks) {
    auto link = std::make_shared<PeerLink>(Private{}, std::move(remote_id), std::move(me), std::move(callbacks));
    link->setup(offerer, config);
    return link;
}

PeerLink::PeerLink(Private, std::string remote_id, LocalIdentity me, LinkCallbacks callbacks)
    : id_(std::move(remote_id)), me_(std::move(me)), cb_(std::move(callbacks)) {
    info_.id = id_;
    info_.created_at = Clock::now();
}

PeerLink::~PeerLink() {
    if (pc_) {
        try {
            pc_->close();
        } catch (...) {
        }
    }
}

void PeerLink::setup(bool offerer, const rtc::Configuration& config) {
    pc_ = std::make_shared<rtc::PeerConnection>(config);
    std::weak_ptr<PeerLink> weak = weak_from_this();

    pc_->onLocalDescription([weak](rtc::Description d) {
        if (auto self = weak.lock(); self && self->cb_.send_signal) {
            self->cb_.send_signal(json{{"kind", "description"}, {"type", d.typeString()}, {"sdp", std::string(d)}});
        }
    });
    pc_->onLocalCandidate([weak](rtc::Candidate c) {
        if (auto self = weak.lock(); self && self->cb_.send_signal) {
            self->cb_.send_signal(json{{"kind", "candidate"}, {"candidate", std::string(c)}, {"mid", c.mid()}});
        }
    });
    pc_->onStateChange([weak](rtc::PeerConnection::State st) {
        auto self = weak.lock();
        if (!self) return;
        using S = rtc::PeerConnection::State;
        if (st == S::Failed) self->set_state(LinkState::Failed, "conexão ICE falhou (NAT/firewall?)");
        else if (st == S::Disconnected) self->set_state(LinkState::Failed, "conexão perdida");
        else if (st == S::Closed) self->set_state(LinkState::Closed);
    });

    set_state(LinkState::Connecting);
    if (offerer) {
        rtc::DataChannelInit fast_init;
        fast_init.reliability.unordered = true;
        fast_init.reliability.maxRetransmits = 0u;
        attach(pc_->createDataChannel(kReliableLabel));
        attach(pc_->createDataChannel(kFastLabel, fast_init));
    } else {
        pc_->onDataChannel([weak](std::shared_ptr<rtc::DataChannel> dc) {
            if (auto self = weak.lock()) self->attach(dc);
        });
    }
}

void PeerLink::attach(const std::shared_ptr<rtc::DataChannel>& dc) {
    const std::string label = dc->label();
    {
        std::scoped_lock lock(mutex_);
        if (label == kReliableLabel) reliable_ = dc;
        else if (label == kFastLabel) fast_ = dc;
        else {
            log::warn("peer {}: ignoring unknown DataChannel '{}'", id_, label);
            return;
        }
    }
    std::weak_ptr<PeerLink> weak = weak_from_this();
    std::weak_ptr<rtc::DataChannel> weak_dc = dc;
    dc->onOpen([weak] {
        if (auto self = weak.lock()) self->on_channel_open();
    });
    dc->onClosed([weak, label] {
        if (auto self = weak.lock(); self && label == kReliableLabel) self->set_state(LinkState::Closed);
    });
    dc->onMessage([weak, weak_dc](rtc::message_variant data) {
        auto self = weak.lock();
        auto channel = weak_dc.lock();
        if (!self || !channel) return;
        if (const auto* bin = std::get_if<rtc::binary>(&data)) self->on_raw(channel, *bin);
    });
    // The answerer may receive channels that are already open.
    if (dc->isOpen()) on_channel_open();
}

void PeerLink::on_channel_open() {
    std::shared_ptr<rtc::DataChannel> reliable;
    {
        std::scoped_lock lock(mutex_);
        reliable = reliable_;
    }
    if (!reliable || !reliable->isOpen()) return;
    if (state_ == LinkState::Connecting) set_state(LinkState::Open);
    if (!hello_sent_.exchange(true)) {
        const json hello{{"proto", kProtocolVersion},
                         {"mod", kModVersion},
                         {"game", me_.game_version},
                         {"name", me_.name},
                         {"role", me_.role}};
        send(MsgType::Hello, hello.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    }
    maybe_ready();  // the remote Hello/Ack may have arrived before our channel reported open
}

void PeerLink::on_raw(const std::shared_ptr<rtc::DataChannel>& from, std::span<const std::byte> data) {
    auto msg = decode(data);
    if (!msg) {
        log::warn("peer {}: dropped malformed message ({} bytes)", id_, data.size());
        return;
    }
    switch (msg->type) {
        case MsgType::Hello: on_hello(*msg); return;
        case MsgType::HelloAck: {
            const json j = json::parse(msg->payload_string(), nullptr, false);
            if (j.is_object() && j.value("ok", false)) {
                ack_received_ = true;
                maybe_ready();
            } else {
                set_state(LinkState::Incompatible, j.is_object() ? j.value("reason", "recusado") : "recusado");
            }
            return;
        }
        case MsgType::Ping: send_on(from, MsgType::Pong, msg->payload); return;
        case MsgType::Pong: on_pong(*msg); return;
        case MsgType::Bye: {
            const json j = json::parse(msg->payload_string(), nullptr, false);
            set_state(LinkState::Closed, j.is_object() ? j.value("reason", "") : "");
            return;
        }
        default:
            if (state_ == LinkState::Ready && cb_.on_message) cb_.on_message(id_, *msg);
            return;
    }
}

void PeerLink::on_hello(const Message& m) {
    const json j = json::parse(m.payload_string(), nullptr, false);
    if (!j.is_object()) {
        set_state(LinkState::Incompatible, "Hello inválido");
        return;
    }
    const unsigned proto = j.value("proto", 0u);
    bool matches = false;
    {
        std::scoped_lock lock(mutex_);
        info_.name = j.value("name", "?");
        info_.role = j.value("role", "?");
        info_.mod_version = j.value("mod", "?");
        info_.game_version = j.value("game", "?");
        info_.game_version_matches = info_.game_version == me_.game_version;
        matches = info_.game_version_matches;
    }
    if (proto != kProtocolVersion) {
        const std::string reason = std::format("protocolo {} != {} (atualize o cts4p)", proto, kProtocolVersion);
        send(MsgType::HelloAck, json{{"ok", false}, {"reason", reason}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        set_state(LinkState::Incompatible, reason);
        return;
    }
    if (!matches) log::warn("peer {}: game version differs ({} vs {})", id_, info().game_version, me_.game_version);
    hello_received_ = true;
    send(MsgType::HelloAck, json{{"ok", true}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    maybe_ready();
}

void PeerLink::on_pong(const Message& m) {
    if (m.payload.size() != sizeof(int64_t)) return;
    int64_t sent = 0;
    std::memcpy(&sent, m.payload.data(), sizeof(sent));
    const double rtt = static_cast<double>(now_us() - sent) / 1000.0;
    if (rtt < 0) return;
    std::scoped_lock lock(mutex_);
    info_.pongs_received++;
    info_.rtt_ms = info_.rtt_ms < 0 ? rtt : info_.rtt_ms * 0.8 + rtt * 0.2;  // EWMA
    info_.rtt_min_ms = info_.rtt_min_ms < 0 ? rtt : std::min(info_.rtt_min_ms, rtt);
    info_.rtt_max_ms = std::max(info_.rtt_max_ms, rtt);
}

void PeerLink::maybe_ready() {
    if (hello_received_ && ack_received_ && state_ == LinkState::Open) {
        set_state(LinkState::Ready);
        log::info("peer {} ({}) ready", id_, info().name);
    }
}

void PeerLink::set_state(LinkState s, std::string error) {
    LinkState prev = state_.load();
    do {
        if (is_terminal(prev) || prev == s) return;  // terminal states are sticky
    } while (!state_.compare_exchange_weak(prev, s));
    {
        std::scoped_lock lock(mutex_);
        info_.state = s;
        if (!error.empty()) info_.error = error;
    }
    if (!error.empty()) log::warn("peer {}: {} ({})", id_, state_name(s), error);
    if (cb_.on_state) cb_.on_state(id_, s);
}

bool PeerLink::send_on(const std::shared_ptr<rtc::DataChannel>& dc, MsgType type, std::span<const std::byte> payload) {
    if (!dc || !dc->isOpen()) return false;
    try {
        auto bytes = encode(type, seq_.fetch_add(1), payload);
        // libdatachannel returns false when the message was queued instead of sent immediately;
        // that is not an error (failures throw). Flow control uses buffered_amount().
        dc->send(std::move(bytes));
        return true;
    } catch (const std::exception& e) {
        log::warn("peer {}: send {} failed: {}", id_, type_name(type), e.what());
        return false;
    }
}

bool PeerLink::send(MsgType type, const std::string& payload, bool reliable) {
    std::shared_ptr<rtc::DataChannel> dc;
    {
        std::scoped_lock lock(mutex_);
        dc = (reliable || !fast_ || !fast_->isOpen()) ? reliable_ : fast_;
    }
    return send_on(dc, type, std::as_bytes(std::span(payload.data(), payload.size())));
}

void PeerLink::ping() {
    if (state_ != LinkState::Ready) return;
    const int64_t t = now_us();
    std::shared_ptr<rtc::DataChannel> dc;
    {
        std::scoped_lock lock(mutex_);
        dc = (fast_ && fast_->isOpen()) ? fast_ : reliable_;
        info_.pings_sent++;
    }
    send_on(dc, MsgType::Ping, std::as_bytes(std::span(&t, 1)));
}

void PeerLink::handle_signal(const nlohmann::json& signal) {
    try {
        const std::string kind = signal.value("kind", "");
        if (kind == "description") {
            pc_->setRemoteDescription(rtc::Description(signal.at("sdp").get<std::string>(), signal.at("type").get<std::string>()));
        } else if (kind == "candidate") {
            pc_->addRemoteCandidate(rtc::Candidate(signal.at("candidate").get<std::string>(), signal.value("mid", "")));
        }
    } catch (const std::exception& e) {
        log::warn("peer {}: bad signal: {}", id_, e.what());
    }
}

void PeerLink::close(const std::string& reason) {
    if (state_ == LinkState::Ready || state_ == LinkState::Open) send(MsgType::Bye, json{{"reason", reason}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    set_state(LinkState::Closed, reason);
    try {
        if (pc_) pc_->close();
    } catch (...) {
    }
}

size_t PeerLink::buffered_amount() const {
    std::shared_ptr<rtc::DataChannel> dc;
    {
        std::scoped_lock lock(mutex_);
        dc = reliable_;
    }
    return dc ? dc->bufferedAmount() : 0;
}

PeerInfo PeerLink::info() const {
    std::scoped_lock lock(mutex_);
    return info_;
}

}  // namespace cts4p::net
