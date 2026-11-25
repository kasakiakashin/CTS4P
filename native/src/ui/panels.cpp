#include "ui/panels.h"

#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstring>
#include <deque>
#include <format>
#include <string>
#include <thread>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "bridge/bridge.h"
#include "build/build_observer.h"
#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/version.h"
#include "diag/diagnostics.h"
#include "net/selftest.h"
#include "net/session.h"
#include "render/dx11_hook.h"
#include "sync/save_transfer.h"
#include "ui/overlay.h"
#include "ui/theme.h"
#include "ui/view_model.h"

namespace cts4p::panels {
namespace {

using Clock = std::chrono::steady_clock;
using net::LinkState;
using net::SessionMode;
using net::StepStatus;
namespace th = theme;

constexpr auto kToastLife = std::chrono::seconds(6);
constexpr float kWindowW = 640.0f;
constexpr float kWindowH = 660.0f;

struct Toast {
    std::string text;
    Clock::time_point born;
};
std::deque<Toast> g_toasts;

char g_name_buf[64] = {};
char g_code_buf[16] = {};
int g_initial_tab = -1;  // demo/screenshot: tab to open first
std::jthread g_rescan;

std::string key_name(int vk) {
    if (vk == VK_INSERT) return "INSERT";
    char name[64]{};
    const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    if (GetKeyNameTextA(static_cast<LONG>(scan << 16), name, sizeof(name)) > 0) return name;
    return std::format("0x{:02X}", vk);
}

size_t ready_peers(const net::SessionSnapshot& s) {
    size_t n = 0;
    for (const auto& p : s.peers) n += p.state == LinkState::Ready ? 1 : 0;
    return n;
}

double best_ping(const net::SessionSnapshot& s) {
    double best = -1;
    for (const auto& p : s.peers)
        if (p.state == LinkState::Ready && p.rtt_ms >= 0 && (best < 0 || p.rtt_ms < best)) best = p.rtt_ms;
    return best;
}

// ------------------------------------------------------------------ header
void header(const ui::ViewModel& vm, bool* open) {
    const auto& s = vm.session;
    ImGui::PushFont(th::strong(), th::title_size());
    th::plumbob(16);
    ImGui::SameLine(0, 8);
    ImGui::TextUnformatted("CTS4P");
    ImGui::PopFont();
    ImGui::SameLine(0, 14);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3);
    if (s.mode == SessionMode::Idle) {
        th::pill("Fora de uma sala", th::kMuted);
    } else {
        const size_t n = ready_peers(s);
        const std::string label = n ? std::format("{}  ·  {} {}", s.room, n, n == 1 ? "jogador" : "jogadores")
                                    : std::format("{}  ·  aguardando", s.room);
        th::pill(label.c_str(), n ? th::kAccent : th::kWarn);
    }
    // Close button (top-right)
    ImGui::SameLine(ImGui::GetWindowWidth() - 46);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 3);
    if (th::ghost_button(th::kIconClose, {30, 30})) *open = false;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fechar (%s)", key_name(config().overlay_key).c_str());
}

// ------------------------------------------------------------------ Sala
void self_test_block(const ui::ViewModel& vm) {
    auto& st = net::SelfTest::instance();
    const auto& r = vm.selftest;
    ImGui::BeginDisabled(r.running);
    if (th::ghost_button(std::format("{}  Testar conexão neste PC", th::kIconPlay).c_str())) st.start_loopback();
    th::help("Cria dois jogadores de teste dentro deste PC (127.0.0.1) e confere conexão,\n"
             "canais, handshake, ping e chat. Não precisa de outra pessoa.");
    ImGui::SameLine();
    if (th::ghost_button(std::format("{}  Testar servidor", th::kIconSync).c_str())) st.start_signaling_probe(vm.signaling_url);
    th::help("Confere se o servidor de sinalização configurado responde e repassa sinais.");
    ImGui::EndDisabled();
    if (r.title.empty()) return;
    if (r.running) {
        ImGui::TextColored(th::kWarn, "%s  Testando...", th::kIconSync);
        return;
    }
    int ok = 0;
    for (const auto& s : r.steps) ok += s.status == StepStatus::Ok ? 1 : 0;
    if (r.passed) {
        ImGui::TextColored(th::kAccent, "%s  %s: tudo certo (%d/%zu, %.0f ms)", th::kIconCheck, r.title.c_str(), ok, r.steps.size(), r.elapsed_ms);
    } else {
        ImGui::TextColored(th::kError, "%s  %s: falhou", th::kIconWarning, r.title.c_str());
        for (const auto& s : r.steps)
            if (s.status != StepStatus::Ok) th::muted("%s - %s", s.name.c_str(), s.detail.c_str());
    }
}

