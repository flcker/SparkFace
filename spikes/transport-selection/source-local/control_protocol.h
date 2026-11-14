// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace SparkFaceSourceLocalControl {

inline constexpr wchar_t MappingName[] =
    L"Global\\SparkFaceSourceLocalControl-v1";
inline constexpr wchar_t EnablePath[] =
    L"C:\\Users\\Public\\SparkFaceTransportSourceLocalH9.flag";
inline constexpr wchar_t AssetName[] = L"SparkFaceSourceLocal.fixture";
inline constexpr std::uint32_t Magic = 0x39304C48U; // H9L9
inline constexpr std::uint16_t Version = 1;
inline constexpr std::uint32_t EffectMinimum = 0;
inline constexpr std::uint32_t EffectMaximum = 100;
inline constexpr std::uint64_t HeartbeatMaximumAgeMilliseconds = 2000;
inline constexpr std::uint32_t AssetHash = 0x1C92CCAAU;
inline constexpr char AssetBytes[] = "SparkFace source-local H9 fixture\n";
inline constexpr std::size_t AssetByteCount = sizeof(AssetBytes) - 1;
inline constexpr std::uint32_t MarkerMagicFirstBit = 0;
inline constexpr std::uint32_t MarkerSequenceFirstBit = 16;
inline constexpr std::uint32_t MarkerQpcFirstBit = 80;
inline constexpr std::uint32_t MarkerSourcePidFirstBit = 144;
inline constexpr std::uint32_t MarkerRevisionFirstBit = 176;
inline constexpr std::uint32_t MarkerEffectValueFirstBit = 208;
inline constexpr std::uint32_t MarkerStatusFirstBit = 240;
inline constexpr std::uint32_t MarkerBitCount = 272;
inline constexpr std::uint32_t MarkerPixelWidth = 6;
static_assert(MarkerBitCount * MarkerPixelWidth <= 1920,
              "H9 marker must fit the source frame width");


enum class Status : std::uint32_t {
    Applied = 0,
    Disabled = 1,
    AssetMissing = 2,
    AssetInvalid = 3,
    MappingUnavailable = 4,
    ProtocolInvalid = 5,
    PublicationBusy = 6,
    ProducerDead = 7,
    HeartbeatStale = 8,
    ValueInvalid = 9,
};

#pragma pack(push, 8)
struct ControlBlock {
    volatile LONG publicationSequence;
    std::uint32_t magic;
    std::uint16_t structSize;
    std::uint16_t version;
    std::uint32_t revision;
    std::uint32_t producerPid;
    std::int64_t heartbeatQpc;
    std::uint32_t flags;
    std::uint32_t effectValue;
    std::uint64_t reserved[2];
};
#pragma pack(pop)

static_assert(sizeof(ControlBlock) == 56, "H9 protocol layout must be stable");
static_assert(std::is_standard_layout<ControlBlock>::value,
              "H9 protocol must be standard layout");
static_assert(std::is_trivial<ControlBlock>::value,
              "H9 protocol must be trivial");
static_assert(offsetof(ControlBlock, publicationSequence) == 0,
              "seqlock must be first");
static_assert(offsetof(ControlBlock, heartbeatQpc) == 24,
              "H9 protocol layout must be stable");
static_assert(offsetof(ControlBlock, effectValue) == 36,
              "H9 protocol layout must be stable");

struct Snapshot {
    std::uint32_t revision = 0;
    std::uint32_t producerPid = 0;
    std::int64_t heartbeatQpc = 0;
    std::uint32_t flags = 0;
    std::uint32_t effectValue = 0;
};

inline std::uint32_t AssetHashBytes(const char* bytes, std::size_t count) {
    std::uint32_t hash = 2166136261U;
    for (std::size_t index = 0; index < count; ++index) {
        hash ^= static_cast<unsigned char>(bytes[index]);
        hash *= 16777619U;
    }
    return hash;
}

inline void Publish(ControlBlock* block, const Snapshot& snapshot,
                    bool protocolMismatch) {
    const LONG before = InterlockedCompareExchange(&block->publicationSequence,
                                                    0, 0);
    const LONG odd = (before & 1) == 0 ? before + 1 : before + 2;
    InterlockedExchange(&block->publicationSequence, odd);
    MemoryBarrier();
    block->magic = protocolMismatch ? Magic ^ 0xFFFFFFFFU : Magic;
    block->structSize = static_cast<std::uint16_t>(sizeof(ControlBlock));
    block->version = Version;
    block->revision = snapshot.revision;
    block->producerPid = snapshot.producerPid;
    block->heartbeatQpc = snapshot.heartbeatQpc;
    block->flags = snapshot.flags;
    block->effectValue = snapshot.effectValue;
    block->reserved[0] = 0;
    block->reserved[1] = 0;
    MemoryBarrier();
    InterlockedExchange(&block->publicationSequence, odd + 1);
}

inline bool ReadSnapshot(const ControlBlock* block, Snapshot* snapshot) {
    if (block == nullptr || snapshot == nullptr) {
        return false;
    }
    const LONG first = ReadAcquire(&block->publicationSequence);
    if ((first & 1) != 0) {
        return false;
    }
    MemoryBarrier();
    const std::uint32_t magic = block->magic;
    const std::uint16_t structSize = block->structSize;
    const std::uint16_t version = block->version;
    Snapshot candidate{};
    candidate.revision = block->revision;
    candidate.producerPid = block->producerPid;
    candidate.heartbeatQpc = block->heartbeatQpc;
    candidate.flags = block->flags;
    candidate.effectValue = block->effectValue;
    MemoryBarrier();
    const LONG second = ReadAcquire(&block->publicationSequence);
    if (first != second || (second & 1) != 0 || magic != Magic ||
        structSize != sizeof(ControlBlock) || version != Version) {
        return false;
    }
    *snapshot = candidate;
    return true;
}

inline bool IsHeartbeatFresh(std::int64_t heartbeatQpc, std::int64_t nowQpc,
                             std::int64_t qpcFrequency) {
    if (heartbeatQpc <= 0 || nowQpc < heartbeatQpc || qpcFrequency <= 0) {
        return false;
    }
    const std::uint64_t age = static_cast<std::uint64_t>(nowQpc - heartbeatQpc);
    const std::uint64_t maximum =
        (static_cast<std::uint64_t>(qpcFrequency) * HeartbeatMaximumAgeMilliseconds) /
        1000U;
    return age <= maximum;
}

inline bool IsEffectValueValid(std::uint32_t value) {
    return value >= EffectMinimum && value <= EffectMaximum;
}

} // namespace SparkFaceSourceLocalControl
