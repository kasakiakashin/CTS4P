#include "sync/tray.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>

#include "core/paths.h"

namespace cts4p::sync::tray {
namespace fs = std::filesystem;
namespace {

constexpr char kMagic[4] = {'T', '4', 'T', 'R'};
constexpr uint64_t kGroupSpan = 64;               // Sim portraits use id+1..id+n (households hold at most 8 Sims)
constexpr auto kGroupWindow = std::chrono::minutes(10);

bool is_hex(const std::string& s, size_t from, size_t n) {
    if (from + n > s.size()) return false;
    for (size_t i = from; i < from + n; ++i)
        if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    return true;
}

// "0xGGGGGGGG!0xIIIIIIIIIIIIIIII.ext" -> (group, instance, ext)
bool split_name(const std::string& name, uint32_t& group, uint64_t& instance, std::string& ext) {
    if (name.size() < 32 || name.compare(0, 2, "0x") != 0 || name[10] != '!' || name.compare(11, 2, "0x") != 0 || name[29] != '.')
        return false;
    if (!is_hex(name, 2, 8) || !is_hex(name, 13, 16)) return false;
    group = static_cast<uint32_t>(std::stoul(name.substr(2, 8), nullptr, 16));
    instance = std::stoull(name.substr(13, 16), nullptr, 16);
    ext = name.substr(30);
    return true;
}

uint64_t key_of(uint64_t instance) { return instance & 0x00FFFFFFFFFFFFFFull; }  // portraits use a different top byte

bool valid_utf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) >> 6) != 2) return false;
        i += n;
    }
    return true;
}

std::optional<std::vector<std::byte>> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;
    const auto size = static_cast<size_t>(in.tellg());
    std::vector<std::byte> data(size);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (!in) return std::nullopt;
    return data;
}

