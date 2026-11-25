#include "net/signaling.h"

#include <windows.h>
#include <wincrypt.h>

#include <fstream>
#include <optional>

#include <rtc/rtc.hpp>

#include "core/log.h"
#include "core/paths.h"
#include "core/version.h"

namespace cts4p::net {
namespace {

// libdatachannel's OpenSSL backend cannot read the Windows certificate store, and
// without a CA file it connects to wss:// servers WITHOUT verifying them. We export
// the machine's trusted roots (the same ones the browser uses) to a PEM bundle once
// per run and hand it to OpenSSL, so the signaling server's certificate is verified.
std::optional<std::string> windows_root_bundle() {
    static const std::optional<std::string> path = []() -> std::optional<std::string> {
        HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
        if (!store) {
            log::error("TLS: cannot open Windows ROOT store ({})", GetLastError());
            return std::nullopt;
        }
        std::string pem;
        int count = 0;
        for (PCCERT_CONTEXT cert = CertEnumCertificatesInStore(store, nullptr); cert;
             cert = CertEnumCertificatesInStore(store, cert)) {
            DWORD len = 0;
            const DWORD flags = CRYPT_STRING_BASE64HEADER;
            if (!CryptBinaryToStringA(cert->pbCertEncoded, cert->cbCertEncoded, flags, nullptr, &len)) continue;
            std::string block(len, '\0');
            if (!CryptBinaryToStringA(cert->pbCertEncoded, cert->cbCertEncoded, flags, block.data(), &len)) continue;
            block.resize(len);
            pem += block;
            ++count;
        }
        CertCloseStore(store, 0);
        if (count == 0) {
            log::error("TLS: Windows ROOT store is empty");
            return std::nullopt;
        }
        const auto file = paths::mod_dir() / L"ca-roots.pem";
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream(file, std::ios::binary | std::ios::trunc) << pem;
        log::info("TLS: exported {} trusted root certificates for wss:// verification", count);
        return file.string();
    }();
    return path;
}

}  // namespace

SignalingClient::~SignalingClient() { close(); }

std::string SignalingClient::url_encode(const std::string& s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

bool SignalingClient::connect(const std::string& base_url, const std::string& room, const std::string& role,
                              const std::string& name, Callbacks callbacks) {
    close();
    if (!base_url.starts_with("ws://") && !base_url.starts_with("wss://")) {
        log::error("signaling: URL must start with ws:// or wss:// (got '{}')", base_url);
        return false;
    }
    const char sep = base_url.find('?') == std::string::npos ? '?' : '&';
    const std::string url = std::format("{}{}room={}&role={}&name={}&proto={}", base_url, sep, url_encode(room), role,
                                        url_encode(name), kProtocolVersion);

    rtc::WebSocket::Configuration cfg;
    cfg.connectionTimeout = std::chrono::milliseconds(8000);
    cfg.pingInterval = std::chrono::milliseconds(15000);
    if (base_url.starts_with("wss://")) {
        if (auto bundle = windows_root_bundle()) {
            cfg.caCertificatePemFile = *bundle;
        } else {
            log::error("signaling: refusing wss:// without a CA bundle (certificate could not be verified)");
            return false;
        }
    }
    auto ws = std::make_shared<rtc::WebSocket>(cfg);

    auto cb = std::make_shared<Callbacks>(std::move(callbacks));
    std::weak_ptr<rtc::WebSocket> weak = ws;
    ws->onOpen([room, role] { log::info("signaling: connected (room {}, {})", room, role); });
    ws->onError([cb](std::string err) {
        log::warn("signaling: error: {}", err);
        if (cb->on_closed) cb->on_closed("erro: " + err);
    });
    ws->onClosed([cb] {
        if (cb->on_closed) cb->on_closed("conexão com o servidor de sinalização fechada");
    });
    ws->onMessage([cb](rtc::message_variant data) {
        const auto* text = std::get_if<std::string>(&data);
        if (!text) return;
        nlohmann::json msg = nlohmann::json::parse(*text, nullptr, false);
        if (!msg.is_object() || !msg.contains("t")) {
            log::warn("signaling: ignoring malformed message");
            return;
        }
        if (cb->on_message) cb->on_message(msg);
    });

    try {
        ws->open(url);
    } catch (const std::exception& e) {
        log::error("signaling: open failed: {}", e.what());
        return false;
    }
    std::scoped_lock lock(mutex_);
    ws_ = std::move(ws);
    return true;
}

bool SignalingClient::send(const nlohmann::json& msg) {
    std::shared_ptr<rtc::WebSocket> ws;
    {
        std::scoped_lock lock(mutex_);
        ws = ws_;
    }
    if (!ws || !ws->isOpen()) return false;
    try {
        return ws->send(msg.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    } catch (const std::exception& e) {
        log::warn("signaling: send failed: {}", e.what());
        return false;
    }
}

void SignalingClient::close() {
    std::shared_ptr<rtc::WebSocket> ws;
    {
        std::scoped_lock lock(mutex_);
        ws.swap(ws_);
    }
    if (!ws) return;
    // Drop callbacks first so a late onClosed can't call back into a destroyed owner.
    ws->onOpen(nullptr);
    ws->onClosed(nullptr);
    ws->onError(nullptr);
    ws->onMessage(nullptr);
    try {
        ws->close();
    } catch (...) {
    }
}

bool SignalingClient::is_open() const {
    std::scoped_lock lock(mutex_);
    return ws_ && ws_->isOpen();
}

}  // namespace cts4p::net