// Save: the player only needs "is it in sync, and do I have to do anything".
void save_card(const ui::ViewModel& vm) {
    const bool hosting = vm.session.mode == SessionMode::Hosting;
    const sync::TransferStatus* last_in = nullptr;
    bool busy = false;
    bool any_save = false;
    for (const auto& t : vm.transfers) {
        if (t.tray) continue;  // households are shown in "Criar juntos"
        any_save = true;
        if (!t.finished) busy = true;
        if (!t.outgoing && t.finished && t.ok) last_in = &t;
    }
    if (!hosting && !any_save) return;
    th::begin_card("save", {-1, 0});
    th::heading(th::kIconSave, "Save");
    if (hosting) {
        bool autosync = vm.auto_sync_save;
        if (ImGui::Checkbox("Sincronizar automaticamente", &autosync)) {
            config().auto_sync_save = autosync;
            config().save(config_path());
            bridge::post_event(nlohmann::json{{"ev", "settings"}, {"auto_sync_save", autosync}});
        }
        th::help("Quem entra na sala já recebe o save sozinho.\n"
                 "Com esta opção ligada, toda vez que você salvar o jogo (ou o jogo salvar sozinho),\n"
                 "os convidados também recebem a versão nova, sem parar de jogar.\n"
                 "Seus próprios saves não são alterados.");
    }
    for (const auto& t : vm.transfers) {
        if (t.finished || t.tray) continue;
        const float frac = t.total_bytes ? static_cast<float>(t.done_bytes) / static_cast<float>(t.total_bytes) : 0.0f;
        ImGui::Text("%s %s", t.outgoing ? "Enviando para" : "Recebendo de", t.peer.c_str());
        ImGui::SameLine();
        th::muted("%.1f de %.1f MB", static_cast<double>(t.done_bytes) / 1e6, static_cast<double>(t.total_bytes) / 1e6);
        ImGui::ProgressBar(frac, {-1, 8}, "");
    }
    if (!busy) {
        if (hosting) {
            if (ready_peers(vm.session))
                th::muted(vm.auto_sync_save ? "Tudo em dia. O próximo envio acontece quando você salvar o jogo."
                                            : "Desligado: o save só vai quando alguém entra ou quando você mandar.");
        } else if (last_in && last_in->background) {
            ImGui::TextColored(th::kAccent, "%s", th::kIconCheck);
            ImGui::SameLine();
            ImGui::TextUnformatted("Cópia do save em dia. Pode continuar jogando.");
        } else if (last_in) {
            ImGui::TextColored(th::kAccent, "%s", th::kIconCheck);
            ImGui::SameLine();
            ImGui::TextWrapped("Pronto! Menu principal > Carregar Jogo > \"%s\"", last_in->label.c_str());
        }
    }
    if (hosting) {
        ImGui::BeginDisabled(!vm.python.python_attached || busy);
        if (th::ghost_button(std::format("{}  Enviar agora", th::kIconSend).c_str()))
            bridge::post_event(nlohmann::json{{"ev", "request_save"}, {"reason", "overlay"}});
        ImGui::EndDisabled();
    }
    th::end_card();
}

std::string ago(int64_t seconds) {
    if (seconds < 60) return "agora";
    if (seconds < 3600) return std::format("há {} min", seconds / 60);
    if (seconds < 86400) return std::format("há {} h", seconds / 3600);
    return std::format("há {} {}", seconds / 86400, seconds < 2 * 86400 ? "dia" : "dias");
}

