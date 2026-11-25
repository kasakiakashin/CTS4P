#pragma once
// Lock-free single-producer / single-consumer ring of length-prefixed frames.
//
// Used for the in-process bridge between the native core and the game's
// Python scripts (which call into this DLL through ctypes). One ring per
// direction; each side is touched by exactly one thread, so no locks are
// needed - only acquire/release ordering on the head/tail counters.
//
// Frame layout inside the ring: uint32 length, then `length` bytes. Frames
// may wrap around the end of the buffer.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace cts4p::ipc {

// C4324 "structure was padded due to alignment specifier": the padding is the point -
// head_ and tail_ live on separate cache lines so producer and consumer don't false-share.
#pragma warning(push)
#pragma warning(disable : 4324)
class FrameRing {
public:
    explicit FrameRing(size_t capacity_pow2) : cap_(capacity_pow2), mask_(capacity_pow2 - 1), buf_(new std::byte[capacity_pow2]) {}

    size_t capacity() const { return cap_; }
    static constexpr uint32_t kMaxFrame = 1u << 20;  // 1 MiB

    // Producer side. Returns false when the frame does not fit (caller decides: drop or retry).
    bool push(const void* data, uint32_t len) {
        if (len > kMaxFrame) return false;
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        const uint64_t need = sizeof(uint32_t) + len;
        if (cap_ - (head - tail) < need) return false;
        write(head, &len, sizeof(len));
        write(head + sizeof(len), data, len);
        head_.store(head + need, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns the frame length, 0 when empty, or -(needed) when `cap` is too small
    // (the frame stays queued so the caller can retry with a bigger buffer).
    int64_t pop(void* out, uint32_t cap) {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_acquire);
        if (head == tail) return 0;
        uint32_t len = 0;
        read(tail, &len, sizeof(len));
        if (len > cap) return -static_cast<int64_t>(len);
        read(tail + sizeof(len), out, len);
        tail_.store(tail + sizeof(len) + len, std::memory_order_release);
        return len;
    }

    size_t used() const { return static_cast<size_t>(head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire)); }

private:
    void write(uint64_t pos, const void* src, size_t n) {
        const size_t off = static_cast<size_t>(pos & mask_);
        const size_t first = std::min(n, cap_ - off);
        std::memcpy(buf_.get() + off, src, first);
        std::memcpy(buf_.get(), static_cast<const std::byte*>(src) + first, n - first);
    }
    void read(uint64_t pos, void* dst, size_t n) const {
        const size_t off = static_cast<size_t>(pos & mask_);
        const size_t first = std::min(n, cap_ - off);
        std::memcpy(dst, buf_.get() + off, first);
        std::memcpy(static_cast<std::byte*>(dst) + first, buf_.get(), n - first);
    }

    const size_t cap_;
    const size_t mask_;
    std::unique_ptr<std::byte[]> buf_;
    alignas(64) std::atomic<uint64_t> head_{0};  // written by producer
    alignas(64) std::atomic<uint64_t> tail_{0};  // written by consumer
};
#pragma warning(pop)

}  // namespace cts4p::ipc
