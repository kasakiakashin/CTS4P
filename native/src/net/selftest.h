#pragma once
// Self-tests driven from the overlay ("Testar Loopback Local") and from the
// headless test executables.
//
// Loopback: two real PeerLinks bound to 127.0.0.1 inside this process, with
// signaling relayed in memory. Exercises ICE, DTLS/SCTP, both DataChannels,
// the Hello handshake, ping RTT, chat and Bye - the same code paths a real
// room uses, without needing a second PC or the signaling server.
//
// Signaling probe: opens a host and a guest WebSocket against the configured
// server and checks that join notification and signal relay work.

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cts4p::net {

enum class StepStatus { Pending, Running, Ok, Failed };

struct TestStep {
    std::string name;
    StepStatus status = StepStatus::Pending;
    std::string detail;
};

struct TestReport {
    std::string title;
    bool running = false;
    bool finished = false;
    bool passed = false;
    double elapsed_ms = 0;
    std::vector<TestStep> steps;
};

class SelfTest {
public:
    static SelfTest& instance();

    // Asynchronous (overlay). Ignored while a test is already running.
    void start_loopback();
    void start_signaling_probe(const std::string& url);

    // Synchronous (test executables).
    TestReport run_loopback();
    TestReport run_signaling_probe(const std::string& url);

    TestReport report() const;

private:
    SelfTest() = default;
    void begin(std::string title, std::vector<std::string> steps);
    void step(size_t index, StepStatus status, std::string detail = {});
    void finish(bool passed, double elapsed_ms);
    void loopback_body();
    void signaling_body(const std::string& url);

    mutable std::mutex mutex_;
    TestReport report_;
    std::jthread worker_;
};

}  // namespace cts4p::net
