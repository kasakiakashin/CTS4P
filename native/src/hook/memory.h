#pragma once
// Memory utilities for locating game code without hard-coded offsets:
//   * AOB (array-of-bytes) pattern scanning with wildcards ("48 8B ?? C4")
//   * function boundaries from the module's .pdata (RtlLookupFunctionEntry)
//   * MSVC x64 RTTI: vtable lookup by mangled class name
//   * safe reads that never fault (VirtualQuery + SEH)

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cts4p::mem {

struct Module {
    uintptr_t base = 0;
    size_t size = 0;
    uintptr_t text_begin = 0;
    uintptr_t text_end = 0;
    uintptr_t rdata_begin = 0;
    uintptr_t rdata_end = 0;
};

std::optional<Module> find_module(const wchar_t* name);

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;  // true = must match
    static std::optional<Pattern> parse(std::string_view text);
};

std::vector<uintptr_t> scan(uintptr_t begin, uintptr_t end, const Pattern& pattern, size_t max_hits = 8);

struct FunctionRange {
    uintptr_t begin = 0;
    uintptr_t end = 0;
};
// Function containing `address`, from the image's unwind data (works for any module with .pdata).
std::optional<FunctionRange> function_at(uintptr_t address);

// True if `needle` occurs inside [begin, end).
bool contains(uintptr_t begin, uintptr_t end, const std::vector<uint8_t>& needle);

// Address of the primary vtable of the class whose TypeDescriptor name is `mangled`
// (e.g. ".?AUPkgRequestBuildBuyOpMsg@Sims4@EA@@").
std::optional<uintptr_t> find_vtable(const Module& m, std::string_view mangled);

// Copies up to n bytes from src; returns bytes actually readable (0 if the page is not readable).
size_t safe_read(const void* src, void* dst, size_t n);

std::string hex(const void* data, size_t n);

}  // namespace cts4p::mem
