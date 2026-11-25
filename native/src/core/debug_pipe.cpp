#include "core/debug_pipe.h"

#include <windows.h>

#include <string>
#include <thread>

#include "core/app.h"
#include "core/log.h"
#include "core/paths.h"
#include "net/session.h"
#include "sync/save_transfer.h"
#include "sync/tray.h"

#include <nlohmann/json.hpp>

namespace cts4p::debug_pipe {
namespace {

std::string handle(const std::string& cmd) {
    if (cmd == "status") return app::status_json();
    if (cmd == "host") {
        net::Session::instance().host();
        return R"({"ok":true})";
    }
    if (cmd.rfind("join ", 0) == 0) {
        net::Session::instance().join(cmd.substr(5));
        return R"({"ok":true})";
    }
    if (cmd == "leave") {
        net::Session::instance().leave("debug pipe");
        return R"({"ok":true})";
    }
    if (cmd == "send_tray") {  // same path as the overlay's "Criar juntos > Enviar" (newest household)
        std::string error;
        const auto dir = paths::tray_dir();
        const auto list = dir ? sync::tray::list_households(*dir) : std::vector<sync::tray::Household>{};
        const bool ok = !list.empty() &&
                        sync::SaveTransfer::instance().send_household(list.front().id, net::Session::instance().ready_links(), error);
        if (list.empty()) error = "nenhuma família na Biblioteca";
        return nlohmann::json{{"ok", ok}, {"error", error}, {"name", list.empty() ? "" : list.front().name}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    return R"({"ok":false,"error":"unknown command"})";
}

void serve() {
    for (;;) {
        // Local-only pipe (PIPE_REJECT_REMOTE_CLIENTS); one client at a time.
        HANDLE pipe = CreateNamedPipeW(L"\\\\.\\pipe\\cts4p-debug", PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                       64 * 1024, 4 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            log::error("debug pipe: CreateNamedPipe failed ({})", GetLastError());
            return;
        }
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char buf[256]{};
            DWORD got = 0;
            if (ReadFile(pipe, buf, sizeof(buf) - 1, &got, nullptr) && got > 0) {
                std::string cmd(buf, got);
                while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' ')) cmd.pop_back();
                log::info("debug pipe: '{}'", cmd);
                const std::string reply = handle(cmd);
                DWORD wrote = 0;
                WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()), &wrote, nullptr);
                FlushFileBuffers(pipe);
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

}  // namespace

void start() {
    std::thread(serve).detach();
    log::warn("debug pipe enabled (config debug_control=true): \\\\.\\pipe\\cts4p-debug");
}

}  // namespace cts4p::debug_pipe
