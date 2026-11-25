#include "net/selftest.h"

#include <condition_variable>
#include <deque>
#include <functional>

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include "core/log.h"
#include "core/version.h"
#include "net/peer_link.h"
#include "net/room_code.h"
#include "net/session.h"
#include "net/signaling.h"

namespace cts4p::net {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Polls `cond` until true or timeout; `pump` runs between polls.
bool wait_for(const std::function<bool()>& cond, std::chrono::milliseconds timeout,
              const std::function<void()>& pump = {}) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (pump) pump();
        if (cond()) return true;
        std::this_thread::sleep_for(5ms);
    }
    if (pump) pump();
    return cond();
}

// Thread-safe FIFO of deferred actions (signals are applied on the test thread,
// never re-entrantly inside a libdatachannel callback).
class TaskQueue {
public:
    void push(std::function<void()> fn) {
        std::scoped_lock lock(mutex_);
        tasks_.push_back(std::move(fn));
    }
    void drain() {
        std::deque<std::function<void()>> batch;
        {
            std::scoped_lock lock(mutex_);
            batch.swap(tasks_);
        }
        for (auto& fn : batch) fn();
    }

private:
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
};

}  // namespace

SelfTest& SelfTest::instance() {
    static SelfTest s;
    return s;
}

void SelfTest::begin(std::string title, std::vector<std::string> steps) {
    std::scoped_lock lock(mutex_);
    report_ = TestReport{};
    report_.title = std::move(title);
    report_.running = true;
    for (auto& s : steps) report_.steps.push_back({std::move(s), StepStatus::Pending, {}});
}

void SelfTest::step(size_t index, StepStatus status, std::string detail) {
    std::scoped_lock lock(mutex_);
    if (index >= report_.steps.size()) return;
    report_.steps[index].status = status;
    if (!detail.empty()) report_.steps[index].detail = std::move(detail);
    if (status == StepStatus::Failed) log::warn("selftest '{}': step '{}' failed: {}", report_.title, report_.steps[index].name, report_.steps[index].detail);
}

void SelfTest::finish(bool passed, double elapsed_ms) {
    std::scoped_lock lock(mutex_);
    report_.running = false;
    report_.finished = true;
    report_.passed = passed;
    report_.elapsed_ms = elapsed_ms;
    for (auto& s : report_.steps)
        if (s.status == StepStatus::Pending || s.status == StepStatus::Running) s.status = StepStatus::Failed;
    log::info("selftest '{}': {} in {:.0f} ms", report_.title, passed ? "PASSED" : "FAILED", elapsed_ms);
}

TestReport SelfTest::report() const {
    std::scoped_lock lock(mutex_);
    return report_;
}

void SelfTest::start_loopback() {
    if (report().running) return;
    if (worker_.joinable()) worker_.join();
    worker_ = std::jthread([this] { loopback_body(); });
}

void SelfTest::start_signaling_probe(const std::string& url) {
    if (report().running) return;
    if (worker_.joinable()) worker_.join();
    worker_ = std::jthread([this, url] { signaling_body(url); });
}

TestReport SelfTest::run_loopback() {
    if (worker_.joinable()) worker_.join();
    loopback_body();
    return report();
}

TestReport SelfTest::run_signaling_probe(const std::string& url) {
    if (worker_.joinable()) worker_.join();
    signaling_body(url);
    return report();
}

