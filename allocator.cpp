// SPDX-License-Identifier: MIT
#include "internal.hpp"
#include <gbm.h>
#include <linux/dma-heap.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <cerrno>

namespace irisva {
namespace {
size_t page_align(size_t size) {
    long page = sysconf(_SC_PAGESIZE);
    check(page > 0, "cannot determine page size");
    return (size + size_t(page) - 1) / size_t(page) * size_t(page);
}
int retry_ioctl(int fd, unsigned long command, void *argument) {
    int result;
    do {
        result = ioctl(fd, command, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}
// The system heap provides cached pages with DMA-BUF CPU access maintenance.
// Distributions usually restrict /dev/dma_heap to root; failure is expected.
int dma_heap_allocate(size_t size) {
    int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (heap < 0)
        return -1;
    dma_heap_allocation_data data{};
    data.len = size;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    int result = retry_ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data);
    close(heap);
    return result == 0 ? int(data.fd) : -1;
}
// systemd grants /dev/udmabuf to the seat user. The kernel limits a single
// buffer (udmabuf.size_limit_mb, 64 MiB by default); larger requests fail.
int udmabuf_allocate(size_t size) {
    int device = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (device < 0)
        return -1;
    int fd = -1;
    int memfd = memfd_create("libva-v4l2", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memfd >= 0 && ftruncate(memfd, off_t(size)) == 0 &&
        fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) == 0) {
        udmabuf_create create{};
        create.memfd = memfd;
        create.flags = UDMABUF_FLAGS_CLOEXEC;
        create.offset = 0;
        create.size = size;
        fd = retry_ioctl(device, UDMABUF_CREATE, &create);
    }
    if (memfd >= 0)
        close(memfd);
    close(device);
    return fd;
}
} // namespace

struct Allocator::Gbm {
    int fd = -1;
    gbm_device *device = nullptr;
    explicit Gbm(int render_fd) {
        // A private file description keeps GBM's GEM handles out of the
        // application's handle namespace. Sandboxed callers may only own the
        // VA fd; a duplicate is still correct because handles are closed
        // before the PRIME fd is returned.
        if (char *name = drmGetRenderDeviceNameFromFd(render_fd)) {
            fd = open(name, O_RDWR | O_CLOEXEC);
            free(name);
        }
        if (fd < 0)
            fd = fcntl(render_fd, F_DUPFD_CLOEXEC, 0);
        check(fd >= 0, "open render node for GBM allocation", VA_STATUS_ERROR_ALLOCATION_FAILED);
        device = gbm_create_device(fd);
        if (!device) {
            close(fd);
            throw Error(VA_STATUS_ERROR_ALLOCATION_FAILED, "create GBM allocation device");
        }
    }
    ~Gbm() {
        gbm_device_destroy(device);
        close(fd);
    }
    Gbm(const Gbm &) = delete;
    Gbm &operator=(const Gbm &) = delete;
    // Allocates rows of at least stride bytes; returns the PRIME fd and the
    // stride chosen by the GPU driver.
    int allocate(unsigned stride, unsigned rows, unsigned &actual_stride) {
        // ABGR8888 texels transport raw bytes; no YUV semantics are needed.
        gbm_bo *bo = gbm_bo_create(device, stride / 4, rows, GBM_FORMAT_ABGR8888,
                                   GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
        if (!bo)
            return -1;
        actual_stride = gbm_bo_get_stride(bo);
        int prime = -1;
        // Export read/write ourselves: some Mesa drivers export read-only fds,
        // which cannot be mapped writable for vaPutImage or encoder upload.
        if (gbm_bo_get_offset(bo, 0) != 0 || actual_stride < stride ||
            drmPrimeHandleToFD(fd, gbm_bo_get_handle(bo).u32, DRM_CLOEXEC | DRM_RDWR, &prime) < 0)
            prime = -1;
        gbm_bo_destroy(bo); // The PRIME fd owns the storage.
        return prime;
    }
};

Allocator::Allocator(int render_fd)
    : render_fd_(render_fd), msm_(render_fd >= 0 && irisva_drm_is_msm(render_fd)) {}
Allocator::~Allocator() = default;

std::shared_ptr<Memory> Allocator::allocate(unsigned width, unsigned height, unsigned fourcc) {
    check(width && height && width <= 8192 && height <= 8192, "invalid surface allocation size",
          VA_STATUS_ERROR_ALLOCATION_FAILED);
    auto m = std::make_shared<Memory>();
    m->width = width;
    m->height = height;
    m->fourcc = fourcc;
    unsigned bytes_per_sample = fourcc == VA_FOURCC_P010 ? 2 : 1;
    // 128-byte pitches satisfy Iris, Linlon and Mali (64-byte) import rules.
    m->stride = (width * bytes_per_sample + 127) & ~127u;
    m->storage_height = (height + 31) & ~31u;
    const unsigned rows = m->storage_height + m->storage_height / 2;
    m->size = page_align(size_t(m->stride) * rows);
    const char *kind = "msm";
    if (msm_) {
        // Keep the original Qualcomm path: coherent MSM GEM where available.
        int coherent = 0;
        m->fd = irisva_msm_allocate(render_fd_, m->size, &coherent);
        m->origin = coherent ? MemoryOrigin::MsmCoherent : MemoryOrigin::MsmWriteCombined;
    } else if ((m->fd = dma_heap_allocate(m->size)) >= 0) {
        m->origin = MemoryOrigin::DmaHeap;
        kind = "dma-heap";
    } else if ((m->fd = udmabuf_allocate(m->size)) >= 0) {
        m->origin = MemoryOrigin::Udmabuf;
        kind = "udmabuf";
    } else {
        check(render_fd_ >= 0, "surface allocation requires a DRM display",
              VA_STATUS_ERROR_ALLOCATION_FAILED);
        if (!gbm_ && !gbm_failed_) {
            try {
                gbm_ = std::make_unique<Gbm>(render_fd_);
            } catch (const Error &) {
                gbm_failed_ = true;
                throw;
            }
        }
        check(bool(gbm_), "no usable DMA-BUF allocator (dma-heap, udmabuf or GBM)",
              VA_STATUS_ERROR_ALLOCATION_FAILED);
        unsigned stride = m->stride;
        m->fd = gbm_->allocate(m->stride, rows, stride);
        check(m->fd >= 0, "allocate linear GBM surface", VA_STATUS_ERROR_ALLOCATION_FAILED);
        auto length = lseek(m->fd, 0, SEEK_END);
        if (length < 0 || uint64_t(length) < uint64_t(stride) * rows) {
            close(m->fd);
            m->fd = -1;
            throw Error(VA_STATUS_ERROR_ALLOCATION_FAILED, "GBM surface smaller than requested");
        }
        m->stride = stride;
        m->size = size_t(length);
        m->origin = MemoryOrigin::Gbm;
        kind = "gbm";
    }
    check(m->fd >= 0, "allocate surface DMA-BUF", VA_STATUS_ERROR_ALLOCATION_FAILED);
    trace("allocated %s surface %ux%u fourcc=%#x pitch=%u size=%zu", kind, width, height, fourcc,
          m->stride, m->size);
    return m;
}
} // namespace irisva
