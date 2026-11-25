#pragma once
// zstd helpers for in-memory compression (manifest, save files).

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace cts4p::sync {

std::vector<std::byte> zstd_compress(std::span<const std::byte> input, int level = 3);

// Rejects frames whose declared content size exceeds `max_output` (protects against bombs).
std::optional<std::vector<std::byte>> zstd_decompress(std::span<const std::byte> input, size_t max_output);

unsigned long long fnv1a64(std::span<const std::byte> data);

}  // namespace cts4p::sync
