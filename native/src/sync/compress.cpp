#include "sync/compress.h"

#include <zstd.h>

namespace cts4p::sync {

std::vector<std::byte> zstd_compress(std::span<const std::byte> input, int level) {
    std::vector<std::byte> out(ZSTD_compressBound(input.size()));
    const size_t n = ZSTD_compress(out.data(), out.size(), input.data(), input.size(), level);
    if (ZSTD_isError(n)) return {};
    out.resize(n);
    return out;
}

std::optional<std::vector<std::byte>> zstd_decompress(std::span<const std::byte> input, size_t max_output) {
    const unsigned long long declared = ZSTD_getFrameContentSize(input.data(), input.size());
    if (declared == ZSTD_CONTENTSIZE_ERROR || declared == ZSTD_CONTENTSIZE_UNKNOWN || declared > max_output) {
        return std::nullopt;
    }
    std::vector<std::byte> out(static_cast<size_t>(declared));
    const size_t n = ZSTD_decompress(out.data(), out.size(), input.data(), input.size());
    if (ZSTD_isError(n) || n != out.size()) return std::nullopt;
    return out;
}

unsigned long long fnv1a64(std::span<const std::byte> data) {
    unsigned long long h = 1469598103934665603ULL;
    for (std::byte b : data) {
        h ^= static_cast<unsigned char>(b);
        h *= 1099511628211ULL;
    }
    return h;
}

}  // namespace cts4p::sync
