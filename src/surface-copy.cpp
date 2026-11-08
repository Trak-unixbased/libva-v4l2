// SPDX-License-Identifier: MIT
#include "internal.hpp"
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <cerrno>

namespace irisva {
namespace {
void wait_object(int fd, short events, std::chrono::steady_clock::time_point start) {
    pollfd fence{fd, events, 0};
    for (;;) {
        int result = poll(&fence, 1, 100);
        if (result < 0 && errno == EINTR)
            continue;
        check(result >= 0 && !(fence.revents & (POLLERR | POLLNVAL | POLLHUP)),
              "persistent surface DMA-BUF fence wait failed");
        if (result && (fence.revents & fence.events))
            break;
        check(std::chrono::steady_clock::now() - start < std::chrono::seconds(5),
              "persistent surface DMA-BUF fence timeout", VA_STATUS_ERROR_TIMEDOUT);
    }
}
} // namespace
void wait_surface_access(const Memory &memory, short events) {
    auto start = std::chrono::steady_clock::now();
    wait_object(memory.fd, events, start);
    if (memory.split())
        wait_object(memory.chroma_fd, events, start);
}
namespace {
struct CpuAccess {
    Memory &memory;
    uint64_t access;
    bool active = false;
    CpuAccess(Memory &m, uint64_t flags) : memory(m), access(flags) {
        // DMA_BUF_IOCTL_SYNC maintains CPU cache coherency, not exclusion
        // against an outstanding GPU reader of a recycled exported surface.
        wait_surface_access(memory, flags & DMA_BUF_SYNC_WRITE ? POLLOUT : POLLIN);
        check(memory.sync(DMA_BUF_SYNC_START | access), "persistent surface DMA-BUF access failed");
        active = true;
    }
    void finish() {
        if (!active)
            return;
        active = false;
        check(memory.sync(DMA_BUF_SYNC_END | access),
              "persistent surface DMA-BUF access end failed");
    }
    ~CpuAccess() {
        if (active)
            memory.sync(DMA_BUF_SYNC_END | access);
    }
};
void validate(const Memory &m, unsigned width, unsigned height, unsigned bytes) {
    const uint64_t chroma_rows = (height + 1) / 2;
    const uint64_t row_bytes = uint64_t((width + 1) & ~1u) * bytes;
    check(width && height && width <= m.width && height <= m.height && height <= m.storage_height &&
              row_bytes <= m.stride &&
              uint64_t(m.data_offset) + uint64_t(height - 1) * m.stride + row_bytes <= m.size &&
              m.chroma_object_offset() + (chroma_rows - 1) * m.stride + row_bytes <=
                  m.chroma_object_size(),
          "persistent surface copy exceeds buffer layout", VA_STATUS_ERROR_INVALID_SURFACE);
}
} // namespace
void copy_surface(Memory &destination, Memory &source, unsigned width, unsigned height) {
    check(destination.fourcc == source.fourcc &&
              (source.fourcc == VA_FOURCC_NV12 || source.fourcc == VA_FOURCC_P010),
          "persistent surface copy format mismatch", VA_STATUS_ERROR_INVALID_IMAGE_FORMAT);
    unsigned bytes = source.fourcc == VA_FOURCC_P010 ? 2 : 1;
    validate(source, width, height, bytes);
    validate(destination, width, height, bytes);
    auto start = std::chrono::steady_clock::now();
    auto *src = source.map();
    auto *dst = destination.map();
    CpuAccess read(source, DMA_BUF_SYNC_READ);
    CpuAccess write(destination, DMA_BUF_SYNC_WRITE);
    for (unsigned plane = 0; plane < 2; ++plane) {
        size_t src_offset = plane ? source.chroma_mapping_offset() : source.data_offset;
        size_t dst_offset = plane ? destination.chroma_mapping_offset() : destination.data_offset;
        unsigned rows = plane ? (height + 1) / 2 : height;
        unsigned row_bytes = (plane ? ((width + 1) & ~1u) : width) * bytes;
        if (source.stride == destination.stride && row_bytes + 128 >= source.stride) {
            // Identical pitches: one large copy is faster than per-row calls.
            std::memcpy(dst + dst_offset, src + src_offset,
                        size_t(rows - 1) * source.stride + row_bytes);
            continue;
        }
        for (unsigned row = 0; row < rows; ++row)
            std::memcpy(dst + dst_offset + size_t(row) * destination.stride,
                        src + src_offset + size_t(row) * source.stride, row_bytes);
    }
    write.finish();
    read.finish();
    if (debug()) {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
        trace("surface copy mode=cpu %ux%u fourcc=%#x capture=%u time-us=%lld", width, height,
              source.fourcc, source.index, (long long)us);
    }
}
} // namespace irisva