bool read_varint(std::span<const std::byte> d, size_t& pos, uint64_t& out) {
    out = 0;
    for (int shift = 0; shift < 64 && pos < d.size(); shift += 7) {
        const auto b = static_cast<uint8_t>(d[pos++]);
        out |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

int64_t unix_seconds(fs::file_time_type t) {
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
    return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

}  // namespace

bool valid_file_name(const std::string& name) {
    uint32_t group = 0;
    uint64_t instance = 0;
    std::string ext;
    if (!split_name(name, group, instance, ext)) return false;
    return ext == "trayitem" || ext == "householdbinary" || ext == "hhi" || ext == "sgi";
}

std::string read_name(std::span<const std::byte> d) {
    // 8-byte header (u32 0, u32 protobuf length), then a protobuf message.
    if (d.size() < 8) return {};
    uint32_t len = 0;
    std::memcpy(&len, d.data() + 4, 4);
    const size_t end = std::min(d.size(), static_cast<size_t>(8) + len);
    size_t pos = 8;
    while (pos < end) {
        uint64_t key = 0;
        if (!read_varint(d.first(end), pos, key)) return {};
        const auto field = key >> 3;
        const auto wire = key & 7;
        if (wire == 0) {
            uint64_t v = 0;
            if (!read_varint(d.first(end), pos, v)) return {};
        } else if (wire == 2) {
            uint64_t n = 0;
            if (!read_varint(d.first(end), pos, n) || n > end - pos) return {};
            if (field == 4) {
                if (n > 128) return {};
                std::string s(reinterpret_cast<const char*>(d.data() + pos), static_cast<size_t>(n));
                return valid_utf8(s) ? s : std::string();
            }
            pos += static_cast<size_t>(n);
        } else if (wire == 1) {
            if (end - pos < 8) return {};
            pos += 8;
        } else if (wire == 5) {
            if (end - pos < 4) return {};
            pos += 4;
        } else {
            return {};
        }
    }
    return {};
}

std::vector<Household> list_households(const fs::path& dir) {
    struct Entry {
        fs::path path;
        uint32_t group;
        uint64_t instance;
        std::string ext;
        fs::file_time_type time;
        uint64_t size;
    };
    std::vector<Entry> entries;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = paths::to_utf8(it->path().filename().wstring());
        Entry e{it->path(), 0, 0, {}, it->last_write_time(ec), it->file_size(ec)};
        if (!split_name(name, e.group, e.instance, e.ext)) continue;
        if (valid_file_name(name)) entries.push_back(std::move(e));
    }
    std::vector<Household> out;
    for (const auto& item : entries) {
        if (item.ext != "trayitem") continue;
        const uint64_t key = key_of(item.instance);
        const bool is_household = std::any_of(entries.begin(), entries.end(), [&](const Entry& e) {
            return e.ext == "householdbinary" && key_of(e.instance) == key;
        });
        if (!is_household) continue;  // lots and rooms are not sent
        Household h;
        h.id = item.instance;
        h.modified_unix = unix_seconds(item.time);
        if (auto bytes = read_file(item.path)) h.name = read_name(*bytes);
        for (const auto& e : entries) {
            const uint64_t k = key_of(e.instance);
            bool member = k == key;
            if (!member && e.ext == "sgi" && k > key && k - key <= kGroupSpan) {
                const auto dt = e.time > item.time ? e.time - item.time : item.time - e.time;
                member = dt <= kGroupWindow;
            }
            if (member && h.files.size() < kMaxFiles) {
                h.files.push_back(e.path);
                h.total_bytes += e.size;
            }
        }
        out.push_back(std::move(h));
    }
    std::sort(out.begin(), out.end(), [](const Household& a, const Household& b) { return a.modified_unix > b.modified_unix; });
    return out;
}

std::optional<std::vector<std::byte>> pack(const std::vector<fs::path>& files, std::string& error) {
    if (files.empty() || files.size() > kMaxFiles) {
        error = "quantidade de arquivos inválida";
        return std::nullopt;
    }
    std::vector<std::byte> blob(8);
    std::memcpy(blob.data(), kMagic, 4);
    const auto count = static_cast<uint32_t>(files.size());
    std::memcpy(blob.data() + 4, &count, 4);
    uint64_t total = 0;
    for (const auto& f : files) {
        const std::string name = paths::to_utf8(f.filename().wstring());
        if (!valid_file_name(name)) {
            error = "nome de arquivo inesperado: " + name;
            return std::nullopt;
        }
        auto data = read_file(f);
        if (!data) {
            error = "não consegui ler " + name;
            return std::nullopt;
        }
        total += data->size();
        if (total > kMaxBytes) {
            error = "família grande demais";
            return std::nullopt;
        }
        const auto n = static_cast<uint16_t>(name.size());
        const uint64_t size = data->size();
        const size_t at = blob.size();
        blob.resize(at + 2 + n + 8 + data->size());
        std::memcpy(blob.data() + at, &n, 2);
        std::memcpy(blob.data() + at + 2, name.data(), n);
        std::memcpy(blob.data() + at + 2 + n, &size, 8);
        if (!data->empty()) std::memcpy(blob.data() + at + 2 + n + 8, data->data(), data->size());
    }
    return blob;
}

std::optional<Unpacked> unpack_into(std::span<const std::byte> blob, const fs::path& dir, std::string& error) {
    struct Item {
        std::string name;
        std::span<const std::byte> data;
    };
    // 1) validate everything before touching the disk
    if (blob.size() < 8 || std::memcmp(blob.data(), kMagic, 4) != 0) {
        error = "pacote de Sim inválido";
        return std::nullopt;
    }
    uint32_t count = 0;
    std::memcpy(&count, blob.data() + 4, 4);
    if (count == 0 || count > kMaxFiles) {
        error = "quantidade de arquivos inválida";
        return std::nullopt;
    }
    std::vector<Item> items;
    size_t pos = 8;
    bool has_item = false, has_household = false;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t n = 0;
        uint64_t size = 0;
        if (pos + 2 > blob.size()) break;
        std::memcpy(&n, blob.data() + pos, 2);
        if (pos + 2 + n + 8 > blob.size()) break;
        std::string name(reinterpret_cast<const char*>(blob.data() + pos + 2), n);
        std::memcpy(&size, blob.data() + pos + 2 + n, 8);
        pos += 2 + n + 8;
        if (size > blob.size() - pos) break;
        if (!valid_file_name(name)) {
            error = "nome de arquivo recusado: " + name;
            return std::nullopt;
        }
        has_item |= name.ends_with(".trayitem");
        has_household |= name.ends_with(".householdbinary");
        items.push_back({std::move(name), blob.subspan(pos, static_cast<size_t>(size))});
        pos += static_cast<size_t>(size);
    }
    if (items.size() != count || pos != blob.size()) {
        error = "pacote de Sim incompleto";
        return std::nullopt;
    }
    if (!has_item || !has_household) {
        error = "o pacote não contém uma família";
        return std::nullopt;
    }
    // 2) write (temp + rename per file)
    Unpacked out;
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (const auto& it : items) {
        if (it.name.ends_with(".trayitem")) out.name = read_name(it.data);
        const fs::path target = dir / paths::from_utf8(it.name);
        const fs::path temp = fs::path(target).concat(L".cts4p-tmp");
        if (fs::exists(target, ec))  // never lose a household the player already had: keep a copy
            fs::copy_file(target, fs::path(target).concat(L".cts4p-bak"), fs::copy_options::overwrite_existing, ec);
        {
            std::ofstream f(temp, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(it.data.data()), static_cast<std::streamsize>(it.data.size()));
            if (!f) {
                error = "falha ao gravar " + it.name;
                return std::nullopt;
            }
        }
        fs::rename(temp, target, ec);
        if (ec) {
            fs::remove(temp, ec);
            error = "falha ao gravar " + it.name;
            return std::nullopt;
        }
        out.written.push_back(target);
    }
    return out;
}

}  // namespace cts4p::sync::tray
