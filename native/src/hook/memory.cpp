#include "hook/memory.h"

#include <windows.h>

#include <cstring>

namespace cts4p::mem {
namespace {

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok)) return false;
    const auto end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return reinterpret_cast<uintptr_t>(p) + n <= end;
}

// SEH must live in a function without C++ objects that need unwinding.
size_t guarded_copy(const void* src, void* dst, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

}  // namespace

std::optional<Module> find_module(const wchar_t* name) {
    HMODULE h = GetModuleHandleW(name);
    if (!h) return std::nullopt;
    Module m;
    m.base = reinterpret_cast<uintptr_t>(h);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + static_cast<uintptr_t>(dos->e_lfanew));
    m.size = nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        const std::string_view sname(reinterpret_cast<const char*>(sec->Name), strnlen(reinterpret_cast<const char*>(sec->Name), 8));
        const uintptr_t b = m.base + sec->VirtualAddress;
        const uintptr_t e = b + sec->Misc.VirtualSize;
        if (sname == ".text") {
            m.text_begin = b;
            m.text_end = e;
        } else if (sname == ".rdata") {
            m.rdata_begin = b;
            m.rdata_end = e;
        }
    }
    if (!m.text_begin) return std::nullopt;
    return m;
}

std::optional<Pattern> Pattern::parse(std::string_view text) {
    Pattern p;
    size_t i = 0;
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    while (i < text.size()) {
        if (text[i] == ' ') {
            ++i;
            continue;
        }
        if (text[i] == '?') {
            p.bytes.push_back(0);
            p.mask.push_back(false);
            i += (i + 1 < text.size() && text[i + 1] == '?') ? 2 : 1;
            continue;
        }
        if (i + 1 >= text.size()) return std::nullopt;
        const int hi = hexval(text[i]), lo = hexval(text[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        p.bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
        p.mask.push_back(true);
        i += 2;
    }
    if (p.bytes.empty() || !p.mask.front()) return std::nullopt;  // first byte anchors the search
    return p;
}

std::vector<uintptr_t> scan(uintptr_t begin, uintptr_t end, const Pattern& pattern, size_t max_hits) {
    std::vector<uintptr_t> hits;
    const size_t n = pattern.bytes.size();
    if (end <= begin || end - begin < n) return hits;
    const auto* data = reinterpret_cast<const uint8_t*>(begin);
    const size_t last = (end - begin) - n;
    const uint8_t first = pattern.bytes[0];
    for (size_t i = 0; i <= last; ++i) {
        const void* found = std::memchr(data + i, first, last - i + 1);
        if (!found) break;
        i = static_cast<size_t>(static_cast<const uint8_t*>(found) - data);
        bool ok = true;
        for (size_t k = 1; k < n && ok; ++k) ok = !pattern.mask[k] || data[i + k] == pattern.bytes[k];
        if (ok) {
            hits.push_back(begin + i);
            if (hits.size() >= max_hits) break;
        }
    }
    return hits;
}

std::optional<FunctionRange> function_at(uintptr_t address) {
    DWORD64 image_base = 0;
    const PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(address, &image_base, nullptr);
    if (!rf) return std::nullopt;
    return FunctionRange{static_cast<uintptr_t>(image_base) + rf->BeginAddress, static_cast<uintptr_t>(image_base) + rf->EndAddress};
}

bool contains(uintptr_t begin, uintptr_t end, const std::vector<uint8_t>& needle) {
    if (needle.empty() || end - begin < needle.size()) return false;
    const auto* data = reinterpret_cast<const uint8_t*>(begin);
    const size_t last = (end - begin) - needle.size();
    for (size_t i = 0; i <= last; ++i)
        if (data[i] == needle[0] && std::memcmp(data + i, needle.data(), needle.size()) == 0) return true;
    return false;
}

std::optional<uintptr_t> find_vtable(const Module& m, std::string_view mangled) {
    // TypeDescriptor = { void* vftable; void* spare; char name[]; } -> name is at +16.
    std::string needle(mangled);
    needle.push_back('\0');
    const auto* image = reinterpret_cast<const char*>(m.base);
    const char* hit = nullptr;
    for (size_t i = 0; i + needle.size() <= m.size; ++i) {
        if (image[i] == needle[0] && std::memcmp(image + i, needle.data(), needle.size()) == 0) {
            hit = image + i;
            break;
        }
    }
    if (!hit || !m.rdata_begin) return std::nullopt;
    const auto td_rva = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hit) - 16 - m.base);
    // CompleteObjectLocator (x64): { u32 signature=1; u32 offset; u32 cd_offset; u32 td_rva; u32 chd_rva; u32 self_rva; }
    for (uintptr_t p = m.rdata_begin; p + 24 <= m.rdata_end; p += 4) {
        const auto* col = reinterpret_cast<const uint32_t*>(p);
        if (col[0] != 1 || col[3] != td_rva || col[1] != 0 || col[5] != static_cast<uint32_t>(p - m.base)) continue;
        // vtable[-1] holds the COL's address.
        for (uintptr_t q = m.rdata_begin; q + 8 <= m.rdata_end; q += 8) {
            if (*reinterpret_cast<const uintptr_t*>(q) == p) return q + 8;
        }
    }
    return std::nullopt;
}

size_t safe_read(const void* src, void* dst, size_t n) {
    if (!src || !readable(src, n)) return 0;
    return guarded_copy(src, dst, n);
}

std::string hex(const void* data, size_t n) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    const auto* b = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) {
        out += kHex[b[i] >> 4];
        out += kHex[b[i] & 0xF];
    }
    return out;
}

}  // namespace cts4p::mem