// "Criar juntos": each player makes their Sim in their own Create a Sim, saves it to the
// Library, and sends it here; it lands in the other players' Library.
void together_card(const ui::ViewModel& vm) {
    static uint64_t selected = 0;
    th::begin_card("together", {-1, 0});
    th::heading(th::kIconPeople, "Criar juntos");
    th::help("1. Cada um cria o seu Sim no Criar um Sim, ao mesmo tempo.\n"
             "2. Salve a família na Biblioteca (botão de salvar no Criar um Sim).\n"
             "3. Escolha ela aqui e clique em Enviar: ela aparece na Biblioteca dos outros.\n"
             "4. O host junta todo mundo numa família só e começa o jogo.");
    bool sending = false;
    for (const auto& t : vm.transfers) {
        if (!t.tray || t.finished) continue;
        sending |= t.outgoing;
        const float frac = t.total_bytes ? static_cast<float>(t.done_bytes) / static_cast<float>(t.total_bytes) : 0.0f;
        ImGui::Text("%s a família %s %s %s", t.outgoing ? "Enviando" : "Recebendo", t.label.c_str(), t.outgoing ? "para" : "de", t.peer.c_str());
        ImGui::ProgressBar(frac, {-1, 8}, "");
    }
    if (vm.library.empty()) {
        th::muted("Crie seu Sim e salve a família na Biblioteca: ela aparece aqui para você enviar.");
        th::end_card();
        return;
    }
    const sync::tray::Household* pick = &vm.library.front();
    for (const auto& h : vm.library)
        if (h.id == selected) pick = &h;
    selected = pick->id;
    auto label_of = [&](const sync::tray::Household& h) {
        return std::format("Família {}  ·  salva {}", h.name.empty() ? "sem nome" : h.name, ago(vm.now_unix - h.modified_unix));
    };
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 150);
    if (ImGui::BeginCombo("##family", label_of(*pick).c_str())) {
        for (const auto& h : vm.library) {
            ImGui::PushID(static_cast<int>(h.id & 0x7FFFFFFF));
            if (ImGui::Selectable(label_of(h).c_str(), h.id == selected)) selected = h.id;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(sending || ready_peers(vm.session) == 0);
    if (th::primary_button(std::format("{}  Enviar", th::kIconSend).c_str(), {-1, 0})) {
        std::string error;
        if (!sync::SaveTransfer::instance().send_household(selected, net::Session::instance().ready_links(), error))
            net::Session::instance().notice("Não foi possível enviar: " + error);
    }
    ImGui::EndDisabled();
    th::end_card();
}

void tab_room(const ui::ViewModel& vm) {
    auto& session = net::Session::instance();
    const auto& s = vm.session;

    if (s.mode == SessionMode::Idle) {
        const float w = (ImGui::GetContentRegionAvail().x - 12) * 0.5f;
        th::begin_card("create", {w, 172});
        th::heading(th::kIconHome, "Hospedar");
        th::muted("Abra uma sala no seu jogo e passe o código para os amigos.");
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 54);
        if (th::primary_button(std::format("{}  Criar sala", th::kIconAdd).c_str(), {-1, 40})) session.host();
        th::end_card();
        ImGui::SameLine(0, 12);
        th::begin_card("join", {w, 172});
        th::heading(th::kIconEnter, "Entrar");
        th::muted("Digite o código que o host passou.");
        ImGui::SetNextItemWidth(-1);
        const bool enter = ImGui::InputTextWithHint("##code", "CTS-XXXX", g_code_buf, sizeof(g_code_buf),
                                                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsUppercase);
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 54);
        if (th::primary_button(std::format("{}  Entrar na sala", th::kIconEnter).c_str(), {-1, 40}) || enter) session.join(g_code_buf);
        th::end_card();
    } else {
        th::begin_card("room", {-1, 0});
        th::muted(s.mode == SessionMode::Hosting ? "Código da sua sala" : "Você está na sala");
        ImGui::PushFont(th::strong(), th::big_size());
        ImGui::TextColored(th::kAccent, "%s", s.room.c_str());
        ImGui::PopFont();
        ImGui::SameLine(0, 16);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12);
        if (th::ghost_button(std::format("{}  Copiar", th::kIconCopy).c_str())) ImGui::SetClipboardText(s.room.c_str());
        const size_t n = ready_peers(s);
        if (n) {
            const double ping = best_ping(s);
            ImGui::TextColored(th::kAccent, "%s", th::kIconPeople);
            ImGui::SameLine();
            ImGui::Text("%zu %s conectado%s", n, n == 1 ? "jogador" : "jogadores", n == 1 ? "" : "s");
            if (ping >= 0) {
                ImGui::SameLine(0, 16);
                ImGui::TextColored(th::ping_color(ping), "%.0f ms", ping);
            }
        } else {
            ImGui::TextColored(th::kWarn, "%s", th::kIconSync);
            ImGui::SameLine();
            ImGui::TextUnformatted(s.mode == SessionMode::Hosting ? "Esperando alguém entrar..." : "Conectando ao host...");
        }
        th::end_card();

        ImGui::Spacing();
        save_card(vm);
        ImGui::Spacing();
        together_card(vm);
        ImGui::Spacing();
        if (th::ghost_button(std::format("{}  {}", th::kIconLeave, s.mode == SessionMode::Hosting ? "Fechar sala" : "Sair da sala").c_str()))
            session.leave();
    }
    if (!s.error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(th::kError, "%s", th::kIconWarning);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, th::kError);
        ImGui::TextWrapped("%s", s.error.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Avançado")) {
        ImGui::SetNextItemWidth(200);
        ImGui::InputTextWithHint("##name", "Seu nome", g_name_buf, sizeof(g_name_buf));
        ImGui::SameLine();
        if (th::ghost_button("Salvar nome") && g_name_buf[0] != '\0') {
            config().nickname = g_name_buf;
            config().save(config_path());
            session.set_local_name(config().nickname);
        }
        th::muted("Servidor: %s", vm.signaling_url.c_str());
        if (vm.signaling_url.find("127.0.0.1") != std::string::npos || vm.signaling_url.find("localhost") != std::string::npos)
            ImGui::TextColored(th::kWarn, "Servidor local: só funciona nesta rede. Veja o LEIA-ME para jogar pela internet.");
        ImGui::Spacing();
        self_test_block(vm);
    }
}

