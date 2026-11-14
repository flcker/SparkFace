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
    bool sourceOwned = false;
};

Options parse_options(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--frames" && index + 1 < argc) {
            options.frames = std::stoull(argv[++index]);
            if (options.frames == 0) {
                throw std::invalid_argument("--frames must be positive");
            }
        } else if (argument == L"--source-owned") {
            options.sourceOwned = true;
        } else {
            throw std::invalid_argument(
                "usage: transport_shared_buffer_producer [--frames N] [--source-owned]");
        }
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

void generate_frame(std::byte* destination, std::uint64_t sequence,
                    std::int64_t producer_qpc) {
    auto* luma = destination;
    auto* chroma = destination + static_cast<std::size_t>(Width) * Height;
    const auto phase = static_cast<std::uint32_t>(sequence % 220U);
    for (std::uint32_t y = 0; y < Height; ++y) {
        auto* row = luma + static_cast<std::size_t>(y) * Width;
        for (std::uint32_t x = 0; x < Width; ++x) {
            row[x] = static_cast<std::byte>(16U + ((x + y + phase) % 220U));
        }
    }
    std::fill_n(chroma, FrameBytes - static_cast<std::size_t>(Width) * Height,
                std::byte{128});
    encode_word(luma, 0, 16, PayloadMagic);
    encode_word(luma, 16, 64, sequence);
    encode_word(luma, 80, 64, static_cast<std::uint64_t>(producer_qpc));
    encode_word(luma, 144, 32, GetCurrentProcessId());
}

class ProducerRing final {
  public:
    explicit ProducerRing(bool sourceOwned) {
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
        event_ = Handle(CreateEventW(nullptr, FALSE, FALSE, FrameReadyEventName));
        if (event_.get() == nullptr) {
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
            header->width = Width;
            header->height = Height;
            header->frameBytes = static_cast<std::uint32_t>(FrameBytes);
            header->slotCount = SlotCount;
            header->ownerMode = 1;
            header->qpcFrequency = frequency.QuadPart;
        } else if (header->magic != Magic || header->version != Version ||
                   header->ownerMode != 2 || header->frameBytes != FrameBytes) {
            throw std::runtime_error("source-owned ring layout is invalid");
        }
        header->producerPid = GetCurrentProcessId();
    }

    void run(std::uint64_t frame_count) {
        auto* header = static_cast<RingHeader*>(view_.get());
        const std::int64_t start = qpc_now();
        for (std::uint64_t sequence = 1; sequence <= frame_count; ++sequence) {
            const std::int64_t target =
                start + static_cast<std::int64_t>(
                            (sequence - 1) * header->qpcFrequency / FrameRate);
            while (true) {
                const std::int64_t remaining = target - qpc_now();
                if (remaining <= 0) {
                    break;
                }
                const DWORD sleep_ms = static_cast<DWORD>(
                    remaining * 1000 / header->qpcFrequency);
                if (sleep_ms > 1) {
                    Sleep(sleep_ms - 1);
                } else {
                    SwitchToThread();
                }
            }

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
                --sequence;
                Sleep(1);
                continue;
            }

            InterlockedExchange64(&selected->sequence, 0);
            const std::int64_t producer_qpc = qpc_now();
            generate_frame(slot_payload(selected), sequence, producer_qpc);
            selected->producerQpc = producer_qpc;
            selected->publishedQpc = qpc_now();
            MemoryBarrier();
            InterlockedExchange64(&selected->sequence,
                                  static_cast<LONG64>(sequence));
            InterlockedExchange(&selected->leaseCount, 0);
            InterlockedExchange64(&header->publishedSequence,
                                  static_cast<LONG64>(sequence));
            SetEvent(event_.get());
        }
        const double seconds = static_cast<double>(qpc_now() - start) /
                               static_cast<double>(header->qpcFrequency);
        std::cout << std::fixed << std::setprecision(3)
                  << "producer_frames=" << frame_count << '\n'
                  << "producer_fps="
                  << static_cast<double>(frame_count - 1) / seconds << '\n'
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
    Handle file_;
    Handle mapping_;
    View view_{nullptr};
    Handle event_;
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const auto options = parse_options(argc, argv);
        ProducerRing ring(options.sourceOwned);
        ring.run(options.frames);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SHARED_BUFFER_PRODUCER_ERROR: " << error.what() << '\n';
        return 1;
    }
}
