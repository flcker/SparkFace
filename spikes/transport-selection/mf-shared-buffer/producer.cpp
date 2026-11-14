// SPDX-License-Identifier: Apache-2.0
#include "shared_ring.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace SparkFaceSharedBuffer;

namespace {

class Handle final {
  public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.value_) {
        other.value_ = nullptr;
    }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
                CloseHandle(value_);
            }
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const { return value_; }

  private:
    HANDLE value_ = nullptr;
};

class View final {
  public:
    explicit View(void* value) : value_(value) {}
    ~View() {
        if (value_ != nullptr) {
            UnmapViewOfFile(value_);
        }
    }
    View(const View&) = delete;
    View& operator=(const View&) = delete;
    View(View&& other) noexcept : value_(other.value_) {
        other.value_ = nullptr;
    }
    View& operator=(View&& other) noexcept {
        if (this != &other) {
            if (value_ != nullptr) {
                UnmapViewOfFile(value_);
            }
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }
    [[nodiscard]] void* get() const { return value_; }

  private:
    void* value_;
};

std::int64_t qpc_now() {
    LARGE_INTEGER value{};
    if (!QueryPerformanceCounter(&value)) {
        throw std::runtime_error("QueryPerformanceCounter failed");
    }
    return value.QuadPart;
}

struct Options {
    std::uint64_t frames = 1200;
    VideoMode mode{};
    bool sourceOwned = false;
    bool requestCredit = false;
};

Options parse_options(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--frames" && index + 1 < argc) {
            options.frames = std::stoull(argv[++index]);
        } else if (argument == L"--width" && index + 1 < argc) {
            options.mode.width = std::stoul(argv[++index]);
        } else if (argument == L"--height" && index + 1 < argc) {
            options.mode.height = std::stoul(argv[++index]);
        } else if (argument == L"--frame-rate" && index + 1 < argc) {
            options.mode.frameRate = std::stoul(argv[++index]);
        } else if (argument == L"--source-owned") {
            options.sourceOwned = true;
        } else if (argument == L"--request-credit") {
            options.requestCredit = true;
        } else {
            throw std::invalid_argument(
                "usage: transport_shared_buffer_producer [--frames N] [--width N] "
                "[--height N] [--frame-rate N] [--source-owned] [--request-credit]");
        }
    }
    if (options.frames == 0) {
        throw std::invalid_argument("--frames must be positive");
    }
    if (!valid_video_mode(options.mode)) {
        throw std::invalid_argument("Video mode is invalid or exceeds 1920x1080 at 60fps");
    }
    if (options.requestCredit && !options.sourceOwned) {
        throw std::invalid_argument("--request-credit requires --source-owned");
    }
    return options;
}

void encode_word(std::byte* luma, std::uint32_t first_bit,
                 std::uint32_t bit_count, std::uint64_t value) {
    for (std::uint32_t bit = 0; bit < bit_count; ++bit) {
        const auto block = first_bit + bit;
        const std::byte encoded =
            ((value >> bit) & 1U) != 0 ? std::byte{235} : std::byte{16};
        std::fill_n(luma + block * 8, 8, encoded);
    }
}

void generate_frame(std::byte* destination, const VideoMode& mode,
                    std::uint64_t sequence, std::int64_t producer_qpc) {
    auto* luma = destination;
    auto* chroma = destination + static_cast<std::size_t>(mode.width) * mode.height;
    const auto phase = static_cast<std::uint32_t>(sequence % 220U);
    for (std::uint32_t y = 0; y < mode.height; ++y) {
        auto* row = luma + static_cast<std::size_t>(y) * mode.width;
        for (std::uint32_t x = 0; x < mode.width; ++x) {
            row[x] = static_cast<std::byte>(16U + ((x + y + phase) % 220U));
        }
    }
    std::fill_n(chroma,
                frame_bytes(mode) - static_cast<std::size_t>(mode.width) * mode.height,
                std::byte{128});
    encode_word(luma, 0, 16, PayloadMagic);
    encode_word(luma, 16, 64, sequence);
    encode_word(luma, 80, 64, static_cast<std::uint64_t>(producer_qpc));
    encode_word(luma, 144, 32, GetCurrentProcessId());
}