// ------------------------------------------------------------------ Jogadores
const build::Presence* presence_of(const ui::ViewModel& vm, const std::string& name) {
    for (const auto& p : vm.presence)
        if (p.name == name) return &p;
    return nullptr;
}

void player_row(const ui::ViewModel& vm, const std::string& name, const char* role, double ping, LinkState state, bool me) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec4 dot = me ? th::kAccent : (state == LinkState::Ready ? th::ping_color(ping) : th::kWarn);
    dl->AddCircleFilled({p.x + 7, p.y + 12}, 5.0f, ImGui::GetColorU32(dot));
    ImGui::Dummy({18, 0});
    ImGui::SameLine();
    ImGui::PushFont(th::strong(), 0.0f);
    ImGui::TextUnformatted(name.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    th::muted("%s%s", role, me ? " (você)" : "");
    // right side: ping / state
    const char* right_txt = nullptr;
    std::string right;
    if (me) right = "";
    else if (state == LinkState::Ready) right = ping >= 0 ? std::format("{:.0f} ms", ping) : "conectado";
    else right = net::state_name(state);
    right_txt = right.c_str();
    if (*right_txt) {
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(right_txt).x);
        ImGui::TextColored(me ? th::kMuted : dot, "%s", right_txt);
    }
    if (const auto* pr = presence_of(vm, name)) {
        ImGui::Dummy({18, 0});
        ImGui::SameLine();
        if (pr->mode == "construção") th::muted("%s  no Modo Construção", th::kIconBuild);
        else if (pr->has_position) th::muted("clicou perto de (%.0f, %.0f), andar %d  ·  há %.0f s", pr->x, pr->z, pr->level, pr->age_seconds);
    }
}

