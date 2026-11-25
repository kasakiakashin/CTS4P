// "Criar juntos" Library files: grouping, name parsing, pack/unpack and rejection of bad input.
// Uses a fake Tray folder next to the exe; with an argument it also lists a real Tray folder (read-only).

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "sync/tray.h"
#include "test_common.h"

namespace fs = std::filesystem;
using namespace cts4p::sync;

namespace {

std::vector<std::byte> bytes(const std::string& s) {
    std::vector<std::byte> v(s.size());
    std::memcpy(v.data(), s.data(), s.size());
    return v;
}

std::string trayitem(const std::string& name) {
    std::string pb;
    pb += '\x08';  // field 1 varint
    pb += '\x17';
    pb += '\x10';  // field 2 = 1 (household)
    pb += '\x01';
    pb += '\x22';  // field 4, length-delimited
    pb += static_cast<char>(name.size());
    pb += name;
    std::string out(8, '\0');
    const auto len = static_cast<uint32_t>(pb.size());
    std::memcpy(out.data() + 4, &len, 4);
    return out + pb;
}

void write(const fs::path& p, const std::string& data) { std::ofstream(p, std::ios::binary) << data; }

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

}  // namespace

int main(int argc, char** argv) {
    test::Checker c;
    const fs::path base = fs::path(test::exe_dir()) / L"tray_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path src = base / L"src", dst = base / L"dst";
    fs::create_directories(src);

    // household A (2 Sims) + household B + a lot (no householdbinary) + junk
    write(src / L"0x00000001!0x00e21785f0900017.trayitem", trayitem("Barros"));
    write(src / L"0x00000000!0x00e21785f0900017.householdbinary", std::string(6000, 'h'));
    write(src / L"0xed902802!0x00e21785f0900017.hhi", std::string(900, 'i'));
    write(src / L"0x00000013!0x01e21785f0900018.sgi", std::string(4000, 's'));
    write(src / L"0x00000013!0x01e21785f0900019.sgi", std::string(4100, 't'));
    write(src / L"0x00000001!0x00aa000000000100.trayitem", trayitem("Silva"));
    write(src / L"0x00000000!0x00aa000000000100.householdbinary", std::string(3000, 'x'));
    write(src / L"0x00000001!0x00bb000000000200.trayitem", trayitem("Lote"));
    write(src / L"0x00000002!0x00bb000000000200.blueprint", "lot");
    write(src / L"readme.txt", "not a tray file");

    c.check(tray::valid_file_name("0x00000001!0x00e21785f0900017.trayitem"), "valid name accepted");
    c.check(!tray::valid_file_name("..\\..\\Mods\\evil.trayitem") && !tray::valid_file_name("0x00000001!0x00e21785f0900017.package") &&
                !tray::valid_file_name("0x0000000g!0x00e21785f0900017.hhi") && !tray::valid_file_name("0x00000001!0x00e21785f0900017.trayitem\\x"),
            "path traversal, wrong extension and non-hex names rejected");
    c.check(tray::read_name(bytes(trayitem("Família Ñ"))) == "Família Ñ", "family name read from .trayitem (UTF-8)");
    c.check(tray::read_name(bytes("\x01\x02")).empty() && tray::read_name(bytes(std::string(8, '\0') + "\xff\xff\xff")).empty(),
            "truncated/garbage .trayitem gives empty name, no crash");
    {
        std::string huge = trayitem("x");
        huge.resize(8 + 4);
        huge += '\x22';  // field 4 with a length near 2^64: must not overflow the bounds check
        huge += std::string("\xff\xff\xff\xff\xff\xff\xff\xff\xff\x01");
        const uint32_t len = static_cast<uint32_t>(huge.size() - 8);
        std::memcpy(huge.data() + 4, &len, 4);
        std::string skip = huge;
        skip[12] = '\x2a';  // field 5 (skipped): pos must not wrap around
        c.check(tray::read_name(bytes(huge)).empty() && tray::read_name(bytes(skip)).empty(), "huge length fields rejected (no overflow, no loop)");
        c.check(tray::read_name(bytes(trayitem("\xC3\x28 bad"))).empty(), "invalid UTF-8 family name rejected");
    }

    auto list = tray::list_households(src);
    c.check(list.size() == 2, "2 households found (lot and junk ignored)");
    const tray::Household* barros = nullptr;
    for (const auto& h : list)
        if (h.name == "Barros") barros = &h;
    c.check(barros && barros->files.size() == 5, "household groups trayitem + householdbinary + hhi + 2 Sim portraits");

    std::string error;
    auto blob = barros ? tray::pack(barros->files, error) : std::nullopt;
    c.check(blob.has_value(), "household packed");
    if (blob) {
        auto un = tray::unpack_into(*blob, dst, error);
        c.check(un && un->written.size() == 5 && un->name == "Barros", "unpacked into another Library with its name");
        bool same = true;
        for (const auto& f : barros->files) same &= read(f) == read(dst / f.filename());
        c.check(same, "files are byte-identical after the trip");
        c.check(tray::list_households(dst).size() == 1, "receiver's Library lists the new household");

        auto cut = *blob;
        cut.resize(cut.size() - 10);
        c.check(!tray::unpack_into(cut, base / L"cut", error) && !fs::exists(base / L"cut"), "truncated blob rejected before writing anything");
        auto evil = *blob;
        const char* from = "0x00000001!0x00e21785f0900017.trayitem";
        auto it = std::search(evil.begin(), evil.end(), reinterpret_cast<const std::byte*>(from),
                              reinterpret_cast<const std::byte*>(from) + std::strlen(from));
        if (it != evil.end()) std::memcpy(&*it, "..\\..\\..\\Mods\\x\\evil.package.trayitem", 38);
        c.check(!tray::unpack_into(evil, base / L"evil", error), "blob with a malicious file name rejected");
    }
    std::vector<fs::path> no_household = {src / L"0x00000001!0x00bb000000000200.trayitem"};
    auto lot_blob = tray::pack(no_household, error);
    c.check(lot_blob && !tray::unpack_into(*lot_blob, base / L"lot", error), "blob without a household is refused");

    if (argc > 1) {  // read-only look at a real Library
        for (const auto& h : tray::list_households(fs::path(argv[1])))
            std::printf("    real Library: '%s' (%zu files, %llu bytes)\n", h.name.c_str(), h.files.size(),
                        static_cast<unsigned long long>(h.total_bytes));
    }
    fs::remove_all(base, ec);
    return c.finish("test_tray");
}