void SelfTest::loopback_body() {
    begin("Loopback local (127.0.0.1)", {"Criar 2 peers WebRTC em 127.0.0.1", "ICE + DTLS conectados",
                                         "DataChannels abertos (reliable + fast)",
                                         std::format("Handshake Hello/HelloAck (protocolo v{})", kProtocolVersion),
                                         "Ping RTT (20 amostras)", "Chat ida e volta", "Encerramento limpo (Bye)"});
    init_rtc_logging();
    const auto t0 = Clock::now();
    bool ok = false;

    TaskQueue queue;
    std::shared_ptr<PeerLink> host;
    std::shared_ptr<PeerLink> guest;
    std::mutex chat_mutex;
    std::string echoed;
    std::atomic<LinkState> guest_state{LinkState::New};

    try {
        // --- 1. peers
        step(0, StepStatus::Running);
        rtc::Configuration cfg;  // no STUN: host candidates only
        cfg.bindAddress = "127.0.0.1";
        cfg.portRangeBegin = 50000;
        cfg.portRangeEnd = 50100;

        std::weak_ptr<PeerLink> weak_host, weak_guest;
        LinkCallbacks hcb, gcb;
        hcb.send_signal = [&queue, &weak_guest](const json& s) {
            queue.push([&weak_guest, s] {
                if (auto g = weak_guest.lock()) g->handle_signal(s);
            });
        };
        gcb.send_signal = [&queue, &weak_host](const json& s) {
            queue.push([&weak_host, s] {
                if (auto h = weak_host.lock()) h->handle_signal(s);
            });
        };
        gcb.on_state = [&guest_state](const std::string&, LinkState s) { guest_state = s; };
        // Guest echoes chat back; host records the echo.
        gcb.on_message = [&queue, &weak_guest](const std::string&, const Message& m) {
            if (m.type != MsgType::Chat) return;
            const std::string payload = m.payload_string();
            queue.push([&weak_guest, payload] {
                json j = json::parse(payload, nullptr, false);
                if (auto g = weak_guest.lock(); g && j.is_object())
                    g->send(MsgType::Chat, json{{"from", "guest"}, {"text", "eco:" + j.value("text", "")}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
            });
        };
        hcb.on_message = [&chat_mutex, &echoed](const std::string&, const Message& m) {
            if (m.type != MsgType::Chat) return;
            json j = json::parse(m.payload_string(), nullptr, false);
            std::scoped_lock lock(chat_mutex);
            if (j.is_object()) echoed = j.value("text", "");
        };

        guest = PeerLink::create("loopback-host", false, cfg, {"Convidado (teste)", "guest", "loopback"}, gcb);
        weak_guest = guest;
        host = PeerLink::create("loopback-guest", true, cfg, {"Host (teste)", "host", "loopback"}, hcb);
        weak_host = host;
        step(0, StepStatus::Ok, "bind 127.0.0.1, portas 50000-50100");

        auto pump = [&queue] { queue.drain(); };
        auto fail_with = [&](size_t idx, const std::string& why) {
            const PeerInfo hi = host->info();
            const PeerInfo gi = guest->info();
            step(idx, StepStatus::Failed,
                 std::format("{} | host: {} {} | guest: {} {}", why, state_name(hi.state), hi.error, state_name(gi.state), gi.error));
        };

        // --- 2+3. connection and channels (Open is reached once the reliable channel is up)
        step(1, StepStatus::Running);
        step(2, StepStatus::Running);
        const auto tc = Clock::now();
        if (!wait_for([&] { return host->state() >= LinkState::Open && guest->state() >= LinkState::Open; }, 10s, pump)) {
            fail_with(1, "timeout de 10 s");
            throw std::runtime_error("connect");
        }
        step(1, StepStatus::Ok, std::format("{:.0f} ms", ms_since(tc)));
        step(2, StepStatus::Ok);

        // --- 4. handshake
        step(3, StepStatus::Running);
        if (!wait_for([&] { return host->state() == LinkState::Ready && guest->state() == LinkState::Ready; }, 5s, pump)) {
            fail_with(3, "handshake não concluiu");
            throw std::runtime_error("hello");
        }
        const PeerInfo seen = host->info();
        step(3, StepStatus::Ok, std::format("host viu '{}' (mod {})", seen.name, seen.mod_version));

        // --- 5. ping
        step(4, StepStatus::Running);
        for (int i = 0; i < 20; ++i) {
            host->ping();
            wait_for([] { return false; }, 15ms, pump);
        }
        wait_for([&] { return host->info().pongs_received >= 20; }, 2s, pump);
        const PeerInfo pi = host->info();
        if (pi.pongs_received < 18) {
            fail_with(4, std::format("só {} de 20 pongs", pi.pongs_received));
            throw std::runtime_error("ping");
        }
        step(4, StepStatus::Ok, std::format("{} pongs, média {:.2f} ms, mín {:.2f}, máx {:.2f}", pi.pongs_received, pi.rtt_ms,
                                            pi.rtt_min_ms, pi.rtt_max_ms));

        // --- 6. chat round trip (UTF-8 included)
        step(5, StepStatus::Running);
        const std::string probe = "Olá, vizinho! ação ç";
        host->send(MsgType::Chat, json{{"from", "host"}, {"text", probe}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        const bool chat_ok = wait_for([&] {
            std::scoped_lock lock(chat_mutex);
            return echoed == "eco:" + probe;
        }, 3s, pump);
        if (!chat_ok) {
            fail_with(5, "eco não recebido");
            throw std::runtime_error("chat");
        }
        step(5, StepStatus::Ok, "\"" + probe + "\" -> eco OK");

        // --- 7. bye
        step(6, StepStatus::Running);
        host->close("loopback concluído");
        if (!wait_for([&] { return guest_state.load() == LinkState::Closed; }, 3s, pump)) {
            fail_with(6, "convidado não recebeu Bye");
            throw std::runtime_error("bye");
        }
        step(6, StepStatus::Ok);
        ok = true;
    } catch (const std::exception& e) {
        log::warn("loopback aborted at '{}'", e.what());
    }

    if (host) host->close("fim do teste");
    if (guest) guest->close("fim do teste");
    queue.drain();
    host.reset();
    guest.reset();
    // Let in-flight libdatachannel callbacks (which may still reference `queue`) finish.
    std::this_thread::sleep_for(300ms);
    queue.drain();
    finish(ok, ms_since(t0));
}

void SelfTest::signaling_body(const std::string& url) {
    begin("Servidor de sinalização", {"Host conecta e recebe 'welcome'", "Convidado entra na mesma sala",
                                      "Host é avisado (peer_joined)", "Relay de sinal convidado -> host",
                                      "Sala inexistente é recusada (no_host)"});
    init_rtc_logging();
    const auto t0 = Clock::now();
    bool ok = false;

    std::mutex m;
    std::vector<json> host_msgs, guest_msgs, stray_msgs;
    auto has = [&](std::vector<json>& v, const std::string& t) {
        std::scoped_lock lock(m);
        for (const auto& j : v)
            if (j.value("t", "") == t) return true;
        return false;
    };
    auto find = [&](std::vector<json>& v, const std::string& t) {
        std::scoped_lock lock(m);
        for (const auto& j : v)
            if (j.value("t", "") == t) return j;
        return json{};
    };
    auto collector = [&](std::vector<json>& v) {
        SignalingClient::Callbacks cb;
        cb.on_message = [&m, &v](const json& j) {
            std::scoped_lock lock(m);
            v.push_back(j);
        };
        return cb;
    };

    SignalingClient host, guest, stray;
    const std::string room = generate_room_code();
    try {
        step(0, StepStatus::Running);
        if (!host.connect(url, room, "host", "probe-host", collector(host_msgs)) || !wait_for([&] { return has(host_msgs, "welcome") || has(host_msgs, "error"); }, 8s)) {
            step(0, StepStatus::Failed, "sem resposta de " + url);
            throw std::runtime_error("host");
        }
        if (has(host_msgs, "error")) {
            step(0, StepStatus::Failed, find(host_msgs, "error").dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
            throw std::runtime_error("host-error");
        }
        step(0, StepStatus::Ok, std::format("sala {}, {:.0f} ms", room, ms_since(t0)));

        step(1, StepStatus::Running);
        if (!guest.connect(url, room, "guest", "probe-guest", collector(guest_msgs)) || !wait_for([&] { return has(guest_msgs, "welcome"); }, 8s)) {
            step(1, StepStatus::Failed, "convidado não recebeu welcome");
            throw std::runtime_error("guest");
        }
        const json gw = find(guest_msgs, "welcome");
        step(1, StepStatus::Ok, "host id: " + gw.value("host", std::string("?")));

        step(2, StepStatus::Running);
        if (!wait_for([&] { return has(host_msgs, "peer_joined"); }, 5s)) {
            step(2, StepStatus::Failed, "peer_joined não chegou");
            throw std::runtime_error("joined");
        }
        const std::string guest_id = find(host_msgs, "peer_joined").value("id", "");
        step(2, StepStatus::Ok, "guest id: " + guest_id);

        step(3, StepStatus::Running);
        guest.send(json{{"t", "signal"}, {"to", gw.value("host", std::string())}, {"data", {{"kind", "probe"}, {"n", 42}}}});
        if (!wait_for([&] { return has(host_msgs, "signal"); }, 5s)) {
            step(3, StepStatus::Failed, "sinal não foi repassado");
            throw std::runtime_error("relay");
        }
        const json sig = find(host_msgs, "signal");
        if (sig.value("from", "") != guest_id || sig["data"].value("n", 0) != 42) {
            step(3, StepStatus::Failed, "conteúdo inesperado: " + sig.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
            throw std::runtime_error("relay-content");
        }
        step(3, StepStatus::Ok);

        step(4, StepStatus::Running);
        std::string empty_room = generate_room_code();
        while (empty_room == room) empty_room = generate_room_code();
        if (!stray.connect(url, empty_room, "guest", "probe-stray", collector(stray_msgs)) ||
            !wait_for([&] { return has(stray_msgs, "error"); }, 5s) || find(stray_msgs, "error").value("code", "") != "no_host") {
            step(4, StepStatus::Failed, "servidor não recusou sala vazia");
            throw std::runtime_error("no_host");
        }
        step(4, StepStatus::Ok);
        ok = true;
    } catch (const std::exception& e) {
        log::warn("signaling probe aborted at '{}'", e.what());
    }
    host.close();
    guest.close();
    stray.close();
    finish(ok, ms_since(t0));
}

}  // namespace cts4p::net