class ProducerRing final {
  public:
    ProducerRing(bool sourceOwned, bool requestCredit, const VideoMode& mode)
        : mode_(mode), pacingMode_(requestCredit ? PacingMode::RequestCredit
                                                : PacingMode::Latest) {
        HANDLE demand = create_transport_event(FrameDemandEventName);
        demandEvent_ = Handle(demand);
        if (demandEvent_.get() == nullptr) {
            throw std::runtime_error("CreateEvent frame-demand failed: " +
                                     std::to_string(GetLastError()));
        }
        if (sourceOwned) {
            for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
                file_ = Handle(CreateFileW(RingPath, GENERIC_READ | GENERIC_WRITE,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           nullptr, OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL, nullptr));
                if (file_.get() != INVALID_HANDLE_VALUE) {
                    break;
                }
                Sleep(100);
            }
        } else {
            file_ = Handle(CreateFileW(RingPath, GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr));
        }
        if (file_.get() == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("CreateFile ring failed");
        }
        if (!sourceOwned) {
            LARGE_INTEGER size{};
            size.QuadPart = static_cast<LONGLONG>(MappingBytes);
            if (!SetFilePointerEx(file_.get(), size, nullptr, FILE_BEGIN) ||
                !SetEndOfFile(file_.get())) {
                throw std::runtime_error("SetEndOfFile ring failed");
            }
        }
        mapping_ = Handle(CreateFileMappingW(file_.get(), nullptr, PAGE_READWRITE,
                                             0, 0, nullptr));
        if (mapping_.get() == nullptr) {
            throw std::runtime_error("CreateFileMapping ring failed");
        }
        view_ = View(MapViewOfFile(mapping_.get(), FILE_MAP_ALL_ACCESS, 0, 0,
                                   MappingBytes));
        if (view_.get() == nullptr) {
            throw std::runtime_error("MapViewOfFile ring failed");
        }
        frameReadyEvent_ =
            Handle(CreateEventW(nullptr, FALSE, FALSE, FrameReadyEventName));
        if (frameReadyEvent_.get() == nullptr) {
            throw std::runtime_error("CreateEvent frame-ready failed");
        }
        auto* header = static_cast<RingHeader*>(view_.get());
        if (!sourceOwned) {
            std::memset(view_.get(), 0, MappingBytes);
            LARGE_INTEGER frequency{};
            if (!QueryPerformanceFrequency(&frequency)) {
                throw std::runtime_error("QueryPerformanceFrequency failed");
            }
            header->magic = Magic;
            header->version = Version;
            header->width = mode_.width;
            header->height = mode_.height;
            header->frameBytes = static_cast<std::uint32_t>(frame_bytes(mode_));
            header->frameRate = mode_.frameRate;
            header->slotCount = SlotCount;
            header->ownerMode = 1;
            header->pacingMode = static_cast<std::uint32_t>(pacingMode_);
            header->qpcFrequency = frequency.QuadPart;
        } else {
            bool initialized = false;
            for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
                MemoryBarrier();
                initialized =
                    header->magic == Magic && header->version == Version &&
                    header->ownerMode == 2 && header->width == mode_.width &&
                    header->height == mode_.height &&
                    header->frameBytes == frame_bytes(mode_) &&
                    header->frameRate == mode_.frameRate &&
                    header->pacingMode ==
                        static_cast<std::uint32_t>(pacingMode_);
                if (initialized) {
                    break;
                }
                Sleep(100);
            }
            if (!initialized) {
                throw std::runtime_error("source-owned ring layout is invalid");
            }
        }
        header->producerPid = GetCurrentProcessId();
    }

    void run(std::uint64_t frameCount) {
        auto* header = static_cast<RingHeader*>(view_.get());
        const std::int64_t start = qpc_now();
        if (pacingMode_ == PacingMode::RequestCredit) {
            run_request_credit(frameCount);
        } else {
            run_latest(frameCount, start);
        }
        const double seconds = static_cast<double>(qpc_now() - start) /
                               static_cast<double>(header->qpcFrequency);
        const LONG64 totalRequestQpc =
            atomic_load(&header->requestToPublishTotalQpc);
        const double requestToPublishAverageMs =
            pacingMode_ == PacingMode::RequestCredit
                ? static_cast<double>(totalRequestQpc) * 1000.0 /
                      static_cast<double>(header->qpcFrequency) /
                      static_cast<double>(frameCount)
                : 0.0;
        const double requestToPublishMaxMs =
            static_cast<double>(atomic_load(&header->requestToPublishMaxQpc)) *
            1000.0 / static_cast<double>(header->qpcFrequency);
        std::cout << std::fixed << std::setprecision(3)
                  << "producer_width=" << mode_.width << '\n'
                  << "producer_height=" << mode_.height << '\n'
                  << "producer_frame_rate=" << mode_.frameRate << '\n'
                  << "producer_pacing_mode="
                  << (pacingMode_ == PacingMode::RequestCredit ? "request-credit"
                                                               : "latest")
                  << '\n'
                  << "producer_frames=" << frameCount << '\n'
                  << "producer_fps="
                  << static_cast<double>(frameCount - 1) / seconds << '\n'
                  << "requested_sequence="
                  << atomic_load(&header->requestedSequence) << '\n'
                  << "published_sequence="
                  << atomic_load(&header->publishedSequence) << '\n'
                  << "consumed_sequence="
                  << atomic_load(&header->consumedSequence) << '\n'
                  << "cancelled_through_sequence="
                  << atomic_load(&header->cancelledThroughSequence) << '\n'
                  << "credit_signal_count="
                  << atomic_load(&header->creditSignalCount) << '\n'
                  << "cancelled_credit_count="
                  << atomic_load(&header->cancelledCreditCount) << '\n'
                  << "credit_wait_count="
                  << atomic_load(&header->creditWaitCount) << '\n'
                  << "request_to_publish_average_ms="
                  << requestToPublishAverageMs << '\n'
                  << "request_to_publish_max_ms=" << requestToPublishMaxMs
                  << '\n'
                  << "no_free_slot_count="
                  << atomic_load(&header->noFreeSlotCount) << '\n'
                  << "buffer_lock_calls="
                  << atomic_load(&header->bufferLockCalls) << '\n'
                  << "buffer_lock2d_calls="
                  << atomic_load(&header->bufferLock2DCalls) << '\n'
                  << "contiguous_copy_to_calls="
                  << atomic_load(&header->contiguousCopyToCalls) << '\n'
                  << "contiguous_copy_from_calls="
                  << atomic_load(&header->contiguousCopyFromCalls) << '\n'
                  << "copy2d_to_calls="
                  << atomic_load(&header->copy2DToCalls) << '\n'
                  << "released_buffer_count="
                  << atomic_load(&header->releasedBufferCount) << '\n'
                  << "SHARED_BUFFER_PRODUCER_PASS\n";
    }

  private:
    void run_latest(std::uint64_t frameCount, std::int64_t start) {
        auto* header = static_cast<RingHeader*>(view_.get());
        for (std::uint64_t sequence = 1; sequence <= frameCount; ++sequence) {
            const std::int64_t target =
                start + static_cast<std::int64_t>(
                            (sequence - 1) * header->qpcFrequency / mode_.frameRate);
            while (true) {
                const std::int64_t remaining = target - qpc_now();
                if (remaining <= 0) {
                    break;
                }
                const DWORD sleepMs = static_cast<DWORD>(
                    remaining * 1000 / header->qpcFrequency);
                if (sleepMs > 1) {
                    Sleep(sleepMs - 1);
                } else {
                    SwitchToThread();
                }
            }
            if (!publish(sequence, 0)) {
                --sequence;
                Sleep(1);
            }
        }
    }

    void run_request_credit(std::uint64_t frameCount) {
        auto* header = static_cast<RingHeader*>(view_.get());
        std::uint64_t produced = 0;
        LONG64 nextSequence = 1;
        while (produced < frameCount) {
            const LONG64 cancelled =
                atomic_load(&header->cancelledThroughSequence);
            if (cancelled >= nextSequence) {
                nextSequence = cancelled + 1;
                continue;
            }
            const LONG64 requested = atomic_load(&header->requestedSequence);
            const LONG64 consumed = atomic_load(&header->consumedSequence);
            if (requested < nextSequence || consumed < nextSequence - 1) {
                InterlockedIncrement64(&header->creditWaitCount);
                WaitForSingleObject(demandEvent_.get(), 5000);
                continue;
            }
            auto* credit = credit_record(header, nextSequence);
            if (atomic_load(&credit->sequence) != nextSequence) {
                SwitchToThread();
                continue;
            }
            if (!publish(static_cast<std::uint64_t>(nextSequence),
                         credit->requestQpc)) {
                Sleep(1);
                continue;
            }
            ++produced;
            ++nextSequence;
        }
        while (true) {
            const LONG64 requested = atomic_load(&header->requestedSequence);
            const LONG64 consumed = atomic_load(&header->consumedSequence);
            const LONG64 published = atomic_load(&header->publishedSequence);
            const LONG64 released = atomic_load(&header->releasedBufferCount);
            if (requested >= static_cast<LONG64>(frameCount) &&
                consumed >= requested && released >= published) {
                break;
            }
            InterlockedIncrement64(&header->creditWaitCount);
            WaitForSingleObject(demandEvent_.get(), 5000);
        }
    }

    bool publish(std::uint64_t sequence, std::int64_t requestQpc) {
        auto* header = static_cast<RingHeader*>(view_.get());
        SlotHeader* selected = nullptr;
        const std::uint32_t preferred =
            static_cast<std::uint32_t>(sequence % SlotCount);
        for (std::uint32_t offset = 0; offset < SlotCount; ++offset) {
            auto* candidate = slot_header(
                view_.get(), (preferred + offset) % SlotCount);
            if (InterlockedCompareExchange(&candidate->leaseCount, -1, 0) == 0) {
                selected = candidate;
                break;
            }
        }
        if (selected == nullptr) {
            InterlockedIncrement64(&header->noFreeSlotCount);
            return false;
        }

        InterlockedExchange64(&selected->sequence, 0);
        const std::int64_t producerQpc = qpc_now();
        generate_frame(slot_payload(selected), mode_, sequence, producerQpc);
        selected->requestQpc = requestQpc;
        selected->producerQpc = producerQpc;
        selected->publishedQpc = qpc_now();
        if (requestQpc > 0) {
            const LONG64 elapsed = selected->publishedQpc - requestQpc;
            InterlockedExchangeAdd64(&header->requestToPublishTotalQpc, elapsed);
            LONG64 maximum = atomic_load(&header->requestToPublishMaxQpc);
            while (elapsed > maximum) {
                const LONG64 observed = InterlockedCompareExchange64(
                    &header->requestToPublishMaxQpc, elapsed, maximum);
                if (observed == maximum) {
                    break;
                }
                maximum = observed;
            }
        }
        MemoryBarrier();
        InterlockedExchange64(&selected->sequence,
                              static_cast<LONG64>(sequence));
        InterlockedExchange(&selected->leaseCount, 0);
        InterlockedExchange64(&header->publishedSequence,
                              static_cast<LONG64>(sequence));
        if (!SetEvent(frameReadyEvent_.get())) {
            throw std::runtime_error("SetEvent frame-ready failed");
        }
        return true;
    }

    VideoMode mode_;
    PacingMode pacingMode_;
    Handle file_;
    Handle mapping_;
    View view_{nullptr};
    Handle frameReadyEvent_;
    Handle demandEvent_;
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const auto options = parse_options(argc, argv);
        ProducerRing ring(options.sourceOwned, options.requestCredit, options.mode);
        ring.run(options.frames);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SHARED_BUFFER_PRODUCER_ERROR: " << error.what() << '\n';
        return 1;
    }
}
