// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace sparkface::fallback {

inline constexpr std::uint32_t kWidth = 1920;
inline constexpr std::uint32_t kHeight = 1080;
inline constexpr std::uint32_t kFrameRate = 30;
inline constexpr std::size_t kNv12Bytes =
    static_cast<std::size_t>(kWidth) * kHeight * 3 / 2;
inline constexpr std::uint32_t kSlotCount = 3;
inline constexpr wchar_t kRingFilePath[] = L"C:\\Users\\Public\\SparkFacePhase0B2Ring.bin";
inline constexpr wchar_t kEventName[] = L"Local\\SparkFacePhase0B2FrameReady";
inline constexpr std::uint32_t kMagic = 0x32424653U; // "SFB2"
inline constexpr std::uint32_t kVersion = 1;

struct alignas(64) RingHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t frame_bytes;
    std::uint32_t slot_count;
    std::int64_t qpc_frequency;
    volatile LONG64 published_sequence;
    std::array<std::byte, 16> reserved{};
};

struct alignas(64) SlotHeader {
    volatile LONG64 sequence;
    std::int64_t producer_qpc;
    std::int64_t published_qpc;
    std::array<std::byte, 40> reserved{};
};

inline constexpr std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

inline constexpr std::size_t kHeaderBytes = align_up(sizeof(RingHeader), 64);
inline constexpr std::size_t kSlotBytes = align_up(sizeof(SlotHeader) + kNv12Bytes, 64);
inline constexpr std::size_t kMappingBytes = kHeaderBytes + kSlotCount * kSlotBytes;

inline SlotHeader* slot_header(void* base, std::uint32_t index) {
    auto* bytes = static_cast<std::byte*>(base);
    return reinterpret_cast<SlotHeader*>(bytes + kHeaderBytes + index * kSlotBytes);
}

inline const SlotHeader* slot_header(const void* base, std::uint32_t index) {
    auto* bytes = static_cast<const std::byte*>(base);
    return reinterpret_cast<const SlotHeader*>(bytes + kHeaderBytes + index * kSlotBytes);
}

inline std::byte* slot_payload(SlotHeader* slot) {
    return reinterpret_cast<std::byte*>(slot) + sizeof(SlotHeader);
}

inline const std::byte* slot_payload(const SlotHeader* slot) {
    return reinterpret_cast<const std::byte*>(slot) + sizeof(SlotHeader);
}

