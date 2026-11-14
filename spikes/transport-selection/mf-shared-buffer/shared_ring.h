// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace SparkFaceSharedBuffer {

inline constexpr std::uint32_t Width = 1920;
inline constexpr std::uint32_t Height = 1080;
inline constexpr std::uint32_t FrameRate = 30;
inline constexpr std::uint32_t SlotCount = 8;
inline constexpr std::size_t FrameBytes =
    static_cast<std::size_t>(Width) * Height * 3 / 2;
inline constexpr std::uint32_t Magic = 0x4D534653U; // "SFSM"
inline constexpr std::uint32_t Version = 1;
inline constexpr std::uint16_t PayloadMagic = 0xD311U;
inline constexpr wchar_t RingPath[] =
    L"C:\\Users\\Public\\SparkFaceTransportSelectionRing.bin";
inline constexpr wchar_t FrameReadyEventName[] =
    L"Local\\SparkFacePhase0B2FrameReady";
inline constexpr wchar_t SourceOwnedModePath[] =
    L"C:\\Users\\Public\\SparkFaceTransportSelectionSourceOwned.flag";

struct alignas(64) RingHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t frameBytes;
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
inline constexpr std::size_t SlotBytes = align_up(sizeof(SlotHeader) + FrameBytes, 64);
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