void tab_players(const ui::ViewModel& vm) {
    const auto& s = vm.session;
    if (s.mode == SessionMode::Idle) {
        ImGui::Spacing();
        th::muted("Ninguém por aqui ainda. Crie ou entre numa sala na aba Sala.");
        return;
    }
    th::begin_card("players", {-1, 0});
    player_row(vm, s.local_name, s.mode == SessionMode::Hosting ? "host" : "convidado", -1, LinkState::Ready, true);
    for (const auto& p : s.peers) {
        ImGui::Separator();
        player_row(vm, p.name.empty() ? p.id : p.name, p.role == "host" ? "host" : "convidado", p.rtt_ms, p.state, false);
        if (!p.error.empty() && p.state != LinkState::Ready) th::muted("%s", p.error.c_str());
    }
    th::end_card();

    ImGui::Spacing();
    th::heading(th::kIconInfo, "Pacotes e mods de cada um");
    if (s.compat.empty()) th::muted("Comparando pacotes e mods...");
    for (const auto& c : s.compat) {
        const auto& r = c.report;
        ImGui::PushID(c.peer_id.c_str());
        if (r.fully_compatible()) {
            ImGui::TextColored(th::kAccent, "%s", th::kIconCheck);
            ImGui::SameLine();
            ImGui::Text("%s: tudo igual", r.remote_name.c_str());
        } else {
            ImGui::TextColored(th::kWarn, "%s", th::kIconWarning);
            ImGui::SameLine();
            const bool openned = ImGui::TreeNodeEx("diff", ImGuiTreeNodeFlags_SpanAvailWidth, "%s: %s", r.remote_name.c_str(), r.summary().c_str());
            if (openned) {
                auto list = [](const char* title, const std::vector<std::string>& items, ImVec4 color) {
                    if (items.empty()) return;
                    ImGui::TextColored(color, "%s", title);
                    for (size_t i = 0; i < items.size() && i < 50; ++i) th::muted("  %s", items[i].c_str());
                    if (items.size() > 50) th::muted("  ... e mais %zu", items.size() - 50);
                };
                list("Pacotes que faltam aqui", r.packs_missing_here, th::kError);
                list("Pacotes que faltam no outro PC", r.packs_missing_there, th::kWarn);
                list("Mods que faltam aqui", r.mods_missing_here, th::kError);
                list("Mods que faltam no outro PC", r.mods_missing_there, th::kWarn);
                list("Mods com versão diferente", r.mods_different, th::kWarn);
                ImGui::TreePop();
            }
        }
        ImGui::PopID();
    }
}

// ------------------------------------------------------------------ Construção
void tab_build(const ui::ViewModel& vm) {
    const auto& b = vm.build;
    th::begin_card("buildstatus", {-1, 0});
    th::heading(th::kIconBuild, "Construção compartilhada");
    if (!b.installed) {
        ImGui::TextColored(th::kError, "%s  Indisponível nesta versão do jogo", th::kIconWarning);
        th::muted("O resto do mod continua funcionando normalmente.");
    } else if (!b.replication) {
        th::pill("Desligada", th::kMuted);
    } else if (b.gate_open) {
        th::pill("Ativa: construções aparecem para todos", th::kAccent);
    } else {
        th::pill("Em espera até os jogos estarem no mesmo lote", th::kWarn);
    }
    if (b.installed) {
        bool replicate = b.replication;
        if (ImGui::Checkbox("Compartilhar paredes, pisos, telhados e terreno", &replicate)) build::set_replication(replicate);
        th::help("Experimental. Se os lotes ficarem diferentes, o CTS4P devolve o lote do host\n"
                 "aos convidados automaticamente (Lot State Hash).");
        th::muted("%llu enviadas  ·  %llu recebidas%s", static_cast<unsigned long long>(b.ops_sent),
                  static_cast<unsigned long long>(b.ops_replayed),
                  b.ops_failed ? std::format("  ·  {} com erro", b.ops_failed).c_str() : "");
    }
    th::end_card();

    ImGui::Spacing();
    th::begin_card("lothash", {-1, 0});
    th::heading(th::kIconSync, "Verificação do lote");
    if (vm.lot.local.empty()) th::muted("Começa quando houver alguém sincronizado no mesmo lote.");
    else {
        const bool same = vm.lot.summary.find("Idêntico") != std::string::npos || vm.lot.summary.find("enviado") != std::string::npos;
        ImGui::TextColored(same ? th::kAccent : th::kWarn, "%s", same ? th::kIconCheck : th::kIconWarning);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", vm.lot.summary.c_str());
        if (!vm.lot.last_heal.empty()) th::muted("Última correção: %s", vm.lot.last_heal.c_str());
    }
    th::end_card();

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Avançado##build")) {
        th::muted("Hook: %s (rva 0x%llx, por %s)", b.installed ? "ativo" : "inativo", static_cast<unsigned long long>(b.rva), b.how_found.c_str());
        th::muted("Mensagens do jogo: %llu  ·  de construção: %llu  ·  na fila: %zu",
                  static_cast<unsigned long long>(b.total_messages), static_cast<unsigned long long>(b.build_messages), b.queued);
        bool capture = b.capture_to_file;
        if (ImGui::Checkbox("Gravar operações em build_capture.jsonl", &capture)) build::set_capture(capture);
        ImGui::BeginDisabled(!b.context_ready);
        if (th::ghost_button("Teste local: reaplicar minha última operação")) build::replay_last_local();
        ImGui::EndDisabled();
        th::help("Construa uma parede, aperte Ctrl+Z e clique aqui: a parede deve voltar.");
        for (const auto& m : vm.messages) th::muted("%s: %llu", m.name, static_cast<unsigned long long>(m.count));
    }
}