class Handle {
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
    Handle(Handle&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
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

class MappingView {
  public:
    MappingView() = default;
    explicit MappingView(void* value) : value_(value) {}
    ~MappingView() {
        if (value_ != nullptr) {
            UnmapViewOfFile(value_);
        }
    }
    MappingView(const MappingView&) = delete;
    MappingView& operator=(const MappingView&) = delete;
    MappingView(MappingView&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
    MappingView& operator=(MappingView&& other) noexcept {
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
    void* value_ = nullptr;
};

inline LONG64 atomic_load(volatile LONG64* value) {
    return InterlockedCompareExchange64(value, 0, 0);
}

class ProducerRing {
  public:
    ProducerRing()
        : file_(CreateFileW(kRingFilePath, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr)),
          event_(CreateEventW(nullptr, FALSE, FALSE, kEventName)) {
        if (file_.get() == INVALID_HANDLE_VALUE || event_.get() == nullptr) {
            throw std::runtime_error("Failed to create fallback transport file");
        }
        LARGE_INTEGER current_size{};
        if (!GetFileSizeEx(file_.get(), &current_size)) {
            throw std::runtime_error("Failed to read fallback transport file size");
        }
        if (current_size.QuadPart != static_cast<LONGLONG>(kMappingBytes)) {
            LARGE_INTEGER required_size{};
            required_size.QuadPart = static_cast<LONGLONG>(kMappingBytes);
            if (!SetFilePointerEx(file_.get(), required_size, nullptr, FILE_BEGIN) ||
                !SetEndOfFile(file_.get())) {
                throw std::runtime_error("Failed to size fallback transport file");
            }
        }
        mapping_ = Handle(CreateFileMappingW(file_.get(), nullptr, PAGE_READWRITE, 0, 0, nullptr));
        if (mapping_.get() == nullptr) {
            throw std::runtime_error("Failed to create fallback file mapping");
        }
        view_ = MappingView(MapViewOfFile(mapping_.get(), FILE_MAP_ALL_ACCESS, 0, 0, kMappingBytes));
        if (view_.get() == nullptr) {
            throw std::runtime_error("Failed to map fallback transport file");
        }

        std::memset(view_.get(), 0, kMappingBytes);
        auto* header = static_cast<RingHeader*>(view_.get());
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        header->magic = kMagic;
        header->version = kVersion;
        header->width = kWidth;
        header->height = kHeight;
        header->frame_bytes = static_cast<std::uint32_t>(kNv12Bytes);
        header->slot_count = kSlotCount;
        header->qpc_frequency = frequency.QuadPart;
        InterlockedExchange64(&header->published_sequence, 0);
    }

    void publish(const std::byte* frame, std::uint64_t sequence,
                 std::int64_t producer_qpc, std::int64_t published_qpc) {
        const auto index = static_cast<std::uint32_t>(sequence % kSlotCount);
        auto* slot = slot_header(view_.get(), index);
        InterlockedExchange64(&slot->sequence, 0);
        slot->producer_qpc = producer_qpc;
        slot->published_qpc = published_qpc;
        std::memcpy(slot_payload(slot), frame, kNv12Bytes);
        MemoryBarrier();
        InterlockedExchange64(&slot->sequence, static_cast<LONG64>(sequence));
        auto* header = static_cast<RingHeader*>(view_.get());
        InterlockedExchange64(&header->published_sequence, static_cast<LONG64>(sequence));
        SetEvent(event_.get());
    }

  private:
    Handle file_;
    Handle mapping_;
    Handle event_;
    MappingView view_;
};

class ConsumerRing {
  public:
    ConsumerRing() {
        for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
            file_ = Handle(CreateFileW(kRingFilePath, GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (file_.get() != INVALID_HANDLE_VALUE) {
                mapping_ = Handle(
                    CreateFileMappingW(file_.get(), nullptr, PAGE_READWRITE, 0, 0, nullptr));
            }
            event_ = Handle(OpenEventW(SYNCHRONIZE, FALSE, kEventName));
            if (mapping_.get() != nullptr && event_.get() != nullptr) {
                break;
            }
            file_ = Handle();
            mapping_ = Handle();
            event_ = Handle();
            Sleep(100);
        }
        if (mapping_.get() == nullptr || event_.get() == nullptr) {
            throw std::runtime_error("Fallback producer did not appear within 10 seconds");
        }
        view_ = MappingView(
            MapViewOfFile(mapping_.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kMappingBytes));
        if (view_.get() == nullptr) {
            throw std::runtime_error("Failed to map fallback transport file");
        }
        const auto* header = static_cast<const RingHeader*>(view_.get());
        if (header->magic != kMagic || header->version != kVersion ||
            header->frame_bytes != kNv12Bytes || header->slot_count != kSlotCount) {
            throw std::runtime_error("Fallback shared-memory transport has an incompatible layout");
        }
    }

    bool read_latest(std::byte* destination, std::uint64_t& sequence,
                     std::int64_t& producer_qpc, std::int64_t& published_qpc,
                     DWORD timeout_ms) {
        const auto* header = static_cast<const RingHeader*>(view_.get());
        auto* published = const_cast<volatile LONG64*>(&header->published_sequence);
        LONG64 latest = atomic_load(published);
        if (latest <= 0 || static_cast<std::uint64_t>(latest) == sequence) {
            WaitForSingleObject(event_.get(), timeout_ms);
            latest = atomic_load(published);
        }
        if (latest <= 0 || static_cast<std::uint64_t>(latest) == sequence) {
            return false;
        }

        const auto index = static_cast<std::uint32_t>(latest % kSlotCount);
        const auto* slot = slot_header(view_.get(), index);
        auto* slot_sequence = const_cast<volatile LONG64*>(&slot->sequence);
        const LONG64 before = atomic_load(slot_sequence);
        if (before != latest) {
            return false;
        }
        std::memcpy(destination, slot_payload(slot), kNv12Bytes);
        MemoryBarrier();
        const LONG64 after = atomic_load(slot_sequence);
        if (before != after || after != latest) {
            return false;
        }
        sequence = static_cast<std::uint64_t>(after);
        producer_qpc = slot->producer_qpc;
        published_qpc = slot->published_qpc;
        return true;
    }

  private:
    Handle file_;
    Handle mapping_;
    Handle event_;
    MappingView view_;
};

} // namespace sparkface::fallback
