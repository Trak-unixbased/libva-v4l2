// SPDX-License-Identifier: MIT
#include "internal.hpp"
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <glob.h>
#include <climits>
#include <cerrno>
#include <set>

namespace irisva {
namespace {
constexpr unsigned OUTPUT = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
constexpr unsigned CAPTURE = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
// Arm Linlon (mvx) private control, V4L2_CTRL_CLASS_CODEC + 0x2000 + 7.
// It is looked up by name first; the ID is only a fallback.
constexpr uint32_t MVX_CID_FRAME_REORDERING = 0x00992007;
int call(int fd, unsigned long command, void *p) {
    int r;
    do {
        r = ioctl(fd, command, p);
    } while (r < 0 && errno == EINTR);
    return r;
}
void checked(int fd, unsigned long command, void *p, const char *name) {
    if (call(fd, command, p) < 0)
        throw Error(VA_STATUS_ERROR_OPERATION_FAILED, std::string(name) + ": " + strerror(errno));
}
// Multi-planar buffer descriptor. The plane count must match the queue:
// Iris uses one plane, Linlon reports NV12/P010 CAPTURE as two planes.
struct QueueBuffer {
    v4l2_plane planes[VIDEO_MAX_PLANES]{};
    v4l2_buffer buffer{};
    explicit QueueBuffer(unsigned type, unsigned index = 0, unsigned count = 1) {
        buffer.type = type;
        buffer.index = index;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.length = count;
        buffer.m.planes = planes;
    }
};
bool expired(std::chrono::steady_clock::time_point start) {
    return std::chrono::steady_clock::now() - start > std::chrono::seconds(5);
}
size_t page_round(size_t size) {
    static const size_t page = [] {
        long p = sysconf(_SC_PAGESIZE);
        return p > 0 ? size_t(p) : size_t(4096);
    }();
    return (size + page - 1) / page * page;
}
const char *driver_name(CodecDriver driver) {
    return driver == CodecDriver::Iris ? "iris" : driver == CodecDriver::Mvx ? "mvx" : "unknown";
}
std::string sysfs_name(const std::string &node) {
    auto slash = node.rfind('/');
    std::string path = "/sys/class/video4linux/" +
                       node.substr(slash == std::string::npos ? 0 : slash + 1) + "/name";
    std::string name;
    if (FILE *f = fopen(path.c_str(), "re")) {
        char line[64] = {};
        if (fgets(line, sizeof(line), f))
            name = line;
        fclose(f);
    }
    while (!name.empty() && (name.back() == '\n' || name.back() == ' '))
        name.pop_back();
    return name;
}
bool known_codec_node(const std::string &name) {
    // Linlon registers "mvxdec"/"mvxenc"; Iris "qcom-iris-decoder/encoder".
    return name == "mvxdec" || name == "mvxenc" || name.rfind("qcom-iris", 0) == 0;
}
} // namespace

Memory::~Memory() {
    fastcv_registration.reset();
    if (mapping)
        munmap(mapping, mapping_size());
    if (fd >= 0)
        close(fd);
    if (chroma_fd >= 0)
        close(chroma_fd);
}
size_t Memory::mapping_size() const {
    return split() ? page_round(size) + page_round(chroma_size) : size;
}
size_t Memory::chroma_mapping_offset() const {
    return split() ? page_round(size) + chroma_offset
                   : size_t(data_offset) + size_t(stride) * storage_height;
}
uint8_t *Memory::map() {
    if (!mapping) {
        if (!split()) {
            void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            check(p != MAP_FAILED, "mmap DMA-BUF failed");
            mapping = p;
        } else {
            // Reserve one range, then place both planes in it. VAImage and the
            // CPU copy paths can then address chroma relative to one pointer.
            const size_t luma = page_round(size), total = mapping_size();
            void *base =
                mmap(nullptr, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            check(base != MAP_FAILED, "reserve split DMA-BUF mapping failed");
            auto *bytes = static_cast<uint8_t *>(base);
            if (mmap(bytes, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) ==
                    MAP_FAILED ||
                mmap(bytes + luma, chroma_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                     chroma_fd, 0) == MAP_FAILED) {
                munmap(base, total);
                throw Error(VA_STATUS_ERROR_OPERATION_FAILED, "mmap split DMA-BUF failed");
            }
            mapping = base;
        }
    }
    return static_cast<uint8_t *>(mapping);
}
bool Memory::sync(uint64_t flags) const {
    for (int object : {fd, chroma_fd}) {
        if (object < 0)
            continue;
        dma_buf_sync request{flags};
        if (call(object, DMA_BUF_IOCTL_SYNC, &request) < 0)
            return false;
    }
    return true;
}

CodecDriver codec_driver(int fd) {
    v4l2_capability cap{};
    if (call(fd, VIDIOC_QUERYCAP, &cap) < 0)
        return CodecDriver::Unknown;
    const char *name = reinterpret_cast<const char *>(cap.driver);
    if (!strcmp(name, "iris_driver"))
        return CodecDriver::Iris;
    if (!strcmp(name, "mvx"))
        return CodecDriver::Mvx;
    return CodecDriver::Unknown;
}
uint32_t find_control(int fd, const char *name) {
    v4l2_queryctrl query{};
    query.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    while (call(fd, VIDIOC_QUERYCTRL, &query) == 0) {
        if (!(query.flags & V4L2_CTRL_FLAG_DISABLED) &&
            !strcmp(reinterpret_cast<const char *>(query.name), name))
            return query.id;
        query.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }
    return 0;
}
std::string find_codec_device(bool encoder, const char *const *environment) {
    std::vector<std::string> paths;
    bool explicit_device = false;
    for (; environment && *environment; ++environment)
        if (const char *p = std::getenv(*environment)) {
            paths.emplace_back(p);
            explicit_device = true;
            break;
        }
    if (!explicit_device) {
        glob_t g{};
        if (!glob("/dev/video*", 0, nullptr, &g))
            for (size_t i = 0; i < g.gl_pathc; ++i)
                paths.emplace_back(g.gl_pathv[i]);
        globfree(&g);
        // udev aliases (e.g. /dev/video-cixdec0) resolve to the same nodes.
        std::set<std::string> unique;
        std::vector<std::string> known, other;
        for (const auto &path : paths) {
            char resolved[PATH_MAX];
            std::string node = realpath(path.c_str(), resolved) ? resolved : path;
            if (!unique.insert(node).second)
                continue;
            (known_codec_node(sysfs_name(node)) ? known : other).push_back(node);
        }
        // Never open unrelated nodes once codec nodes are identified: on CIX
        // Sky1 the vendor ISP node is known to misbehave on probing.
        paths = known.empty() ? other : known;
    }
    for (const auto &path : paths) {
        int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        v4l2_capability cap{};
        bool match = false;
        CodecDriver driver = codec_driver(fd);
        if ((driver != CodecDriver::Unknown || explicit_device) &&
            !call(fd, VIDIOC_QUERYCAP, &cap)) {
            unsigned caps =
                cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
            if (caps & V4L2_CAP_VIDEO_M2M_MPLANE) {
                v4l2_fmtdesc fmt{};
                fmt.type = encoder ? CAPTURE : OUTPUT;
                while (!call(fd, VIDIOC_ENUM_FMT, &fmt)) {
                    if (fmt.pixelformat == V4L2_PIX_FMT_H264)
                        match = true;
                    ++fmt.index;
                }
            }
        }
        close(fd);
        if (match) {
            trace("%s device %s driver=%s", encoder ? "encoder" : "decoder", path.c_str(),
                  driver_name(driver));
            return path;
        }
    }
    return {};
}
std::string find_device() {
    static const char *const environment[] = {"LIBVA_V4L2_DECODER_DEVICE", "IRIS_VAAPI_DEVICE",
                                              nullptr};
    auto device = find_codec_device(false, environment);
    check(!device.empty(), "no Iris or Arm Linlon (mvx) stateful H264 decoder found");
    return device;
}
Decoder::Decoder(const std::string &device, unsigned width, unsigned height, unsigned surfaces,
                 VAProfile profile, int render_fd)
    : width_(width), height_(height), pool_size_(std::min(64u, std::max(32u, surfaces + 4))),
      fourcc_(is_10bit(profile) ? VA_FOURCC_P010 : VA_FOURCC_NV12), copier_(render_fd) {
    fd_ = open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    check(fd_ >= 0, "open V4L2 decoder");
    try {
        driver_ = codec_driver(fd_);
        v4l2_event_subscription sub{};
        sub.type = V4L2_EVENT_SOURCE_CHANGE;
        checked(fd_, VIDIOC_SUBSCRIBE_EVENT, &sub, "SUBSCRIBE SOURCE_CHANGE");
        v4l2_format fmt{};
        fmt.type = OUTPUT;
        fmt.fmt.pix_mp.width = width;
        fmt.fmt.pix_mp.height = height;
        fmt.fmt.pix_mp.pixelformat = is_vp9(profile)    ? V4L2_PIX_FMT_VP9
                                     : is_hevc(profile) ? V4L2_PIX_FMT_HEVC
                                                        : V4L2_PIX_FMT_H264;
        unsigned codec = fmt.fmt.pix_mp.pixelformat;
        fmt.fmt.pix_mp.num_planes = 1;
        checked(fd_, VIDIOC_S_FMT, &fmt, "S_FMT OUTPUT");
        check(fmt.fmt.pix_mp.pixelformat == codec, "decoder does not support the requested codec");
        configure_decode_order(profile);
        allocate(OUTPUT, 4, 1, output_);
        unsigned type = OUTPUT;
        checked(fd_, VIDIOC_STREAMON, &type, "STREAMON OUTPUT");
        output_on_ = true;
        if (const char *path = std::getenv("IRIS_VAAPI_DUMP"))
            dump_ = fopen(path, "wb");
        trace("%s session %s %ux%u output-buffers=%zu", driver_name(driver_), device.c_str(), width,
              height, output_.size());
    } catch (...) {
        close(fd_);
        fd_ = -1;
        throw;
    }
}
Decoder::~Decoder() {
    if (dump_)
        fclose(dump_);
    if (fd_ >= 0) {
        unsigned type = OUTPUT;
        if (output_on_)
            call(fd_, VIDIOC_STREAMOFF, &type);
        type = CAPTURE;
        if (capture_on_)
            call(fd_, VIDIOC_STREAMOFF, &type);
        close(fd_);
    }
    for (auto &[token, surface] : pending_) {
        surface->status = VA_STATUS_ERROR_DECODING_ERROR;
        surface->pending = false;
    }
}
void Decoder::configure_decode_order(VAProfile profile) {
    (void)profile;
    // Every VA picture must complete independently: request decode-order
    // output. Iris uses the standard display-delay controls (patched kernel).
    v4l2_control delay{};
    delay.id = V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY;
    delay.value = 0;
    if (call(fd_, VIDIOC_S_CTRL, &delay) == 0) {
        delay.id = V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE;
        delay.value = 1;
        checked(fd_, VIDIOC_S_CTRL, &delay, "S_CTRL DISPLAY_DELAY_ENABLE");
        return;
    }
    if (driver_ != CodecDriver::Mvx)
        throw Error(VA_STATUS_ERROR_OPERATION_FAILED,
                    std::string("S_CTRL DISPLAY_DELAY (Iris decode-order patch required): ") +
                        strerror(errno));
    // Linlon: the firmware option MVE_SET_OPT_INDEX_FRAME_REORDERING receives
    // the control value as-is, and arg 0 disables reordering. The driver's
    // tristate names are inverted (MVX_TRI_TRUE == 0), so 0 is correct here.
    // It must be set before STREAMON, when the firmware is loaded.
    uint32_t id = find_control(fd_, "frame reordering");
    if (!id) {
        v4l2_queryctrl query{};
        query.id = MVX_CID_FRAME_REORDERING;
        check(call(fd_, VIDIOC_QUERYCTRL, &query) == 0 && query.type == V4L2_CTRL_TYPE_INTEGER &&
                  query.minimum == -1 && query.maximum == 1,
              "Linlon frame reordering control not found");
        id = query.id;
    }
    v4l2_control reorder{};
    reorder.id = id;
    reorder.value = 0;
    checked(fd_, VIDIOC_S_CTRL, &reorder, "S_CTRL Linlon frame reordering");
    trace("Linlon decode-order output enabled (control %#x)", id);
}
void Decoder::allocate(unsigned type, unsigned count, unsigned planes,
                       std::vector<std::shared_ptr<Memory>> &pool) {
    check(planes >= 1 && planes <= 2, "unsupported V4L2 plane count");
    v4l2_requestbuffers req{};
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    req.count = count;
    checked(fd_, VIDIOC_REQBUFS, &req, "REQBUFS");
    check(req.count > 0 && req.count <= 64, "invalid allocated buffer count");
    for (unsigned i = 0; i < req.count; ++i) {
        QueueBuffer q(type, i, planes);
        checked(fd_, VIDIOC_QUERYBUF, &q.buffer, "QUERYBUF");
        check(q.buffer.length == planes, "unexpected V4L2 buffer plane count");
        auto m = std::make_shared<Memory>();
        m->origin = MemoryOrigin::Iris;
        m->index = i;
        m->size = q.planes[0].length;
        if (type == OUTPUT) {
            void *p = mmap(nullptr, m->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                           q.planes[0].m.mem_offset);
            check(p != MAP_FAILED, "mmap OUTPUT");
            m->mapping = p;
        } else {
            for (unsigned p = 0; p < planes; ++p) {
                v4l2_exportbuffer exp{};
                exp.type = type;
                exp.index = i;
                exp.plane = p;
                exp.flags = O_CLOEXEC | O_RDWR;
                checked(fd_, VIDIOC_EXPBUF, &exp, "EXPBUF");
                if (p) {
                    m->chroma_fd = exp.fd;
                    m->chroma_size = q.planes[p].length;
                } else {
                    m->fd = exp.fd;
                }
            }
        }
        pool.push_back(m);
    }
}
bool Decoder::capture_format_unchanged() {
    v4l2_format fmt{};
    fmt.type = CAPTURE;
    if (capture_.empty() || call(fd_, VIDIOC_G_FMT, &fmt) < 0)
        return false;
    const auto &p = fmt.fmt.pix_mp;
    const auto &m = *capture_.front();
    unsigned pixel_format = fourcc_ == VA_FOURCC_P010 ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12;
    return p.pixelformat == pixel_format && p.num_planes == capture_planes_ && p.width == m.width &&
           p.height == m.height && p.plane_fmt[0].bytesperline == capture_bytesperline_;
}
void Decoder::configure_capture() {
    check(!capture_on_, "in-context dynamic resolution change not yet implemented",
          VA_STATUS_ERROR_UNIMPLEMENTED);
    v4l2_format fmt{};
    fmt.type = CAPTURE;
    checked(fd_, VIDIOC_G_FMT, &fmt, "G_FMT CAPTURE");
    unsigned pixel_format = fourcc_ == VA_FOURCC_P010 ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.pixelformat = pixel_format;
    if (driver_ == CodecDriver::Mvx) {
        // Linlon always describes NV12/P010 as two discrete planes and
        // otherwise chooses a pitch of exactly width * bytes. Request a
        // 128-byte pitch so Mali (Panfrost) and display engines can import
        // the decoded planes without copies.
        unsigned bytes = fourcc_ == VA_FOURCC_P010 ? 2 : 1;
        unsigned pitch = (fmt.fmt.pix_mp.width * bytes + 127) & ~127u;
        fmt.fmt.pix_mp.num_planes = 2;
        for (unsigned p = 0; p < 2; ++p) {
            fmt.fmt.pix_mp.plane_fmt[p].bytesperline = pitch;
            fmt.fmt.pix_mp.plane_fmt[p].sizeimage = 0;
        }
    }
    checked(fd_, VIDIOC_S_FMT, &fmt, "S_FMT CAPTURE");
    const auto &format = fmt.fmt.pix_mp;
    const unsigned planes = format.num_planes;
    check(format.pixelformat == pixel_format &&
              (planes == 1 || (planes == 2 && format.plane_fmt[1].bytesperline ==
                                                  format.plane_fmt[0].bytesperline)),
          "unsupported CAPTURE layout");
    capture_planes_ = planes;
    capture_bytesperline_ = format.plane_fmt[0].bytesperline;
    v4l2_control minimum{};
    minimum.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
    checked(fd_, VIDIOC_G_CTRL, &minimum, "G_CTRL minimum capture buffers");
    // FFmpeg may pass zero render targets and allocate VA surfaces later.
    // The firmware minimum alone leaves no room for surfaces retained in VA's
    // reference/reorder queues. Reserve at least 32 slots and honor larger
    // renderer pools (VLC), within the patched Iris driver's 64-buffer limit.
    const unsigned target = std::max(pool_size_, unsigned(std::max(0, minimum.value)));
    trace("CAPTURE pool target=%u firmware-min=%d planes=%u", target, minimum.value, planes);
    allocate(CAPTURE, target, planes, capture_);
    if (driver_ == CodecDriver::Iris) {
        check(capture_.size() >= pool_size_,
              "CAPTURE pool too small; apply the Iris gen2 capture-pool kernel patch",
              VA_STATUS_ERROR_ALLOCATION_FAILED);
    } else if (capture_.size() < target) {
        // videobuf2 limits a queue to 32 buffers unless the driver raises it
        // (Linlon does not). Keep a margin above the firmware's DPB needs.
        check(capture_.size() >= unsigned(std::max(0, minimum.value)) + 4,
              "CAPTURE pool too small for the stream's reference frames",
              VA_STATUS_ERROR_ALLOCATION_FAILED);
        trace("CAPTURE pool limited by driver: granted=%zu wanted=%u", capture_.size(), target);
        pool_size_ = unsigned(capture_.size());
    }
    for (auto &m : capture_) {
        m->stride = format.plane_fmt[0].bytesperline;
        m->storage_height = format.height;
        m->width = format.width;
        m->height = format.height;
        m->fourcc = fourcc_;
    }
    requeue();
    unsigned type = CAPTURE;
    checked(fd_, VIDIOC_STREAMON, &type, "STREAMON CAPTURE");
    capture_on_ = true;
    source_change_ = false;
    trace("CAPTURE %ux%u pitch=%u planes=%u count=%zu", format.width, format.height,
          format.plane_fmt[0].bytesperline, planes, capture_.size());
}
void Decoder::requeue() {
    if (last_)
        return;
    for (auto &m : capture_) {
        if (!m->queued && m.use_count() == 1) {
            QueueBuffer q(CAPTURE, m->index, capture_planes_);
            q.planes[0].length = m->size;
            if (capture_planes_ > 1)
                q.planes[1].length = m->chroma_size;
            checked(fd_, VIDIOC_QBUF, &q.buffer, "QBUF CAPTURE");
            m->queued = true;
        }
    }
}
void Decoder::pump(int timeout_ms) {
    requeue();
    if (timeout_ms) {
        pollfd p{fd_, POLLIN | POLLPRI | POLLOUT, 0};
        int ret;
        do {
            ret = poll(&p, 1, timeout_ms);
        } while (ret < 0 && errno == EINTR);
        check(ret >= 0 && !(p.revents & POLLNVAL), "poll V4L2 decoder failed");
    }
    for (;;) {
        v4l2_event event{};
        if (call(fd_, VIDIOC_DQEVENT, &event) < 0) {
            check(errno == EAGAIN || errno == ENOENT, "DQEVENT failed");
            break;
        }
        if (event.type != V4L2_EVENT_SOURCE_CHANGE)
            continue;
        // A driver may repeat SOURCE_CHANGE once CAPTURE is streaming even
        // though nothing changed. Only a real change needs reconfiguration.
        if (capture_on_ && capture_format_unchanged())
            trace("%s", "ignoring SOURCE_CHANGE without a CAPTURE format change");
        else
            source_change_ = true;
    }
    if (source_change_ && !capture_on_)
        configure_capture();
    for (;;) {
        QueueBuffer q(OUTPUT);
        if (call(fd_, VIDIOC_DQBUF, &q.buffer) < 0) {
            check(errno == EAGAIN, "DQBUF OUTPUT failed");
            break;
        }
        check(q.buffer.index < output_.size(), "invalid OUTPUT index");
        output_[q.buffer.index]->queued = false;
        check(!(q.buffer.flags & V4L2_BUF_FLAG_ERROR), "decoder bitstream error",
              VA_STATUS_ERROR_DECODING_ERROR);
    }
    if (!capture_on_ || last_)
        return;
    for (;;) {
        QueueBuffer q(CAPTURE, 0, capture_planes_);
        if (call(fd_, VIDIOC_DQBUF, &q.buffer) < 0) {
            check(errno == EAGAIN || errno == EPIPE, "DQBUF CAPTURE failed");
            break;
        }
        check(q.buffer.index < capture_.size(), "invalid CAPTURE index");
        auto &m = capture_[q.buffer.index];
        m->queued = false;
        const auto &luma = q.planes[0];
        trace("CAPTURE index=%u used=%u offset=%u ts=%lld:%lld flags=%x", q.buffer.index,
              luma.bytesused, luma.data_offset, (long long)q.buffer.timestamp.tv_sec,
              (long long)q.buffer.timestamp.tv_usec, q.buffer.flags);
        const uint64_t token =
            uint64_t(q.buffer.timestamp.tv_sec) * 1000000 + q.buffer.timestamp.tv_usec;
        // Linlon can emit two pictures for one OUTPUT buffer: a decode-only
        // (hidden VP9) frame, which completes the VA surface, then the shown
        // copy requested by show-existing. The repeat is simply requeued.
        const bool repeated =
            driver_ == CodecDriver::Mvx && token && token < next_token_ && !pending_.count(token);
        if (repeated)
            trace("ignoring repeated CAPTURE token=%llu index=%u flags=%x",
                  (unsigned long long)token, q.buffer.index, q.buffer.flags);
        if (luma.bytesused > luma.data_offset && !repeated) {
            auto it = pending_.find(token);
            check(it != pending_.end(), "unmatched CAPTURE timestamp",
                  VA_STATUS_ERROR_DECODING_ERROR);
            auto surface = it->second;
            m->data_offset = luma.data_offset;
            if (capture_planes_ > 1)
                m->chroma_offset = q.planes[1].data_offset;
            surface->status = q.buffer.flags & V4L2_BUF_FLAG_ERROR ? VA_STATUS_ERROR_DECODING_ERROR
                                                                   : VA_STATUS_SUCCESS;
            if (surface->status == VA_STATUS_SUCCESS && surface->persistent_export) {
                // Keep the fd/offset/pitch which the client already imported.
                // CAPTURE selection is owned by the stateful decoder, not VA.
                try {
                    check(bool(surface->memory), "persistent export lost its storage");
                    copier_.copy(surface->memory, m, surface->width, surface->height);
                } catch (const Error &e) {
                    surface->status = e.status;
                    surface->pending = false;
                    pending_.erase(it);
                    throw;
                }
            } else if (!surface->persistent_export) {
                surface->memory = m;
            }
            surface->pending = false;
            trace("decoded token=%llu capture=%u flags=%x", (unsigned long long)token,
                  q.buffer.index, q.buffer.flags);
            pending_.erase(it);
        }
        if (q.buffer.flags & V4L2_BUF_FLAG_LAST) {
            last_ = true;
            break;
        }
    }
    check(!source_change_, "in-context dynamic resolution change not yet implemented",
          VA_STATUS_ERROR_UNIMPLEMENTED);
}
void Decoder::submit(const std::vector<uint8_t> &bytes, const std::shared_ptr<Surface> &surface) {
    std::lock_guard<std::mutex> lock(mutex_);
    // VLC allocates its renderer pool before creating the FFmpeg context,
    // which can pass zero render targets to VA. Preserve the allocation hint
    // on each surface so CAPTURE can cover that pool plus in-flight output.
    if (!capture_on_)
        pool_size_ = std::min(64u, std::max(pool_size_, surface->allocation_count + 4));
    pump(0);
    auto start = std::chrono::steady_clock::now();
    std::shared_ptr<Memory> slot;
    while (!slot) {
        for (auto &m : output_)
            if (!m->queued) {
                slot = m;
                break;
            }
        if (slot)
            break;
        check(!expired(start), "OUTPUT queue timeout", VA_STATUS_ERROR_TIMEDOUT);
        pump(20);
    }
    check(!bytes.empty() && bytes.size() <= slot->size, "compressed picture too large",
          VA_STATUS_ERROR_INVALID_BUFFER);
    std::memcpy(slot->mapping, bytes.data(), bytes.size());
    if (dump_) {
        fwrite(bytes.data(), 1, bytes.size(), dump_);
        fflush(dump_);
    }
    uint64_t token = next_token_++;
    QueueBuffer q(OUTPUT, slot->index);
    q.planes[0].bytesused = bytes.size();
    q.planes[0].length = slot->size;
    q.buffer.timestamp.tv_sec = token / 1000000;
    q.buffer.timestamp.tv_usec = token % 1000000;
    // Linlon marks the end of a compressed frame with KEYFRAME on OUTPUT
    // (MVX_BUFFER_EOF). Without it the firmware waits for the next frame's
    // start code, so a picture only completes after a later submission.
    if (driver_ == CodecDriver::Mvx)
        q.buffer.flags |= V4L2_BUF_FLAG_KEYFRAME;
    checked(fd_, VIDIOC_QBUF, &q.buffer, "QBUF OUTPUT");
    slot->queued = true;
    surface->pending = true;
    surface->token = token;
    surface->status = VA_STATUS_SUCCESS;
    pending_[token] = surface;
    trace("submit token=%llu bytes=%zu", (unsigned long long)token, bytes.size());
    if (!capture_on_) {
        while (!capture_on_) {
            check(!expired(start), "initial SOURCE_CHANGE timeout", VA_STATUS_ERROR_TIMEDOUT);
            pump(20);
        }
    }
}
void Decoder::refresh() {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (lock.owns_lock())
        pump(0);
}
void Decoder::finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto start = std::chrono::steady_clock::now();
    while (!pending_.empty()) {
        check(!expired(start), "pending pictures at context destruction", VA_STATUS_ERROR_TIMEDOUT);
        pump(20);
    }
}
void Decoder::sync(const std::shared_ptr<Surface> &surface) {
    std::unique_lock<std::mutex> lock(mutex_);
    pump(0);
    auto start = std::chrono::steady_clock::now();
    while (surface->pending) {
        if (expired(start)) {
            unsigned queued = 0, retained = 0;
            for (auto &m : capture_) {
                queued += m->queued;
                retained += m.use_count() > 1;
            }
            trace("sync timeout token=%llu pending=%zu capture=%zu queued=%u retained=%u",
                  (unsigned long long)surface->token, pending_.size(), capture_.size(), queued,
                  retained);
            throw Error(VA_STATUS_ERROR_TIMEDOUT, "surface sync timeout");
        }
        // Decode-order output makes each submitted picture independently
        // synchronizable. Never issue STOP based on an idle-time heuristic:
        // vaSyncSurface is not an end-of-stream notification.
        lock.unlock();
        pollfd p{fd_, POLLIN | POLLPRI, 0};
        poll(&p, 1, 10);
        lock.lock();
        pump(0);
        check(!last_ || !surface->pending, "surface missing at LAST",
              VA_STATUS_ERROR_DECODING_ERROR);
    }
    if (surface->status != VA_STATUS_SUCCESS)
        throw Error(surface->status, "surface decode failed");
}
} // namespace irisva
