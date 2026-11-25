#include "ipc/ipc_api.h"

#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

#include "bridge/bridge.h"
#include "build/build_observer.h"
#include "core/app.h"
#include "core/config.h"
#include "core/version.h"
#include "ipc/ring_buffer.h"
#include "net/selftest.h"
#include "net/session.h"

namespace cts4p::ipc {
namespace {

constexpr size_t kRingBytes = 4u << 20;  // 4 MiB per direction

FrameRing& to_native() {
    static FrameRing r(kRingBytes);
    return r;
}
FrameRing& to_python() {
    static FrameRing r(kRingBytes);
    return r;
}

uint32_t write_string(const std::string& s, char* out, uint32_t cap) {
    if (out && cap > 0) {
        const size_t n = std::min<size_t>(s.size(), cap - 1);
        std::memcpy(out, s.data(), n);
        out[n] = '\0';
    }
    return static_cast<uint32_t>(s.size());
}

std::string report_json(const net::TestReport& r) {
    nlohmann::json steps = nlohmann::json::array();
    for (const auto& s : r.steps) {
        const char* st = s.status == net::StepStatus::Ok       ? "ok"
                         : s.status == net::StepStatus::Failed ? "failed"
                                                               : "pending";
        steps.push_back({{"name", s.name}, {"status", st}, {"detail", s.detail}});
    }
    return nlohmann::json{{"title", r.title}, {"passed", r.passed}, {"elapsed_ms", r.elapsed_ms}, {"steps", steps}}.dump(2);
}

}  // namespace

bool post_to_python(const void* data, uint32_t len) { return to_python().push(data, len); }
int64_t take_from_python(void* out, uint32_t cap) { return to_native().pop(out, cap); }

}  // namespace cts4p::ipc

using namespace cts4p;

CTS4P_API uint32_t cts4p_api_version() { return kApiVersion; }

CTS4P_API uint32_t cts4p_mod_version(char* out, uint32_t cap) { return ipc::write_string(kModVersion, out, cap); }

CTS4P_API int32_t cts4p_ipc_send(const void* data, uint32_t len) {
    if (data == nullptr && len != 0) return 0;
    if (!ipc::to_native().push(data, len)) return 0;
    bridge::notify_python_frame();
    return 1;
}

CTS4P_API int64_t cts4p_ipc_recv(void* out, uint32_t cap) {
    if (out == nullptr) return 0;
    return ipc::to_python().pop(out, cap);
}

CTS4P_API uint32_t cts4p_status_json(char* out, uint32_t cap) { return ipc::write_string(app::status_json(), out, cap); }

CTS4P_API int32_t cts4p_selftest(int32_t kind, const char* url, char* out, uint32_t cap) {
    auto& st = net::SelfTest::instance();
    net::TestReport r;
    if (kind == 0) r = st.run_loopback();
    else if (kind == 1) r = st.run_signaling_probe(url ? std::string(url) : config().signaling_url);
    else return 0;
    ipc::write_string(ipc::report_json(r), out, cap);
    return r.passed ? 1 : 0;
}

CTS4P_API int32_t cts4p_session_host() {
    if (!app::started()) return 0;
    net::Session::instance().host();
    return 1;
}

CTS4P_API int32_t cts4p_session_join(const char* code) {
    if (!app::started() || code == nullptr) return 0;
    net::Session::instance().join(code);
    return 1;
}

CTS4P_API int32_t cts4p_session_leave() {
    net::Session::instance().leave();
    return 1;
}

CTS4P_API int32_t cts4p_session_chat(const char* text) {
    if (text == nullptr || *text == '\0') return 0;
    net::Session::instance().send_chat(text);
    return 1;
}

CTS4P_API int32_t cts4p_build_pump() { return static_cast<int32_t>(build::pump()); }

CTS4P_API int32_t cts4p_debug_build_emit(const void* frame, uint32_t len) {
    if (!frame || len == 0) return 0;
    const auto* p = static_cast<const uint8_t*>(frame);
    return net::Session::instance().send_build_op(std::vector<uint8_t>(p, p + len), std::string()) ? 1 : 0;
}

CTS4P_API uint32_t cts4p_debug_build_probe(const wchar_t* module_name, char* out, uint32_t cap) {
    return ipc::write_string(build::probe(module_name ? module_name : L"Simulation_x64.dll"), out, cap);
}

CTS4P_API int32_t cts4p_debug_start() {
    app::start();
    return 1;
}
