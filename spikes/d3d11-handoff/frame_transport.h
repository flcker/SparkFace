// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace sparkface::d3d11_handoff {

inline constexpr std::uint32_t kWidth = 1920;
inline constexpr std::uint32_t kHeight = 1080;
inline constexpr std::uint32_t kFrameRate = 30;
inline constexpr std::uint32_t kSlotCount = 3;
inline constexpr std::uint32_t kMagic = 0x44474653U; // "SFGD"
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint16_t kPayloadMagic = 0xD311U;
inline constexpr wchar_t kControlPath[] = L"C:\\Users\\Public\\SparkFacePhase0DControl.bin";
inline constexpr wchar_t kFrameReadyEventName[] = L"Local\\SparkFacePhase0DFrameReady";
inline constexpr wchar_t kTextureNamePrefix[] = L"Global\\SparkFacePhase0DTexture";

struct alignas(64) SlotMetadata {
    volatile LONG64 sequence;
    std::int64_t producer_qpc;
    std::int64_t published_qpc;
    std::array<std::byte, 40> reserved{};
};

struct alignas(64) ControlBlock {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t slot_count;
    std::uint32_t producer_pid;
    LUID adapter_luid;
    std::int64_t qpc_frequency;
    volatile LONG64 published_sequence;
    volatile LONG published_slot;
    std::array<std::byte, 12> reserved{};
    std::array<SlotMetadata, kSlotCount> slots{};
};

inline LONG64 atomic_load(volatile LONG64* value) {
    return InterlockedCompareExchange64(value, 0, 0);
}

inline LONG atomic_load(volatile LONG* value) {
    return InterlockedCompareExchange(value, 0, 0);
}

inline std::wstring texture_name(std::uint32_t producer_pid, std::uint32_t slot) {
    return std::wstring(kTextureNamePrefix) + std::to_wstring(producer_pid) + L"_" +
           std::to_wstring(slot);
}

class Handle {
  public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }
    void reset(HANDLE value = nullptr) {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }
    [[nodiscard]] HANDLE get() const { return value_; }

  private:
    HANDLE value_ = nullptr;
};

class MappingView {
  public:
    MappingView() = default;
    explicit MappingView(void* value) : value_(value) {}
    ~MappingView() { reset(); }
    MappingView(const MappingView&) = delete;
    MappingView& operator=(const MappingView&) = delete;
    MappingView(MappingView&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
    MappingView& operator=(MappingView&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }
    void reset(void* value = nullptr) {
        if (value_ != nullptr) {
            UnmapViewOfFile(value_);
        }
        value_ = value;
    }
    [[nodiscard]] void* get() const { return value_; }

  private:
    void* value_ = nullptr;
};

class PermissiveSecurityAttributes {
  public:
    PermissiveSecurityAttributes() {
        if (!InitializeSecurityDescriptor(&descriptor_, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorDacl(&descriptor_, TRUE, nullptr, FALSE)) {
            throw std::runtime_error("Failed to initialize probe IPC security");
        }
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &descriptor_;
        attributes_.bInheritHandle = FALSE;
    }

    SECURITY_ATTRIBUTES* get() { return &attributes_; }

  private:
    SECURITY_DESCRIPTOR descriptor_{};
    SECURITY_ATTRIBUTES attributes_{};
};

inline SECURITY_ATTRIBUTES* permissive_security_attributes() {
    // Probe-only: Frame Server runs under a restricted token and must open the
    // control file, event, and D3D texture handles across the process boundary.
    static PermissiveSecurityAttributes security;
    return security.get();
}

class ProducerControl {
  public:
    explicit ProducerControl(LUID adapter_luid)
        : file_(CreateFileW(kControlPath, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            permissive_security_attributes(), OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr)),
          mapping_(file_.get() == INVALID_HANDLE_VALUE
                       ? nullptr
                       : CreateFileMappingW(file_.get(), permissive_security_attributes(),
                                            PAGE_READWRITE, 0,
                                            static_cast<DWORD>(sizeof(ControlBlock)), nullptr)),
          event_(CreateEventW(permissive_security_attributes(), FALSE, FALSE,
                              kFrameReadyEventName)) {
        if (file_.get() == INVALID_HANDLE_VALUE || mapping_.get() == nullptr ||
            event_.get() == nullptr) {
            throw std::runtime_error("Failed to create Spike D control objects");
        }
        view_ = MappingView(MapViewOfFile(mapping_.get(), FILE_MAP_ALL_ACCESS, 0, 0,
                                          sizeof(ControlBlock)));
        if (view_.get() == nullptr) {
            throw std::runtime_error("Failed to map Spike D control block");
        }
        auto* control = get();
        ZeroMemory(control, sizeof(*control));
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency)) {
            throw std::runtime_error("QueryPerformanceFrequency failed");
        }
        control->magic = kMagic;
        control->version = kVersion;
        control->width = kWidth;
        control->height = kHeight;
        control->slot_count = kSlotCount;
        control->producer_pid = GetCurrentProcessId();
        control->adapter_luid = adapter_luid;
        control->qpc_frequency = frequency.QuadPart;
    }

    void publish(std::uint32_t slot, std::uint64_t sequence, std::int64_t producer_qpc,
                 std::int64_t published_qpc) {
        auto* control = get();
        auto& metadata = control->slots.at(slot);
        metadata.producer_qpc = producer_qpc;
        metadata.published_qpc = published_qpc;
        MemoryBarrier();
        InterlockedExchange64(&metadata.sequence, static_cast<LONG64>(sequence));
        InterlockedExchange(&control->published_slot, static_cast<LONG>(slot));
        InterlockedExchange64(&control->published_sequence, static_cast<LONG64>(sequence));
        SetEvent(event_.get());
    }

  private:
    [[nodiscard]] ControlBlock* get() const {
        return static_cast<ControlBlock*>(view_.get());
    }

    Handle file_;
    Handle mapping_;
    Handle event_;
    MappingView view_;
};

} // namespace sparkface::d3d11_handoff