// ------------------------------------------------------------------ Ajustes
void tab_settings(const ui::ViewModel& vm) {
    const auto env = diag::cached();
    auto& dx = dx11::stats();
    th::begin_card("ver", {-1, 0});
    th::heading(th::kIconInfo, "Sobre");
    ImGui::Text("CTS4P %s", kModVersion);
    ImGui::SameLine();
    th::muted("·  The Sims 4 %s", vm.game_version.c_str());
    auto line = [](bool ok, const char* text) {
        ImGui::TextColored(ok ? th::kAccent : th::kError, "%s", ok ? th::kIconCheck : th::kIconWarning);
        ImGui::SameLine();
        ImGui::TextUnformatted(text);
    };
    line(dx.installed.load() || vm.demo, "Painel na tela");
    line(vm.python.python_attached, vm.python.python_attached ? "Scripts do mod carregados" : "Scripts do mod não carregaram (mods de script ligados?)");
    line(vm.build.installed, vm.build.installed ? "Construção compartilhada disponível" : "Construção compartilhada indisponível nesta versão");
    line(env->script_mods_enabled || vm.demo, env->script_mods_enabled || vm.demo ? "Mods de script ligados no jogo" : "Mods de script DESLIGADOS nas opções do jogo");
    th::end_card();

    ImGui::Spacing();
    if (th::ghost_button(std::format("{}  Abrir pasta do CTS4P", th::kIconHome).c_str()))
        ShellExecuteW(nullptr, L"open", paths::mod_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::SameLine();
    if (th::ghost_button(std::format("{}  Reescanear mods", th::kIconSync).c_str())) {
        if (g_rescan.joinable()) g_rescan.join();
        g_rescan = std::jthread([] { diag::refresh_cache(); });
    }

    ImGui::Spacing();
    size_t scripts = 0;
    for (const auto& m : env->mods) scripts += m.is_script ? 1 : 0;
    if (ImGui::CollapsingHeader(std::format("Pacotes ({}) e mods ({})###content", env->packs.size(), env->mods.size()).c_str())) {
        std::string packs;
        for (const auto& p : env->packs) packs += p + "  ";
        th::muted("Pacotes: %s", packs.empty() ? "só o jogo base" : packs.c_str());
        ImGui::BeginChild("mods", {0, 140}, ImGuiChildFlags_Borders);
        for (const auto& m : env->mods) ImGui::TextColored(m.is_script ? th::kWarn : th::kMuted, "%s", m.relative_path.c_str());
        ImGui::EndChild();
        th::muted("%zu .package  ·  %zu .ts4script", env->mods.size() - scripts, scripts);
    }
    if (ImGui::CollapsingHeader("Registro (log)")) {
        ImGui::BeginChild("log", {0, 180}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& l : log::recent(150)) {
            const ImVec4 col = l.find("[ERR]") != std::string::npos ? th::kError : l.find("[WRN]") != std::string::npos ? th::kWarn : th::kMuted;
            ImGui::TextColored(col, "%s", l.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
}

// ------------------------------------------------------------------ HUD pill (overlay closed, in a room)
void draw_hud(const ui::ViewModel& vm) {
    const auto& s = vm.session;
    if (s.mode == SessionMode::Idle || overlay::stats().visible) return;
    const size_t n = ready_peers(s);
    const double ping = best_ping(s);
    const std::string text = n ? std::format("CTS4P  ·  {}  ·  {} {}{}", s.room, n, n == 1 ? "jogador" : "jogadores",
                                             ping >= 0 ? std::format("  ·  {:.0f} ms", ping) : std::string())
                               : std::format("CTS4P  ·  {}  ·  aguardando jogadores", s.room);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 10}, ImGuiCond_Always, {0.5f, 0.0f});
    ImGui::SetNextWindowBgAlpha(0.78f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14, 6});
    ImGui::Begin("##hud", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(n ? th::ping_color(ping < 0 ? 0 : ping) : th::kWarn, "%s", "\xE2\x97\x8F");  // ●
    ImGui::SameLine(0, 8);
    ImGui::TextUnformatted(text.c_str());
    ImGui::End();
    ImGui::PopStyleVar(2);
}

}  // namespace

void on_overlay_ready() {
    // Show the name actually in use (config nickname, or the Windows user name fallback).
    const std::string name = net::Session::instance().snapshot().local_name;
    strncpy_s(g_name_buf, name.c_str(), _TRUNCATE);
    if (auto tab = paths::env(L"CTS4P_UI_TAB")) g_initial_tab = _wtoi(tab->c_str());
    if (config().show_welcome_toast && !ui::demo_mode())
        g_toasts.push_back({std::format("CTS4P {} ativo  ·  aperte {} para abrir", kModVersion, key_name(config().overlay_key)), Clock::now()});
    if (ui::demo_mode() == 1) g_toasts.push_back({"Bia entrou no Modo Construção", Clock::now()});
}

void draw_main_window(bool* open) {
    const ui::ViewModel vm = ui::gather();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize({kWindowW, kWindowH}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos({vp->WorkPos.x + 60, vp->WorkPos.y + 70}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("CTS4P###cts4p_main", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    header(vm, open);
    ImGui::Spacing();
    if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_FittingPolicyResizeDown)) {
        struct Tab {
            const char* icon;
            const char* name;
            void (*draw)(const ui::ViewModel&);
        };
        static const Tab tabs[] = {{th::kIconLink, "Sala", &tab_room},
                                   {th::kIconPeople, "Jogadores", &tab_players},
                                   {th::kIconBuild, "Construção", &tab_build},
                                   {th::kIconSettings, "Ajustes", &tab_settings}};
        for (int i = 0; i < static_cast<int>(std::size(tabs)); ++i) {
            const std::string label = std::format("{}  {}###tab{}", tabs[i].icon, tabs[i].name, i);
            const ImGuiTabItemFlags flags = g_initial_tab == i ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(label.c_str(), nullptr, flags)) {
                ImGui::Spacing();
                tabs[i].draw(vm);
                ImGui::EndTabItem();
            }
        }
        g_initial_tab = -1;
        ImGui::EndTabBar();
    }
    ImGui::End();
}

bool toasts_active() {
    // New notices arrive from network threads; pull them here (render thread).
    for (auto& n : net::Session::instance().take_notices()) g_toasts.push_back({std::move(n), Clock::now()});
    while (!g_toasts.empty() && Clock::now() - g_toasts.front().born > kToastLife) g_toasts.pop_front();
    while (g_toasts.size() > 4) g_toasts.pop_front();
    // The HUD pill also needs frames while in a room.
    return !g_toasts.empty() || net::Session::instance().mode() != SessionMode::Idle || ui::demo_mode() == 1;
}

void draw_toasts() {
    draw_hud(ui::gather());
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float y = vp->WorkPos.y + vp->WorkSize.y - 24;
    int i = 0;
    for (auto it = g_toasts.rbegin(); it != g_toasts.rend(); ++it, ++i) {
        const float age = std::chrono::duration<float>(Clock::now() - it->born).count();
        const float life = std::chrono::duration<float>(kToastLife).count();
        const float alpha = ui::demo_mode() ? 1.0f : (age > life - 1.0f ? std::max(0.0f, life - age) : 1.0f);
        ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x - 24, y}, ImGuiCond_Always, {1.0f, 1.0f});
        ImGui::SetNextWindowBgAlpha(0.92f * alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16, 10});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
        const std::string id = std::format("##toast{}", i);
        ImGui::Begin(id.c_str(), nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
        // accent stripe on the left
        const ImVec2 wp = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddRectFilled(wp, {wp.x + 4, wp.y + ImGui::GetWindowHeight()}, ImGui::GetColorU32(th::kAccent), 10.0f,
                                                  ImDrawFlags_RoundCornersLeft);
        th::plumbob(11);
        ImGui::SameLine(0, 8);
        ImGui::TextUnformatted(it->text.c_str());
        y -= ImGui::GetWindowHeight() + 10;
        ImGui::End();
        ImGui::PopStyleVar(3);
    }
}

}  // namespace cts4p::panels
