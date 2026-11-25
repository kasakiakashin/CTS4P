#include "sync/manifest.h"

#include <algorithm>
#include <format>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "diag/diagnostics.h"
#include "sync/compress.h"

namespace cts4p::sync {
namespace {

using json = nlohmann::json;
constexpr size_t kMaxManifestBytes = 32u << 20;  // 32 MiB of JSON (~300k mod files)

}  // namespace

Manifest from_environment(const diag::Environment& env) {
    Manifest m;
    m.game_version = env.game_version;
    m.packs = env.packs;
    std::sort(m.packs.begin(), m.packs.end());
    for (const auto& f : env.mods) m.mods.push_back({f.relative_path, f.size, f.fingerprint});
    std::sort(m.mods.begin(), m.mods.end(), [](const ManifestMod& a, const ManifestMod& b) { return a.path < b.path; });
    return m;
}

std::vector<std::byte> encode(const Manifest& m) {
    json mods = json::array();
    for (const auto& f : m.mods) mods.push_back(json::array({f.path, f.size, f.fingerprint}));
    const std::string text = json{{"v", 1}, {"game", m.game_version}, {"packs", m.packs}, {"mods", mods}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    return zstd_compress(std::as_bytes(std::span(text.data(), text.size())), 9);
}

std::optional<Manifest> decode(const std::byte* data, size_t size) {
    auto raw = zstd_decompress(std::span(data, size), kMaxManifestBytes);
    if (!raw) return std::nullopt;
    const json j = json::parse(reinterpret_cast<const char*>(raw->data()), reinterpret_cast<const char*>(raw->data()) + raw->size(),
                               nullptr, false);
    if (!j.is_object()) return std::nullopt;
    try {
        Manifest m;
        m.game_version = j.value("game", "");
        m.packs = j.value("packs", std::vector<std::string>{});
        for (const auto& e : j.at("mods")) {
            if (!e.is_array() || e.size() != 3) continue;
            m.mods.push_back({e[0].get<std::string>(), e[1].get<unsigned long long>(), e[2].get<unsigned long long>()});
        }
        return m;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

CompatReport compare(const Manifest& local, const Manifest& remote, std::string remote_name) {
    CompatReport r;
    r.received = true;
    r.remote_name = std::move(remote_name);
    r.local_game_version = local.game_version;
    r.remote_game_version = remote.game_version;

    const std::set<std::string> lp(local.packs.begin(), local.packs.end());
    const std::set<std::string> rp(remote.packs.begin(), remote.packs.end());
    std::set_difference(rp.begin(), rp.end(), lp.begin(), lp.end(), std::back_inserter(r.packs_missing_here));
    std::set_difference(lp.begin(), lp.end(), rp.begin(), rp.end(), std::back_inserter(r.packs_missing_there));

    std::map<std::string, const ManifestMod*> lm, rm;
    for (const auto& f : local.mods) lm[f.path] = &f;
    for (const auto& f : remote.mods) rm[f.path] = &f;
    for (const auto& [path, f] : rm) {
        auto it = lm.find(path);
        if (it == lm.end()) r.mods_missing_here.push_back(path);
        else if (it->second->size != f->size || it->second->fingerprint != f->fingerprint) r.mods_different.push_back(path);
    }
    for (const auto& [path, f] : lm)
        if (!rm.contains(path)) r.mods_missing_there.push_back(path);
    return r;
}

std::string CompatReport::summary() const {
    if (!received) return "aguardando lista de conteúdo...";
    if (fully_compatible()) return "conteúdo idêntico (versão, pacotes e mods)";
    std::string s;
    auto add = [&](const std::string& part) { s += (s.empty() ? "" : "; ") + part; };
    if (!game_version_matches()) add(std::format("versão do jogo difere ({} x {})", local_game_version, remote_game_version));
    if (!packs_missing_here.empty()) add(std::format("faltam {} pacote(s) aqui", packs_missing_here.size()));
    if (!packs_missing_there.empty()) add(std::format("faltam {} pacote(s) em {}", packs_missing_there.size(), remote_name));
    if (!mods_missing_here.empty()) add(std::format("faltam {} mod(s) aqui", mods_missing_here.size()));
    if (!mods_missing_there.empty()) add(std::format("faltam {} mod(s) em {}", mods_missing_there.size(), remote_name));
    if (!mods_different.empty()) add(std::format("{} mod(s) com versão diferente", mods_different.size()));
    return s;
}

}  // namespace cts4p::sync
