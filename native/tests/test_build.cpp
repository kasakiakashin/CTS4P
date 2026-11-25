// Runs the runtime build-dispatcher locator against the REAL Simulation_x64.dll of the
// installed game, mapped without running any of its code (DONT_RESOLVE_DLL_REFERENCES).
// usage: test_build <path to Simulation_x64.dll>

#include <string>

#include "test_common.h"

int main(int argc, char** argv) {
    test::Checker c;
    if (argc < 2) {
        std::printf("usage: test_build <Simulation_x64.dll>\n");
        return 2;
    }
    const std::string path = argv[1];
    HMODULE sim = LoadLibraryExA(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    c.check(sim != nullptr, "map Simulation_x64.dll (no code executed)");
    if (!sim) return c.finish("test_build");

    HMODULE dll = LoadLibraryW(test::dll_path().c_str());
    using Probe = uint32_t (*)(const wchar_t*, char*, uint32_t);
    auto probe = test::get<Probe>(dll, "cts4p_debug_build_probe");
    c.check(probe != nullptr, "probe export present");
    if (!probe) return c.finish("test_build");
    char out[4096]{};
    probe(L"Simulation_x64.dll", out, sizeof(out));
    const std::string r(out);
    std::printf("    %s\n", r.c_str());
    c.check(r.find("\"found\":true") != std::string::npos, "dispatcher located");
    c.check(r.find("\"how\":\"aob\"") != std::string::npos, "located by the AOB signature (unique match)");
    c.check(r.find("\"rva\":148352") != std::string::npos, "at the RVA found by static analysis (0x24380)");
    c.check(r.find("\"validated\":true") != std::string::npos, "validated by message-id constants in the body");
    c.check(r.find("\"vtable_PkgRequestBuildBuyOpMsg\":12630920") != std::string::npos, "RTTI resolver finds PkgRequestBuildBuyOpMsg (0xc0bb88)");
    c.check(r.find("\"vtable_LotOperations\":14111216") != std::string::npos, "RTTI resolver finds LotOperations (0xd751f0)");
    return c.finish("test_build");
}
