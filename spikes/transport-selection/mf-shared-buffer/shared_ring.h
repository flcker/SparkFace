// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace SparkFaceSharedBuffer {

inline constexpr std::uint32_t MaxWidth = 1920;
inline constexpr std::uint32_t MaxHeight = 1080;
inline constexpr std::uint32_t SlotCount = 8;
inline constexpr std::size_t MaxFrameBytes =
    static_cast<std::size_t>(MaxWidth) * MaxHeight * 3 / 2;
inline constexpr std::uint32_t Magic = 0x4D534653U; // "SFSM"
inline constexpr std::uint32_t Version = 2;
inline constexpr std::uint16_t PayloadMagic = 0xD311U;
inline constexpr wchar_t RingPath[] =
    L"C:\\Users\\Public\\SparkFaceTransportSelectionRing.bin";
inline constexpr wchar_t FrameReadyEventName[] =
    L"Local\\SparkFacePhase0B2FrameReady";
inline constexpr wchar_t SourceOwnedModePath[] =
    L"C:\\Users\\Public\\SparkFaceTransportSelectionSourceOwned.flag";
inline constexpr wchar_t VideoModePath[] =
    L"C:\\Users\\Public\\SparkFaceTransportVideoMode.bin";

struct VideoMode {
    std::uint32_t width = MaxWidth;
    std::uint32_t height = MaxHeight;
    std::uint32_t frameRate = 30;
};

inline constexpr std::size_t frame_bytes(const VideoMode& mode) {
    return static_cast<std::size_t>(mode.width) * mode.height * 3 / 2;
}

inline constexpr bool valid_video_mode(const VideoMode& mode) {
    return mode.width > 0 && mode.width <= MaxWidth && (mode.width % 2) == 0 &&
           mode.height > 0 && mode.height <= MaxHeight && (mode.height % 2) == 0 &&
           mode.frameRate > 0 && mode.frameRate <= 60;
}

inline HRESULT read_video_mode(VideoMode* output) {
    if (output == nullptr) {
        return E_POINTER;
    }
    HANDLE file = CreateFileW(VideoModePath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        const HRESULT result = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(file);
        return result;
    }
    if (size.QuadPart != static_cast<LONGLONG>(sizeof(VideoMode))) {
        CloseHandle(file);
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    VideoMode mode{};
    DWORD bytesRead = 0;
    if (!ReadFile(file, &mode, sizeof(mode), &bytesRead, nullptr) ||
        bytesRead != sizeof(mode)) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error);
    }
    CloseHandle(file);
    if (!valid_video_mode(mode)) {
        return E_INVALIDARG;
    }
    *output = mode;
    return S_OK;
}
static_assert(sizeof(VideoMode) == 12);

struct alignas(64) RingHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t frameBytes;
    std::uint32_t frameRate;
    std::uint32_t slotCount;
    std::uint32_t ownerMode;
    std::uint32_t producerPid;
    std::int64_t qpcFrequency;
    volatile LONG64 publishedSequence;
    volatile LONG64 noFreeSlotCount;
    volatile LONG64 bufferLockCalls;
    volatile LONG64 bufferLock2DCalls;
    volatile LONG64 contiguousCopyToCalls;
    volatile LONG64 contiguousCopyFromCalls;
    volatile LONG64 copy2DToCalls;
    volatile LONG64 releasedBufferCount;
};

struct alignas(64) SlotHeader {
    volatile LONG64 sequence;
    std::int64_t producerQpc;
    std::int64_t publishedQpc;
    volatile LONG leaseCount;
    std::array<std::byte, 36> reserved{};
};

inline constexpr std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

inline constexpr std::size_t HeaderBytes = align_up(sizeof(RingHeader), 64);
inline constexpr std::size_t SlotBytes = align_up(sizeof(SlotHeader) + MaxFrameBytes, 64);
inline constexpr std::size_t MappingBytes = HeaderBytes + SlotCount * SlotBytes;

inline SlotHeader* slot_header(void* base, std::uint32_t index) {
    auto* bytes = static_cast<std::byte*>(base);
    return reinterpret_cast<SlotHeader*>(bytes + HeaderBytes + index * SlotBytes);
}

inline const SlotHeader* slot_header(const void* base, std::uint32_t index) {
    auto* bytes = static_cast<const std::byte*>(base);
    return reinterpret_cast<const SlotHeader*>(bytes + HeaderBytes + index * SlotBytes);
}

inline std::byte* slot_payload(SlotHeader* slot) {
    return reinterpret_cast<std::byte*>(slot) + sizeof(SlotHeader);
}

inline const std::byte* slot_payload(const SlotHeader* slot) {
    return reinterpret_cast<const std::byte*>(slot) + sizeof(SlotHeader);
}

inline LONG64 atomic_load(volatile LONG64* value) {
    return InterlockedCompareExchange64(value, 0, 0);
}

inline LONG atomic_load(volatile LONG* value) {
    return InterlockedCompareExchange(value, 0, 0);
}

} // namespace SparkFaceSharedBuffer
